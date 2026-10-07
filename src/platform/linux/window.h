#pragma once

#include "linux_local.h"
#include <vulkan/vulkan.h>

// the colors the compositor would like the window's frames in, from its
// preferred image description for the window
typedef struct
{
	bool		managed, known, hdr;
	uint32_t	serial;			// bumped by each new description
	float		maxlum;			// the primary volume's maximum, cd/m²
	float		reference;		// reference white: SDR white, cd/m²
	float		targetmin, targetmax;	// the range the display shows, cd/m²
	bool		hastargetprimaries;
	float		targetprimaries[8];		// red, green, blue and white x, y
} window_colors_t;

typedef struct
{
	const char	*name;
	const char	*extension;
	bool	(*Init) (int width, int height);
	void	(*Shutdown) (void);
	VkResult (*CreateSurface) (VkInstance instance, VkSurfaceKHR *surface);
	// FinishRead must follow PrepareRead even when queued events end the wait,
	// so Wayland can complete or cancel the prepared read.
	int		(*ReadEvents) (void);
	int		(*PrepareRead) (void);
	int		(*FinishRead) (bool readable);
	void	(*WindowSize) (int *pixelwidth, int *pixelheight, int *width, int *height);
	bool	(*IsFullscreen) (void);
	void	(*SetFullscreen) (bool fullscreen);
	void	(*SetTitle) (const char *latin1);
	void	(*SetIdleInhibit) (bool inhibit);
	void	(*Activate) (void);
	char	*(*GetClipboardText) (void);
	void	(*SetClipboardText) (const char *utf8);
	void	(*PrintInfo) (bool all);

	// These may be unset when the display provides no such information.
	bool	(*MainDevice) (unsigned *major, unsigned *minor);
	const window_colors_t *(*PreferredColors) (void);
	void	(*SetFrameSize) (int pixelwidth, int pixelheight, int width, int height);
	void	(*BeforePresent) (uint64_t serial, double time);
	bool	(*Presented) (uint64_t serial);

	void	(*InputInit) (void);
	void	(*InputShutdown) (void);
	void	(*InputCommands) (void);
	void	(*InputActivated) (bool active);
	double	(*NextRepeat) (void);
	void	(*Repeat) (void);
} window_backend_t;

extern const window_backend_t	*window;
extern const window_backend_t	window_wayland, window_x11;

void	Window_Init (int width, int height);
const window_colors_t *Window_Colors (void);
