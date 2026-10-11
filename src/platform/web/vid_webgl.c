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
// vid_webgl.c -- the frame shown in a browser page, with WebGL 2 (vid_webgl.js)
//
// The renderer draws into the page's memory, and a frame shown is copied into
// two integer textures the present shader (present.glsl) reads by texel, as
// the other systems' shaders read the buffers. The copy is made as the frame
// is drawn, so the renderer draws the next into the same buffers at once: no
// frame waits for another, and none is queued. With vsync the page's loop runs
// a frame at each of the display's (sys_web.js), shown at the next refresh;
// without, frames run as they are due, one drawn when the GPU has finished the
// last, on a canvas shown as soon as it is drawn where the browser allows
// (desynchronized), as the other systems present without waiting.
//
// With vid_shader_crt the frame is shown through the CRT shader's passes
// (vid_crt.h), which vid_webgl.js draws, made when it's turned on and freed
// when it's turned off.

#include "args.h"
#include "cmd.h"
#include "mem.h"
#include "print.h"
#include "q_string.h"
#include "sound.h"
#include "sys.h"
#include "vid_common.h"
#include "vid_crt.h"
#include "web_local.h"

#include <string.h>

extern const char	vid_present_glsl[];	// present_glsl.c
extern const char	vid_crt_glsl[], vid_crt_core[], vid_crt_params[];	// crt_glsl.c

// vid_webgl.js
bool	web_vid_init (const char *source, bool desync);
bool	web_vid_recreate (bool desync);
void	web_vid_error (char *buf, int size);
void	web_vid_renderer (char *buf, int size);
bool	web_vid_settextures (int width, int height);
void	web_vid_clientsize (int *width, int *height);
bool	web_vid_present (const pixel_t *view, const hudpixel_t *hud, bool huddirty, unsigned rowpixels,
			const vid_present_constants_t *constants, int x, int y, int width, int height, bool paced,
			const vid_present_constants_t *crtconstants, const byte *crtparams, const vid_crt_pass_t *crtpasses,
			const int *crttable);
bool	web_vid_crt_init (const char *glsl, const char *core, const char *params, const char *names, int mipspass,
			int paramssize);
void	web_vid_crt_texture (int texture, int width, int height, bool half, bool linear, int levels, bool feedback,
			const byte *pixels);
void	web_vid_crt_free (void);
int		web_vid_fullscreen (bool on);
void	web_vid_settitle (const char *text);
void	web_vid_focus (void);
void	web_vid_describe (char *buf, int size);

static bool			vid_initialized;
static pixel_t		*vid_view;			// the layers, drawn into and copied from
static hudpixel_t	*vid_hud;
static int			client_width, client_height;	// the canvas, in device pixels
static bool			vid_desync;			// the canvas is shown as soon as drawn (vid_vsync 0)
static bool			vid_hudpending;		// the 2D changed since it was last copied
static bool			vid_fullscreen;
static char			vid_gpuname[256];

static bool				vid_crton;		// the CRT shader's passes are made
static vid_crt_frame_t	vid_crtframe;
static int				vid_crtsizes[4];	// the source's and viewport's sizes its textures have

// vid_webgl.js draws the frame into the shader's texture 0
static_assert (CRT_ORIGINAL == 0, "CRT_ORIGINAL is vid_webgl.js's first texture");

static void VID_SetScale (void);
static void VID_SetCRT (void);
static bool VID_PresentCRT (const vid_fit_t *fit);

/*
===============================================================================

THE PAGE

===============================================================================
*/

void VID_AppActivate (bool active)
{
	static bool	sound_active;		// sound starts blocked (sys_web_gui.c), until the page is active

	if (active == ActiveApp)
		return;
	ActiveApp = active;

// enable/disable sound on focus gain/loss
	if (!ActiveApp && sound_active)
	{
		S_BlockSound ();
		S_ClearBuffer ();
		sound_active = false;
	}
	else if (ActiveApp && !sound_active)
	{
		S_UnblockSound ();
		S_ClearBuffer ();
		sound_active = true;
	}

	IN_WindowActivated (ActiveApp);
}

// another tab, or the browser minimized: nothing is shown, and the browser
// gives the page no animation frames
void VID_WindowSuspended (bool suspended)
{
	Minimized = suspended;
}

void VID_SetFullscreenState (bool fullscreen)
{
	vid_fullscreen = fullscreen;
}

static void VID_SetFullscreen (bool fullscreen)
{
	int		result = web_vid_fullscreen (fullscreen);

	if (result < 0)
		Con_Printf ("This browser has no fullscreen for a page\n");
	else if (!result)
		Con_Printf ("Fullscreen comes with the next click or key\n");
}

static void VID_Fullscreen_f (void)
{
	VID_SetFullscreen (!vid_fullscreen);
}

void VID_ToggleFullscreen (void)
{
	VID_Fullscreen_f ();
}

/*
===============================================================================

THE FRAME

===============================================================================
*/

