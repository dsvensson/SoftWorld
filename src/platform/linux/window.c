#include "args.h"
#include "print.h"
#include "sys.h"
#include <stdlib.h>
#include <string.h>
#include "window.h"

const window_backend_t	*window;

void Window_Init (int width, int height)
{
	const char *name = "auto";
	int         parm = COM_CheckParm ("-window-backend");

	if (parm)
	{
		if (parm + 1 >= com_argc)
			Sys_Error ("-window-backend needs auto, wayland or x11");

		name = com_argv[parm + 1];
	}

	if (strcmp (name, "auto") && strcmp (name, "wayland") && strcmp (name, "x11"))
		Sys_Error ("Unknown window backend '%s': use auto, wayland or x11", name);

#ifdef SW_WAYLAND
	if (!strcmp (name, "wayland") || !strcmp (name, "auto"))
		window = &window_wayland;
#endif
#ifdef SW_X11
	if (!strcmp (name, "x11") || (!strcmp (name, "auto")
		&& (!window || (!getenv ("WAYLAND_DISPLAY") && !getenv ("WAYLAND_SOCKET")))))
		window = &window_x11;
#endif
	if (!window)
		Sys_Error ("The %s window backend was not built", name);

	if (window->Init (width, height))
		return;

#if defined(SW_WAYLAND) && defined(SW_X11)
	if (!strcmp (name, "auto") && window == &window_wayland)
	{
		Con_Printf ("Can't connect to Wayland. Trying X11\n");
		window = &window_x11;
		if (window->Init (width, height))
			return;
	}
#endif
	Sys_Error ("Could not initialize the %s window backend", window->name);
}

const window_colors_t *Window_Colors (void)
{
	static const window_colors_t	sdr;

	return window->PreferredColors ? window->PreferredColors () : &sdr;
}
