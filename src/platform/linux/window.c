#include "sys.h"
#include "window.h"

const window_backend_t	*window;

void Window_Init (int width, int height)
{
	window = &window_wayland;
	if (!window->Init (width, height))
		Sys_Error ("Could not initialize the %s window backend", window->name);
}

const window_colors_t *Window_Colors (void)
{
	static const window_colors_t	sdr;

	return window->PreferredColors ? window->PreferredColors () : &sdr;
}