static void VID_FreeLayers (void)
{
	Mem_FreeAligned (vid_view);
	Mem_FreeAligned (vid_hud);
	vid_view = NULL;
	vid_hud = NULL;
}

static void VID_SetScale (void)
{
	vid_frame_t	frame = VID_WantedFrame (client_width, client_height);
	size_t		size = (size_t)frame.width * (size_t)frame.height;
	char		why[256];

	if (!web_vid_settextures (frame.width, frame.height))
	{
		web_vid_error (why, sizeof(why));
		Sys_Error ("VID_SetScale: %s", why);
	}
	VID_FreeLayers ();
	vid_view = Mem_AllocAligned (size * sizeof(pixel_t), 64);
	vid_hud = Mem_AllocAligned (size * sizeof(hudpixel_t), 64);
	vid_hudpending = true;
	vid.buffer = vid_view;
	vid.hud = vid_hud;
	VID_SetFrame (&frame, (unsigned)frame.width);
}

// the frame last drawn: the one buffer of each layer
void VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud)
{
	*frame = vid_view;
	*hud = vid_hud;
}

void VID_Update (void)
{
	vid_present_constants_t	constants;
	vid_fit_t				fit;
	char					why[512];
	bool					presented;

	if (!vid_initialized || Minimized)
		return;
	web_vid_clientsize (&client_width, &client_height);
	if (client_width <= 0 || client_height <= 0)
		return;

	// the canvas is shown as soon as drawn without vsync, at the refresh with
	// it: a context made for the one or the other
	if (!vid_vsync.value != vid_desync)
	{
		vid_desync = !vid_vsync.value;
		if (!web_vid_recreate (vid_desync))
		{
			web_vid_error (why, sizeof(why));
			Sys_Error ("VID_Update: the canvas couldn't be made again: %s", why);
		}
		vid_hudpending = true;
	}

	fit = VID_Fit (client_width, client_height);
	// the CRT shader made when it's turned on, freed when it's turned off
	if (VID_CRT_Active () != vid_crton)
		VID_SetCRT ();
	if (vid_crton)
		presented = fit.width >= 1 && fit.height >= 1 && VID_PresentCRT (&fit);
	else
	{
		VID_FillConstants (&constants, &fit, VID_OUTPUT_SDR, 1, 1);
		presented = web_vid_present (vid_view, vid_hud, vid.huddirty || vid_hudpending, vid.rowpixels, &constants,
			(int)fit.x, (int)fit.y, (int)fit.width, (int)fit.height, VID_Vsync (), NULL, NULL, NULL, NULL);
	}
	if (presented)
		vid_hudpending = false;
	else if (vid.huddirty)
		vid_hudpending = true;
	vid.huddirty = false;

	// the canvas was resized, or the video settings changed
	if (VID_NeedsResize (client_width, client_height))
		VID_SetScale ();
}

/*
===============================================================================

THE CRT SHADER

===============================================================================
*/

// the CRT shader's passes made for vid_shader_crt 1, freed for 0; a page that
// can't have them turns it off
static void VID_SetCRT (void)
{
	char	names[256] = "", why[512];
	byte	*pixels;
	int		i, w, h;

	if (vid_crton)
	{
		web_vid_crt_free ();
		vid_crton = false;
		return;
	}
	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		if (i)
			Q_strncatz (names, ",", sizeof(names));
		Q_strncatz (names, vid_crt_passes[i].name, sizeof(names));
	}
	if (!web_vid_crt_init (vid_crt_glsl, vid_crt_core, vid_crt_params, names, CRT_PASS_PREPASS,
			(int)VID_CRT_PARAMS_SIZE))
	{
		web_vid_error (why, sizeof(why));
		Con_Printf ("The CRT shader couldn't be made: %s\n", why);
		Cvar_Set ("vid_shader_crt", "0");
		return;
	}
	for (i = 0 ; i < 4 ; i++)
	{
		pixels = VID_CRT_LoadLUT (i, &w, &h);
		if (!pixels)
		{
			Con_Printf ("The CRT shader: LUT %d is broken\n", i + 1);
			web_vid_crt_free ();
			Cvar_Set ("vid_shader_crt", "0");
			return;
		}
		web_vid_crt_texture (CRT_LUT1 + i, w, h, false, true, 1, false, pixels);
		Mem_Free (pixels);
	}
	memset (vid_crtsizes, 0, sizeof(vid_crtsizes));
	vid_crton = true;
}

// the shader's textures for a frame of source pixels shown in a viewport of view pixels
static void VID_SizeCRT (int sourcewidth, int sourceheight, int viewwidth, int viewheight)
{
	const vid_crt_texture_t	*t;
	int						i, w, h;

	if (vid_crtsizes[0] == sourcewidth && vid_crtsizes[1] == sourceheight && vid_crtsizes[2] == viewwidth
		&& vid_crtsizes[3] == viewheight)
		return;
	for (i = 0 ; i < CRT_TEXTURES ; i++)
	{
		t = &vid_crt_textures[i];
		if (t->size == CRT_SIZE_LUT)
			continue;
		VID_CRT_TextureSize (i, sourcewidth, sourceheight, viewwidth, viewheight, &w, &h);
		web_vid_crt_texture (i, w, h, t->format == CRT_RGBA16F, t->linear, t->mips ? VID_CRT_MipLevels (w, h) : 1,
			t->feedback, NULL);
	}
	vid_crtsizes[0] = sourcewidth;
	vid_crtsizes[1] = sourceheight;
	vid_crtsizes[2] = viewwidth;
	vid_crtsizes[3] = viewheight;
}

