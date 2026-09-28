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
// in_mac.m -- keyboard and mouse input from the window's events
//
// Keys are read by their virtual key code, the key's place on the keyboard, so
// bindings don't depend on the layout; typed text comes from the event's
// characters, which do. While the game wants the mouse, it is captured: the
// cursor is hidden and held still in the window, and motion comes from
// GameController's GCMouse, raw counts the mouse speed setting doesn't change
// (from the events' deltas, which it does, for pointers GameController doesn't
// see, such as trackpads). Otherwise the mouse is left alone, its buttons and
// wheel going to the game while it points (in menus).

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <GameController/GameController.h>

#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "vid.h"
#include "mac_local.h"

#include <IOKit/hidsystem/IOLLEvent.h>
#include <stdatomic.h>

cvar_t	_windowed_mouse = {.name = "_windowed_mouse", .string = "1", .archive = true};

// the keys by virtual key code (HIToolbox/Events.h), placed as the Windows
// scancodes are: the keypad is the navigation keys, as without Num Lock
static const byte keycodetokey[128] =
{
	[kVK_ANSI_A] = 'a', [kVK_ANSI_S] = 's', [kVK_ANSI_D] = 'd', [kVK_ANSI_F] = 'f',
	[kVK_ANSI_H] = 'h', [kVK_ANSI_G] = 'g', [kVK_ANSI_Z] = 'z', [kVK_ANSI_X] = 'x',
	[kVK_ANSI_C] = 'c', [kVK_ANSI_V] = 'v', [kVK_ANSI_B] = 'b', [kVK_ANSI_Q] = 'q',
	[kVK_ANSI_W] = 'w', [kVK_ANSI_E] = 'e', [kVK_ANSI_R] = 'r', [kVK_ANSI_Y] = 'y',
	[kVK_ANSI_T] = 't', [kVK_ANSI_1] = '1', [kVK_ANSI_2] = '2', [kVK_ANSI_3] = '3',
	[kVK_ANSI_4] = '4', [kVK_ANSI_6] = '6', [kVK_ANSI_5] = '5', [kVK_ANSI_Equal] = '=',
	[kVK_ANSI_9] = '9', [kVK_ANSI_7] = '7', [kVK_ANSI_Minus] = '-', [kVK_ANSI_8] = '8',
	[kVK_ANSI_0] = '0', [kVK_ANSI_RightBracket] = ']', [kVK_ANSI_O] = 'o', [kVK_ANSI_U] = 'u',
	[kVK_ANSI_LeftBracket] = '[', [kVK_ANSI_I] = 'i', [kVK_ANSI_P] = 'p', [kVK_Return] = K_ENTER,
	[kVK_ANSI_L] = 'l', [kVK_ANSI_J] = 'j', [kVK_ANSI_Quote] = '\'', [kVK_ANSI_K] = 'k',
	[kVK_ANSI_Semicolon] = ';', [kVK_ANSI_Backslash] = '\\', [kVK_ANSI_Comma] = ',',
	[kVK_ANSI_Slash] = '/', [kVK_ANSI_N] = 'n', [kVK_ANSI_M] = 'm', [kVK_ANSI_Period] = '.',
	[kVK_Tab] = K_TAB, [kVK_Space] = K_SPACE, [kVK_ANSI_Grave] = '`', [kVK_Delete] = K_BACKSPACE,
	[kVK_Escape] = K_ESCAPE,
	[kVK_Shift] = K_SHIFT, [kVK_RightShift] = K_SHIFT, [kVK_Option] = K_ALT, [kVK_RightOption] = K_ALT,
	[kVK_Control] = K_CTRL, [kVK_RightControl] = K_CTRL,
	[kVK_ANSI_KeypadDecimal] = K_DEL, [kVK_ANSI_KeypadMultiply] = '*', [kVK_ANSI_KeypadPlus] = '+',
	[kVK_ANSI_KeypadDivide] = '/', [kVK_ANSI_KeypadEnter] = K_ENTER, [kVK_ANSI_KeypadMinus] = '-',
	[kVK_ANSI_KeypadEquals] = '=', [kVK_ANSI_Keypad0] = K_INS, [kVK_ANSI_Keypad1] = K_END,
	[kVK_ANSI_Keypad2] = K_DOWNARROW, [kVK_ANSI_Keypad3] = K_PGDN, [kVK_ANSI_Keypad4] = K_LEFTARROW,
	[kVK_ANSI_Keypad5] = '5', [kVK_ANSI_Keypad6] = K_RIGHTARROW, [kVK_ANSI_Keypad7] = K_HOME,
	[kVK_ANSI_Keypad8] = K_UPARROW, [kVK_ANSI_Keypad9] = K_PGUP,
	[kVK_F1] = K_F1, [kVK_F2] = K_F2, [kVK_F3] = K_F3, [kVK_F4] = K_F4, [kVK_F5] = K_F5,
	[kVK_F6] = K_F6, [kVK_F7] = K_F7, [kVK_F8] = K_F8, [kVK_F9] = K_F9, [kVK_F10] = K_F10,
	[kVK_F11] = K_F11, [kVK_F12] = K_F12,
	[kVK_F15] = K_PAUSE,		// where Pause is on Apple's extended keyboards
	[kVK_Help] = K_INS,			// and Insert
	[kVK_Home] = K_HOME, [kVK_PageUp] = K_PGUP, [kVK_ForwardDelete] = K_DEL, [kVK_End] = K_END,
	[kVK_PageDown] = K_PGDN, [kVK_LeftArrow] = K_LEFTARROW, [kVK_RightArrow] = K_RIGHTARROW,
	[kVK_DownArrow] = K_DOWNARROW, [kVK_UpArrow] = K_UPARROW,
};

