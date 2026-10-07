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
// r_fog.c -- fog: each pixel blended toward the fog's color by how far along
// the view it is, in linear light. The fog is ezQuake's and FitzQuake's exp2:
// a pixel at depth z keeps exp(-(density / 64 * z)^2) of its light. A map
// gives its fog in its worldspawn's fog key, and r_fog one over it, both as
// FTE's fog command takes it; FTE's alpha and depth bias count as there.
//
// The world, the sky and the models are drawn, then fogged together by the
// depth buffer (R_DrawFog). Translucent surfaces and models are drawn after
// that, each span fogged by its own depth before it is blended in, and
// particles each by theirs; the view model is too near for fog.

#include "r_local.h"
#include "d_local.h"

#define FOG_TABLE		(256 * 10)		// 256 entries a doubling of distance (simd_fog_t)

static cvar_t	r_fog = {.name = "r_fog", .string = "", .archive = true,
	.description = "Fog over the map's, as FTE's fog command takes it: \"density\", \"density grey\", "
		"\"red green blue\", or \"density red green blue\", then alpha (how much it fogs at most, 0 to 1) and "
		"depth bias (where it starts). Colors are 0 to 1. What isn't given is the map's fog's, or thin grey "
		"(density 0.05 for colors alone). 0 turns fog off; empty (the default) leaves the map's."};
static cvar_t	r_fog_usemap = {.name = "r_fog_usemap", .string = "1", .archive = true,
	.description = "Fogs the view as the map says, with its worldspawn's fog and skyfog keys.",
	.values = (const cvar_value_t[]){{"0", "Only r_fog's"}, {"1", "The map's, under r_fog's"}, {0}}};
static cvar_t	r_skyfog = {.name = "r_skyfog", .string = "0.5", .archive = true,
	.description = "How far the sky is blended toward the fog's color where there is fog, 0 to 1; the map's "
		"skyfog key says instead where the map's fog is used."};

typedef struct
{
	float	density;
	float	color[3];		// sRGB, 0 to 1
	float	alpha;			// how much it fogs at most
	float	bias;			// where it starts, in density / 64 units
	float	sky;			// how much it fogs the sky
} fog_t;

bool		r_fogactive;
simd_fog_t	d_fog;

static float	r_fogtable[FOG_TABLE];
static fog_t	r_fogbuilt;			// what r_fogtable and d_fog are made for

/*
===============
R_Fog_f

fog <density> [<red> <green> <blue>]: the level's fog as its game sets it,
FitzQuake's and FTE's command (mods stuff it), in place of the map's until
the next map; under r_fog's, as the map's is
===============
*/
static void R_Fog_f (void)
{
	if (Cmd_Argc () < 2)
	{
		Con_Printf ("fog is \"%s\"\n", r_worldspawn.fog);
		return;
	}
	Q_strncpyz (r_worldspawn.fog, Cmd_Args (), sizeof(r_worldspawn.fog));
}

void R_FogInit (void)
{
	Cvar_RegisterVariable (&r_fog);
	Cvar_RegisterVariable (&r_fog_usemap);
	Cvar_RegisterVariable (&r_skyfog);
	Cmd_AddCommand ("fog", R_Fog_f, "Sets the level's fog, as the game does, in place of the map's until the "
		"next map: density, then red, green and blue from 0 to 1. r_fog's is over it.");
}

/*
===============
R_ParseFog

A fog's values over fog, as FTE's fog command takes them: density; density
and grey; red, green and blue (and density 0.05 if fog has none); density,
red, green and blue; then alpha and depth bias
===============
*/
static void R_ParseFog (const char *s, fog_t *fog)
{
	float	v[6];
	char	*end;
	int		n;

	for (n = 0 ; n < 6 ; n++)
	{
		v[n] = strtof (s, &end);
		if (end == s)
			break;
		s = end;
	}

	switch (n)
	{
	case 0:
		return;
	case 1:
		fog->density = v[0];
		return;
	case 2:
		fog->density = v[0];
		fog->color[0] = fog->color[1] = fog->color[2] = v[1];
		return;
	case 3:
		if (fog->density <= 0)
			fog->density = 0.05f;
		fog->color[0] = v[0];
		fog->color[1] = v[1];
		fog->color[2] = v[2];
		return;
	}
	fog->density = v[0];
	fog->color[0] = v[1];
	fog->color[1] = v[2];
	fog->color[2] = v[3];
	if (n >= 5)
		fog->alpha = v[4];
	if (n >= 6)
		fog->bias = v[5];
}

