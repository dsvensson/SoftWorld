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
// r_lightdata.c -- the light samples of a map, from the best source it has,
// and how the lightmap of each face lies on its texture.
//
// Every face's lightmap is lmwidth x lmheight luxels a style, and a texture
// coordinate (s, t) falls on luxel lmvecs . (s, t, 1). A vanilla lightmap has
// a luxel every 16 texels from texturemins; LMSHIFT scales that, and a
// DECOUPLED_LM lightmap has its own projection from the world, which is
// folded into one from the texture here. Past loading nothing depends on
// where a lightmap came from.

#include "r_local.h"

static cvar_t	r_loadlit = {.name = "r_loadlit", .string = "1", .archive = true};
static cvar_t	r_lit_normalize = {.name = "r_lit_normalize", .string = "1", .archive = true};

void R_LightDataInit (void)
{
	Cvar_RegisterVariable (&r_loadlit);
	Cvar_RegisterVariable (&r_lit_normalize);
}

//
// samples
//

// RGB8 is 128 for 1.0, E5BGR9 is linear light with 1.0 for the mono 128;
// both become 16 bits a channel, 2048 for 1.0
static void R_DecodeRGB8 (unsigned short *out, const byte *in, int samples)
{
	int		i;

	for (i = 0 ; i < samples * 3 ; i++)
		out[i] = (unsigned short)(in[i] << 4);
}

static void R_DecodeE5BGR9 (unsigned short *out, const byte *in, int samples)
{
	int			i, c;
	unsigned	e5bgr9;
	float		scale, v;

	for (i = 0 ; i < samples ; i++)
	{
		e5bgr9 = (unsigned)in[i*4] | ((unsigned)in[i*4+1] << 8) | ((unsigned)in[i*4+2] << 16) | ((unsigned)in[i*4+3] << 24);
		scale = ldexpf (2048.0f, (int)(e5bgr9 >> 27) - 15 - 9);
		for (c = 0 ; c < 3 ; c++)
		{
			v = ((e5bgr9 >> (9 * c)) & 0x1ff) * scale + 0.5f;
			out[i*3 + c] = (unsigned short)(v > 65535 ? 65535 : v);
		}
	}
}

// the .lit next to a map: maps/<n>.lit, lits/<n>.lit or maps/lits/<n>.lit
static byte *R_LoadLitFile (const char *mapname, int *size)
{
	static const char	*dirs[] = {"maps/", "lits/", "maps/lits/"};
	char		base[MAX_QPATH], path[MAX_QPATH];
	const char	*slash;
	byte		*buf;
	int			i;

	slash = strrchr (mapname, '/');
	Q_strncpyz (base, slash ? slash + 1 : mapname, sizeof(base));
	if (strlen (base) < 4 || Q_strcasecmp (base + strlen (base) - 4, ".bsp"))
		return NULL;
	base[strlen (base) - 4] = 0;

	for (i = 0 ; i < (int)(sizeof(dirs) / sizeof(dirs[0])) ; i++)
	{
		snprintf (path, sizeof(path), "%s%s.lit", dirs[i], base);
		buf = FS_LoadFile (path, size);
		if (buf)
			return buf;
	}
	return NULL;
}

