// vid_common.c -- what the video backends share (vid_common.h): the vid_ cvars,
// the frame a window gets and where it is shown, the presenter's constants, and
// the frame as the screen shows it, in SDR or HDR, for screenshots and dumps

#include "args.h"
#include "print.h"
#include "q_string.h"
#include "render.h"
#include "vid_common.h"

#include <math.h>

viddef_t	vid;				// global video state

cvar_t	vid_vsync = {.name = "vid_vsync", .string = "0", .archive = true,
	.description = "Shows frames in step with the display's refresh (never in a timedemo).",
	.values = (const cvar_value_t[]){{"0", "Frames shown without waiting for the display"},
		{"1", "A frame at each refresh"}, {0}}};
static bool	vid_unpaced;		// a timedemo: no waiting for the display

void VID_SetUnpaced (bool on)
{
	vid_unpaced = on;
}

bool VID_Vsync (void)
{
	return vid_vsync.value && !vid_unpaced;
}
// render pixels per pixel of the 320x200 layout; 0 is the most the window holds
static cvar_t	vid_scale = {.name = "vid_scale", .string = "1", .archive = true,
	.description = "Render pixels per pixel of the 320x200 layout, up to 16; 0 is the most the window holds. "
		"-scale overrides it."};
// pixels 1.2 times as tall as wide, as 320x200 was shown on 4:3 screens
static cvar_t	vid_crt = {.name = "vid_crt", .string = "0", .archive = true,
	.description = "Shows pixels 1.2 times as tall as wide, as 320x200 was shown on 4:3 screens.",
	.values = (const cvar_value_t[]){{"0", "Square pixels"}, {"1", "Pixels 1.2 times as tall as wide"}, {0}}};
// the layout is as wide as the window instead of 320, and sees more to the sides
static cvar_t	vid_widescreen = {.name = "vid_widescreen", .string = "0", .archive = true,
	.description = "Makes the layout as wide as the window instead of 320, seeing more to the sides.",
	.values = (const cvar_value_t[]){{"0", "320 wide"}, {"1", "As wide as the window"}, {0}}};
// 0: whole multiples of the render size, letterboxed; 1: fill the window, sharp bilinear
static cvar_t	vid_scalemode = {.name = "vid_scalemode", .string = "0", .archive = true,
	.description = "How the frame is fitted to the window.",
	.values = (const cvar_value_t[]){{"0", "Whole multiples of the render size, letterboxed"},
		{"1", "Filling the window, sharp bilinear"}, {0}}};
// how far the 3D view's light spreads from mid gray: over 1 darker darks and brighter lights
static cvar_t	vid_contrast = {.name = "vid_contrast", .string = "1", .archive = true,
	.description = "Contrast of the 3D view around mid gray: over 1 darker darks and brighter lights, under 1 flatter. "
		"The HUD isn't affected."};
// use HDR output when the display can show it
cvar_t	vid_hdr = {.name = "vid_hdr", .string = "1", .archive = true,
	.description = "Uses HDR output when the display can show it.",
	.values = (const cvar_value_t[]){{"0", "SDR output always"}, {"1", "HDR output on an HDR display"}, {0}}};
// brightness of SDR white on an HDR display, in nits; 0 is the system's
cvar_t	vid_hdr_paperwhite = {.name = "vid_hdr_paperwhite", .string = "0", .archive = true,
	.description = "Brightness of SDR white on an HDR display, in nits; 0 is the system's. "
		"On macOS the nits are over a white of 100."};

static int		vid_forcedscale;	// -scale, which the configuration can't change
static bool		vid_crtshown;		// vid_crt when the frame was set
static vid_present_t	vid_present = {.gamma = 1};

int VID_RegisterCommon (void)
{
	int		scale = 2;
	int		i;

	Cvar_RegisterVariable (&vid_vsync);
	Cvar_RegisterVariable (&vid_scale);
	Cvar_RegisterVariable (&vid_crt);
	Cvar_RegisterVariable (&vid_widescreen);
	Cvar_RegisterVariable (&vid_scalemode);
	Cvar_RegisterVariable (&vid_contrast);
	Cvar_RegisterVariable (&vid_hdr);
	Cvar_RegisterVariable (&vid_hdr_paperwhite);

	// -scale forces the render scale; the window starts that size either way
	i = COM_CheckParm ("-scale");
	if (i && i + 1 < com_argc)
	{
		scale = Q_atoi (com_argv[i + 1]);
		if (scale < 1)
			scale = 1;
		if (scale > VID_MAX_SCALE)
			scale = VID_MAX_SCALE;
		vid_forcedscale = scale;
	}
	return scale;
}

