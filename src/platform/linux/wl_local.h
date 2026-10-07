#pragma once
// wl_local.h -- the Wayland connection and window the Linux client shares
// (wl_linux.c), with the protocols' headers

#define VK_USE_PLATFORM_WAYLAND_KHR
#include "window.h"

#include <wayland-client.h>

#include "color-management-v1.h"
#include "content-type-v1.h"
#include "cursor-shape-v1.h"
#include "fractional-scale-v1.h"
#include "idle-inhibit-unstable-v1.h"
#include "linux-dmabuf-v1.h"
#include "pointer-constraints-unstable-v1.h"
#include "presentation-time.h"
#include "relative-pointer-unstable-v1.h"
#include "viewporter.h"
#include "xdg-activation-v1.h"
#include "xdg-decoration-unstable-v1.h"
#include "xdg-shell.h"

typedef struct
{
	struct wl_display		*display;
	struct wl_surface		*surface;		// the window's, which Vulkan presents to

	// the globals bound, NULL where the compositor lacks one
	struct wl_compositor					*compositor;
	struct xdg_wm_base						*wm;
	struct wl_seat							*seat;
	struct wl_data_device_manager			*datadevices;
	struct wp_viewporter					*viewporter;
	struct wp_fractional_scale_manager_v1	*fractionalscale;
	struct zxdg_decoration_manager_v1		*decorations;
	struct wp_presentation					*presentation;
	struct zwp_linux_dmabuf_v1				*dmabuf;
	struct wp_color_manager_v1				*color;
	struct wp_content_type_manager_v1		*contenttype;
	struct zwp_idle_inhibit_manager_v1		*idleinhibit;
	struct zwp_relative_pointer_manager_v1	*relativepointer;
	struct zwp_pointer_constraints_v1		*constraints;
	struct wp_cursor_shape_manager_v1		*cursorshape;
	struct xdg_activation_v1				*activation;

	uint32_t	lastserial;		// of the last input event, which activation asks with
} wayland_t;

extern wayland_t	way;

// in_wayland.c: the seat's keyboard and pointer come and go
void	IN_SeatCapabilities (struct wl_seat *seat, uint32_t capabilities);

// the connection, and the window sized to the largest multiple of width x
// height pixels that fits (wl_linux.c)
bool	WL_Init (int width, int height);
void	WL_Shutdown (void);

// what came from the display, read and handled, the number of events: without
// waiting, or around a wait on the display's fd (prepared before, finished
// after, readable if the wait saw the fd so)
int		WL_ReadEvents (void);
int		WL_PrepareRead (void);
int		WL_FinishRead (bool readable);

// the window's size in pixels, which the frames are, and in the compositor's
// units; the frame presented next is of such a size
void	WL_WindowSize (int *pixelwidth, int *pixelheight, int *width, int *height);
void	WL_SetFrameSize (int pixelwidth, int pixelheight, int width, int height);

bool	WL_IsFullscreen (void);
void	WL_SetFullscreen (bool fullscreen);
bool	WL_IsSuspended (void);
void	WL_SetTitle (const char *latin1);
void	WL_SetIdleInhibit (bool inhibit);
void	WL_Activate (void);
char	*WL_GetClipboardText (void);
void	WL_SetClipboardText (const char *text);

// the GPU the compositor draws with, as a DRM device number
bool	WL_MainDevice (unsigned *major, unsigned *minor);

bool	WL_ColorsSupported (uint32_t tf, uint32_t primaries);

// the frame Vulkan presents next: when it's shown is asked (wp_presentation)
void	WL_BeforePresent (uint64_t serial, double time);

typedef struct
{
	uint64_t	presented;		// the last frame shown or passed over
	uint64_t	count, zerocopy, discarded;	// frames shown, scanned out directly, passed over
	uint32_t	flags;			// the last shown's wp_presentation_feedback kind
	double		refresh;		// seconds, 0 for unknown
	double		latency, worst;	// from present to shown, over the last frames
} wl_present_stats_t;

const wl_present_stats_t	*WL_PresentStats (void);

// the report: the protocols (all: each, else only those missing)
void	WL_PrintProtocols (bool all);
bool	WL_HasGlobal (const char *name);

// wl_hyprland.c: whether the compositor scans windows out, and lets them tear
void	WL_PrintScanout (void);

void	IN_WaylandInit (void);
void	IN_WaylandShutdown (void);
void	IN_WaylandCommands (void);
void	IN_WaylandWindowActivated (bool active);
double	IN_WaylandNextRepeat (void);
void	IN_WaylandRepeat (void);