/*
=================
R_LoadLightData

The samples of every lightmap, from the first usable of: a .lit file, BSPX
LIGHTING_E5BGR9, BSPX RGBLIGHTING, the mono lump. A mono copy is kept for
r_lightmode 0, made from the colored samples if the mono lump doesn't match
them (DECOUPLED_LM maps index all samples by their own offsets, and
-novanilla maps have no mono lump).
=================
*/
void R_LoadLightData (model_t *mod, bspfile_t *bsp)
{
	const byte	*mono, *hdr, *rgb8;
	byte		*lit;
	int			monosize, hdrsize, rgb8size, litsize, facesize, numfaces, samples, i, c, m, v;
	bool		decoupled, normalize = false;
	unsigned short	*out;

	mod->lightdata = NULL;
	mod->lightrgb = NULL;
	mod->lightsamples = 0;

	mono = bsp->data + bsp->lumps[LUMP_LIGHTING].fileofs;
	monosize = bsp->lumps[LUMP_LIGHTING].filelen;
	facesize = bsp->version == BSPVERSION_BSP2 ? (int)sizeof(dface_bsp2_t) : (int)sizeof(dface_t);
	numfaces = bsp->lumps[LUMP_FACES].filelen / facesize;
	decoupled = BSP_FindBSPXLump (bsp, "DECOUPLED_LM", &i) && i == numfaces * (int)sizeof(dlminfo_t);

	hdr = BSP_FindBSPXLump (bsp, "LIGHTING_E5BGR9", &hdrsize);
	if (hdr && (!hdrsize || hdrsize % 4 || (!decoupled && monosize && hdrsize != monosize * 4)))
		hdr = NULL;
	rgb8 = BSP_FindBSPXLump (bsp, "RGBLIGHTING", &rgb8size);
	if (rgb8 && (!rgb8size || rgb8size % 3 || (!decoupled && monosize && rgb8size != monosize * 3)))
		rgb8 = NULL;

	// a .lit belongs to the vanilla lightmaps, so it needs the mono lump
	lit = NULL;
	if (r_loadlit.value && !decoupled && monosize)
	{
		lit = R_LoadLitFile (mod->name, &litsize);
		if (lit && (litsize < 8 || memcmp (lit, "QLIT", 4)
			|| !((lit[4] == 1 && !lit[5] && !lit[6] && !lit[7] && litsize == monosize * 3 + 8)
				|| (lit[4] == 1 && lit[5] == 0 && lit[6] == 1 && lit[7] == 0 && litsize == monosize * 4 + 8))))
		{
			Con_Printf ("%s: ignoring a .lit that doesn't match the map\n", mod->name);
			Mem_Free (lit);
			lit = NULL;
		}
	}

	if (lit)
	{
		samples = monosize;
		out = Arena_Alloc (mod->arena, (size_t)samples * 3 * sizeof(*out));
		if (lit[6] == 1)	// version 0x10001: E5BGR9
			R_DecodeE5BGR9 (out, lit + 8, samples);
		else
		{
			R_DecodeRGB8 (out, lit + 8, samples);
			normalize = true;
		}
		Mem_Free (lit);
	}
	else if (hdr)
	{
		samples = hdrsize / 4;
		out = Arena_Alloc (mod->arena, (size_t)samples * 3 * sizeof(*out));
		R_DecodeE5BGR9 (out, hdr, samples);
	}
	else if (rgb8)
	{
		samples = rgb8size / 3;
		out = Arena_Alloc (mod->arena, (size_t)samples * 3 * sizeof(*out));
		R_DecodeRGB8 (out, rgb8, samples);
		normalize = !decoupled && monosize;
	}
	else
	{
		out = NULL;
		samples = monosize;
	}
	if (!samples)
		return;

	mod->lightsamples = samples;
	mod->lightrgb = out;
	mod->lightdata = Arena_Alloc (mod->arena, (size_t)samples);
	if (!out || (monosize == samples && !decoupled))
		memcpy (mod->lightdata, mono, (size_t)samples);	// without color, the mono lump is all there is
	else
	{	// the brightest channel, as mono
		for (i = 0 ; i < samples ; i++)
		{
			m = out[i*3] > out[i*3+1] ? out[i*3] : out[i*3+1];
			m = out[i*3+2] > m ? out[i*3+2] : m;
			m = (m + 8) >> 4;
			mod->lightdata[i] = (byte)(m > 255 ? 255 : m);
		}
	}

	// colored light no brighter than the mono light the map was checksummed
	// with: each sample scaled so its brightest channel is the mono one
	if (normalize && r_lit_normalize.value)
	{
		for (i = 0 ; i < samples ; i++)
		{
			m = out[i*3] > out[i*3+1] ? out[i*3] : out[i*3+1];
			m = out[i*3+2] > m ? out[i*3+2] : m;
			for (c = 0 ; c < 3 ; c++)
			{
				v = m ? out[i*3 + c] * (mod->lightdata[i] << 4) / m : mod->lightdata[i] << 4;
				out[i*3 + c] = (unsigned short)v;
			}
		}
	}
}

//
// faces
//