void VID_SetPresent (const vid_present_t *present)
{
	vid_present = *present;
}

/*
================
VID_WantedScale

vid_scale, or the largest whole multiple of 320x200 the window holds
================
*/
static int VID_WantedScale (int clientwidth, int clientheight)
{
	int		scale;
	float	stretch = vid_crt.value ? VID_CRT_STRETCH : 1.0f;

	if (vid_forcedscale)
		scale = vid_forcedscale;
	else if (vid_scale.value >= 1)
		scale = (int)vid_scale.value;
	else
	{
		scale = clientwidth / VID_BASE_WIDTH;
		if ((int)(clientheight / (VID_BASE_HEIGHT * stretch)) < scale)
			scale = (int)(clientheight / (VID_BASE_HEIGHT * stretch));
	}
	if (scale < 1)
		scale = 1;
	if (scale > VID_MAX_SCALE)
		scale = VID_MAX_SCALE;
	return scale;
}

/*
================
VID_WantedWidth

The width of the layout: 320, or with vid_widescreen what the window holds
at the scale, in steps of 8
================
*/
static int VID_WantedWidth (int scale, int clientwidth, int clientheight)
{
	float	stretch = vid_crt.value ? VID_CRT_STRETCH : 1.0f;
	int		width;

	if (!vid_widescreen.value || clientheight <= 0)
		return VID_BASE_WIDTH;
	// as wide as the window is at the height the layout is shown at
	width = (int)(clientwidth * (VID_BASE_HEIGHT * stretch * scale) / clientheight / scale) & ~7;
	if (width < VID_BASE_WIDTH)
		width = VID_BASE_WIDTH;
	if (width > MAX_CONWIDTH)
		width = MAX_CONWIDTH;
	return width;
}

vid_frame_t VID_WantedFrame (int clientwidth, int clientheight)
{
	vid_frame_t	frame;

	frame.scale = VID_WantedScale (clientwidth, clientheight);
	frame.width = VID_WantedWidth (frame.scale, clientwidth, clientheight) * frame.scale;
	frame.height = VID_BASE_HEIGHT * frame.scale;
	return frame;
}

void VID_SetFrame (const vid_frame_t *frame, unsigned rowpixels)
{
	vid_crtshown = vid_crt.value != 0;
	vid.huddirty = true;

	vid.rowpixels = rowpixels;
	vid.width = (unsigned)frame->width;
	vid.height = (unsigned)frame->height;
	vid.conwidth = (unsigned)(frame->width / frame->scale);
	vid.conheight = (unsigned)(frame->height / frame->scale);
	// the renderer's pixel aspect: width over height of a pixel as shown
	vid.aspect = vid_crt.value ? ((float)VID_BASE_HEIGHT / (float)VID_BASE_WIDTH) * (320.0f / 240.0f) : 1.0f;
	vid.recalc_refdef = 1;

	vid.scale = (unsigned)frame->scale;
	R_SetRenderSize (frame->width, frame->height, frame->scale);
	Con_DPrintf ("Render size %dx%d\n", frame->width, frame->height);
}

bool VID_NeedsResize (int clientwidth, int clientheight)
{
	vid_frame_t	frame;

	if (clientwidth <= 0 || clientheight <= 0)
		return false;
	frame = VID_WantedFrame (clientwidth, clientheight);
	return frame.scale != (int)vid.scale || frame.width / frame.scale != (int)vid.conwidth
		|| (vid_crt.value != 0) != vid_crtshown;
}

vid_fit_t VID_Fit (int clientwidth, int clientheight)
{
	vid_fit_t	fit;
	float		sx, sy;

	// aspect-preserving fit; whole multiples of the render size unless filling the window
	fit.stretch = vid_crt.value ? VID_CRT_STRETCH : 1.0f;
	sx = (float)clientwidth / (float)vid.width;
	sy = (float)clientheight / ((float)vid.height * fit.stretch);
	fit.scale = sx < sy ? sx : sy;
	if (fit.scale >= 1.0f && !vid_scalemode.value)
		fit.scale = floorf (fit.scale);

	fit.width = floorf (vid.width * fit.scale);
	fit.height = floorf (vid.height * fit.scale * fit.stretch);
	fit.x = floorf ((clientwidth - fit.width) * 0.5f);
	fit.y = floorf ((clientheight - fit.height) * 0.5f);
	return fit;
}

