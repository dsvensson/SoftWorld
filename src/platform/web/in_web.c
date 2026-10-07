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
// in_web.c -- keyboard and mouse in a browser page (in_web.js has the page's
// events)
//
// The page's events are queued as they come and sent to the game in the
// frame, as the other systems take their window's. Keys go by where they are
// on the keyboard (KeyboardEvent.code, placed as the Windows scancodes are),
// and the text they type by the layout. While the game wants the mouse it is
// locked to the page, and motion comes from the lock: raw counts where the
// browser has them (unadjustedMovement), else the system's pointer motion.
// Otherwise the mouse is left alone, its buttons and wheel going to the game
// while it points (in menus).

#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "print.h"
#include "vid.h"
#include "web_local.h"

#include <stdlib.h>
#include <string.h>

cvar_t	_windowed_mouse = {.name = "_windowed_mouse", .string = "1", .archive = true,
	.description = "Captures the mouse in a window too, as fullscreen always does; the console and menus let it go.",
	.values = (const cvar_value_t[]){{"0", "Captured only in fullscreen"}, {"1", "Captured in a window too"}, {0}}};

// in_web.js's events
enum { WEB_KEY, WEB_BUTTON, WEB_FOCUS, WEB_HIDDEN, WEB_FULLSCREEN, WEB_USEREXIT };

typedef struct
{
	int		type;
	int		down;
	int		value;			// a key's text, a button, a state
	int		captured;		// the mouse was locked when the button came
	char	code[32];		// a key's KeyboardEvent.code
} web_event_t;

void	web_in_start (void);
bool	web_in_next (web_event_t *ev);
bool	web_in_setcapture (bool want);
bool	web_in_raw (void);
void	web_in_takemotion (int *dx, int *dy);
int		web_in_takewheel (void);
char	*web_in_pasted (void);
void	web_in_copy (const char *text);

// the keys by KeyboardEvent.code, but the letters' and digits', placed as the
// Windows scancodes are: the keypad is the navigation keys, as without Num Lock
static const struct
{
	const char	*code;
	int			key;
} in_codes[] =
{
	{"Escape", K_ESCAPE}, {"Minus", '-'}, {"Equal", '='}, {"Backspace", K_BACKSPACE}, {"Tab", K_TAB},
	{"BracketLeft", '['}, {"BracketRight", ']'}, {"Enter", K_ENTER}, {"ControlLeft", K_CTRL},
	{"Semicolon", ';'}, {"Quote", '\''}, {"Backquote", '`'}, {"ShiftLeft", K_SHIFT}, {"Backslash", '\\'},
	{"Comma", ','}, {"Period", '.'}, {"Slash", '/'}, {"ShiftRight", K_SHIFT}, {"NumpadMultiply", '*'},
	{"AltLeft", K_ALT}, {"Space", K_SPACE},
	{"F1", K_F1}, {"F2", K_F2}, {"F3", K_F3}, {"F4", K_F4}, {"F5", K_F5}, {"F6", K_F6},
	{"F7", K_F7}, {"F8", K_F8}, {"F9", K_F9}, {"F10", K_F10}, {"F11", K_F11}, {"F12", K_F12},
	{"Numpad7", K_HOME}, {"Numpad8", K_UPARROW}, {"Numpad9", K_PGUP}, {"NumpadSubtract", '-'},
	{"Numpad4", K_LEFTARROW}, {"Numpad5", '5'}, {"Numpad6", K_RIGHTARROW}, {"NumpadAdd", '+'},
	{"Numpad1", K_END}, {"Numpad2", K_DOWNARROW}, {"Numpad3", K_PGDN}, {"Numpad0", K_INS}, {"NumpadDecimal", K_DEL},
	{"NumpadEnter", K_ENTER}, {"ControlRight", K_CTRL}, {"NumpadDivide", '/'}, {"AltRight", K_ALT},
	{"NumpadEqual", '='},
	{"Home", K_HOME}, {"ArrowUp", K_UPARROW}, {"PageUp", K_PGUP}, {"ArrowLeft", K_LEFTARROW},
	{"ArrowRight", K_RIGHTARROW}, {"End", K_END}, {"ArrowDown", K_DOWNARROW}, {"PageDown", K_PGDN},
	{"Insert", K_INS}, {"Delete", K_DEL}, {"Pause", K_PAUSE},
};