/*
=================
R_FindFaceLumps

The BSPX lumps with something for every face, if they fit the face count
=================
*/
void R_FindFaceLumps (bspfile_t *bsp, int numfaces, facelumps_t *lumps)
{
	int		size;

	memset (lumps, 0, sizeof(*lumps));
	if (!numfaces)
		return;
	lumps->decoupled = BSP_FindBSPXLump (bsp, "DECOUPLED_LM", &size);
	if (lumps->decoupled && size != numfaces * (int)sizeof(dlminfo_t))
		lumps->decoupled = NULL;
	lumps->shifts = BSP_FindBSPXLump (bsp, "LMSHIFT", &size);
	if (lumps->shifts && size != numfaces)
		lumps->shifts = NULL;
	lumps->offsets = BSP_FindBSPXLump (bsp, "LMOFFSET", &size);
	if (lumps->offsets && size != numfaces * 4)
		lumps->offsets = NULL;
	lumps->styles = BSP_FindBSPXLump (bsp, "LMSTYLE", &size);
	if (lumps->styles && (size % numfaces || size / numfaces < 1))
		lumps->styles = NULL;
	if (lumps->styles)
		lumps->stylesperface = size / numfaces;
}

/*
=================
R_FoldDecoupled

A DECOUPLED_LM projection from the world, as one from the face's texture: the
point with texture coordinate (s, t) on the face's plane solves
[S; T; N] p = (s - S3, t - T3, dist), and its luxel is U . p + U3
=================
*/
static bool R_FoldDecoupled (msurface_t *surf, const float world[2][4])
{
	const float	*S = surf->texinfo->vecs[0], *T = surf->texinfo->vecs[1];
	const float	*N = surf->plane->normal;
	double		col[3][3], det, u[3];
	int			i, r;

	// the columns of the inverse of the matrix with rows S, T and N
	col[0][0] = (double)T[1]*N[2] - (double)T[2]*N[1];
	col[0][1] = (double)T[2]*N[0] - (double)T[0]*N[2];
	col[0][2] = (double)T[0]*N[1] - (double)T[1]*N[0];
	col[1][0] = (double)N[1]*S[2] - (double)N[2]*S[1];
	col[1][1] = (double)N[2]*S[0] - (double)N[0]*S[2];
	col[1][2] = (double)N[0]*S[1] - (double)N[1]*S[0];
	col[2][0] = (double)S[1]*T[2] - (double)S[2]*T[1];
	col[2][1] = (double)S[2]*T[0] - (double)S[0]*T[2];
	col[2][2] = (double)S[0]*T[1] - (double)S[1]*T[0];
	det = S[0]*col[0][0] + S[1]*col[0][1] + S[2]*col[0][2];
	if (fabs (det) < 1e-12)
		return false;

	for (r = 0 ; r < 2 ; r++)
	{
		for (i = 0 ; i < 3 ; i++)
			u[i] = (world[r][0]*col[i][0] + world[r][1]*col[i][1] + world[r][2]*col[i][2]) / det;
		surf->lmvecs[r][0] = (float)u[0];
		surf->lmvecs[r][1] = (float)u[1];
		surf->lmvecs[r][2] = (float)(u[2]*surf->plane->dist - u[0]*S[3] - u[1]*T[3] + world[r][3]);
	}
	return true;
}

