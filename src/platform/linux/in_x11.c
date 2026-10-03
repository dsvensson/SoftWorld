#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "vid.h"
#include "x11_local.h"

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <string.h>

extern cvar_t _windowed_mouse;

static bool   in_keydown[256];
static bool   in_captured, in_detectablerepeat;
static Cursor in_cursor;
static double in_dx, in_dy;

static void IN_SetCapture (bool capture);

void IN_X11Init (void)
{
	Bool          supported;
	Pixmap        bitmap;
	XColor        black = {0};
	const char    bits[1] = {0};
	unsigned char mask[XIMaskLen (XI_RawMotion)] = {0};
	XIEventMask   events = {.deviceid = XIAllMasterDevices, .mask_len = sizeof(mask), .mask = mask};

	in_detectablerepeat = XkbSetDetectableAutoRepeat (x11.display, True, &supported) && supported;
	bitmap = XCreateBitmapFromData (x11.display, x11.window, bits, 1, 1);
	in_cursor = XCreatePixmapCursor (x11.display, bitmap, bitmap, &black, &black, 0, 0);
	XFreePixmap (x11.display, bitmap);
	XISetMask (mask, XI_RawMotion);
	XISelectEvents (x11.display, x11.root, &events, 1);
	XFlush (x11.display);
}

void IN_X11Shutdown (void)
{
	IN_SetCapture (false);
	if (x11.display && in_cursor)
		XFreeCursor (x11.display, in_cursor);

	in_cursor = None;
}

static void IN_SetCapture (bool capture)
{
	int result;

	if (!x11.display || capture == in_captured)
		return;

	if (capture)
	{
		result = XGrabPointer (x11.display, x11.window, False, ButtonPressMask | ButtonReleaseMask,
			GrabModeAsync, GrabModeAsync, x11.window, in_cursor, CurrentTime);
		if (result != GrabSuccess)
			return;  // a focus change can make the grab fail temporarily
	}
	else
		XUngrabPointer (x11.display, CurrentTime);

	in_captured = capture;
	in_dx = in_dy = 0;
	XFlush (x11.display);
}

void IN_X11Activated (bool active)
{
	if (!active)
		IN_SetCapture (false);

	memset (in_keydown, 0, sizeof(in_keydown));
	Key_ClearStates ();
}

void IN_X11Commands (void)
{
	bool want = ActiveApp && !Minimized && IN_WantsMouse ()
		&& (VID_IsFullscreen () || _windowed_mouse.value);
	int  dx, dy;

	IN_SetCapture (want);
	if (!in_captured)
		return;

	dx = (int)in_dx;
	dy = (int)in_dy;
	in_dx -= dx;
	in_dy -= dy;
	if (dx || dy)
		IN_MouseMotion (dx, dy);
}

static void IN_Keyboard (XKeyEvent *event)
{
	unsigned code = event->keycode;
	bool     down = event->type == KeyPress;
	int      key, count, i;
	char     text[32];
	KeySym   sym;
	XEvent   next;

	if (code >= sizeof(in_keydown) || code < 8)
		return;

	// Old servers repeat with release/press pairs at the same time. Ignore
	// the synthetic release so a held button does not flicker off each repeat.
	if (!down && !in_detectablerepeat && XPending (x11.display))
	{
		XPeekEvent (x11.display, &next);
		if (next.type == KeyPress && next.xkey.keycode == code && next.xkey.time == event->time)
			return;
	}

	if (!ActiveApp || (!down && !in_keydown[code]))
		return;

	// X keycodes start at eight, so subtract the offset to keep bindings physical.
	key = IN_EvdevKey (code - 8);
	if (down && key == K_ENTER && (event->state & Mod1Mask))
	{
		if (!in_keydown[code])
			VID_ToggleFullscreen ();

		in_keydown[code] = true;
		return;
	}

	in_keydown[code] = down;
	if (key)
		Key_Event (key, down);

	if (!down)
		return;

	count = XLookupString (event, text, sizeof(text), &sym, NULL);
	for (i = 0 ; i < count ; i++)
		if ((unsigned char)text[i] > 0 && (unsigned char)text[i] < 128)
			Key_CharEvent ((unsigned char)text[i]);
}

static void IN_Button (XButtonEvent *event)
{
	int  key;
	bool down = event->type == ButtonPress;

	if (!ActiveApp || (!(in_captured || IN_WantsMouseButtons ()) && event->button != 4 && event->button != 5))
		return;

	switch (event->button)
	{
	case 1:
		key = K_MOUSE1;
		break;
	case 2:
		key = K_MOUSE3;
		break;
	case 3:
		key = K_MOUSE2;
		break;
	case 4:
		key = K_MWHEELUP;
		break;
	case 5:
		key = K_MWHEELDOWN;
		break;
	case 8:
		key = K_MOUSE4;
		break;
	case 9:
		key = K_MOUSE5;
		break;
	default:
		return;
	}

	Key_Event (key, down);
}

void IN_X11Event (XEvent *event)
{
	XIRawEvent *raw;
	double     *value;
	int         i;

	switch (event->type)
	{
	case KeyPress:
	case KeyRelease:
		x11.time = event->xkey.time;
		IN_Keyboard (&event->xkey);
		break;
	case ButtonPress:
	case ButtonRelease:
		x11.time = event->xbutton.time;
		IN_Button (&event->xbutton);
		break;
	case MappingNotify:
		XRefreshKeyboardMapping (&event->xmapping);
		break;
	case GenericEvent:
		if (event->xcookie.extension != x11.xinput || !XGetEventData (x11.display, &event->xcookie))
			break;

		if (event->xcookie.evtype == XI_RawMotion && in_captured)
		{
			raw = event->xcookie.data;
			value = raw->raw_values;
			for (i = 0 ; i < raw->valuators.mask_len * 8 ; i++)
				if (XIMaskIsSet (raw->valuators.mask, i))
				{
					if (i == 0)
						in_dx += *value;
					else if (i == 1)
						in_dy += *value;

					value++;
				}
		}

		XFreeEventData (x11.display, &event->xcookie);
		break;
	}
}
