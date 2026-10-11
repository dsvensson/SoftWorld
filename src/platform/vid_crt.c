// vid_crt.c -- guest(r)'s CRT-Guest-Advanced as the backends run it (vid_crt.h):
// vid_shader_crt and its settings, the passes and their textures, the constants
// each frame, the mip chain's dispatches, and the LUTs

#include "mem.h"
#include "tga.h"
#include "vid_crt.h"

#include <math.h>

cvar_t	vid_shader_crt = {.name = "vid_shader_crt", .string = "0", .archive = true,
	.description = "Shows the frame through guest(r)'s CRT-Guest-Advanced (libretro's slang-shaders): scanlines, "
		"a phosphor mask, glow, bloom, halation, curvature, afterglow, deconvergence. Its settings are "
		"vid_shader_crt_<name>, RetroArch's names in lower case.",
	.values = (const cvar_value_t[]){{"0", "The frame as it is"}, {"1", "Through the CRT shader"}, {0}}};

// a setting: its variable, default and range, what it does as RetroArch labels
// it after its name in the slang shaders, and where the shaders' block has it
typedef struct
{
	const char	*name;
	const char	*string;
	float		min, max;
	const char	*description;
	size_t		offset;
} vid_crt_setting_t;

#define CRT_SET(param, cvar, def, lo, hi, desc) \
	{"vid_shader_crt_" cvar, def, (float)(lo), (float)(hi), #param ": " desc, offsetof (vid_crt_params_t, param)}

static const vid_crt_setting_t	vid_crt_settings[] = {
	// afterglow0.slang
	CRT_SET (esrc, "esrc", "1", 1, 2, "Afterglow effect source, 1 OriginalHistory, 2 Source (the same frame here)."),
	CRT_SET (bth, "bth", "4", 1, 125, "Afterglow effect threshold."),
	CRT_SET (PR, "pr", "0.32", 0, 0.5, "Persistence red."),
	CRT_SET (PG, "pg", "0.32", 0, 0.5, "Persistence green."),
	CRT_SET (PB, "pb", "0.32", 0, 0.5, "Persistence blue."),
	// pre-shaders-afterglow.slang
	CRT_SET (AS, "as", "0.2", 0, 0.6, "Afterglow strength."),
	CRT_SET (agsat, "agsat", "0.5", 0, 1, "Afterglow saturation."),
	CRT_SET (CS, "cs", "0", 0, 5, "Display gamut: 0 sRGB, 1 Modern, 2 DCI, 3 Adobe, 4 Rec.2020, 5 P3."),
	CRT_SET (CP, "cp", "0", -1, 5, "CRT profile: EBU, P22, SMPTE-C, Philips, Trinitron (0 to 5; -1 none)."),
	CRT_SET (TNTC, "tntc", "0", 0, 4, "LUT colors: 0 none, 1 NTSC-J, 2 Trinitron, 3 NEC MultiSync, 4 NTSC 1953."),
	CRT_SET (LS, "ls", "32", 16, 64, "LUT size."),
	CRT_SET (WP, "wp", "0", -100, 100, "Color temperature, percent."),
	CRT_SET (wp_saturation, "wp_saturation", "1", 0, 2, "Saturation adjustment."),
	CRT_SET (pre_bb, "pre_bb", "1", 0, 2, "Brightness adjustment."),
	CRT_SET (contr, "contr", "0", -2, 2, "Contrast adjustment."),
	CRT_SET (pre_gc, "pre_gc", "1", 0.5, 1.75, "Gamma correct adjustment."),
	CRT_SET (sega_fix, "sega_fix", "0", 0, 1, "Sega brightness fix."),
	CRT_SET (BP, "bp", "0", -100, 25, "Raise black level."),
	CRT_SET (vigstr, "vigstr", "0", 0, 2, "Vignette strength."),
	CRT_SET (vigdef, "vigdef", "1", 0.5, 3, "Vignette size."),
	// avg-lum.slang
	CRT_SET (lsmooth, "lsmooth", "0.7", 0.5, 0.99, "Raster bloom effect smoothing."),
	CRT_SET (lsdev, "lsdev", "0", -0.25, 0.25, "Raster bloom smoothing bright deviation."),
	CRT_SET (OS, "os", "1", 0, 2, "Raster bloom overscan mode."),
	CRT_SET (BLOOM, "raster_bloom", "0", 0, 20, "Raster bloom, percent."),
	// linearize.slang
	CRT_SET (GAMMA_INPUT, "gamma_input", "2.4", 1, 5, "Gamma input."),
	CRT_SET (gamma_out, "gamma_out", "2.4", 1, 5, "Gamma out."),
	CRT_SET (inter, "inter", "375", 0, 800, "Interlace trigger resolution, VGA trigger."),
	CRT_SET (interm, "interm", "0", 0, 6, "Interlace mode: 0 off, 1 to 3 and 6 normal, 4 and 5 interpolation. "
		"RetroArch's default is 1: frames over inter lines tall alternate their fields."),
	CRT_SET (iscan, "iscan", "0.2", 0, 1, "Interlacing scanline effect ('laced brightness)."),
	CRT_SET (intres, "intres", "0", 0, 10, "Internal resolution Y: 0.5 to 10 divides the lines, 1 smart scanline mode."),
	CRT_SET (downsample_levelx, "downsample_levelx", "0", 0, 3, "Downsampling X (high resolution content, prescalers)."),
	CRT_SET (downsample_levely, "downsample_levely", "0", 0, 3, "Downsampling Y (high resolution content, prescalers)."),
	CRT_SET (iscans, "iscans", "0.25", 0, 1, "Interlacing (scanline) saturation."),
	CRT_SET (vga_mode, "vga_mode", "0", 0, 1, "VGA single/double scan mode."),
	CRT_SET (hiscan, "hiscan", "0", 0, 1, "High resolution scanlines."),
	// gaussian_horizontal.slang
	CRT_SET (m_glow, "m_glow", "0", 0, 2, "Ordinary glow (0) or magic glow (1, 2)."),
	CRT_SET (m_glow_cutoff, "m_glow_cutoff", "0.12", 0, 0.4, "Magic glow cutoff."),
	CRT_SET (m_glow_low, "m_glow_low", "0.35", 0, 7, "Magic glow low strength."),
	CRT_SET (m_glow_high, "m_glow_high", "5", 0, 7, "Magic glow high strength."),
	CRT_SET (m_glow_dist, "m_glow_dist", "1", 0.2, 4, "Magic glow distribution."),
	CRT_SET (m_glow_mask, "m_glow_mask", "1", 0, 2, "Magic glow mask strength."),
	CRT_SET (FINE_GLOW, "fine_glow", "1", -1, 5, "Fine glow/magic glow sampling."),
	CRT_SET (SIZEH, "sizeh", "6", 1, 50, "Horizontal glow radius."),
	CRT_SET (SIGMA_H, "sigma_h", "1.2", 0.2, 15, "Horizontal glow sigma."),
	// gaussian_vertical.slang
	CRT_SET (SIZEV, "sizev", "6", 1, 50, "Vertical glow radius."),
	CRT_SET (SIGMA_V, "sigma_v", "1.2", 0.2, 15, "Vertical glow sigma."),
	// bloom_horizontal.slang
	CRT_SET (FINE_BLOOM, "fine_bloom", "1", -1, 5, "Fine bloom/halation sampling."),
	CRT_SET (SIZEHB, "sizehb", "3", 1, 50, "Horizontal bloom/halation radius."),
	CRT_SET (SIGMA_HB, "sigma_hb", "0.75", 0.25, 15, "Horizontal bloom/halation sigma."),
	// bloom_vertical.slang
	CRT_SET (SIZEVB, "sizevb", "3", 1, 50, "Vertical bloom/halation radius."),
	CRT_SET (SIGMA_VB, "sigma_vb", "0.6", 0.25, 15, "Vertical bloom/halation sigma."),
	// crt-guest-advanced.slang
	CRT_SET (glow, "glow", "0.08", -2, 2, "(Magic) glow strength."),
	CRT_SET (bloom, "bloom", "0", -2, 2, "Bloom strength."),
	CRT_SET (mask_bloom, "mask_bloom", "0", -2, 2, "Mask bloom."),
	CRT_SET (bloom_dist, "bloom_dist", "0", -2, 3, "Bloom distribution."),
	CRT_SET (halation, "halation", "0", -2, 2, "Halation strength."),
	CRT_SET (bmask1, "bmask1", "0", -1, 1, "Bloom mask strength."),
	CRT_SET (hmask1, "hmask1", "0.35", -1, 1, "Halation mask strength."),
	CRT_SET (gamma_c, "gamma_c", "1", 0.5, 2, "Gamma correct."),
	CRT_SET (gamma_c2, "gamma_c2", "1", 1, 2, "Complementary gamma correct."),
	CRT_SET (brightboost, "brightboost", "1.4", 0.25, 10, "Bright boost dark pixels."),
	CRT_SET (brightboost1, "brightboost1", "1.1", 0.25, 3, "Bright boost bright pixels."),
	CRT_SET (clips, "clips", "0", -1, 1, "Clip saturated color beams."),
	CRT_SET (gsl, "gsl", "0", -1, 2, "Scanline type."),
	CRT_SET (scanline1, "scanline1", "6", -20, 40, "Scanline beam shape center."),
	CRT_SET (scanline2, "scanline2", "8", 0, 70, "Scanline beam shape edges."),
	CRT_SET (beam_min, "beam_min", "1.3", 0.25, 10, "Scanline shape dark pixels."),
	CRT_SET (beam_max, "beam_max", "1", 0.2, 3.5, "Scanline shape bright pixels."),
	CRT_SET (tds, "tds", "0", 0, 1, "Thinner dark scanlines."),
	CRT_SET (beam_size, "beam_size", "0.6", 0, 1, "Increased bright scanline beam."),
	CRT_SET (scans, "scans", "0.5", 0, 6, "Scanline saturation/mask falloff."),
	CRT_SET (scan_falloff, "scan_falloff", "1", 0.1, 2, "Scanline falloff."),
	CRT_SET (spike, "spike", "1", 0, 2, "Scanline spike removal."),
	CRT_SET (scangamma, "scangamma", "2.4", 0.5, 5, "Scanline gamma."),
	CRT_SET (rolling_scan, "rolling_scan", "0", -1, 1, "Rolling scanlines."),
	CRT_SET (no_scanlines, "no_scanlines", "0", 0, 1.5, "No-scanline mode."),
	CRT_SET (h_sharp, "h_sharp", "5.2", 0.2, 15, "Horizontal sharpness."),
	CRT_SET (s_sharp, "s_sharp", "0.5", 0, 2, "Substractive sharpness (1.0 recommended)."),
	CRT_SET (ring, "ring", "0", 0, 3, "Substractive sharpness ringing."),
	CRT_SET (smart_ei, "smart_ei", "0", 0, 0.75, "Smart edges effect strength."),
	CRT_SET (ei_limit, "ei_limit", "0.25", 0, 0.75, "Smart edges effect strength limit."),
	CRT_SET (sth, "sth", "0.23", 0, 1, "Smart edges smoothing threshold."),
	CRT_SET (TATE, "tate", "0", 0, 1, "TATE mode: a vertical game on a screen turned on its side."),
	CRT_SET (IOS, "ios", "0", 0, 4, "Integer scaling: odd Y, even X and Y."),
	CRT_SET (warpX, "warpx", "0", 0, 0.25, "Curvature X (0.03 suggested)."),
	CRT_SET (warpY, "warpy", "0", 0, 0.25, "Curvature Y (0.04 suggested)."),
	CRT_SET (c_shape, "c_shape", "0.25", 0.05, 0.6, "Curvature shape."),
	CRT_SET (overscanX, "overscanx", "0", -200, 200, "Overscan X, in the frame's pixels."),
	CRT_SET (overscanY, "overscany", "0", -200, 200, "Overscan Y, in the frame's pixels."),
	CRT_SET (VShift, "vshift", "0", -200, 200, "Vertical shift, in the frame's pixels."),
	// deconvergence.slang
	CRT_SET (csize, "csize", "0", 0, 0.35, "Corner size."),
	CRT_SET (bsize1, "bsize1", "0", 0, 2, "Border size."),
	CRT_SET (sborder, "sborder", "0.75", 0.25, 2, "Border intensity."),
	CRT_SET (barspeed, "barspeed", "50", 5, 200, "Hum bar speed."),
	CRT_SET (barintensity, "barintensity", "0", -1, 1, "Hum bar intensity."),
	CRT_SET (bardir, "bardir", "0", 0, 1, "Hum bar direction."),
	CRT_SET (shadowMask, "shadowmask", "0", -1, 14, "CRT mask: -1 none, 0 CGWG, 1 to 4 Lottes, 5 to 14 Trinitron "
		"(11, 13 and 14 for 4K)."),
	CRT_SET (maskstr, "maskstr", "0.3", -0.5, 1, "Mask strength (masks 0 and 5 to 14)."),
	CRT_SET (mcut, "mcut", "1.1", 0, 2, "Mask 5 to 14 low strength."),
	CRT_SET (maskboost, "maskboost", "1", 1, 3, "CRT mask boost."),
	CRT_SET (masksize, "masksize", "1", 1, 4, "CRT mask size."),
	CRT_SET (mask_zoom, "mask_zoom", "0", -10, 6, "CRT mask zoom (+ mask width)."),
	CRT_SET (mzoom_sh, "mzoom_sh", "0", 0, 1, "CRT mask zoom sharpen (needs mask zoom)."),
	CRT_SET (mshift, "mshift", "0", 0, 1, "(Transform to) shadow mask."),
	CRT_SET (mask_layout, "mask_layout", "0", 0, 1, "Mask layout: 0 RGB, 1 BGR, as the display's subpixels are."),
	CRT_SET (maskDark, "maskdark", "0.5", 0, 2, "Lottes maskDark."),
	CRT_SET (maskLight, "masklight", "1.5", 0, 2, "Lottes maskLight."),
	CRT_SET (mask_gamma, "mask_gamma", "2.4", 1, 5, "Mask gamma."),
	CRT_SET (slotmask, "slotmask", "0", 0, 1, "Slot mask strength bright pixels."),
	CRT_SET (slotmask1, "slotmask1", "0", 0, 1, "Slot mask strength dark pixels."),
	CRT_SET (slotwidth, "slotwidth", "0", 0, 16, "Slot mask width (0 automatic)."),
	CRT_SET (double_slot, "double_slot", "2", 1, 4, "Slot mask height: 2x1 or 4x1..."),
	CRT_SET (slotms, "slotms", "1", 1, 4, "Slot mask thickness."),
	CRT_SET (smoothmask, "smoothmask", "0", 0, 2, "Smooth masks in bright scanlines."),
	CRT_SET (smask_mit, "smask_mit", "0", 0, 1, "Mitigate slot mask interaction."),
	CRT_SET (bmask, "bmask", "0", 0, 0.25, "Base (black) mask strength."),
	CRT_SET (mclip, "mclip", "0", 0, 1, "Preserve mask strength."),
	CRT_SET (pr_scan, "pr_scan", "0.1", 0, 1, "Preserve scanline properties."),
	CRT_SET (edgemask, "edgemask", "0", 0, 1, "Mitigate mask on edges."),
	CRT_SET (dctypex, "dctypex", "0", 0, 0.75, "Deconvergence type X: 0 static, else dynamic."),
	CRT_SET (dctypey, "dctypey", "0", 0, 0.75, "Deconvergence type Y: 0 static, else dynamic."),
	CRT_SET (deconrr, "deconrr", "0", -15, 15, "Horizontal deconvergence red range."),
	CRT_SET (deconrg, "deconrg", "0", -15, 15, "Horizontal deconvergence green range."),
	CRT_SET (deconrb, "deconrb", "0", -15, 15, "Horizontal deconvergence blue range."),
	CRT_SET (deconrry, "deconrry", "0", -15, 15, "Vertical deconvergence red range."),
	CRT_SET (deconrgy, "deconrgy", "0", -15, 15, "Vertical deconvergence green range."),
	CRT_SET (deconrby, "deconrby", "0", -15, 15, "Vertical deconvergence blue range."),
	CRT_SET (decons, "decons", "1", 0, 3, "Deconvergence strength."),
	CRT_SET (addnoised, "addnoised", "0", -1, 1, "Add noise."),
	CRT_SET (noiseresd, "noiseresd", "2", 1, 10, "Noise resolution."),
	CRT_SET (noisetype, "noisetype", "0", 0, 1, "Noise type: 0 colored, 1 luma."),
	CRT_SET (post_br, "post_br", "1", 0.25, 5, "Post brightness."),
	CRT_SET (oimage, "oimage", "0", 0, 1, "Show original image: the frame as it is left of this part of the width."),
};

#define VID_CRT_SETTINGS	(sizeof(vid_crt_settings) / sizeof(vid_crt_settings[0]))

static_assert (VID_CRT_SETTINGS == sizeof(vid_crt_params_t) / sizeof(float),
	"a setting for each of crt_params.h's parameters");

static cvar_t	vid_crt_cvars[VID_CRT_SETTINGS];

void VID_CRT_Register (void)
{
	const vid_crt_setting_t	*s;
	size_t					i;

	Cvar_RegisterVariable (&vid_shader_crt);
	for (i = 0 ; i < VID_CRT_SETTINGS ; i++)
	{
		s = &vid_crt_settings[i];
		vid_crt_cvars[i] = (cvar_t){.name = (char *)s->name, .string = (char *)s->string, .archive = true,
			.description = s->description, .min = s->min, .max = s->max};
		Cvar_RegisterVariable (&vid_crt_cvars[i]);
	}
}

void VID_CRT_FillParams (vid_crt_params_t *params)
{
	size_t	i;

	for (i = 0 ; i < VID_CRT_SETTINGS ; i++)
		*(float *)((byte *)params + vid_crt_settings[i].offset) = vid_crt_cvars[i].value;
}

/*
===============================================================================

THE PASSES

===============================================================================
*/

// The preset's filter_linear and float_framebuffer: a texture is read as the
// pass after the one that draws it says, and RetroArch's default wrap mode is a
// black border. Its size is the preset's scale_type: the source's, the
// viewport's, or 800 (and 600) across.
const vid_crt_texture_t	vid_crt_textures[CRT_TEXTURES] = {
	[CRT_ORIGINAL] = {CRT_SIZE_SOURCE, CRT_RGBA8, false, false, false},
	[CRT_AFTERGLOW] = {CRT_SIZE_SOURCE, CRT_RGBA8, false, false, true},
	[CRT_AFTERGLOW_LAST] = {CRT_SIZE_SOURCE, CRT_RGBA8, false, false, true},
	[CRT_PREPASS] = {CRT_SIZE_SOURCE, CRT_RGBA8, true, true, false},
	[CRT_AVGLUM] = {CRT_SIZE_SOURCE, CRT_RGBA8, true, false, true},
	[CRT_AVGLUM_LAST] = {CRT_SIZE_SOURCE, CRT_RGBA8, true, false, true},
	[CRT_LINEARIZE] = {CRT_SIZE_SOURCE, CRT_RGBA16F, true, false, false},
	[CRT_GAUSS_H] = {CRT_SIZE_800_SOURCE, CRT_RGBA16F, true, false, false},
	[CRT_GLOW] = {CRT_SIZE_800_600, CRT_RGBA16F, true, false, false},
	[CRT_BLOOM_H] = {CRT_SIZE_800_600, CRT_RGBA16F, true, false, false},
	[CRT_BLOOM] = {CRT_SIZE_800_600, CRT_RGBA16F, true, false, false},
	[CRT_CRT] = {CRT_SIZE_VIEWPORT, CRT_RGBA16F, true, false, false},
	[CRT_LUT1] = {CRT_SIZE_LUT, CRT_RGBA8, true, false, false},
	[CRT_LUT2] = {CRT_SIZE_LUT, CRT_RGBA8, true, false, false},
	[CRT_LUT3] = {CRT_SIZE_LUT, CRT_RGBA8, true, false, false},
	[CRT_LUT4] = {CRT_SIZE_LUT, CRT_RGBA8, true, false, false},
};

// crt-guest-advanced.slangp's shaders 2 to 11, their inputs as each binds them
const vid_crt_pass_info_t	vid_crt_passes[CRT_PASSES] = {
	{"afterglow", CRT_AFTERGLOW, 2, {CRT_ORIGINAL, CRT_AFTERGLOW_LAST}},
	{"prepass", CRT_PREPASS, 6, {CRT_ORIGINAL, CRT_AFTERGLOW, CRT_LUT1, CRT_LUT2, CRT_LUT3, CRT_LUT4}},
	{"avglum", CRT_AVGLUM, 2, {CRT_PREPASS, CRT_AVGLUM_LAST}},
	{"linearize", CRT_LINEARIZE, 1, {CRT_PREPASS}},
	{"gauss_h", CRT_GAUSS_H, 1, {CRT_LINEARIZE}},
	{"gauss_v", CRT_GLOW, 1, {CRT_GAUSS_H}},
	{"bloom_h", CRT_BLOOM_H, 1, {CRT_LINEARIZE}},
	{"bloom_v", CRT_BLOOM, 1, {CRT_BLOOM_H}},
	{"crt", CRT_CRT, 3, {CRT_LINEARIZE, CRT_AVGLUM, CRT_PREPASS}},
	{"deconvergence", CRT_SCREEN, 7, {CRT_LINEARIZE, CRT_AVGLUM, CRT_GLOW, CRT_BLOOM, CRT_PREPASS, CRT_CRT,
		CRT_ORIGINAL}},
};

void VID_CRT_TextureSize (int texture, int sourcewidth, int sourceheight, int viewwidth, int viewheight,
	int *width, int *height)
{
	switch (vid_crt_textures[texture].size)
	{
	case CRT_SIZE_VIEWPORT:
		*width = viewwidth;
		*height = viewheight;
		break;
	case CRT_SIZE_800_SOURCE:
		*width = 800;
		*height = sourceheight;
		break;
	case CRT_SIZE_800_600:
		*width = 800;
		*height = 600;
		break;
	case CRT_SIZE_LUT:
		*width = 1024;
		*height = 32;
		break;
	default:
		*width = sourcewidth;
		*height = sourceheight;
		break;
	}
}

void VID_CRT_BeginFrame (vid_crt_frame_t *frame, double time)
{
	double	frames;

	frame->parity ^= 1;
	frames = frame->time > 0 && time >= frame->time ? (time - frame->time) * 60 : 1;
	frame->timescale = (float)fmin (frames, 3600);
	frame->framecount = (uint32_t)fmax (time * 60, 0);
	frame->time = time;
}

static void VID_CRT_Size (float size[4], int width, int height)
{
	size[0] = (float)width;
	size[1] = (float)height;
	size[2] = 1.0f / (float)width;
	size[3] = 1.0f / (float)height;
}

void VID_CRT_FillPasses (vid_crt_pass_t passes[CRT_PASSES], const vid_crt_frame_t *frame, int sourcewidth,
	int sourceheight, int viewwidth, int viewheight, int output, float paperwhite)
{
	int		i, w = sourcewidth, h = sourceheight, outw, outh;

	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		if (vid_crt_passes[i].output == CRT_SCREEN)
		{
			outw = viewwidth;
			outh = viewheight;
		}
		else
			VID_CRT_TextureSize (vid_crt_passes[i].output, sourcewidth, sourceheight, viewwidth, viewheight,
				&outw, &outh);
		// Source is the pass before's output (the first's, the stock passes' copy of the frame)
		VID_CRT_Size (passes[i].source, w, h);
		VID_CRT_Size (passes[i].original, sourcewidth, sourceheight);
		VID_CRT_Size (passes[i].output, outw, outh);
		passes[i].framecount = frame->framecount;
		passes[i].timescale = frame->timescale;
		passes[i].hdr = (float)output;
		passes[i].paperwhite = paperwhite;
		w = outw;
		h = outh;
	}
}