// KBGetLayoutType is exported by HIToolbox but no longer declared
extern OSType KBGetLayoutType (SInt16 keyboardType);
#define KEYBOARD_ISO	'ISO '

static bool		in_iso;				// ISO keyboard: two key codes are swapped
static bool		in_keydown[128];	// the key codes whose downs were sent
static float	in_scroll;			// precise (trackpad) scrolling not yet a wheel step

static bool		in_captured;
static atomic_bool		in_gccapture;	// in_captured, for the GCMouse handlers
static atomic_llong		in_gcdx, in_gcdy;	// GCMouse motion since the last frame, 1/256 counts
static _Atomic double	in_gctime;		// when a GCMouse last moved
static dispatch_queue_t	in_mousequeue;	// the GCMouse handlers'
static double	in_nsdx, in_nsdy;		// the events' motion since the last frame
static double	in_restx, in_resty;		// motion not yet a whole count

#define SCROLL_STEP		12.0f		// points of precise scrolling for a wheel step

/*
=======
IN_MapKey

Maps a virtual key code to a quake key number
=======
*/
static int IN_MapKey (unsigned code)
{
	// Apple's ISO keyboards give the key left of 1 the code of the key beside
	// left shift on ANSI ones, and that key the section key's
	if (in_iso && (code == kVK_ISO_Section || code == kVK_ANSI_Grave))
		code = code == kVK_ISO_Section ? kVK_ANSI_Grave : kVK_ISO_Section;
	return code < 128 ? keycodetokey[code] : 0;
}

static void IN_Key (unsigned code, bool down)
{
	if (code >= 128)
		return;
	if (!down && !in_keydown[code])
		return;		// a down AppKit had (a Command shortcut), or one before the focus
	in_keydown[code] = down;
	Key_Event (IN_MapKey (code), down);
}

