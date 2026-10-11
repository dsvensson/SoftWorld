// vid_crt.h -- guest(r)'s CRT-Guest-Advanced (libretro's slang-shaders,
// crt/crt-guest-advanced.slangp) as the backends run it after the frame:
// vid_shader_crt and its settings, the passes and the textures they draw in and
// read, the constants each takes, and the mip chain's dispatches. The shaders
// are crt/crt_core.inc in each backend's language (crt.hlsl, crt.glsl,
// crt.metal); each backend has the GPU's side (crt_d3d11.c, crt_vulkan.c,
// crt_metal.m, vid_webgl.js).
//
// The frame is drawn first as present.* shows it, at its own size, into
// CRT_ORIGINAL (RetroArch's Original; the preset's two stock passes copy it), then
// the passes run in order, the last into the window's letterboxed viewport.
#pragma once

#include "cvar.h"
#include "q_types.h"

#include <stddef.h>
#include <stdint.h>

extern cvar_t	vid_shader_crt;

// the CRT shader's variables (vid_shader_crt and its settings); vid_common.c's
// VID_RegisterCommon registers them
void	VID_CRT_Register (void);

// the frame is shown through the shader: the one test when it isn't
static inline bool VID_CRT_Active (void)
{
	return vid_shader_crt.value != 0;
}

// the shaders' parameters as their block holds them (crt/crt_params.h), a float each
typedef struct
{
#define CRT_PARAM(name) float name;
#include "crt/crt_params.h"
#undef CRT_PARAM
} vid_crt_params_t;

#define VID_CRT_PARAMS_SIZE	((sizeof(vid_crt_params_t) + 15) & ~(size_t)15)	// a GPU buffer's, 16 byte multiples

// the settings' values for the shaders
void	VID_CRT_FillParams (vid_crt_params_t *params);

// the textures: the targets the passes draw in, the frame's own, and the LUTs.
// AFTERGLOW and AVGLUM are drawn again from what they were the frame before:
// two each, which the passes name as this frame's and the last's
enum
{
	CRT_ORIGINAL,			// the frame as shown, its size (Original)
	CRT_AFTERGLOW,			// afterglow0's: this frame's (AfterglowPass)
	CRT_AFTERGLOW_LAST,		// the last frame's (AfterglowPassFeedback)
	CRT_PREPASS,			// pre-shaders-afterglow's (PrePass), with its mip chain
	CRT_AVGLUM,				// avg-lum's: this frame's (AvgLumPass)
	CRT_AVGLUM_LAST,		// the last frame's (AvgLumPassFeedback)
	CRT_LINEARIZE,			// linearize's (LinearizePass)
	CRT_GAUSS_H,			// gaussian_horizontal's
	CRT_GLOW,				// gaussian_vertical's (GlowPass)
	CRT_BLOOM_H,			// bloom_horizontal's
	CRT_BLOOM,				// bloom_vertical's (BloomPass)
	CRT_CRT,				// crt-guest-advanced's, deconvergence's Source
	CRT_LUT1,				// SamplerLUT1 to 4: trinitron, inv-trinitron, nec, ntsc
	CRT_LUT2,
	CRT_LUT3,
	CRT_LUT4,
	CRT_TEXTURES,
	CRT_SCREEN = -1			// a pass's output: the window
};

enum { CRT_SIZE_SOURCE, CRT_SIZE_VIEWPORT, CRT_SIZE_800_SOURCE, CRT_SIZE_800_600, CRT_SIZE_LUT };
enum { CRT_RGBA8, CRT_RGBA16F };

typedef struct
{
	int		size;		// CRT_SIZE_*
	int		format;		// CRT_RGBA8, CRT_RGBA16F
	bool	linear;		// read bilinearly, else the nearest texel; always black past the edges
	bool	mips;		// with its mip chain (VID_CRT_MipLevels)
	bool	feedback;	// read the frame after it's drawn: black when made
} vid_crt_texture_t;

extern const vid_crt_texture_t	vid_crt_textures[CRT_TEXTURES];

// a texture's size for a frame of source pixels shown in a viewport of view pixels
void	VID_CRT_TextureSize (int texture, int sourcewidth, int sourceheight, int viewwidth, int viewheight,
			int *width, int *height);

