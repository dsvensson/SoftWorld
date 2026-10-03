#include "cmd.h"
#include "print.h"
#include "sys.h"
#include "x11_local.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/scrnsaver.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#define X11_CLIP_LIMIT  (1024 * 1024)

typedef struct
{
	Window requestor;
	int    type;
} x11_selection_wait_t;

x11_t x11;

static int  x11_width, x11_height;
static bool x11_fullscreen, x11_screensaver, x11_inhibited;
static Atom x11_protocols, x11_delete, x11_state, x11_fullscreenatom;
static Atom x11_active, x11_name, x11_utf8, x11_clipboard, x11_incr, x11_property;

static void X11_SetTitle (const char *text);
static void X11_SetIdleInhibit (bool inhibit);

static bool X11_Init (int width, int height)
{
	int           screen, multiple, event, error, major = 2, minor = 1;
	XSizeHints    size = {.flags = PMinSize, .min_width = 1, .min_height = 1};
	XClassHint    class = {.res_name = "softworld", .res_class = "SoftWorld"};
	XWMHints      hints = {.flags = InputHint, .input = True};
	Atom          type, normal, bypass;
	unsigned long one = 1;

	// Vulkan presentation may access Xlib from other threads.
	if (!XInitThreads ())
		Sys_Error ("Can't initialize X11 threading");

	x11.display = XOpenDisplay (NULL);
	if (!x11.display)
		return false;

	// XI 2.1 is needed to receive raw motion during a core pointer grab.
	if (!XQueryExtension (x11.display, "XInputExtension", &x11.xinput, &event, &error)
		|| XIQueryVersion (x11.display, &major, &minor) != Success
		|| major < 2 || (major == 2 && minor < 1))
		Sys_Error ("X11 needs XInput 2.1 for raw mouse input while captured");

	if (XScreenSaverQueryExtension (x11.display, &event, &error)
		&& XScreenSaverQueryVersion (x11.display, &major, &minor))
		x11_screensaver = major > 1 || (major == 1 && minor >= 1);

	screen = DefaultScreen (x11.display);
	x11.root = RootWindow (x11.display, screen);
	for (multiple = 4 ; multiple > 1 ; multiple--)
		if (width * multiple <= DisplayWidth (x11.display, screen)
			&& height * multiple <= DisplayHeight (x11.display, screen))
			break;

	x11_width = width * multiple;
	x11_height = height * multiple;
	x11.window = XCreateSimpleWindow (x11.display, x11.root, 0, 0, (unsigned)x11_width,
		(unsigned)x11_height, 0, 0, 0);
	XSelectInput (x11.display, x11.window, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask
		| FocusChangeMask | StructureNotifyMask | PropertyChangeMask);
	x11_protocols = XInternAtom (x11.display, "WM_PROTOCOLS", False);
	x11_delete = XInternAtom (x11.display, "WM_DELETE_WINDOW", False);
	x11_state = XInternAtom (x11.display, "_NET_WM_STATE", False);
	x11_fullscreenatom = XInternAtom (x11.display, "_NET_WM_STATE_FULLSCREEN", False);
	x11_active = XInternAtom (x11.display, "_NET_ACTIVE_WINDOW", False);
	x11_name = XInternAtom (x11.display, "_NET_WM_NAME", False);
	x11_utf8 = XInternAtom (x11.display, "UTF8_STRING", False);
	x11_clipboard = XInternAtom (x11.display, "CLIPBOARD", False);
	x11_incr = XInternAtom (x11.display, "INCR", False);
	x11_property = XInternAtom (x11.display, "SOFTWORLD_SELECTION", False);
	XSetWMProtocols (x11.display, x11.window, &x11_delete, 1);
	XSetWMNormalHints (x11.display, x11.window, &size);
	XSetClassHint (x11.display, x11.window, &class);
	XSetWMHints (x11.display, x11.window, &hints);
	type = XInternAtom (x11.display, "_NET_WM_WINDOW_TYPE", False);
	normal = XInternAtom (x11.display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
	XChangeProperty (x11.display, x11.window, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&normal, 1);
	bypass = XInternAtom (x11.display, "_NET_WM_BYPASS_COMPOSITOR", False);
	XChangeProperty (x11.display, x11.window, bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
	X11_SetTitle ("SoftWorld");
	XMapWindow (x11.display, x11.window);
	XFlush (x11.display);
	if (!Sys_AddWindowFd (ConnectionNumber (x11.display)))
		Sys_Error ("Can't add the X11 connection to the event queue");

	return true;
}

static void X11_Shutdown (void)
{
	if (!x11.display)
		return;

	X11_SetIdleInhibit (false);
	XDestroyWindow (x11.display, x11.window);
	XCloseDisplay (x11.display);
	memset (&x11, 0, sizeof(x11));
	x11_fullscreen = false;
}

static VkResult X11_CreateSurface (VkInstance instance, VkSurfaceKHR *surface)
{
	return vkCreateXlibSurfaceKHR (instance, &(VkXlibSurfaceCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
		.dpy = x11.display,
		.window = x11.window,
	}, NULL, surface);
}

static void X11_WindowSize (int *pixelwidth, int *pixelheight, int *width, int *height)
{
	*pixelwidth = *width = x11_width;
	*pixelheight = *height = x11_height;
}

static void X11_SetTitle (const char *text)
{
	char        utf8[512], *out = utf8;
	const byte *in;

	for (in = (const byte *)text ; *in && out < utf8 + sizeof(utf8) - 3 ; in++)
		if (*in < 0x80)
			*out++ = (char)*in;
		else
		{
			*out++ = (char)(0xc0 | (*in >> 6));
			*out++ = (char)(0x80 | (*in & 0x3f));
		}

	*out = 0;
	XStoreName (x11.display, x11.window, text);
	XChangeProperty (x11.display, x11.window, x11_name, x11_utf8, 8, PropModeReplace,
		(unsigned char *)utf8, (int)(out - utf8));
}

static void X11_Message (Atom type, long a, long b, long c)
{
	XEvent event = {0};

	event.xclient = (XClientMessageEvent){
		.type = ClientMessage, .display = x11.display, .window = x11.window,
		.message_type = type, .format = 32, .data.l = {a, b, c, 0, 0},
	};
	if (type == x11_state)
		event.xclient.data.l[3] = 1;  // identify the source so the window manager can apply its policy

	XSendEvent (x11.display, x11.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
	XFlush (x11.display);
}

static void X11_SetFullscreen (bool fullscreen)
{
	X11_Message (x11_state, fullscreen ? 1 : 0, (long)x11_fullscreenatom, 0);
}

static bool X11_IsFullscreen (void)
{
	return x11_fullscreen;
}

static void X11_Activate (void)
{
	X11_Message (x11_active, 1, (long)x11.time, 0);
}

static void X11_SetIdleInhibit (bool inhibit)
{
	if (!x11.display || !x11_screensaver || inhibit == x11_inhibited)
		return;

	XScreenSaverSuspend (x11.display, inhibit);
	x11_inhibited = inhibit;
	XFlush (x11.display);
}

static void X11_PrintInfo (bool all)
{
	(void)all;
	Con_Printf ("X11: XInput 2 raw mouse input, SDR output, %s screen saver inhibition\n",
		x11_screensaver ? "with" : "without");
	Con_Printf ("Display timing uses Vulkan present wait where available. No X11 presentation feedback\n");
}

static void X11_ReadState (void)
{
	Atom           actual;
	int            format;
	unsigned long  count, remaining, i;
	unsigned char *data = NULL;

	x11_fullscreen = false;
	if (XGetWindowProperty (x11.display, x11.window, x11_state, 0, 1024, False, XA_ATOM,
		&actual, &format, &count, &remaining, &data) == Success && actual == XA_ATOM && format == 32)
		for (i = 0 ; i < count ; i++)
			if (((Atom *)data)[i] == x11_fullscreenatom)
				x11_fullscreen = true;

	if (data)
		XFree (data);

	X11_SetIdleInhibit (ActiveApp && !Minimized && x11_fullscreen);
}

// XPending flushes requests so the main loop cannot wait for events
// from requests that are still buffered locally.
static int X11_ReadEvents (void)
{
	XEvent event;
	int    n = 0, revert;
	Window focus;

	while (XPending (x11.display))
	{
		XNextEvent (x11.display, &event);
		n++;
		IN_X11Event (&event);
		if (event.xany.window != x11.window)
			continue;

		switch (event.type)
		{
		case ConfigureNotify:
			x11_width = event.xconfigure.width;
			x11_height = event.xconfigure.height;
			break;
		case FocusIn:
		case FocusOut:
			// A grab alone does not change focus. Query the real focus because
			// the window manager may move it while holding a grab.
			if (event.xfocus.mode != NotifyGrab && event.xfocus.detail != NotifyInferior)
			{
				XGetInputFocus (x11.display, &focus, &revert);
				VID_AppActivate (focus == x11.window);
			}
			break;
		case UnmapNotify:
			VID_AppActivate (false);
			VID_WindowSuspended (true);
			break;
		case MapNotify:
			VID_WindowSuspended (false);
			break;
		case PropertyNotify:
			if (event.xproperty.atom == x11_state)
				X11_ReadState ();
			break;
		case ClientMessage:
			if (event.xclient.message_type == x11_protocols && event.xclient.format == 32
				&& (Atom)event.xclient.data.l[0] == x11_delete)
				Cbuf_AddText ("quit\n");
			break;
		}
	}

	return n;
}

static int X11_FinishRead (bool readable)
{
	(void)readable;
	return X11_ReadEvents ();
}

static Bool X11_SelectionEvent (Display *display, XEvent *event, XPointer data)
{
	const x11_selection_wait_t *wait = (const x11_selection_wait_t *)data;

	(void)display;
	if (event->type != wait->type)
		return False;

	if (event->type == SelectionNotify)
		return event->xselection.requestor == wait->requestor && event->xselection.selection == x11_clipboard;

	return event->xproperty.window == wait->requestor && event->xproperty.atom == x11_property
		&& event->xproperty.state == PropertyNewValue;
}

// Dispatching a paste key here could start another clipboard transfer
// before this one finishes, so only selection events are handled.
static bool X11_WaitSelection (Window requestor, int type, XEvent *event, double until)
{
	x11_selection_wait_t wait = {.requestor = requestor, .type = type};
	struct pollfd        p = {.fd = ConnectionNumber (x11.display), .events = POLLIN};
	double               left;
	int                  result;

	XFlush (x11.display);
	for (;;)
	{
		if (XCheckIfEvent (x11.display, event, X11_SelectionEvent, (XPointer)&wait))
			return true;

		left = until - Sys_DoubleTime ();
		if (left <= 0)
			return false;

		result = poll (&p, 1, (int)(left * 1000) + 1);
		if (result < 0 && errno == EINTR)
			continue;

		if (result <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return false;

		XEventsQueued (x11.display, QueuedAfterReading);
	}
}

static char *X11_ReadSelection (Window requestor, Atom target)
{
	XEvent         event;
	Atom           actual;
	int            format;
	unsigned long  count, remaining;
	unsigned char *data = NULL;
	char          *text = NULL, *grown;
	size_t         used = 0;
	bool           incremental = false;
	double         until = Sys_DoubleTime () + 2;

	XConvertSelection (x11.display, x11_clipboard, target, x11_property, requestor, CurrentTime);
	if (!X11_WaitSelection (requestor, SelectionNotify, &event, until) || event.xselection.property != x11_property
		|| event.xselection.target != target)
		return NULL;

	if (XGetWindowProperty (x11.display, requestor, x11_property, 0, X11_CLIP_LIMIT / 4, False,
		AnyPropertyType, &actual, &format, &count, &remaining, &data) != Success)
		return NULL;

	if (actual == x11_incr)
	{
		XFree (data);
		data = NULL;
		// The header notification must not be mistaken for the first data chunk.
		while (XCheckTypedWindowEvent (x11.display, requestor, PropertyNotify, &event))
			;

		XDeleteProperty (x11.display, requestor, x11_property);
		incremental = true;
	}

	for (;;)
	{
		if (incremental)
		{
			if (!X11_WaitSelection (requestor, PropertyNotify, &event, until))
				break;

			if (XGetWindowProperty (x11.display, requestor, x11_property, 0, X11_CLIP_LIMIT / 4, True,
				AnyPropertyType, &actual, &format, &count, &remaining, &data) != Success)
				break;
		}

		if (actual != target || format != 8 || remaining || count > X11_CLIP_LIMIT - used)
			break;

		grown = realloc (text, used + count + 1);
		if (!grown)
			break;

		text = grown;
		memcpy (text + used, data, count);
		used += count;
		text[used] = 0;
		XFree (data);
		data = NULL;
		if (!incremental || !count)
			return text;
	}

	if (data)
		XFree (data);

	free (text);
	return NULL;
}

static char *X11_GetClipboardText (void)
{
	Window requestor;
	char  *text;
	XEvent event;
	Atom   targets[2] = {x11_utf8, XA_STRING};
	int    i;

	if (XGetSelectionOwner (x11.display, x11_clipboard) == None)
		return NULL;

	for (i = 0 ; i < 2 ; i++)
	{
		// A separate requestor prevents late replies from an earlier attempt
		// from being accepted as the current transfer.
		requestor = XCreateSimpleWindow (x11.display, x11.root, 0, 0, 1, 1, 0, 0, 0);
		XSelectInput (x11.display, requestor, PropertyChangeMask);
		text = X11_ReadSelection (requestor, targets[i]);
		XDestroyWindow (x11.display, requestor);
		XSync (x11.display, False);
		while (XCheckTypedWindowEvent (x11.display, requestor, PropertyNotify, &event))
			;

		if (text)
			return text;
	}

	return NULL;
}

const window_backend_t window_x11 =
{
	.name = "X11",
	.extension = VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
	.Init = X11_Init,
	.Shutdown = X11_Shutdown,
	.CreateSurface = X11_CreateSurface,
	.ReadEvents = X11_ReadEvents,
	.PrepareRead = X11_ReadEvents,
	.FinishRead = X11_FinishRead,
	.WindowSize = X11_WindowSize,
	.IsFullscreen = X11_IsFullscreen,
	.SetFullscreen = X11_SetFullscreen,
	.SetTitle = X11_SetTitle,
	.SetIdleInhibit = X11_SetIdleInhibit,
	.Activate = X11_Activate,
	.GetClipboardText = X11_GetClipboardText,
	.PrintInfo = X11_PrintInfo,
	.InputInit = IN_X11Init,
	.InputShutdown = IN_X11Shutdown,
	.InputCommands = IN_X11Commands,
	.InputActivated = IN_X11Activated,
};
