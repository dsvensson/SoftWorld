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
// wl_linux.c -- the Wayland connection: the compositor's globals, the window,
// what the compositor says of the frames it shows, and of the colors it wants
//
// Vulkan presents to the window's surface and commits it (the driver's own
// objects on it are its fifo, commit timer, tearing control, explicit sync,
// color management and dmabuf feedback, which nothing here may make again).
// What is here is the rest: the window's role and size, the scale, content
// type and idle inhibition, and the feedback that tells when a frame was
// shown, and whether the display scanned it out directly (zero-copy) or the
// compositor drew it into its own.
//
// The window is sized in the compositor's units; its frames in pixels, the
// size times the (fractional) scale, which the viewport shows at the size.
// A frame of a fullscreen window is the display's pixel size, as scanning it
// out needs.

#include "args.h"
#include "cmd.h"
#include "mem.h"
#include "print.h"
#include "sys.h"
#include "wl_local.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

wayland_t	way;

// the globals the report names, and whether the compositor has them
typedef struct
{
	const char	*name;
	const char	*what;
	uint32_t	version;	// 0: missing
} wl_global_t;

static wl_global_t	wl_globals[] =
{
	{"wp_presentation",						"presentation-time: when each frame is shown", 0},
	{"wp_fifo_manager_v1",					"fifo: vsync without frame callbacks", 0},
	{"wp_commit_timing_manager_v1",			"commit-timing: frames shown at a time", 0},
	{"wp_tearing_control_manager_v1",		"tearing-control: tearing without vsync", 0},
	{"wp_linux_drm_syncobj_manager_v1",		"linux-drm-syncobj: explicit sync", 0},
	{"zwp_linux_dmabuf_v1",					"linux-dmabuf: GPU buffers, scanout tranches", 0},
	{"wp_color_manager_v1",					"color-management: HDR", 0},
	{"wp_content_type_manager_v1",			"content-type: the window is a game", 0},
	{"wp_fractional_scale_manager_v1",		"fractional-scale", 0},
	{"wp_viewporter",						"viewporter", 0},
	{"zwp_relative_pointer_manager_v1",		"relative-pointer: raw mouse motion", 0},
	{"zwp_pointer_constraints_v1",			"pointer-constraints: the mouse locked", 0},
	{"zwp_idle_inhibit_manager_v1",			"idle-inhibit", 0},
	{"zxdg_decoration_manager_v1",			"xdg-decoration: the compositor's title bar", 0},
	{"xdg_activation_v1",					"xdg-activation", 0},
	{"wp_cursor_shape_manager_v1",			"cursor-shape", 0},
};
#define NUM_GLOBALS	(sizeof(wl_globals) / sizeof(wl_globals[0]))

static struct wl_registry					*wl_registry;
static struct xdg_surface					*wl_xdgsurface;
static struct xdg_toplevel					*wl_toplevel;
static struct zxdg_toplevel_decoration_v1	*wl_decoration;
static struct wp_viewport					*wl_viewport;
static struct wp_fractional_scale_v1		*wl_fscale;
static struct wp_content_type_v1			*wl_contenttype;
static struct zwp_idle_inhibitor_v1			*wl_inhibitor;
static struct wp_color_management_surface_feedback_v1	*wl_colorfeedback;
static struct wl_data_device				*wl_datadevice;
static struct wl_data_offer					*wl_selection;		// the clipboard's offer
static struct wl_data_offer					*wl_dragged;		// what is dragged over the window

static bool		wl_configured;			// the first configure came, and was acked
static int		wl_width, wl_height;	// the window, in the compositor's units
static int		wl_boundswidth, wl_boundsheight;	// the most it may be, 0 for unknown
static bool		wl_fullscreen, wl_suspended;
static int		wl_scale120 = 120;		// the fractional scale, in 120ths
static int		wl_bufferscale = 1;		// the integer one, without fractional-scale
static int		wl_viewwidth, wl_viewheight;	// the viewport's destination set
static int		wl_viewscale = 1;		// the buffer scale set, without a viewport

static clockid_t	wl_clock = CLOCK_MONOTONIC;		// what presentation times count in
static dev_t		wl_maindevice;
static bool			wl_hasmaindevice;

static wl_colors_t	wl_colors;
static uint32_t		wl_colorfeatures;		// 1 << wp_color_manager_v1 feature
static uint32_t		wl_colortfs;			// 1 << transfer function
static uint32_t		wl_colorprimaries;		// 1 << primaries

static wl_present_stats_t	wl_stats;

/*
===============================================================================

GLOBALS

===============================================================================
*/

static void WL_Ping (void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong (wm, serial);
}

static const struct xdg_wm_base_listener	wl_wm_listener = {.ping = WL_Ping};

static void WL_SeatCapabilities (void *data, struct wl_seat *seat, uint32_t capabilities)
{
	(void)data;
	IN_SeatCapabilities (seat, capabilities);
}

