// vid_common.h -- what the video backends share: the vid_ cvars, the frame a
// window gets, where it is shown in the window, the presenter's constants, and
// the frame as the screen shows it in SDR. The backends allocate the frame and
// present it.
#pragma once

#include "cvar.h"
#include "vid.h"

#define VID_BASE_WIDTH	320
#define VID_BASE_HEIGHT	200
#define VID_MAX_SCALE	16
#define VID_CRT_STRETCH	1.2f	// 320x200 shown as 320x240

extern cvar_t	vid_vsync;
extern cvar_t	vid_hdr;
extern cvar_t	vid_hdr_paperwhite;

// registers the vid_ cvars and reads -scale; returns the scale the window
// starts at
int		VID_RegisterCommon (void);

// the frame for a window of that many pixels: the layout's width (320, or
// wider with vid_widescreen) and 200, times the scale
typedef struct
{
	int		width, height;
	int		scale;
} vid_frame_t;

vid_frame_t	VID_WantedFrame (int clientwidth, int clientheight);

// the frame just allocated becomes the one drawn: vid.buffer and vid.hud hold
// width x height pixels, rowpixels apart
void	VID_SetFrame (const vid_frame_t *frame, unsigned rowpixels);

// the window was resized, or the video settings changed, since VID_SetFrame
bool	VID_NeedsResize (int clientwidth, int clientheight);

// where the frame is shown in the window: aspect kept, whole multiples of its
// size unless vid_scalemode fills the window, centered
typedef struct
{
	float	x, y, width, height;	// in window pixels
	float	scale;					// window pixels per frame pixel, across
	float	stretch;				// how much taller than wide a frame pixel is shown
} vid_fit_t;

vid_fit_t	VID_Fit (int clientwidth, int clientheight);

// the constants of the present shaders (present.hlsl, present.metal,
// present.glsl), laid out as they declare them
typedef struct
{
	float	blend[4];		// sRGB color, and how much of it covers the view
	float	texsize[2];		// frame size in texels
	float	scale[2];		// screen pixels per texel
	float	gamma;			// exponent applied to the view's light; 1 keeps it
	float	contrast;		// the light as mid gray times (light / mid gray) to this
	float	sharp;			// 0: integer scale, nearest texel; 1: sharp bilinear
	float	hdr;			// the output: VID_OUTPUT_SDR, _LINEAR or _PQ
	float	paperwhite;		// output value of SDR white
	float	peak;			// output value of the display's brightest white
	float	pad[2];
} vid_present_constants_t;

static_assert (sizeof(vid_present_constants_t) == 64, "the shaders' constants are 64 bytes");

// what the shaders output: sRGB; linear light (scRGB, EDR); or that light as
// PQ in BT.2020, 1 being 203 cd/m² (present.glsl alone). paperwhite and peak
// are in the output's linear values.
enum { VID_OUTPUT_SDR, VID_OUTPUT_LINEAR, VID_OUTPUT_PQ };

void	VID_FillConstants (vid_present_constants_t *constants, const vid_fit_t *fit, int output,
			float paperwhite, float peak);

// the layers of the frame last drawn, which VID_FrameToRGB reads (the
// backend's: it may have handed the renderer other buffers since)
void	VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud);