/*
===============================================================================

THE MIP CHAIN

===============================================================================
*/

int VID_CRT_MipLevels (int width, int height)
{
	int		size = width > height ? width : height, levels = 1;

	while (size > 1)
	{
		size >>= 1;
		levels++;
	}
	return levels;
}

int VID_CRT_SpdDispatches (int width, int height, vid_crt_spd_t spd[CRT_SPD_MAX_DISPATCHES],
	int groups[CRT_SPD_MAX_DISPATCHES][2])
{
	int		levels = VID_CRT_MipLevels (width, height), base = 0, count = 0, w, h, n;

	while (base < levels - 1 && count < CRT_SPD_MAX_DISPATCHES)
	{
		w = width >> base > 1 ? width >> base : 1;
		h = height >> base > 1 ? height >> base : 1;
		n = levels - 1 - base;
		if (n > CRT_SPD_LEVELS)
			n = CRT_SPD_LEVELS;
		// the last workgroup goes on from level 6, at most 64x64 of them
		if ((w > CRT_SPD_TILE * CRT_SPD_TILE || h > CRT_SPD_TILE * CRT_SPD_TILE) && n > 6)
			n = 6;
		groups[count][0] = (w + CRT_SPD_TILE - 1) / CRT_SPD_TILE;
		groups[count][1] = (h + CRT_SPD_TILE - 1) / CRT_SPD_TILE;
		spd[count] = (vid_crt_spd_t){
			.base = (uint32_t)base,
			.levels = (uint32_t)n,
			.numgroups = (uint32_t)(groups[count][0] * groups[count][1]),
			.groupsx = (uint32_t)groups[count][0],
			.width = (uint32_t)w,
			.height = (uint32_t)h,
		};
		count++;
		base += n;
	}
	return count;
}

/*
===============================================================================

THE LUTS

===============================================================================
*/

// crt/lut's TGA files (cmake/embed.cmake)
extern const unsigned char	vid_crt_lut_trinitron[], vid_crt_lut_inv_trinitron[], vid_crt_lut_nec[], vid_crt_lut_ntsc[];
extern const size_t			vid_crt_lut_trinitron_size, vid_crt_lut_inv_trinitron_size, vid_crt_lut_nec_size,
							vid_crt_lut_ntsc_size;

byte *VID_CRT_LoadLUT (int lut, int *width, int *height)
{
	static const struct
	{
		const unsigned char	*data;
		const size_t		*size;
	} luts[4] = {
		{vid_crt_lut_trinitron, &vid_crt_lut_trinitron_size},
		{vid_crt_lut_inv_trinitron, &vid_crt_lut_inv_trinitron_size},
		{vid_crt_lut_nec, &vid_crt_lut_nec_size},
		{vid_crt_lut_ntsc, &vid_crt_lut_ntsc_size},
	};

	return TGA_Decode (luts[lut].data, (int)*luts[lut].size, width, height, NULL);
}