// the texture a frame's pass draws in or reads: AFTERGLOW and AVGLUM's two
// change places each frame (parity, vid_crt_frame_t)
static inline int VID_CRT_Physical (int texture, int parity)
{
	if (texture == CRT_AFTERGLOW || texture == CRT_AFTERGLOW_LAST)
		return CRT_AFTERGLOW + ((texture - CRT_AFTERGLOW) ^ parity);
	if (texture == CRT_AVGLUM || texture == CRT_AVGLUM_LAST)
		return CRT_AVGLUM + ((texture - CRT_AVGLUM) ^ parity);
	return texture;
}

// the passes, in order: each draws its output from its inputs, which the shader
// takes as T0 up
#define CRT_PASSES		10
#define CRT_MAX_INPUTS	7
#define CRT_PASS_PREPASS	1	// the one after which the mip chain is made

typedef struct
{
	const char	*name;						// the shaders' crt_<name>
	int			output;						// a texture, or CRT_SCREEN
	int			numinputs;
	int			inputs[CRT_MAX_INPUTS];
} vid_crt_pass_info_t;

extern const vid_crt_pass_info_t	vid_crt_passes[CRT_PASSES];

// a pass's own constants: the slang shaders' SourceSize (the pass before's
// output), OriginalSize and OutputSize, each width, height, 1/width and
// 1/height; FrameCount; and for the last its output
typedef struct
{
	float		source[4];
	float		original[4];
	float		output[4];
	uint32_t	framecount;		// frames of 60 Hz since the program started
	float		timescale;		// frames of 60 Hz since the last frame drawn through the shader
	float		hdr;			// the window's: VID_OUTPUT_SDR, _LINEAR or _PQ
	float		paperwhite;		// the output value of SDR white
} vid_crt_pass_t;

static_assert (sizeof(vid_crt_pass_t) == 64, "the shaders' Pass is 64 bytes");

// a frame through the shader: which of the two feedback textures is this
// frame's, and the time the constants count. The shaders were made for 60 frames
// a second: afterglow's persistence, raster bloom's smoothing, the noise, the hum
// bar and rolling scanlines go by frames of 60 Hz, as many as have passed, at
// any frame rate.
typedef struct
{
	int			parity;
	uint32_t	framecount;
	float		timescale;
	double		time;			// when the last frame was drawn; 0 before the first
} vid_crt_frame_t;

void	VID_CRT_BeginFrame (vid_crt_frame_t *frame, double time);

// the passes' constants for a frame
void	VID_CRT_FillPasses (vid_crt_pass_t passes[CRT_PASSES], const vid_crt_frame_t *frame, int sourcewidth,
			int sourceheight, int viewwidth, int viewheight, int output, float paperwhite);

// PREPASS's mip levels: as the GPUs have them, each half the one before
// (rounded down, at least 1) down to 1x1
int		VID_CRT_MipLevels (int width, int height);

// The mip chain is made by a compute shader, crt_spd, as AMD's FidelityFX
// Single Pass Downsampler makes one. A dispatch reads a level and makes
// up to 12 after it, each of its workgroups a tile of 64x64 texels down to one;
// the last workgroup to finish makes the rest from those. A level of more than
// 4096 texels across makes 6, and the next dispatch the rest.
#define CRT_SPD_LEVELS		12		// a dispatch makes at most
#define CRT_SPD_TILE		64		// texels across a workgroup's tile
#define CRT_SPD_THREADS		256		// a workgroup's
#define CRT_SPD_MAX_DISPATCHES	3

typedef struct
{
	uint32_t	base;			// the level it reads
	uint32_t	levels;			// how many it makes after it, 1 to CRT_SPD_LEVELS
	uint32_t	numgroups;		// workgroups, groups[0] by groups[1]
	uint32_t	groupsx;
	uint32_t	width, height;	// the base level's size
	uint32_t	pad[2];
} vid_crt_spd_t;

static_assert (sizeof(vid_crt_spd_t) == 32, "crt_spd's constants are 32 bytes");

// the dispatches that make a texture's mip chain, and their workgroups across
// and down; returns how many
int		VID_CRT_SpdDispatches (int width, int height, vid_crt_spd_t spd[CRT_SPD_MAX_DISPATCHES],
			int groups[CRT_SPD_MAX_DISPATCHES][2]);

// the buffer the workgroups share: the counter of those done, then the level
// 6 after the base, a texel a workgroup (at most 64x64)
#define CRT_SPD_BUFFER_SIZE	(16 + CRT_SPD_TILE * CRT_SPD_TILE * 16)

// a LUT (0 to 3) as 8 bit RGBA, top row first, from Mem_Alloc
byte	*VID_CRT_LoadLUT (int lut, int *width, int *height);