// the output the presenter last drew to, for screenshots
typedef struct
{
	int		output;			// VID_OUTPUT_*
	float	paperwhite;		// in the output's values
	float	peak;
} vid_output_t;

static vid_output_t	vid_shown = {VID_OUTPUT_SDR, 1, 1};

void VID_FillConstants (vid_present_constants_t *constants, const vid_fit_t *fit, int output,
	float paperwhite, float peak)
{
	int		i;

	for (i = 0 ; i < 4 ; i++)
		constants->blend[i] = vid_present.blend[i];
	constants->texsize[0] = (float)vid.width;
	constants->texsize[1] = (float)vid.height;
	constants->scale[0] = fit->scale > 1.0f ? fit->scale : 1.0f;
	constants->scale[1] = fit->scale * fit->stretch > 1.0f ? fit->scale * fit->stretch : 1.0f;
	constants->gamma = vid_present.gamma;
	constants->contrast = vid_contrast.value > 0 ? vid_contrast.value : 1;
	constants->sharp = fit->scale == floorf (fit->scale) && fit->stretch == 1.0f ? 0.0f : 1.0f;
	constants->hdr = (float)output;
	constants->paperwhite = paperwhite;
	constants->peak = peak;
	constants->pad[0] = constants->pad[1] = 0;
	vid_shown = (vid_output_t){output, paperwhite, peak};
}

/*
===============================================================================

THE FRAME AS SHOWN

===============================================================================
*/

#define VID_MIDGRAY		0.18f	// linear light that contrast keeps

static double VID_LinearToSrgb (double l)
{
	return l <= 0.0031308 ? l * 12.92 : 1.055 * pow (l, 1 / 2.4) - 0.055;
}

static double VID_SrgbToLinear (double c)
{
	return c <= 0.04045 ? c / 12.92 : pow (fmax ((c + 0.055) / 1.055, 0), 2.4);
}

// light for SDR: what is brighter than white keeps its hue and goes toward
// white the brighter it is; the rest is as it is
static void VID_FitWhite (float light[3])
{
	float	m = fmaxf (light[0], fmaxf (light[1], light[2]));
	int		i;

	if (m > 1)
		for (i = 0 ; i < 3 ; i++)
			light[i] = light[i] / m + (1 - light[i] / m) * (1 - 1 / m);
}

// a channel of the view as linear light (vid.h)
static float VID_ChannelLight (unsigned code)
{
	float	c = code / 512.0f;

	c *= c;
	return c * c;
}

/*
================
VID_FrameToRGB

What the present shaders do for SDR (in light the view's gamma and contrast,
what is brighter than white toward white, then sRGB, the view's blend and the
2D over it), or if not shown only white clipped, sRGB and the 2D
================
*/
void VID_FrameToRGB (byte *rgb, bool shown)
{
	static byte	raw[1024];
	const pixel_t		*frame;
	const hudpixel_t	*hud;
	pixel_t		p;
	hudpixel_t	h;
	float		light[3], c, contrast;
	unsigned	x, y, a, i;
	int			v;

	for (v = 0 ; v < 1024 ; v++)
		raw[v] = (byte)(255 * VID_LinearToSrgb (fmin (pow (v / 512.0, 4), 1)) + 0.5);
	contrast = vid_contrast.value > 0 ? vid_contrast.value : 1;
	VID_ShownLayers (&frame, &hud);

	for (y = 0 ; y < vid.height ; y++)
		for (x = 0 ; x < vid.width ; x++, rgb += 3)
		{
			p = frame[y * vid.rowpixels + x];
			h = hud[y * vid.rowpixels + x];
			if (!shown)
			{
				rgb[0] = raw[RGB30_R (p)];
				rgb[1] = raw[RGB30_G (p)];
				rgb[2] = raw[RGB30_B (p)];
			}
			else
			{
				light[0] = VID_ChannelLight (RGB30_R (p));
				light[1] = VID_ChannelLight (RGB30_G (p));
				light[2] = VID_ChannelLight (RGB30_B (p));
				for (i = 0 ; i < 3 ; i++)
				{
					light[i] = powf (fmaxf (light[i], 0), vid_present.gamma);
					light[i] = VID_MIDGRAY * powf (light[i] / VID_MIDGRAY, contrast);
				}
				VID_FitWhite (light);
				for (i = 0 ; i < 3 ; i++)
				{
					c = (float)VID_LinearToSrgb (fminf (light[i], 1));
					c += (vid_present.blend[i] - c) * vid_present.blend[3];
					rgb[i] = (byte)(255 * c + 0.5f);
				}
			}
			a = HUD_A (h);
			if (!a)
				continue;
			// premultiplied: the 2D's color, and the view's through the rest
			for (i = 0 ; i < 3 ; i++)
				rgb[i] = (byte)(((h >> (i * 8)) & 255) + (rgb[i] * (255 - a) + 127) / 255);
		}
}