static float R_Clamp01 (float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/*
===============
R_BuildFog

The table of what a pixel keeps of its light, by its 1/z, from where the fog
is all but all it fogs to where it is all but none, and the fog's color in
the pixels' light
===============
*/
static void R_BuildFog (const fog_t *fog)
{
	double	d = fog->density / 64.0, alpha = R_Clamp01 (fog->alpha), bias = fog->bias > 0 ? fog->bias : 0;
	double	x, light;
	float	zi;
	int32_t	bits;
	int		i, c;

	// the first entry at d * distance - bias 4, where exp(-x^2) is 1e-7; the
	// last past 1/64, where it is 0.9998, 8 doublings of distance nearer
	zi = (float)(d / (bias + 4));
	memcpy (&bits, &zi, sizeof(bits));
	d_fog.table = r_fogtable;
	d_fog.size = FOG_TABLE;
	d_fog.base = bits >> SIMD_FOG_SHIFT;
	for (i = 0 ; i < FOG_TABLE ; i++)
	{
		// the middle of the 1/z the entry is for
		bits = ((d_fog.base + i) << SIMD_FOG_SHIFT) | (1 << (SIMD_FOG_SHIFT - 1));
		memcpy (&zi, &bits, sizeof(zi));
		x = d / zi - bias;
		x = x > 0 ? x : 0;
		r_fogtable[i] = (float)(256.0 * ((1 - alpha) + alpha * exp (-x * x)));
	}
	for (c = 0 ; c < 3 ; c++)
	{
		// a channel is 512 times the fourth root of its light
		light = R_SrgbToLinear (fog->color[c] < 0 ? 0 : fog->color[c] > 2 ? 2 : fog->color[c]);
		d_fog.color[c] = (float)(light * 512.0 * 512.0 * 512.0 * 512.0);
	}
	d_fog.sky = 256.0f * (1 - R_Clamp01 (fog->sky) * (float)alpha);
	r_fogbuilt = *fog;
}

/*
===============
R_SetupFog

The frame's fog: the map's (r_fog_usemap; or the fog command's), with r_fog's
over it
===============
*/
void R_SetupFog (void)
{
	fog_t	fog = {.density = 0, .color = {0.3f, 0.3f, 0.3f}, .alpha = 1, .bias = 0};
	bool	mapfog = r_fog_usemap.value && r_worldspawn.fog[0];

	if (mapfog)
		R_ParseFog (r_worldspawn.fog, &fog);
	if (r_fog.string[0])
		R_ParseFog (r_fog.string, &fog);
	fog.sky = mapfog && !r_fog.string[0] && r_worldspawn.hasskyfog ? r_worldspawn.skyfog : r_skyfog.value;

	r_fogactive = fog.density > 0;
	if (r_fogactive && memcmp (&fog, &r_fogbuilt, sizeof(fog)))
		R_BuildFog (&fog);
}

// a row of the view fogged by the depth buffer
static void R_FogRow (void *ctx, int index)
{
	int		u = r_refdef.vrect.x, v = r_refdef.vrect.y + index;

	(void)ctx;
	simd_fogspan (d_viewbuffer + screenwidth * v + u, d_pzbuffer + d_zwidth * v + u, 0, 0,
		r_refdef.vrect.width, &d_fog);
}

/*
===============
R_DrawFog

What is drawn so far fogged by its depth, and the sky by r_skyfog; its rows
spread over the threads
===============
*/
void R_DrawFog (void)
{
	if (r_fogactive)
		Sys_Parallel (r_refdef.vrect.height, R_FogRow, NULL);
}

// a pixel as its 1/z fogs it
pixel_t R_FogPixel (pixel_t p, float zi)
{
	simd_fogspan (&p, NULL, zi, 0, 1, &d_fog);
	return p;
}
