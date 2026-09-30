/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// in_wayland.c -- keyboard and mouse input from the Wayland seat
//
// Keys are read by their evdev code, the key's place on the keyboard, so
// bindings don't depend on the layout; typed text comes from the compositor's
// keymap (xkbcommon), which does. Keys repeat here, at the compositor's rate:
// Wayland sends a key down once. While the game wants the mouse, it is
// captured: the pointer is locked in the window and hidden, and motion comes
// from relative-pointer's unaccelerated deltas, raw counts the pointer speed
// doesn't change. Otherwise the mouse is left alone, its buttons and wheel
// going to the game while it points (in menus).

#include "cvar.h"
#include "in_events.h"
#include "print.h"
#include "keys.h"
#include "sys.h"
#include "vid.h"
#include "wl_local.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

cvar_t	_windowed_mouse = {.name = "_windowed_mouse", .string = "1", .archive = true,
	.description = "Captures the mouse in a window too, as fullscreen always does; the console and menus let it go.",
	.values = (const cvar_value_t[]){{"0", "Captured only in fullscreen"}, {"1", "Captured in a window too"}, {0}}};

// the keys by evdev code, placed as the Windows scancodes are: the keypad is the
// navigation keys, as without Num Lock
static const byte	codetokey[128] =
{
	[KEY_ESC] = K_ESCAPE, [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
	[KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-',
	[KEY_EQUAL] = '=', [KEY_BACKSPACE] = K_BACKSPACE, [KEY_TAB] = K_TAB,
	[KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u',
	[KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
	[KEY_ENTER] = K_ENTER, [KEY_LEFTCTRL] = K_CTRL,
	[KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j',
	[KEY_K] = 'k', [KEY_L] = 'l', [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'', [KEY_GRAVE] = '`',
	[KEY_LEFTSHIFT] = K_SHIFT, [KEY_BACKSLASH] = '\\',
	[KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm',
	[KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/', [KEY_RIGHTSHIFT] = K_SHIFT,
	[KEY_KPASTERISK] = '*', [KEY_LEFTALT] = K_ALT, [KEY_SPACE] = K_SPACE,
	[KEY_F1] = K_F1, [KEY_F2] = K_F2, [KEY_F3] = K_F3, [KEY_F4] = K_F4, [KEY_F5] = K_F5, [KEY_F6] = K_F6,
	[KEY_F7] = K_F7, [KEY_F8] = K_F8, [KEY_F9] = K_F9, [KEY_F10] = K_F10, [KEY_F11] = K_F11, [KEY_F12] = K_F12,
	[KEY_KP7] = K_HOME, [KEY_KP8] = K_UPARROW, [KEY_KP9] = K_PGUP, [KEY_KPMINUS] = '-',
	[KEY_KP4] = K_LEFTARROW, [KEY_KP5] = '5', [KEY_KP6] = K_RIGHTARROW, [KEY_KPPLUS] = '+',
	[KEY_KP1] = K_END, [KEY_KP2] = K_DOWNARROW, [KEY_KP3] = K_PGDN, [KEY_KP0] = K_INS, [KEY_KPDOT] = K_DEL,
	[KEY_KPENTER] = K_ENTER, [KEY_RIGHTCTRL] = K_CTRL, [KEY_KPSLASH] = '/', [KEY_RIGHTALT] = K_ALT,
	[KEY_KPEQUAL] = '=',
	[KEY_HOME] = K_HOME, [KEY_UP] = K_UPARROW, [KEY_PAGEUP] = K_PGUP, [KEY_LEFT] = K_LEFTARROW,
	[KEY_RIGHT] = K_RIGHTARROW, [KEY_END] = K_END, [KEY_DOWN] = K_DOWNARROW, [KEY_PAGEDOWN] = K_PGDN,
	[KEY_INSERT] = K_INS, [KEY_DELETE] = K_DEL, [KEY_PAUSE] = K_PAUSE,
};

static struct wl_keyboard	*in_keyboard;
static struct wl_pointer	*in_pointer;
static struct xkb_context	*in_xkb;
static struct xkb_keymap	*in_keymap;
static struct xkb_state		*in_xkbstate;

static bool		in_keydown[128];		// the codes whose downs were sent
static int		in_repeatrate;			// repeats a second, 0 for none
static double	in_repeatdelay;			// seconds before the first
static int		in_repeatcode = -1;		// the key repeating
static double	in_repeattime;			// when it repeats next

static uint32_t	in_enterserial;			// the pointer's entering the window, which the cursor is set with
static bool		in_inside;				// the pointer is over the window
static struct wp_cursor_shape_device_v1	*in_cursorshape;

static bool		in_captured;
static struct zwp_locked_pointer_v1		*in_lock;
static struct zwp_relative_pointer_v1	*in_relative;
static double	in_dx, in_dy;			// motion since the last frame, in counts
static double	in_restx, in_resty;		// motion not yet a whole count

static uint32_t	in_axissource = WL_POINTER_AXIS_SOURCE_WHEEL;	// the frame's
static int		in_wheel120;			// wheel motion in 120ths of a step, not yet a step
static float	in_scroll;				// finger scrolling not yet a step
static bool		in_frame120;			// the frame had the wheel in 120ths

#define SCROLL_STEP		12.0f			// scrolled a step, by fingers

/*
===============================================================================

KEYBOARD

===============================================================================
*/

static void IN_Key (unsigned code, bool down)
{
	if (code >= 128)
		return;
	if (!down && !in_keydown[code])
		return;		// a down before the focus came
	in_keydown[code] = down;
	if (codetokey[code])
		Key_Event (codetokey[code], down);
}

// the text a key types, with the layout and modifiers; the font is ASCII
static void IN_KeyText (unsigned code)
{
	uint32_t	c;

	if (!in_xkbstate)
		return;
	c = xkb_state_key_get_utf32 (in_xkbstate, code + 8);
	if (c > 0 && c < 128)
		Key_CharEvent ((int)c);
}

static void IN_StopRepeat (void)
{
	in_repeatcode = -1;
}

double IN_NextRepeat (void)
{
	return in_repeatcode >= 0 ? in_repeattime : 0;
}

// the key held repeats at the compositor's rate, as a down and its text
void IN_Repeat (void)
{
	double	now;

	if (in_repeatcode < 0)
		return;
	now = Sys_DoubleTime ();
	while (in_repeatcode >= 0 && now >= in_repeattime)
	{
		IN_Key ((unsigned)in_repeatcode, true);
		IN_KeyText ((unsigned)in_repeatcode);
		in_repeattime += 1.0 / in_repeatrate;
	}
}

static void IN_Keymap (void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
	char	*map;

	(void)data;
	(void)keyboard;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
	{
		close (fd);
		return;
	}
	map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close (fd);
	if (map == MAP_FAILED)
		return;
	// the keymap comes as the window opens, before IN_Init
	if (!in_xkb)
		in_xkb = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
	if (!in_xkb)
	{
		munmap (map, size);
		return;
	}

	if (in_xkbstate)
		xkb_state_unref (in_xkbstate);
	if (in_keymap)
		xkb_keymap_unref (in_keymap);
	in_keymap = xkb_keymap_new_from_string (in_xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap (map, size);
	in_xkbstate = in_keymap ? xkb_state_new (in_keymap) : NULL;
}

static void IN_KeyboardEnter (void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
	struct wl_array *keys)
{
	(void)data;
	(void)keyboard;
	(void)surface;
	(void)keys;		// held before the focus came: not the game's
	way.lastserial = serial;
	VID_AppActivate (true);
}

static void IN_KeyboardLeave (void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
	VID_AppActivate (false);
}

static void IN_KeyboardKey (void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t code,
	uint32_t state)
{
	bool	down = state == WL_KEYBOARD_KEY_STATE_PRESSED;

	(void)data;
	(void)keyboard;
	(void)time;
	way.lastserial = serial;

	// Alt+Enter toggles fullscreen, as on Windows
	if (down && (code == KEY_ENTER || code == KEY_KPENTER) && (in_keydown[KEY_LEFTALT] || in_keydown[KEY_RIGHTALT]))
	{
		VID_ToggleFullscreen ();
		return;
	}

	IN_Key (code, down);
	if (!down)
	{
		if ((int)code == in_repeatcode)
			IN_StopRepeat ();
		return;
	}

	// then the text it typed, as WM_CHAR follows WM_KEYDOWN
	IN_KeyText (code);
	if (in_repeatrate > 0 && in_keymap && xkb_keymap_key_repeats (in_keymap, code + 8))
	{
		in_repeatcode = (int)code;
		in_repeattime = Sys_DoubleTime () + in_repeatdelay;
	}
}

static void IN_KeyboardModifiers (void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
	uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	if (in_xkbstate)
		xkb_state_update_mask (in_xkbstate, depressed, latched, locked, 0, 0, group);
}

static void IN_KeyboardRepeat (void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
	(void)data;
	(void)keyboard;
	in_repeatrate = rate > 0 ? rate : 0;
	in_repeatdelay = delay / 1000.0;
	if (!in_repeatrate)
		IN_StopRepeat ();
}

static const struct wl_keyboard_listener	in_keyboard_listener =
{
	.keymap = IN_Keymap,
	.enter = IN_KeyboardEnter,
	.leave = IN_KeyboardLeave,
	.key = IN_KeyboardKey,
	.modifiers = IN_KeyboardModifiers,
	.repeat_info = IN_KeyboardRepeat,
};

/*
===============================================================================

MOUSE

===============================================================================
*/

// the cursor over the window: none while captured, the compositor's arrow otherwise
static void IN_UpdateCursor (void)
{
	if (!in_pointer || !in_inside)
		return;
	if (in_captured)
		wl_pointer_set_cursor (in_pointer, in_enterserial, NULL, 0, 0);
	else if (in_cursorshape)
		wp_cursor_shape_device_v1_set_shape (in_cursorshape, in_enterserial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static int IN_ButtonKey (uint32_t button)
{
	switch (button)
	{
	case BTN_LEFT:		return K_MOUSE1;
	case BTN_RIGHT:		return K_MOUSE2;
	case BTN_MIDDLE:	return K_MOUSE3;
	case BTN_SIDE:		return K_MOUSE4;
	case BTN_EXTRA:		return K_MOUSE5;
	default:			return 0;
	}
}

static void IN_WheelSteps (int steps)
{
	int		key = steps < 0 ? K_MWHEELUP : K_MWHEELDOWN;

	for (steps = abs (steps) ; steps > 0 ; steps--)
	{
		Key_Event (key, true);
		Key_Event (key, false);
	}
}

static void IN_PointerEnter (void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
	wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)pointer;
	(void)surface;
	(void)x;
	(void)y;
	in_enterserial = serial;
	in_inside = true;
	IN_UpdateCursor ();
}

static void IN_PointerLeave (void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
	in_inside = false;
}

static void IN_PointerMotion (void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)x;
	(void)y;
}

// captured, every button is the game's; otherwise only where they can be bound (menus)
static void IN_PointerButton (void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
	uint32_t button, uint32_t state)
{
	int		key = IN_ButtonKey (button);

	(void)data;
	(void)pointer;
	(void)time;
	way.lastserial = serial;
	if (!key || !(in_captured || IN_WantsMouseButtons ()))
		return;
	Key_Event (key, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void IN_PointerAxis (void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)pointer;
	(void)time;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && in_axissource != WL_POINTER_AXIS_SOURCE_WHEEL)
		in_scroll += (float)wl_fixed_to_double (value);
}

static void IN_PointerFrame (void *data, struct wl_pointer *pointer)
{
	int		steps;

	(void)data;
	(void)pointer;
	// a wheel steps by its notches; fingers by the distance
	if (in_axissource != WL_POINTER_AXIS_SOURCE_WHEEL && !in_frame120)
	{
		steps = (int)(in_scroll / SCROLL_STEP);
		in_scroll -= steps * SCROLL_STEP;
		IN_WheelSteps (steps);
	}
	in_axissource = WL_POINTER_AXIS_SOURCE_WHEEL;
	in_frame120 = false;
}

static void IN_PointerAxisSource (void *data, struct wl_pointer *pointer, uint32_t source)
{
	(void)data;
	(void)pointer;
	in_axissource = source;
}

static void IN_PointerAxisStop (void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)pointer;
	(void)time;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		in_scroll = 0;
}

static void IN_PointerAxisDiscrete (void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
	(void)data;
	(void)pointer;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
	{
		IN_WheelSteps (discrete);
		in_frame120 = true;
	}
}

static void IN_PointerAxis120 (void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
	int		steps;

	(void)data;
	(void)pointer;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	in_frame120 = true;
	in_wheel120 += value120;
	steps = in_wheel120 / 120;
	in_wheel120 -= steps * 120;
	IN_WheelSteps (steps);
}

static const struct wl_pointer_listener	in_pointer_listener =
{
	.enter = IN_PointerEnter,
	.leave = IN_PointerLeave,
	.motion = IN_PointerMotion,
	.button = IN_PointerButton,
	.axis = IN_PointerAxis,
	.frame = IN_PointerFrame,
	.axis_source = IN_PointerAxisSource,
	.axis_stop = IN_PointerAxisStop,
	.axis_discrete = IN_PointerAxisDiscrete,
	.axis_value120 = IN_PointerAxis120,
};

/*
===============================================================================

MOUSE CAPTURE

===============================================================================
*/

static void IN_RelativeMotion (void *data, struct zwp_relative_pointer_v1 *relative, uint32_t utime_hi,
	uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
	(void)data;
	(void)relative;
	(void)utime_hi;
	(void)utime_lo;
	(void)dx;
	(void)dy;
	if (!in_captured)
		return;
	in_dx += wl_fixed_to_double (dx_unaccel);
	in_dy += wl_fixed_to_double (dy_unaccel);
}

static const struct zwp_relative_pointer_v1_listener	in_relative_listener = {.relative_motion = IN_RelativeMotion};

static void IN_Locked (void *data, struct zwp_locked_pointer_v1 *lock)
{
	(void)data;
	(void)lock;
}

static void IN_Unlocked (void *data, struct zwp_locked_pointer_v1 *lock)
{
	(void)data;
	(void)lock;
}

static const struct zwp_locked_pointer_v1_listener	in_lock_listener =
{
	.locked = IN_Locked,
	.unlocked = IN_Unlocked,
};

static void IN_SetCapture (bool capture)
{
	static bool	warned;

	if (capture == in_captured || !in_pointer)
		return;
	if (capture && (!way.constraints || !way.relativepointer))
	{
		if (!warned)
			Con_Printf ("The compositor can't lock the mouse (pointer-constraints, relative-pointer): "
				"no mouse look\n");
		warned = true;
		return;
	}
	in_captured = capture;

	if (capture)
	{
		// held where it is while captured, however long: the lock lasts until let go
		in_lock = zwp_pointer_constraints_v1_lock_pointer (way.constraints, way.surface, in_pointer, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener (in_lock, &in_lock_listener, NULL);
		in_relative = zwp_relative_pointer_manager_v1_get_relative_pointer (way.relativepointer, in_pointer);
		zwp_relative_pointer_v1_add_listener (in_relative, &in_relative_listener, NULL);
	}
	else
	{
		zwp_locked_pointer_v1_destroy (in_lock);
		zwp_relative_pointer_v1_destroy (in_relative);
		in_lock = NULL;
		in_relative = NULL;
	}
	IN_UpdateCursor ();

	// nothing moved while it wasn't captured
	in_dx = in_dy = in_restx = in_resty = 0;
}

// the motion since the last frame, in whole counts
static void IN_MouseMove (void)
{
	int		dx, dy;

	in_restx += in_dx;
	in_resty += in_dy;
	in_dx = in_dy = 0;
	dx = (int)in_restx;
	dy = (int)in_resty;
	in_restx -= dx;
	in_resty -= dy;
	if (dx || dy)
		IN_MouseMotion (dx, dy);
}

/*
===============================================================================

SEAT

===============================================================================
*/

void IN_SeatCapabilities (struct wl_seat *seat, uint32_t capabilities)
{
	bool	keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
	bool	pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;

	if (keyboard && !in_keyboard)
	{
		in_keyboard = wl_seat_get_keyboard (seat);
		wl_keyboard_add_listener (in_keyboard, &in_keyboard_listener, NULL);
	}
	else if (!keyboard && in_keyboard)
	{
		wl_keyboard_release (in_keyboard);
		in_keyboard = NULL;
	}

	if (pointer && !in_pointer)
	{
		in_pointer = wl_seat_get_pointer (seat);
		wl_pointer_add_listener (in_pointer, &in_pointer_listener, NULL);
		if (way.cursorshape)
			in_cursorshape = wp_cursor_shape_manager_v1_get_pointer (way.cursorshape, in_pointer);
	}
	else if (!pointer && in_pointer)
	{
		IN_SetCapture (false);
		if (in_cursorshape)
			wp_cursor_shape_device_v1_destroy (in_cursorshape);
		wl_pointer_release (in_pointer);
		in_cursorshape = NULL;
		in_pointer = NULL;
		in_inside = false;
	}
}

/*
===============
IN_WindowActivated
===============
*/
void IN_WindowActivated (bool active)
{
	if (!active)
		IN_SetCapture (false);
	memset (in_keydown, 0, sizeof(in_keydown));
	IN_StopRepeat ();
	in_scroll = 0;
	in_wheel120 = 0;
	Key_ClearStates ();
}

/*
===============================================================================

INPUT CONTRACT

===============================================================================
*/

void IN_Init (void)
{
	Cvar_RegisterVariable (&_windowed_mouse);
	if (!in_xkbstate)
		Con_Printf ("No keymap from the compositor: no typed text\n");
	IN_InitGamepad ();
}

void IN_Shutdown (void)
{
	IN_SetCapture (false);
}

void IN_Commands (void)
{
	bool	want;

	want = ActiveApp && !Minimized && IN_WantsMouse ()
		&& (VID_IsFullscreen () || _windowed_mouse.value);
	IN_SetCapture (want);
	if (in_captured)
		IN_MouseMove ();

	IN_PollGamepad ();
}