bool VID_ShowsHDR (void)
{
	return vid_shown.output != VID_OUTPUT_SDR;
}

// linear BT.709 light, 1 being SDR white at 203 cd/m², as PQ in BT.2020 (SMPTE
// ST 2084), 16 bit
static void VID_LightToPQ (const float light[3], uint16_t pq[3])
{
	static const double	bt709to2020[3][3] = {
		{0.627404, 0.329283, 0.043313},
		{0.069097, 0.919541, 0.011362},
		{0.016391, 0.088013, 0.895595}};
	const double	m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128;
	const double	c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
	double			y, ym;
	int				i;

	for (i = 0 ; i < 3 ; i++)
	{
		y = bt709to2020[i][0] * light[0] + bt709to2020[i][1] * light[1] + bt709to2020[i][2] * light[2];
		y = fmin (fmax (y * (203.0 / 10000), 0), 1);
		ym = pow (y, m1);
		pq[i] = (uint16_t)(65535 * pow ((c1 + c2 * ym) / (1 + c3 * ym), m2) + 0.5);
	}
}

/*
================
VID_FrameToPQ

What the present shaders do for HDR (in light the view's gamma and contrast,
the view's blend over its sRGB values, SDR white at paper white and what is
brighter rolled off toward the display's peak, the 2D over it in light at
paper white), as light over the output's paper white, then PQ
================
*/
void VID_FrameToPQ (uint16_t *rgb)
{
	const pixel_t		*frame;
	const hudpixel_t	*hud;
	pixel_t		p;
	hudpixel_t	h;
	float		light[3], white, peak, knee, span, contrast, a, s;
	unsigned	x, y, i;

	white = vid_shown.paperwhite > 0 ? vid_shown.paperwhite : 1;
	peak = vid_shown.peak;
	knee = fmaxf (white, 0.75f * peak);
	span = fmaxf (peak - knee, 1e-3f);
	contrast = vid_contrast.value > 0 ? vid_contrast.value : 1;
	VID_ShownLayers (&frame, &hud);

	for (y = 0 ; y < vid.height ; y++)
		for (x = 0 ; x < vid.width ; x++, rgb += 3)
		{
			p = frame[y * vid.rowpixels + x];
			h = hud[y * vid.rowpixels + x];
			light[0] = VID_ChannelLight (RGB30_R (p));
			light[1] = VID_ChannelLight (RGB30_G (p));
			light[2] = VID_ChannelLight (RGB30_B (p));
			for (i = 0 ; i < 3 ; i++)
			{
				light[i] = powf (fmaxf (light[i], 0), vid_present.gamma);
				light[i] = VID_MIDGRAY * powf (fmaxf (light[i] / VID_MIDGRAY, 0), contrast);
				s = (float)VID_LinearToSrgb (light[i]);
				light[i] = (float)VID_SrgbToLinear (s + (vid_present.blend[i] - s) * vid_present.blend[3]);
			}
			if (peak <= white * 1.05f)
			{
				VID_FitWhite (light);
				for (i = 0 ; i < 3 ; i++)
					light[i] *= white;
			}
			else
				for (i = 0 ; i < 3 ; i++)
				{
					light[i] *= white;
					light[i] = fminf (light[i], knee) + span * (1 - expf (-fmaxf (light[i] - knee, 0) / span));
				}
			// the 2D, premultiplied, in light at paper white
			a = HUD_A (h) / 255.0f;
			for (i = 0 ; i < 3 ; i++)
			{
				s = (float)VID_SrgbToLinear (((h >> (i * 8)) & 255) / 255.0 / fmax (a, 1 / 255.0));
				light[i] = (s * white * a + light[i] * (1 - a)) / white;
			}
			VID_LightToPQ (light, rgb);
		}
}
