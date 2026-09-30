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
// in_rawinput.c -- keyboard and mouse input from window messages
//
// Keys are read by scancode, so bindings don't depend on the keyboard layout; typed
// text comes from WM_CHAR, which does. While the game wants the mouse, it is captured:
// the cursor is hidden and clipped to the window, and motion and buttons come from
// Raw Input. Otherwise the mouse is left alone.

#include "win_local.h"

#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "vid.h"

cvar_t	_windowed_mouse = {.name = "_windowed_mouse", .string = "1", .archive = true,
	.description = "Captures the mouse in a window too, as fullscreen always does; the console and menus let it go.",
	.values = (const cvar_value_t[]){{"0", "Captured only in fullscreen"}, {"1", "Captured in a window too"}, {0}}};

static bool	in_captured;

static const byte scantokey[128] =
{
//  0           1       2       3       4       5       6       7
//  8           9       A       B       C       D       E       F
	0  ,    27,     '1',    '2',    '3',    '4',    '5',    '6',
	'7',    '8',    '9',    '0',    '-',    '=',    K_BACKSPACE, 9, // 0
	'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
	'o',    'p',    '[',    ']',    13 ,    K_CTRL,'a',  's',      // 1
	'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
	'\'' ,    '`',    K_SHIFT,'\\',  'z',    'x',    'c',    'v',      // 2
	'b',    'n',    'm',    ',',    '.',    '/',    K_SHIFT,'*',
	K_ALT,' ',   0  ,    K_F1, K_F2, K_F3, K_F4, K_F5,   // 3
	K_F6, K_F7, K_F8, K_F9, K_F10,  K_PAUSE,    0  , K_HOME,
	K_UPARROW,K_PGUP,'-',K_LEFTARROW,'5',K_RIGHTARROW,'+',K_END, //4
	K_DOWNARROW,K_PGDN,K_INS,K_DEL,0,0,             0,              K_F11,
	K_F12,0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 5
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 6
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0         // 7
};

/*
=======
IN_MapKey

Maps a window message's scancode to a quake key number
=======
*/
static int IN_MapKey (LPARAM lParam)
{
	int		key = (int)(lParam >> 16) & 255;

	if (key > 127)
		return 0;
	return scantokey[key];
}

/*
===============================================================================

MOUSE CAPTURE

===============================================================================
*/

static void IN_ClipCursor (void)
{
	RECT	rect;
	POINT	topleft = {0, 0}, bottomright;

	GetClientRect (mainwindow, &rect);
	bottomright.x = rect.right;
	bottomright.y = rect.bottom;
	ClientToScreen (mainwindow, &topleft);
	ClientToScreen (mainwindow, &bottomright);
	rect.left = topleft.x;
	rect.top = topleft.y;
	rect.right = bottomright.x;
	rect.bottom = bottomright.y;
	ClipCursor (&rect);
}

static void IN_SetCapture (bool capture)
{
	RAWINPUTDEVICE	rid;

	if (capture == in_captured)
		return;

	rid.usUsagePage = 0x01;		// generic desktop
	rid.usUsage = 0x02;			// mouse
	rid.dwFlags = capture ? RIDEV_NOLEGACY : RIDEV_REMOVE;
	rid.hwndTarget = capture ? mainwindow : NULL;
	if (!RegisterRawInputDevices (&rid, 1, sizeof(rid)) && capture)
		return;

	in_captured = capture;
	if (capture)
	{
		IN_ClipCursor ();
		while (ShowCursor (FALSE) >= 0)
			;
	}
	else
	{
		ClipCursor (NULL);
		while (ShowCursor (TRUE) < 0)
			;
	}
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
		IN_ClipCursor ();
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
	Key_ClearStates ();
}

/*
===============================================================================

WINDOW MESSAGES

===============================================================================
*/