/*
=================
R_SetFaceLightmap

The lightmap of face facenum, whose texture coordinates run from texmins to
texmaxs
=================
*/
void R_SetFaceLightmap (model_t *mod, msurface_t *surf, const bspface_t *face, const facelumps_t *lumps,
	int facenum, const double texmins[2], const double texmaxs[2])
{
	dlminfo_t	lm;
	int			i, lightofs, numstyles, shift, lo[2], hi[2];
	float		world[2][4];
	double		luxels, spacing;
	int64_t		need;

	surf->samples = NULL;
	surf->samples_rgb = NULL;
	surf->lmvanilla = true;
	surf->lmgridshift = 4;
	surf->lmwidth = (surf->extents[0] >> 4) + 1;
	surf->lmheight = (surf->extents[1] >> 4) + 1;
	surf->lmvecs[0][0] = 1.0f / 16;
	surf->lmvecs[0][1] = 0;
	surf->lmvecs[0][2] = -surf->texturemins[0] / 16.0f;
	surf->lmvecs[1][0] = 0;
	surf->lmvecs[1][1] = 1.0f / 16;
	surf->lmvecs[1][2] = -surf->texturemins[1] / 16.0f;

	// styles, from LMSTYLE if the map has it; 255 ends them
	for (i = 0 ; i < MAXLIGHTMAPS ; i++)
	{
		if (lumps->styles)
			surf->styles[i] = i < lumps->stylesperface ? lumps->styles[facenum * lumps->stylesperface + i] : 255;
		else
			surf->styles[i] = face->styles[i];
	}
	for (numstyles = 0 ; numstyles < MAXLIGHTMAPS && surf->styles[numstyles] != 255 ; numstyles++)
		;

	lightofs = face->lightofs;
	if (lumps->offsets)
	{
		memcpy (&lightofs, lumps->offsets + facenum * 4, 4);
		lightofs = LittleLong (lightofs);
	}

	if (lumps->decoupled)
	{
		memcpy (&lm, lumps->decoupled + facenum * sizeof(lm), sizeof(lm));
		lightofs = LittleLong (lm.lightofs);
		for (i = 0 ; i < 4 ; i++)
		{
			world[0][i] = LittleFloat (lm.vecs[0][i]);
			world[1][i] = LittleFloat (lm.vecs[1][i]);
		}
		// a face without a size keeps its offset but lies on the texture
		if (LittleShort ((short)lm.lmwidth) && LittleShort ((short)lm.lmheight))
		{
			if (!R_FoldDecoupled (surf, world))
				return;
			surf->lmvanilla = false;
			surf->lmwidth = (unsigned short)LittleShort ((short)lm.lmwidth);
			surf->lmheight = (unsigned short)LittleShort ((short)lm.lmheight);
			// sampled every luxel or finer, from a texel up to 16
			luxels = sqrt ((double)surf->lmvecs[0][0]*surf->lmvecs[0][0] + (double)surf->lmvecs[1][0]*surf->lmvecs[1][0]);
			spacing = sqrt ((double)surf->lmvecs[0][1]*surf->lmvecs[0][1] + (double)surf->lmvecs[1][1]*surf->lmvecs[1][1]);
			luxels = luxels > spacing ? luxels : spacing;	// per texel, the most in any direction
			spacing = luxels > 0 ? 1 / luxels : 16;
			for (shift = 0 ; shift < 4 && (2 << shift) <= spacing ; shift++)
				;
			surf->lmgridshift = shift;
		}
	}
	else if (lumps->shifts && lumps->shifts[facenum] != 4)
	{
		// a luxel every 1 << shift texels, from its own texturemins
		shift = lumps->shifts[facenum];
		if (shift > 15)
			return;
		for (i = 0 ; i < 2 ; i++)
		{
			lo[i] = (int)floor (texmins[i] / (1 << shift));
			hi[i] = (int)ceil (texmaxs[i] / (1 << shift));
		}
		surf->lmvanilla = false;
		surf->lmwidth = hi[0] - lo[0] + 1;
		surf->lmheight = hi[1] - lo[1] + 1;
		surf->lmvecs[0][0] = 1.0f / (1 << shift);
		surf->lmvecs[0][2] = (float)-lo[0];
		surf->lmvecs[1][1] = 1.0f / (1 << shift);
		surf->lmvecs[1][2] = (float)-lo[1];
		surf->lmgridshift = shift < 4 ? shift : 4;
	}

	// the samples of every style must be there
	if (lightofs < 0 || !mod->lightdata || surf->lmwidth < 1 || surf->lmheight < 1
		|| surf->lmwidth > 4096 || surf->lmheight > 4096)
		return;
	need = (int64_t)surf->lmwidth * surf->lmheight * numstyles;
	if (lightofs > mod->lightsamples || need > mod->lightsamples - lightofs)
		return;
	surf->samples = mod->lightdata + lightofs;
	surf->samples_rgb = mod->lightrgb ? mod->lightrgb + (size_t)lightofs * 3 : NULL;
}