/*
================
VID_PresentCRT

The frame through the CRT shader: drawn as present.glsl shows it, at its own
size, then its passes, the last on the canvas at the fit
================
*/
static bool VID_PresentCRT (const vid_fit_t *fit)
{
	static byte				params[VID_CRT_PARAMS_SIZE];
	vid_fit_t				whole = {0, 0, (float)vid.width, (float)vid.height, 1, 1};
	vid_present_constants_t	constants;
	vid_crt_pass_t			passes[CRT_PASSES];
	vid_crt_frame_t			frame = vid_crtframe;
	int						table[CRT_PASSES][8], i, j;
	const vid_crt_pass_info_t	*pass;

	VID_SizeCRT ((int)vid.width, (int)vid.height, (int)fit->width, (int)fit->height);
	VID_FillConstants (&constants, &whole, VID_OUTPUT_SDR, 1, 1);
	VID_CRT_BeginFrame (&frame, Sys_DoubleTime ());
	VID_CRT_FillParams ((vid_crt_params_t *)params);
	VID_CRT_FillPasses (passes, &frame, (int)vid.width, (int)vid.height, (int)fit->width, (int)fit->height,
		VID_OUTPUT_SDR, 1);
	// what each pass draws in and reads this frame
	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		pass = &vid_crt_passes[i];
		table[i][0] = pass->output == CRT_SCREEN ? -1 : VID_CRT_Physical (pass->output, frame.parity);
		for (j = 0 ; j < CRT_MAX_INPUTS ; j++)
			table[i][j + 1] = j < pass->numinputs ? VID_CRT_Physical (pass->inputs[j], frame.parity) : -1;
	}
	if (!web_vid_present (vid_view, vid_hud, vid.huddirty || vid_hudpending, vid.rowpixels, &constants,
			(int)fit->x, (int)fit->y, (int)fit->width, (int)fit->height, VID_Vsync (), &constants, params, passes,
			&table[0][0]))
		return false;
	// the feedback textures change places with the frame drawn
	vid_crtframe = frame;
	return true;
}

/*
===============================================================================

THE REPORT

===============================================================================
*/

static void VID_Info_f (void)
{
	char	text[1024];

	web_vid_describe (text, sizeof(text));
	Con_Printf ("%s\n", text);
}

void VID_Init (void)
{
	char	why[512];

	VID_RegisterCommon ();
	Cmd_AddCommand ("vid_fullscreen", VID_Fullscreen_f,
		"Toggles fullscreen, the browser's, as Alt+Enter does.");
	Cmd_AddCommand ("vid_info", VID_Info_f,
		"Prints the GPU WebGL draws with, the canvas, whether it is shown as soon as drawn, and the frames drawn.");

	vid_desync = !vid_vsync.value;
	if (!web_vid_init (vid_present_glsl, vid_desync))
	{
		web_vid_error (why, sizeof(why));
		Sys_Error ("SoftWorld needs WebGL 2: %s", why);
	}
	web_vid_renderer (vid_gpuname, sizeof(vid_gpuname));
	Con_Printf ("WebGL 2 on %s\n", vid_gpuname);

	IN_StartEvents ();
	web_vid_clientsize (&client_width, &client_height);
	VID_SetScale ();
	vid_initialized = true;

	VID_Update ();
	if (COM_CheckParm ("-fullscreen"))
		VID_SetFullscreen (true);
}

void VID_Shutdown (void)
{
	if (!vid_initialized)
		return;
	vid_initialized = false;
	if (vid_crton)
		web_vid_crt_free ();
	vid_crton = false;
	VID_FreeLayers ();
	vid.buffer = NULL;
	vid.hud = NULL;
}

// the page's title: the text's characters as ASCII, Quake's colored ones as plain
void VID_SetCaption (const char *text)
{
	char	title[256];
	size_t	i;

	for (i = 0 ; text[i] && i < sizeof(title) - 1 ; i++)
		title[i] = (char)((text[i] & 0x7f) >= 32 ? text[i] & 0x7f : ' ');
	title[i] = 0;
	web_vid_settitle (title);
}

void VID_BringToFront (void)
{
	web_vid_focus ();
}

bool VID_IsActive (void)
{
	return ActiveApp;
}

bool VID_IsMinimized (void)
{
	return Minimized;
}

// frames come at the display's pace (requestAnimationFrame): none to give
float VID_RefreshRate (void)
{
	return 0;
}

bool VID_IsFullscreen (void)
{
	return vid_fullscreen;
}

const char *VID_GPUName (void)
{
	return vid_gpuname;
}
