#pragma once

#define VK_USE_PLATFORM_XLIB_KHR
#include "window.h"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

typedef struct
{
	Display *display;
	Window   window, root;
	int      xinput;  // distinguishes XInput cookies from other extensions
	Time     time;    // lets the window manager reject stale activation requests
} x11_t;

extern x11_t x11;

void IN_X11Init (void);
void IN_X11Shutdown (void);
void IN_X11Activated (bool active);
void IN_X11Commands (void);
void IN_X11Event (XEvent *event);