// the modifier key an event changed, down or up by its side's flag
static void IN_FlagsChanged (NSEvent *event)
{
	static const struct { unsigned short code; NSUInteger mask; } modifiers[] =
	{
		{kVK_Shift, NX_DEVICELSHIFTKEYMASK}, {kVK_RightShift, NX_DEVICERSHIFTKEYMASK},
		{kVK_Control, NX_DEVICELCTLKEYMASK}, {kVK_RightControl, NX_DEVICERCTLKEYMASK},
		{kVK_Option, NX_DEVICELALTKEYMASK}, {kVK_RightOption, NX_DEVICERALTKEYMASK},
		{kVK_Command, NX_DEVICELCMDKEYMASK}, {kVK_RightCommand, NX_DEVICERCMDKEYMASK},
	};
	NSUInteger	flags = event.modifierFlags;
	size_t		i;

	for (i = 0 ; i < sizeof(modifiers) / sizeof(modifiers[0]) ; i++)
		if (event.keyCode == modifiers[i].code)
		{
			bool	down = (flags & modifiers[i].mask) != 0;

			if (down != in_keydown[event.keyCode])
				IN_Key (event.keyCode, down);
			return;
		}
}

static void IN_KeyDown (NSEvent *event)
{
	NSString	*chars;
	NSUInteger	i;
	unichar		c;

	IN_Key (event.keyCode, true);

	// then the text it typed, as WM_CHAR follows WM_KEYDOWN
	chars = event.characters;
	for (i = 0 ; i < chars.length ; i++)
	{
		c = [chars characterAtIndex:i];
		if (c < 128)
			Key_CharEvent (c);
	}
}

/*
===============================================================================

MOUSE

===============================================================================
*/

// the mouse is over the picture, not the title bar or another window
static bool IN_InContent (NSEvent *event)
{
	return event.window == vid_window
		&& NSPointInRect (event.locationInWindow, vid_window.contentView.frame);
}

static int IN_ButtonKey (NSEvent *event)
{
	switch (event.buttonNumber)
	{
	case 0:		return K_MOUSE1;
	case 1:		return K_MOUSE2;
	case 2:		return K_MOUSE3;
	case 3:		return K_MOUSE4;
	case 4:		return K_MOUSE5;
	default:	return 0;
	}
}

static void IN_Wheel (NSEvent *event)
{
	float	dy = (float)event.scrollingDeltaY;
	int		steps, key;

	// wheel away from the player is up, whether or not scrolling is "natural"
	if (event.isDirectionInvertedFromDevice)
		dy = -dy;
	if (!dy)
		return;
	if (event.hasPreciseScrollingDeltas)
	{
		in_scroll += dy;
		steps = (int)(in_scroll / SCROLL_STEP);
		in_scroll -= steps * SCROLL_STEP;
	}
	else
		steps = dy > 0 ? 1 : -1;

	key = steps > 0 ? K_MWHEELUP : K_MWHEELDOWN;
	for (steps = abs (steps) ; steps > 0 ; steps--)
	{
		Key_Event (key, true);
		Key_Event (key, false);
	}
}

/*
===============================================================================

MOUSE CAPTURE

===============================================================================
*/

// the cursor to the middle of the picture, where clicks land in the window
static void IN_CenterCursor (void)
{
	NSRect	rect = [vid_window convertRectToScreen:[vid_window.contentView convertRect:
		vid_window.contentView.bounds toView:nil]];
	CGFloat	top = NSMaxY (NSScreen.screens.firstObject.frame);

	CGWarpMouseCursorPosition (CGPointMake (NSMidX (rect), top - NSMidY (rect)));
	// a warp holds back the mouse's events for a while, unless it is joined again
	CGAssociateMouseAndMouseCursorPosition (true);
}

static void IN_SetCapture (bool capture)
{
	if (capture == in_captured)
		return;
	in_captured = capture;

	if (capture)
	{
		IN_CenterCursor ();
		CGAssociateMouseAndMouseCursorPosition (false);
		[NSCursor hide];
		vid_window.acceptsMouseMovedEvents = YES;
	}
	else
	{
		CGAssociateMouseAndMouseCursorPosition (true);
		[NSCursor unhide];
		vid_window.acceptsMouseMovedEvents = NO;
	}

	// nothing moved while it wasn't captured
	atomic_store (&in_gccapture, capture);
	atomic_store (&in_gcdx, 0);
	atomic_store (&in_gcdy, 0);
	in_nsdx = in_nsdy = in_restx = in_resty = 0;
}