static void WL_SeatName (void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener	wl_seat_listener =
{
	.capabilities = WL_SeatCapabilities,
	.name = WL_SeatName,
};

static void WL_ClockId (void *data, struct wp_presentation *presentation, uint32_t clock)
{
	(void)data;
	(void)presentation;
	wl_clock = (clockid_t)clock;
}

static const struct wp_presentation_listener	wl_presentation_listener = {.clock_id = WL_ClockId};

static void WL_ColorIntent (void *data, struct wp_color_manager_v1 *manager, uint32_t intent)
{
	(void)data;
	(void)manager;
	(void)intent;
}

static void WL_ColorFeature (void *data, struct wp_color_manager_v1 *manager, uint32_t feature)
{
	(void)data;
	(void)manager;
	if (feature < 32)
		wl_colorfeatures |= 1u << feature;
}

static void WL_ColorTf (void *data, struct wp_color_manager_v1 *manager, uint32_t tf)
{
	(void)data;
	(void)manager;
	if (tf < 32)
		wl_colortfs |= 1u << tf;
}

static void WL_ColorPrimaries (void *data, struct wp_color_manager_v1 *manager, uint32_t primaries)
{
	(void)data;
	(void)manager;
	if (primaries < 32)
		wl_colorprimaries |= 1u << primaries;
}

static void WL_ColorDone (void *data, struct wp_color_manager_v1 *manager)
{
	(void)data;
	(void)manager;
}

static const struct wp_color_manager_v1_listener	wl_color_listener =
{
	.supported_intent = WL_ColorIntent,
	.supported_feature = WL_ColorFeature,
	.supported_tf_named = WL_ColorTf,
	.supported_primaries_named = WL_ColorPrimaries,
	.done = WL_ColorDone,
};

static uint32_t WL_Version (uint32_t offered, uint32_t wanted)
{
	return offered < wanted ? offered : wanted;
}

static void WL_Global (void *data, struct wl_registry *registry, uint32_t name, const char *interface,
	uint32_t version)
{
	size_t	i;

	(void)data;
	for (i = 0 ; i < NUM_GLOBALS ; i++)
		if (!strcmp (interface, wl_globals[i].name))
			wl_globals[i].version = version;

#define BIND(field, iface, wanted) \
	way.field = wl_registry_bind (registry, name, &iface##_interface, WL_Version (version, wanted))

	if (!strcmp (interface, wl_compositor_interface.name))
		BIND (compositor, wl_compositor, 6);
	else if (!strcmp (interface, xdg_wm_base_interface.name))
	{
		BIND (wm, xdg_wm_base, 6);
		xdg_wm_base_add_listener (way.wm, &wl_wm_listener, NULL);
	}
	else if (!strcmp (interface, wl_seat_interface.name) && !way.seat)
	{
		// the first seat's keyboard and mouse; v8 has the wheel in 120ths of a step
		BIND (seat, wl_seat, 8);
		wl_seat_add_listener (way.seat, &wl_seat_listener, NULL);
	}
	else if (!strcmp (interface, wl_data_device_manager_interface.name))
		BIND (datadevices, wl_data_device_manager, 3);
	else if (!strcmp (interface, wp_viewporter_interface.name))
		BIND (viewporter, wp_viewporter, 1);
	else if (!strcmp (interface, wp_fractional_scale_manager_v1_interface.name))
		BIND (fractionalscale, wp_fractional_scale_manager_v1, 1);
	else if (!strcmp (interface, zxdg_decoration_manager_v1_interface.name))
		BIND (decorations, zxdg_decoration_manager_v1, 1);
	else if (!strcmp (interface, wp_presentation_interface.name))
	{
		BIND (presentation, wp_presentation, 1);
		wp_presentation_add_listener (way.presentation, &wl_presentation_listener, NULL);
	}
	else if (!strcmp (interface, zwp_linux_dmabuf_v1_interface.name) && version >= 4)
		BIND (dmabuf, zwp_linux_dmabuf_v1, 4);
	else if (!strcmp (interface, wp_color_manager_v1_interface.name))
	{
		// version 1: its events are the ones this client has
		BIND (color, wp_color_manager_v1, 1);
		wp_color_manager_v1_add_listener (way.color, &wl_color_listener, NULL);
	}
	else if (!strcmp (interface, wp_content_type_manager_v1_interface.name))
		BIND (contenttype, wp_content_type_manager_v1, 1);
	else if (!strcmp (interface, zwp_idle_inhibit_manager_v1_interface.name))
		BIND (idleinhibit, zwp_idle_inhibit_manager_v1, 1);
	else if (!strcmp (interface, zwp_relative_pointer_manager_v1_interface.name))
		BIND (relativepointer, zwp_relative_pointer_manager_v1, 1);
	else if (!strcmp (interface, zwp_pointer_constraints_v1_interface.name))
		BIND (constraints, zwp_pointer_constraints_v1, 1);
	else if (!strcmp (interface, wp_cursor_shape_manager_v1_interface.name))
		BIND (cursorshape, wp_cursor_shape_manager_v1, 1);
	else if (!strcmp (interface, xdg_activation_v1_interface.name))
		BIND (activation, xdg_activation_v1, 1);

#undef BIND
}

static void WL_GlobalRemove (void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener	wl_registry_listener =
{
	.global = WL_Global,
	.global_remove = WL_GlobalRemove,
};

/*
===============================================================================

THE COMPOSITOR'S GPU

===============================================================================
*/

static void WL_FeedbackDone (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
	(void)data;
	(void)feedback;
}

static void WL_FeedbackFormatTable (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback, int32_t fd,
	uint32_t size)
{
	(void)data;
	(void)feedback;
	(void)size;
	close (fd);
}

static void WL_FeedbackMainDevice (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
	struct wl_array *device)
{
	(void)data;
	(void)feedback;
	if (device->size != sizeof(dev_t))
		return;
	memcpy (&wl_maindevice, device->data, sizeof(dev_t));
	wl_hasmaindevice = true;
}

static void WL_FeedbackTrancheDone (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
	(void)data;
	(void)feedback;
}

static void WL_FeedbackTrancheDevice (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
	struct wl_array *device)
{
	(void)data;
	(void)feedback;
	(void)device;
}

static void WL_FeedbackTrancheFormats (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
	struct wl_array *indices)
{
	(void)data;
	(void)feedback;
	(void)indices;
}

static void WL_FeedbackTrancheFlags (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback, uint32_t flags)
{
	(void)data;
	(void)feedback;
	(void)flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener	wl_feedback_listener =
{
	.done = WL_FeedbackDone,
	.format_table = WL_FeedbackFormatTable,
	.main_device = WL_FeedbackMainDevice,
	.tranche_done = WL_FeedbackTrancheDone,
	.tranche_target_device = WL_FeedbackTrancheDevice,
	.tranche_formats = WL_FeedbackTrancheFormats,
	.tranche_flags = WL_FeedbackTrancheFlags,
};

// the device the compositor draws with, from the dmabuf default feedback (not
// the surface's: the driver has that, and the scanout tranches go to the first
// asked for)
bool WL_MainDevice (unsigned *major, unsigned *minor)
{
	if (!wl_hasmaindevice)
		return false;
	*major = major (wl_maindevice);
	*minor = minor (wl_maindevice);
	return true;
}

/*
===============================================================================

THE WINDOW

===============================================================================
*/

static int WL_Pixels (int size)
{
	if (way.fractionalscale && way.viewporter)
		return (size * wl_scale120 + 60) / 120;
	return size * wl_bufferscale;
}

// pixels in the compositor's units
static int WL_Units (int pixels)
{
	if (way.fractionalscale && way.viewporter)
		return (pixels * 120 + wl_scale120 / 2) / wl_scale120;
	return pixels / wl_bufferscale;
}

// the window's size in pixels, the frames', and in the compositor's units
void WL_WindowSize (int *pixelwidth, int *pixelheight, int *width, int *height)
{
	*pixelwidth = WL_Pixels (wl_width);
	*pixelheight = WL_Pixels (wl_height);
	*width = wl_width;
	*height = wl_height;
}

// the frame presented next, its size in pixels and in the compositor's units:
// the viewport shows it at that size, or without one the buffer's scale is
// the pixels a unit, from that commit on
void WL_SetFrameSize (int pixelwidth, int pixelheight, int width, int height)
{
	int		scale;

	(void)pixelheight;
	if (wl_viewport)
	{
		if (width != wl_viewwidth || height != wl_viewheight)
			wp_viewport_set_destination (wl_viewport, width, height);
		wl_viewwidth = width;
		wl_viewheight = height;
		return;
	}
	scale = width > 0 ? pixelwidth / width : 1;
	if (scale != wl_viewscale)
		wl_surface_set_buffer_scale (way.surface, scale);
	wl_viewscale = scale;
}

static void WL_Configure (void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
	struct wl_array *states)
{
	uint32_t	*state;
	bool		fullscreen = false, suspended = false;

	(void)data;
	(void)toplevel;
	wl_array_for_each (state, states)
	{
		if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
			fullscreen = true;
		else if (*state == XDG_TOPLEVEL_STATE_SUSPENDED)
			suspended = true;
	}
	// 0: the size is ours to choose, the one it is
	if (width > 0 && height > 0)
	{
		wl_width = width;
		wl_height = height;
	}
	wl_fullscreen = fullscreen;
	if (suspended != wl_suspended)
	{
		wl_suspended = suspended;
		VID_WindowSuspended (suspended);
	}
}

// the close button asks the game, which asks the player
static void WL_Close (void *data, struct xdg_toplevel *toplevel)
{
	(void)data;
	(void)toplevel;
	Cbuf_AddText ("quit\n");
}

static void WL_ConfigureBounds (void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height)
{
	(void)data;
	(void)toplevel;
	wl_boundswidth = width;
	wl_boundsheight = height;
}

static void WL_Capabilities (void *data, struct xdg_toplevel *toplevel, struct wl_array *capabilities)
{
	(void)data;
	(void)toplevel;
	(void)capabilities;
}

static const struct xdg_toplevel_listener	wl_toplevel_listener =
{
	.configure = WL_Configure,
	.close = WL_Close,
	.configure_bounds = WL_ConfigureBounds,
	.wm_capabilities = WL_Capabilities,
};

static void WL_SurfaceConfigure (void *data, struct xdg_surface *surface, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure (surface, serial);
	wl_configured = true;
}

static const struct xdg_surface_listener	wl_xdgsurface_listener = {.configure = WL_SurfaceConfigure};

static void WL_PreferredScale (void *data, struct wp_fractional_scale_v1 *scale, uint32_t scale120)
{
	(void)data;
	(void)scale;
	if (scale120 > 0)
		wl_scale120 = (int)scale120;
}

static const struct wp_fractional_scale_v1_listener	wl_fscale_listener = {.preferred_scale = WL_PreferredScale};

static void WL_SurfaceEnter (void *data, struct wl_surface *surface, struct wl_output *output)
{
	(void)data;
	(void)surface;
	(void)output;
}

static void WL_SurfaceLeave (void *data, struct wl_surface *surface, struct wl_output *output)
{
	(void)data;
	(void)surface;
	(void)output;
}

static void WL_BufferScale (void *data, struct wl_surface *surface, int32_t factor)
{
	(void)data;
	(void)surface;
	if (factor > 0)
		wl_bufferscale = factor;
}

static void WL_BufferTransform (void *data, struct wl_surface *surface, uint32_t transform)
{
	(void)data;
	(void)surface;
	(void)transform;
}

static const struct wl_surface_listener	wl_surface_listener =
{
	.enter = WL_SurfaceEnter,
	.leave = WL_SurfaceLeave,
	.preferred_buffer_scale = WL_BufferScale,
	.preferred_buffer_transform = WL_BufferTransform,
};

static void WL_DecorationMode (void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode)
{
	(void)data;
	(void)decoration;
	(void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener	wl_decoration_listener = {.configure = WL_DecorationMode};

bool WL_IsFullscreen (void)
{
	return wl_fullscreen;
}

void WL_SetFullscreen (bool fullscreen)
{
	if (fullscreen)
		xdg_toplevel_set_fullscreen (wl_toplevel, NULL);
	else
		xdg_toplevel_unset_fullscreen (wl_toplevel);
	wl_display_flush (way.display);
}

bool WL_IsSuspended (void)
{
	return wl_suspended;
}

// the title in UTF-8, from the game's Latin-1
void WL_SetTitle (const char *text)
{
	char			utf8[512], *out = utf8;
	const byte		*in;

	for (in = (const byte *)text ; *in && out < utf8 + sizeof(utf8) - 3 ; in++)
		if (*in < 0x80)
			*out++ = (char)*in;
		else
		{
			*out++ = (char)(0xc0 | (*in >> 6));
			*out++ = (char)(0x80 | (*in & 0x3f));
		}
	*out = 0;
	xdg_toplevel_set_title (wl_toplevel, utf8);
}

// no idle blanking or locking while played in fullscreen
void WL_SetIdleInhibit (bool inhibit)
{
	if (!way.idleinhibit || inhibit == (wl_inhibitor != NULL))
		return;
	if (inhibit)
		wl_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor (way.idleinhibit, way.surface);
	else
	{
		zwp_idle_inhibitor_v1_destroy (wl_inhibitor);
		wl_inhibitor = NULL;
	}
}

static void WL_TokenDone (void *data, struct xdg_activation_token_v1 *token, const char *name)
{
	(void)data;
	xdg_activation_v1_activate (way.activation, name, way.surface);
	xdg_activation_token_v1_destroy (token);
}

static const struct xdg_activation_token_v1_listener	wl_token_listener = {.done = WL_TokenDone};

// asks the compositor to raise the window; it may only mark it as wanting
// attention, as a window without the focus can't take it
void WL_Activate (void)
{
	struct xdg_activation_token_v1	*token;

	if (!way.activation)
		return;
	token = xdg_activation_v1_get_activation_token (way.activation);
	xdg_activation_token_v1_add_listener (token, &wl_token_listener, NULL);
	if (way.lastserial && way.seat)
		xdg_activation_token_v1_set_serial (token, way.lastserial, way.seat);
	xdg_activation_token_v1_set_surface (token, way.surface);
	xdg_activation_token_v1_commit (token);
	wl_display_flush (way.display);
}

/*
===============================================================================

THE COLORS THE COMPOSITOR WANTS

===============================================================================
*/

static wl_colors_t	wl_pending;		// the description being read

static void WL_InfoDone (void *data, struct wp_image_description_info_v1 *info)
{
	(void)data;
	wp_image_description_info_v1_destroy (info);
	wl_pending.known = true;
	wl_pending.serial = wl_colors.serial + 1;
	wl_colors = wl_pending;
}

static void WL_InfoIcc (void *data, struct wp_image_description_info_v1 *info, int32_t icc, uint32_t size)
{
	(void)data;
	(void)info;
	(void)size;
	close (icc);
}

static void WL_InfoPrimaries (void *data, struct wp_image_description_info_v1 *info, int32_t r_x, int32_t r_y,
	int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{
	(void)data;
	(void)info;
	(void)r_x; (void)r_y; (void)g_x; (void)g_y; (void)b_x; (void)b_y; (void)w_x; (void)w_y;
}

static void WL_InfoPrimariesNamed (void *data, struct wp_image_description_info_v1 *info, uint32_t primaries)
{
	(void)data;
	(void)info;
	wl_pending.primaries = primaries;
}

static void WL_InfoTfPower (void *data, struct wp_image_description_info_v1 *info, uint32_t eexp)
{
	(void)data;
	(void)info;
	(void)eexp;
}

static void WL_InfoTfNamed (void *data, struct wp_image_description_info_v1 *info, uint32_t tf)
{
	(void)data;
	(void)info;
	wl_pending.tf = tf;
}

static void WL_InfoLuminances (void *data, struct wp_image_description_info_v1 *info, uint32_t min_lum,
	uint32_t max_lum, uint32_t reference_lum)
{
	(void)data;
	(void)info;
	wl_pending.minlum = min_lum / 10000.0f;
	wl_pending.maxlum = (float)max_lum;
	wl_pending.reference = (float)reference_lum;
}

static void WL_InfoTargetPrimaries (void *data, struct wp_image_description_info_v1 *info, int32_t r_x,
	int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{
	int32_t	xy[8] = {r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y};
	int		i;

	(void)data;
	(void)info;
	for (i = 0 ; i < 8 ; i++)
		wl_pending.targetprimaries[i] = xy[i] / 1000000.0f;
	wl_pending.hastargetprimaries = true;
}

static void WL_InfoTargetLuminance (void *data, struct wp_image_description_info_v1 *info, uint32_t min_lum,
	uint32_t max_lum)
{
	(void)data;
	(void)info;
	wl_pending.targetmin = min_lum / 10000.0f;
	wl_pending.targetmax = (float)max_lum;
}

static void WL_InfoMaxCll (void *data, struct wp_image_description_info_v1 *info, uint32_t max_cll)
{
	(void)data;
	(void)info;
	(void)max_cll;
}

static void WL_InfoMaxFall (void *data, struct wp_image_description_info_v1 *info, uint32_t max_fall)
{
	(void)data;
	(void)info;
	(void)max_fall;
}

static const struct wp_image_description_info_v1_listener	wl_info_listener =
{
	.done = WL_InfoDone,
	.icc_file = WL_InfoIcc,
	.primaries = WL_InfoPrimaries,
	.primaries_named = WL_InfoPrimariesNamed,
	.tf_power = WL_InfoTfPower,
	.tf_named = WL_InfoTfNamed,
	.luminances = WL_InfoLuminances,
	.target_primaries = WL_InfoTargetPrimaries,
	.target_luminance = WL_InfoTargetLuminance,
	.target_max_cll = WL_InfoMaxCll,
	.target_max_fall = WL_InfoMaxFall,
};

static void WL_DescriptionFailed (void *data, struct wp_image_description_v1 *description, uint32_t cause,
	const char *msg)
{
	(void)data;
	(void)cause;
	Con_DPrintf ("The compositor's preferred colors: %s\n", msg);
	wp_image_description_v1_destroy (description);
}

static void WL_DescriptionReady (void *data, struct wp_image_description_v1 *description, uint32_t identity)
{
	struct wp_image_description_info_v1	*info;

	(void)data;
	(void)identity;
	memset (&wl_pending, 0, sizeof(wl_pending));
	info = wp_image_description_v1_get_information (description);
	wp_image_description_info_v1_add_listener (info, &wl_info_listener, NULL);
	wp_image_description_v1_destroy (description);
}

static const struct wp_image_description_v1_listener	wl_description_listener =
{
	.failed = WL_DescriptionFailed,
	.ready = WL_DescriptionReady,
};

static void WL_GetPreferred (void)
{
	struct wp_image_description_v1	*description;

	description = wp_color_management_surface_feedback_v1_get_preferred (wl_colorfeedback);
	wp_image_description_v1_add_listener (description, &wl_description_listener, NULL);
}

// the window moved to another display, or the display's range changed
static void WL_PreferredChanged (void *data, struct wp_color_management_surface_feedback_v1 *feedback,
	uint32_t identity)
{
	(void)data;
	(void)feedback;
	(void)identity;
	WL_GetPreferred ();
}

static const struct wp_color_management_surface_feedback_v1_listener	wl_colorfeedback_listener =
{
	.preferred_changed = WL_PreferredChanged,
};

// the image description the compositor would like the window's frames in: the
// display's, as the compositor has it; serial 0 until it said
const wl_colors_t *WL_PreferredColors (void)
{
	return &wl_colors;
}

bool WL_ColorsSupported (uint32_t tf, uint32_t primaries)
{
	return way.color && (wl_colorfeatures & (1u << WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC))
		&& (wl_colortfs & (1u << tf)) && (wl_colorprimaries & (1u << primaries));
}

/*
===============================================================================

PRESENTATION FEEDBACK

===============================================================================
*/

typedef struct
{
	uint64_t	serial;		// VID_Update's frame
	double		time;		// when it was given to Vulkan
} wl_presenting_t;

#define STATS_FRAMES	120
#define SCANOUT_FRAMES	30		// frames of a kind before the kind is said

static double	wl_latencies[STATS_FRAMES];
static int		wl_numlatencies, wl_nextlatency;
static int		wl_kindrun;			// frames in a row of the kind not yet said
static int		wl_kindsaid = -1;	// 1 zero-copy, 0 composited, -1 not said yet

static void WL_FeedbackSyncOutput (void *data, struct wp_presentation_feedback *feedback, struct wl_output *output)
{
	(void)data;
	(void)feedback;
	(void)output;
}

static void WL_FeedbackPresented (void *data, struct wp_presentation_feedback *feedback, uint32_t tv_sec_hi,
	uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
	wl_presenting_t	*frame = data;
	uint64_t		ns = (((uint64_t)tv_sec_hi << 32) | tv_sec_lo) * 1000000000u + tv_nsec;
	double			shown;
	int				kind, i;

	(void)seq_hi;
	(void)seq_lo;
	wp_presentation_feedback_destroy (feedback);

	if (frame->serial > wl_stats.presented)
		wl_stats.presented = frame->serial;
	wl_stats.flags = flags;
	wl_stats.refresh = refresh * 1e-9;
	wl_stats.count++;
	if (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY)
		wl_stats.zerocopy++;

	// how long from the frame's present to its showing, on the clock Sys_DoubleTime counts
	if (wl_clock == CLOCK_MONOTONIC)
	{
		shown = Sys_MonotonicToTime (ns);
		wl_latencies[wl_nextlatency] = shown - frame->time;
		wl_nextlatency = (wl_nextlatency + 1) % STATS_FRAMES;
		if (wl_numlatencies < STATS_FRAMES)
			wl_numlatencies++;
		wl_stats.latency = wl_stats.worst = 0;
		for (i = 0 ; i < wl_numlatencies ; i++)
		{
			wl_stats.latency += wl_latencies[i] / wl_numlatencies;
			if (wl_latencies[i] > wl_stats.worst)
				wl_stats.worst = wl_latencies[i];
		}
	}
	Mem_Free (frame);

	// scanned out directly, or drawn by the compositor: said once it has lasted
	kind = (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) != 0;
	if (kind == wl_kindsaid)
		wl_kindrun = 0;
	else if (++wl_kindrun >= SCANOUT_FRAMES)
	{
		if (kind)
			Con_Printf ("Direct scanout: on, the display shows the frames as they are\n");
		else if (wl_kindsaid == 1)
			Con_Printf ("Direct scanout: off, the compositor draws the frames into its own\n");
		wl_kindsaid = kind;
		wl_kindrun = 0;
	}
}

static void WL_FeedbackDiscarded (void *data, struct wp_presentation_feedback *feedback)
{
	wl_presenting_t	*frame = data;

	wp_presentation_feedback_destroy (feedback);
	if (frame->serial > wl_stats.presented)
		wl_stats.presented = frame->serial;
	wl_stats.discarded++;
	Mem_Free (frame);
}

static const struct wp_presentation_feedback_listener	wl_presented_listener =
{
	.sync_output = WL_FeedbackSyncOutput,
	.presented = WL_FeedbackPresented,
	.discarded = WL_FeedbackDiscarded,
};

// asks when the frame Vulkan presents next is shown; Vulkan's commit is the
// next, so the feedback is for it
void WL_BeforePresent (uint64_t serial, double time)
{
	struct wp_presentation_feedback	*feedback;
	wl_presenting_t					*frame;

	if (!way.presentation)
		return;
	frame = Mem_Alloc (sizeof(*frame));
	frame->serial = serial;
	frame->time = time;
	feedback = wp_presentation_feedback (way.presentation, way.surface);
	wp_presentation_feedback_add_listener (feedback, &wl_presented_listener, frame);
}

const wl_present_stats_t *WL_PresentStats (void)
{
	return &wl_stats;
}

/*
===============================================================================

THE CLIPBOARD

===============================================================================
*/

#define TEXT_MIME	"text/plain;charset=utf-8"

static char	wl_hastext;		// an offer's user data once it has offered text

static void WL_OfferMime (void *data, struct wl_data_offer *offer, const char *mime)
{
	(void)data;
	if (!strcmp (mime, TEXT_MIME))
		wl_data_offer_set_user_data (offer, &wl_hastext);
}

static void WL_OfferSourceActions (void *data, struct wl_data_offer *offer, uint32_t actions)
{
	(void)data;
	(void)offer;
	(void)actions;
}

static void WL_OfferAction (void *data, struct wl_data_offer *offer, uint32_t action)
{
	(void)data;
	(void)offer;
	(void)action;
}

static const struct wl_data_offer_listener	wl_offer_listener =
{
	.offer = WL_OfferMime,
	.source_actions = WL_OfferSourceActions,
	.action = WL_OfferAction,
};

// a new offer, its types to come: the clipboard's, or something dragged over
static void WL_DataOffer (void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
	(void)data;
	(void)device;
	wl_data_offer_add_listener (offer, &wl_offer_listener, NULL);
}

// nothing is dropped here
static void WL_DataEnter (void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface,
	wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer)
{
	(void)data;
	(void)device;
	(void)surface;
	(void)x;
	(void)y;
	if (wl_dragged)
		wl_data_offer_destroy (wl_dragged);
	wl_dragged = offer;
	if (offer)
		wl_data_offer_accept (offer, serial, NULL);
}

static void WL_DataLeave (void *data, struct wl_data_device *device)
{
	(void)data;
	(void)device;
	if (wl_dragged)
		wl_data_offer_destroy (wl_dragged);
	wl_dragged = NULL;
}

static void WL_DataMotion (void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)device;
	(void)time;
	(void)x;
	(void)y;
}

static void WL_DataDrop (void *data, struct wl_data_device *device)
{
	WL_DataLeave (data, device);
}

// the clipboard's offer, or none
static void WL_Selection (void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
	(void)data;
	(void)device;
	if (wl_selection && wl_selection != offer)
		wl_data_offer_destroy (wl_selection);
	wl_selection = offer;
}

static const struct wl_data_device_listener	wl_datadevice_listener =
{
	.data_offer = WL_DataOffer,
	.enter = WL_DataEnter,
	.leave = WL_DataLeave,
	.motion = WL_DataMotion,
	.drop = WL_DataDrop,
	.selection = WL_Selection,
};

/*
================
WL_GetClipboardText

The clipboard's text, from whoever holds it through a pipe, as a malloc'd
string; NULL if there is none, or it doesn't come in time
================
*/
char *WL_GetClipboardText (void)
{
	int				fds[2];
	char			*text = NULL, *grown;
	size_t			size = 0, used = 0;
	ssize_t			got;
	struct pollfd	p;

	if (!wl_selection || wl_data_offer_get_user_data (wl_selection) != &wl_hastext || pipe2 (fds, O_CLOEXEC))
		return NULL;
	wl_data_offer_receive (wl_selection, TEXT_MIME, fds[1]);
	close (fds[1]);
	wl_display_flush (way.display);

	p.fd = fds[0];
	p.events = POLLIN;
	for (;;)
	{
		if (poll (&p, 1, 200) <= 0)
			break;
		if (used + 4096 + 1 > size)
		{
			size = size ? size * 2 : 8192;
			grown = realloc (text, size);
			if (!grown)
				break;
			text = grown;
		}
		got = read (fds[0], text + used, size - used - 1);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		used += (size_t)got;
	}
	close (fds[0]);
	if (text)
		text[used] = 0;
	return text;
}

/*
===============================================================================

CONNECTION

===============================================================================
*/

/*
================
WL_Init

Connects, and opens the window, sized to the largest whole multiple (up to
4) of width x height pixels that fits what the compositor allows, unless it
gives a size of its own; true once the compositor has configured it
================
*/
bool WL_Init (int width, int height)
{
	struct zwp_linux_dmabuf_feedback_v1	*feedback;
	int		multiple;

	way.display = wl_display_connect (NULL);
	if (!way.display)
		return false;
	Sys_AddWindowFd (wl_display_get_fd (way.display));

	wl_registry = wl_display_get_registry (way.display);
	wl_registry_add_listener (wl_registry, &wl_registry_listener, NULL);
	wl_display_roundtrip (way.display);
	if (!way.compositor || !way.wm)
		Sys_Error ("The Wayland compositor has no %s", way.compositor ? "xdg-shell" : "wl_compositor");

	// the compositor's device, and what the globals say of themselves
	if (way.dmabuf)
	{
		feedback = zwp_linux_dmabuf_v1_get_default_feedback (way.dmabuf);
		zwp_linux_dmabuf_feedback_v1_add_listener (feedback, &wl_feedback_listener, NULL);
	}
	wl_display_roundtrip (way.display);
	if (way.dmabuf)
		zwp_linux_dmabuf_feedback_v1_destroy (feedback);

	way.surface = wl_compositor_create_surface (way.compositor);
	wl_surface_add_listener (way.surface, &wl_surface_listener, NULL);
	if (way.viewporter)
		wl_viewport = wp_viewporter_get_viewport (way.viewporter, way.surface);
	if (way.fractionalscale && wl_viewport)
	{
		wl_fscale = wp_fractional_scale_manager_v1_get_fractional_scale (way.fractionalscale, way.surface);
		wp_fractional_scale_v1_add_listener (wl_fscale, &wl_fscale_listener, NULL);
	}
	// a game: the compositor may scan it out, or let the display's refresh follow it
	if (way.contenttype)
	{
		wl_contenttype = wp_content_type_manager_v1_get_surface_content_type (way.contenttype, way.surface);
		wp_content_type_v1_set_content_type (wl_contenttype, WP_CONTENT_TYPE_V1_TYPE_GAME);
	}
	if (way.color)
	{
		wl_colorfeedback = wp_color_manager_v1_get_surface_feedback (way.color, way.surface);
		wp_color_management_surface_feedback_v1_add_listener (wl_colorfeedback, &wl_colorfeedback_listener, NULL);
		WL_GetPreferred ();
	}
	if (way.datadevices && way.seat)
	{
		wl_datadevice = wl_data_device_manager_get_data_device (way.datadevices, way.seat);
		wl_data_device_add_listener (wl_datadevice, &wl_datadevice_listener, NULL);
	}

	wl_xdgsurface = xdg_wm_base_get_xdg_surface (way.wm, way.surface);
	xdg_surface_add_listener (wl_xdgsurface, &wl_xdgsurface_listener, NULL);
	wl_toplevel = xdg_surface_get_toplevel (wl_xdgsurface);
	xdg_toplevel_add_listener (wl_toplevel, &wl_toplevel_listener, NULL);
	xdg_toplevel_set_app_id (wl_toplevel, "softworld");
	xdg_toplevel_set_title (wl_toplevel, "SoftWorld");
	if (way.decorations)
	{
		wl_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration (way.decorations, wl_toplevel);
		zxdg_toplevel_decoration_v1_add_listener (wl_decoration, &wl_decoration_listener, NULL);
		zxdg_toplevel_decoration_v1_set_mode (wl_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}

	// the first commit, without a buffer, and the configure it brings
	wl_surface_commit (way.surface);
	while (!wl_configured)
		if (wl_display_dispatch (way.display) < 0)
			Sys_Error ("The Wayland connection was lost opening the window");
	wl_display_roundtrip (way.display);		// the scale, which may come after

	if (!wl_width || !wl_height)
	{
		for (multiple = 4 ; multiple > 1 ; multiple--)
			if (!wl_boundswidth || (WL_Pixels (wl_boundswidth) >= width * multiple
				&& WL_Pixels (wl_boundsheight) >= height * multiple))
				break;
		wl_width = WL_Units (width * multiple);
		wl_height = WL_Units (height * multiple);
	}
	return true;
}

void WL_Shutdown (void)
{
	if (!way.display)
		return;
	WL_SetIdleInhibit (false);
	if (wl_decoration)
		zxdg_toplevel_decoration_v1_destroy (wl_decoration);
	if (wl_toplevel)
		xdg_toplevel_destroy (wl_toplevel);
	if (wl_xdgsurface)
		xdg_surface_destroy (wl_xdgsurface);
	wl_decoration = NULL;
	wl_toplevel = NULL;
	wl_xdgsurface = NULL;
	wl_display_flush (way.display);
}

/*
================
WL_PrepareRead

Before a wait on the display's fd: the events already read handled (the
Vulkan driver's calls read the socket too, and leave ours queued), and what
is to be sent sent; the number handled. A read is prepared either way, which
WL_FinishRead does or lets go.
================
*/
int WL_PrepareRead (void)
{
	int		n = 0;

	while (wl_display_prepare_read (way.display) != 0)
		n += wl_display_dispatch_pending (way.display);
	wl_display_flush (way.display);
	return n;
}

// the read prepared, done if the fd was readable; the events it brought handled
int WL_FinishRead (bool readable)
{
	int		n;

	if (readable)
		wl_display_read_events (way.display);
	else
		wl_display_cancel_read (way.display);
	n = wl_display_dispatch_pending (way.display);
	if (wl_display_get_error (way.display))
		Sys_Error ("The Wayland connection was lost (%s)", strerror (wl_display_get_error (way.display)));
	return n;
}

// what has come from the display, handled without waiting
int WL_ReadEvents (void)
{
	struct pollfd	p = {.fd = wl_display_get_fd (way.display), .events = POLLIN};
	int				n;

	n = WL_PrepareRead ();
	return n + WL_FinishRead (poll (&p, 1, 0) > 0);
}

/*
===============================================================================

THE REPORT

===============================================================================
*/

// the protocols that matter to latency and HDR, and whether there are
void WL_PrintProtocols (bool all)
{
	char	missing[512] = "";
	size_t	i, len = 0;

	for (i = 0 ; i < NUM_GLOBALS ; i++)
	{
		if (all)
			Con_Printf ("  %s %s (%s)\n", wl_globals[i].version ? "  " : "no", wl_globals[i].what,
				wl_globals[i].name);
		else if (!wl_globals[i].version && len < sizeof(missing) - 64)
			len += (size_t)snprintf (missing + len, sizeof(missing) - len, "%s%s", len ? ", " : "",
				wl_globals[i].name);
	}
	if (!all && len)
		Con_Printf ("The compositor lacks %s\n", missing);
}

bool WL_HasGlobal (const char *name)
{
	size_t	i;

	for (i = 0 ; i < NUM_GLOBALS ; i++)
		if (!strcmp (wl_globals[i].name, name))
			return wl_globals[i].version != 0;
	return false;
}