// the buttons by MouseEvent.button
static const int	in_buttons[5] = {K_MOUSE1, K_MOUSE3, K_MOUSE2, K_MOUSE4, K_MOUSE5};

static bool		in_keydown[256];		// the keys whose downs were sent
static bool		in_captured;
static bool		in_toldraw;				// the lock's motion was told once

/*
===============================================================================

EVENTS

===============================================================================
*/

static int IN_KeyFromCode (const char *code)
{
	size_t	i;

	if (!strncmp (code, "Key", 3) && code[3] >= 'A' && code[3] <= 'Z' && !code[4])
		return code[3] - 'A' + 'a';
	if (!strncmp (code, "Digit", 5) && code[5] >= '0' && code[5] <= '9' && !code[6])
		return code[5];
	for (i = 0 ; i < sizeof(in_codes) / sizeof(in_codes[0]) ; i++)
		if (!strcmp (code, in_codes[i].code))
			return in_codes[i].key;
	return 0;
}

// a down the game had before the focus came isn't let up
static void IN_Key (int key, bool down)
{
	if (!down && !in_keydown[key])
		return;
	in_keydown[key] = down;
	Key_Event (key, down);
}

static void IN_Wheel (int steps)
{
	int		key = steps < 0 ? K_MWHEELUP : K_MWHEELDOWN;

	for (steps = abs (steps) ; steps > 0 ; steps--)
	{
		Key_Event (key, true);
		Key_Event (key, false);
	}
}

void IN_StartEvents (void)
{
	web_in_start ();
}

void IN_PumpEvents (void)
{
	web_event_t	ev;
	int			key;

	while (web_in_next (&ev))
		switch (ev.type)
		{
		case WEB_KEY:
			key = IN_KeyFromCode (ev.code);
			if (key)
				IN_Key (key, ev.down);
			// the text after the key, as the systems send it
			if (ev.down && ev.value)
				Key_CharEvent (ev.value);
			break;

		case WEB_BUTTON:
			// locked, every button is the game's; otherwise only where they
			// can be bound (menus): not the click that locks the mouse
			if (ev.value < 0 || ev.value >= 5)
				break;
			key = in_buttons[ev.value];
			if (ev.down && !(ev.captured || IN_WantsMouseButtons ()))
				break;
			IN_Key (key, ev.down);
			break;

		case WEB_FOCUS:
			VID_AppActivate (ev.value != 0);
			break;

		case WEB_HIDDEN:
			VID_WindowSuspended (ev.value != 0);
			break;

		case WEB_FULLSCREEN:
			VID_SetFullscreenState (ev.value != 0);
			break;

		case WEB_USEREXIT:
			// Escape let the mouse go, and the page didn't see the key: the
			// menu, as the key opens it
			if (ActiveApp)
			{
				Key_Event (K_ESCAPE, true);
				Key_Event (K_ESCAPE, false);
			}
			break;
		}

	if (ActiveApp)
		IN_Wheel (web_in_takewheel ());
	else
		web_in_takewheel ();
}

char *IN_PastedText (void)
{
	return web_in_pasted ();
}

void IN_CopyText (const char *text)
{
	web_in_copy (text);
}

/*
===============
IN_WindowActivated
===============
*/
void IN_WindowActivated (bool active)
{
	if (!active)
	{
		web_in_setcapture (false);
		in_captured = false;
	}
	memset (in_keydown, 0, sizeof(in_keydown));
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
	IN_InitGamepad ();
}

void IN_Shutdown (void)
{
	web_in_setcapture (false);
	in_captured = false;
}

void IN_Commands (void)
{
	bool	want;
	int		dx, dy;

	want = ActiveApp && !Minimized && IN_WantsMouse ()
		&& (VID_IsFullscreen () || _windowed_mouse.value);
	in_captured = web_in_setcapture (want);
	if (in_captured)
	{
		if (!in_toldraw)
		{
			Con_Printf (web_in_raw () ? "Mouse: raw motion\n"
				: "Mouse: the system's motion, accelerated (no raw motion in this browser)\n");
			in_toldraw = true;
		}
		web_in_takemotion (&dx, &dy);
		if (dx || dy)
			IN_MouseMotion (dx, dy);
	}
	else
		web_in_takemotion (&dx, &dy);		// nothing moved while it wasn't captured

	IN_PollGamepad ();
}