static void IN_WatchMouse (GCMouse *mouse)
{
	mouse.handlerQueue = in_mousequeue;
	mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput *input, float deltax, float deltay) {
		(void)input;
		if (!atomic_load (&in_gccapture))
			return;
		// GameController's y is up, the game's down
		atomic_fetch_add (&in_gcdx, llroundf (deltax * 256));
		atomic_fetch_add (&in_gcdy, llroundf (-deltay * 256));
		atomic_store (&in_gctime, CFAbsoluteTimeGetCurrent ());
	};
}

// the motion since the last frame: GameController's while a GCMouse is moving,
// the events' otherwise (they count every pointer, a GCMouse too)
static void IN_MouseMove (void)
{
	long long	gx = atomic_exchange (&in_gcdx, 0), gy = atomic_exchange (&in_gcdy, 0);
	int			dx, dy;

	if (gx || gy || CFAbsoluteTimeGetCurrent () - atomic_load (&in_gctime) < 1.0)
	{
		in_restx += gx / 256.0;
		in_resty += gy / 256.0;
	}
	else
	{
		in_restx += in_nsdx;
		in_resty += in_nsdy;
	}
	in_nsdx = in_nsdy = 0;

	dx = (int)in_restx;
	dy = (int)in_resty;
	in_restx -= dx;
	in_resty -= dy;
	if (dx || dy)
		IN_MouseMotion (dx, dy);
}

/*
===============
IN_WindowChanged

The window moved or changed size
===============
*/
void IN_WindowChanged (void)
{
	if (in_captured)
	{
		IN_CenterCursor ();
		CGAssociateMouseAndMouseCursorPosition (false);
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
	in_scroll = 0;
	Key_ClearStates ();
}

/*
===============
IN_HandleEvent

An event from the queue; true if it was input taken here, false for AppKit
to have it too
===============
*/
bool IN_HandleEvent (NSEvent *event)
{
	switch (event.type)
	{
	case NSEventTypeKeyDown:
		// Command shortcuts are the menu's (Command+Q); the rest are the game's
		if (event.modifierFlags & NSEventModifierFlagCommand)
			return false;
		IN_KeyDown (event);
		return true;

	case NSEventTypeKeyUp:
		IN_Key (event.keyCode, false);
		return true;

	case NSEventTypeFlagsChanged:
		IN_FlagsChanged (event);
		return false;

	case NSEventTypeMouseMoved:
	case NSEventTypeLeftMouseDragged:
	case NSEventTypeRightMouseDragged:
	case NSEventTypeOtherMouseDragged:
		if (!in_captured)
			return false;
		in_nsdx += event.deltaX;
		in_nsdy += event.deltaY;
		return true;

	// captured, every button is the game's; otherwise only in the picture, and
	// only where they can be bound (menus)
	case NSEventTypeLeftMouseDown:
	case NSEventTypeRightMouseDown:
	case NSEventTypeOtherMouseDown:
		if (in_captured || (IN_InContent (event) && IN_WantsMouseButtons ()))
			if (IN_ButtonKey (event))
				Key_Event (IN_ButtonKey (event), true);
		return in_captured;

	case NSEventTypeLeftMouseUp:
	case NSEventTypeRightMouseUp:
	case NSEventTypeOtherMouseUp:
		if ((in_captured || IN_WantsMouseButtons ()) && IN_ButtonKey (event))
			Key_Event (IN_ButtonKey (event), false);
		return in_captured;

	case NSEventTypeScrollWheel:
		if (event.window == vid_window)
			IN_Wheel (event);
		return true;

	default:
		return false;
	}
}

/*
===============================================================================

INPUT CONTRACT

===============================================================================
*/

void IN_Init (void)
{
	Cvar_RegisterVariable (&_windowed_mouse);
	in_iso = KBGetLayoutType ((SInt16)LMGetKbdType ()) == KEYBOARD_ISO;

	// mice come and go; the handlers are given each as it is connected
	in_mousequeue = dispatch_queue_create ("softworld.mouse", DISPATCH_QUEUE_SERIAL);
	for (GCMouse *mouse in GCMouse.mice)
		IN_WatchMouse (mouse);
	[NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
			IN_WatchMouse (note.object);
		}];
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
}