static void IN_RawMouse (const RAWMOUSE *mouse)
{
	static const struct { USHORT down, up; int key; } buttons[] =
	{
		{RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_1_UP, K_MOUSE1},
		{RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_2_UP, K_MOUSE2},
		{RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_3_UP, K_MOUSE3},
		{RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, K_MOUSE4},
		{RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, K_MOUSE5},
	};
	USHORT	flags = mouse->usButtonFlags;
	int		i;
	short	wheel;

	if (!(mouse->usFlags & MOUSE_MOVE_ABSOLUTE) && (mouse->lLastX || mouse->lLastY))
		IN_MouseMotion (mouse->lLastX, mouse->lLastY);

	for (i=0 ; i<(int)(sizeof(buttons)/sizeof(buttons[0])) ; i++)
	{
		if (flags & buttons[i].down)
			Key_Event (buttons[i].key, true);
		if (flags & buttons[i].up)
			Key_Event (buttons[i].key, false);
	}

	if (flags & RI_MOUSE_WHEEL)
	{
		wheel = (short)mouse->usButtonData;
		if (wheel)
		{
			Key_Event (wheel > 0 ? K_MWHEELUP : K_MWHEELDOWN, true);
			Key_Event (wheel > 0 ? K_MWHEELUP : K_MWHEELDOWN, false);
		}
	}
}

/*
===============
IN_LegacyButton

A mouse button message while the mouse isn't captured
===============
*/
static void IN_LegacyButton (UINT msg, WPARAM wParam)
{
	switch (msg)
	{
	case WM_LBUTTONDOWN:	Key_Event (K_MOUSE1, true);		break;
	case WM_LBUTTONUP:		Key_Event (K_MOUSE1, false);	break;
	case WM_RBUTTONDOWN:	Key_Event (K_MOUSE2, true);		break;
	case WM_RBUTTONUP:		Key_Event (K_MOUSE2, false);	break;
	case WM_MBUTTONDOWN:	Key_Event (K_MOUSE3, true);		break;
	case WM_MBUTTONUP:		Key_Event (K_MOUSE3, false);	break;
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
		Key_Event (GET_XBUTTON_WPARAM (wParam) == XBUTTON1 ? K_MOUSE4 : K_MOUSE5, msg == WM_XBUTTONDOWN);
		break;
	default:
		break;
	}
}

/*
===============
IN_HandleMessage

Input messages for the main window. Returns true if the message was consumed;
otherwise the window procedure passes it on to DefWindowProc.
===============
*/
bool IN_HandleMessage (UINT msg, WPARAM wParam, LPARAM lParam)
{
	RAWINPUT	raw;
	UINT		size;

	switch (msg)
	{
	case WM_SYSKEYDOWN:
		if (wParam == VK_F4)
			return false;	// Alt+F4 closes the window
		Key_Event (IN_MapKey (lParam), true);
		return true;

	case WM_KEYDOWN:
		Key_Event (IN_MapKey (lParam), true);
		return true;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		Key_Event (IN_MapKey (lParam), false);
		return true;

	case WM_CHAR:
		if (wParam < 256)
			Key_CharEvent ((int)wParam);
		return true;

	case WM_SYSCHAR:
		return true;		// no menu beeps for Alt+key

	case WM_INPUT:
		size = sizeof(raw);
		if (in_captured
			&& GetRawInputData ((HRAWINPUT)lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1
			&& raw.header.dwType == RIM_TYPEMOUSE)
			IN_RawMouse (&raw.data.mouse);
		return false;		// DefWindowProc cleans up after WM_INPUT

	// the rest only arrive while the mouse isn't captured
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
		if (!IN_WantsMouseButtons ())
			return false;
		IN_LegacyButton (msg, wParam);
		return true;

	case WM_MOUSEWHEEL:
		Key_Event ((short)HIWORD (wParam) > 0 ? K_MWHEELUP : K_MWHEELDOWN, true);
		Key_Event ((short)HIWORD (wParam) > 0 ? K_MWHEELUP : K_MWHEELDOWN, false);
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

	IN_PollGamepad ();
}
