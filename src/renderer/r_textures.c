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
// r_textures.c -- TGA files in place of a map's textures, as ezQuake and FTE
// take them: textures/<map>/<name>.tga, else textures/<name>.tga, a '*' in
// the name a '#' (textures/bmodels/ in place of the map's directory for the
// brush models of files other than maps). Each frame of an animation is a
// texture of its own.
//
// A file is scaled to the texture's size, averaging in linear light, and
// kept with the map's texture: the surfaces are lit from it in r_lightmode 1
// (the colormap of r_lightmode 0 needs palette indices), and liquids drawn
// from it in either. A fence's texels less than half opaque are cut out. The
// fullbright light is <name>_luma.tga's or <name>_glow.tga's, else the
// file's texels where the map's texture has fullbright colors, and as with
// those a texel is lit to no less than it times r_fullbright_scale; a
// liquid's, which light doesn't reach, is drawn no darker than that.

#include "r_local.h"

#define LIQUID_SIZE		64		// the turbulent span drawers' texture

cvar_t	r_externaltextures = {.name = "r_externaltextures", .string = "1", .archive = true,
	.description = "Draws textures from TGA files in place of the map's own, where there are any: "
		"textures/<map>/<name>.tga or textures/<name>.tga, with the fullbright light of <name>_luma.tga or "
		"<name>_glow.tga. They are read as the map loads; walls are drawn with them in r_lightmode 1.",
	.values = (const cvar_value_t[]){{"0", "The map's own"}, {"1", "The files'"}, {0}}};

void R_TexturesInit (void)
{
	Cvar_RegisterVariable (&r_externaltextures);
}

// the directory of a model's textures in textures/: a map's name, or bmodels
// for another brush model file (b_*.bsp, progs/*.bsp)
static void R_TextureDir (const char *modelname, char *dir, size_t size)
{
	const char	*base = strrchr (modelname, '/');
	size_t		len;

	base = base ? base + 1 : modelname;
	len = strlen (base);
	if (Q_strncasecmp (modelname, "maps/", 5) || len < 4 || Q_strcasecmp (base + len - 4, ".bsp")
		|| !Q_strncasecmp (base, "b_", 2))
	{
		Q_strncpyz (dir, "bmodels", size);
		return;
	}
	Q_strncpyz (dir, base, size);
	if (len - 4 < size)
		dir[len - 4] = 0;
}

// textures/<dir>/<name><suffix>.tga, else textures/<name><suffix>.tga
static byte *R_FindTexture (const char *dir, const char *name, const char *suffix, int *w, int *h)
{
	char	file[64], path[MAX_QPATH];
	byte	*rgba;
	int		i;

	Q_strncpyz (file, name, sizeof(file));
	for (i = 0 ; file[i] ; i++)
		if (file[i] == '*')
			file[i] = '#';

	snprintf (path, sizeof(path), "textures/%s/%s%s.tga", dir, file, suffix);
	rgba = R_LoadTGA (path, w, h);
	if (rgba)
		return rgba;
	snprintf (path, sizeof(path), "textures/%s%s.tga", file, suffix);
	return R_LoadTGA (path, w, h);
}

// mip levels 1 to 3 from level 0, each texel the average of four; in a fence
// a block half cut out or more stays cut out, and the rest is the average of
// the texels that aren't, as R_BuildMips makes them
static void R_BuildPixelMips (pixel_t *mips[MIPLEVELS], int width, int height, bool fence)
{
	int			mip, x, y, i, w, h, n;
	pixel_t		block[4], texel;
	const pixel_t	*src;

	for (mip = 1 ; mip < MIPLEVELS ; mip++)
	{
		src = mips[mip - 1];
		w = width >> mip;
		h = height >> mip;
		for (y = 0 ; y < h ; y++)
			for (x = 0 ; x < w ; x++)
			{
				n = 0;
				for (i = 0 ; i < 4 ; i++)
				{
					texel = src[(y*2 + (i >> 1)) * w*2 + x*2 + (i & 1)];
					if (!fence || !(texel & PIXEL_TRANSPARENT))
						block[n++] = texel;
				}
				if (fence && n <= 2)
					mips[mip][y * w + x] = R_AveragePixels (block, n) | PIXEL_TRANSPARENT;
				else
					mips[mip][y * w + x] = R_AveragePixels (block, n);
			}
	}
}

// room for four mip levels of a texture's size, from the model's arena
static void R_AllocMips (pixel_t *mips[MIPLEVELS], int width, int height, struct arena_s *arena)
{
	pixel_t	*p = Arena_Alloc (arena, (size_t)width * height / 64 * 85 * sizeof(pixel_t));
	int		mip;

	for (mip = 0 ; mip < MIPLEVELS ; mip++)
	{
		mips[mip] = p;
		p += (width >> mip) * (height >> mip);
	}
}

/*
===============
R_BuildTexturePixels

The map's texels of a wall's texture as pixels (texture_t pixels), each
fullbright one in pixelglow too, and their mip levels averaged in linear light
===============
*/
void R_BuildTexturePixels (texture_t *tx, struct arena_s *arena)
{
	const byte	*index = (const byte *)tx + tx->offsets[0];
	int			i, width = (int)tx->width, height = (int)tx->height;
	bool		fence = tx->name[0] == '{', bright = false;

	if (tx->name[0] == '*' || !Q_strncmp (tx->name, "sky", 3))
		return;		// drawn from their bytes

	R_AllocMips (tx->pixels, width, height, arena);
	for (i = 0 ; i < width * height ; i++)
	{
		if (fence && index[i] == 255)
			tx->pixels[0][i] = d_pal30[255] | PIXEL_TRANSPARENT;
		else
		{
			tx->pixels[0][i] = d_pal30[index[i]];
			bright |= d_fullbright[index[i]];
		}
	}
	R_BuildPixelMips (tx->pixels, width, height, fence);
	if (!bright)
		return;

	R_AllocMips (tx->pixelglow, width, height, arena);
	for (i = 0 ; i < width * height ; i++)
		tx->pixelglow[0][i] = d_fullbright[index[i]] && !(fence && index[i] == 255) ? d_pal30[index[i]] : 0;
	R_BuildPixelMips (tx->pixelglow, width, height, false);
}

/*
===============
R_LightLiquid

A liquid's texels as drawn (rgb[0]) from its file's (rgb[1]): in r_lightmode 1
its fullbright light (glow[0]) times r_fullbright_scale is their floor, as the
palette's fullbrights have theirs (d_pal30_unlit)
===============
*/
static void R_LightLiquid (texture_t *tx)
{
	int		i;
	pixel_t	g;

	if (tx->name[0] != '*' || !tx->rgb[1])
		return;
	for (i = 0 ; i < LIQUID_SIZE * LIQUID_SIZE ; i++)
	{
		if (!d_unlitfloors || !tx->glow[0] || !(g = tx->glow[0][i]))
		{
			tx->rgb[0][i] = tx->rgb[1][i];
			continue;
		}
		tx->rgb[0][i] = R_LitColor (tx->rgb[1][i], R_GlowPixel (g), LIGHT_ONE, LIGHT_ONE, LIGHT_ONE);
	}
}

// the liquids loaded, after r_fullbright_scale or r_lightmode changed
void R_LightLiquids (void)
{
	Mod_ForEachTexture (R_LightLiquid);
}

/*
===============
R_LoadLiquidOverride

A liquid's TGA file (rgba, freed) at 64x64, the turbulent span drawers' size,
in tx->rgb[1], and its fullbright light as a wall's is (<name>_luma.tga's or
<name>_glow.tga's, else the file's texels where the map's texture has
fullbright colors) in tx->glow[0]; tx->rgb[0] is drawn from them
===============
*/
static void R_LoadLiquidOverride (texture_t *tx, const char *dir, byte *rgba, int w, int h, struct arena_s *arena)
{
	const byte	*index = (const byte *)tx + tx->offsets[0];
	int			x, y, width = (int)tx->width, height = (int)tx->height;
	bool		bright = false;
	pixel_t		*texel;

	tx->rgb[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
	tx->rgb[1] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
	R_ImagePixels (rgba, w, h, tx->rgb[1], LIQUID_SIZE, LIQUID_SIZE, false);
	Mem_Free (rgba);

	rgba = R_FindTexture (dir, tx->name, "_luma", &w, &h);
	if (!rgba)
		rgba = R_FindTexture (dir, tx->name, "_glow", &w, &h);
	if (rgba)
	{
		tx->glow[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
		R_ImagePixels (rgba, w, h, tx->glow[0], LIQUID_SIZE, LIQUID_SIZE, false);
		Mem_Free (rgba);
	}
	else
	{
		// the map's texel under each of the file's
		for (y = 0 ; y < LIQUID_SIZE && !bright ; y++)
			for (x = 0 ; x < LIQUID_SIZE && !bright ; x++)
				bright = d_fullbright[index[y * height / LIQUID_SIZE * width + x * width / LIQUID_SIZE]];
		if (bright)
		{
			tx->glow[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
			for (y = 0 ; y < LIQUID_SIZE ; y++)
				for (x = 0 ; x < LIQUID_SIZE ; x++)
				{
					texel = &tx->glow[0][y * LIQUID_SIZE + x];
					*texel = d_fullbright[index[y * height / LIQUID_SIZE * width + x * width / LIQUID_SIZE]]
						? tx->rgb[1][y * LIQUID_SIZE + x] : 0;
				}
		}
	}
	R_LightLiquid (tx);
}

/*
===============
R_LoadTextureOverride

A texture's TGA file, if there is one, into tx->rgb and its fullbright light
into tx->glow; modelname names the model it is of, whose arena owns them
===============
*/
void R_LoadTextureOverride (texture_t *tx, const char *modelname, struct arena_s *arena)
{
	char		dir[MAX_QPATH];
	byte		*rgba;
	const byte	*index;
	int			w, h, i, width = (int)tx->width, height = (int)tx->height;
	bool		fence = tx->name[0] == '{', bright;

	if (!Q_strncmp (tx->name, "sky", 3))
		return;		// a skybox takes the sky's place
	R_TextureDir (modelname, dir, sizeof(dir));
	rgba = R_FindTexture (dir, tx->name, "", &w, &h);
	if (!rgba)
		return;

	if (tx->name[0] == '*')
	{
		R_LoadLiquidOverride (tx, dir, rgba, w, h, arena);
		return;
	}

	R_AllocMips (tx->rgb, width, height, arena);
	R_ImagePixels (rgba, w, h, tx->rgb[0], width, height, fence);
	Mem_Free (rgba);
	R_BuildPixelMips (tx->rgb, width, height, fence);

	rgba = R_FindTexture (dir, tx->name, "_luma", &w, &h);
	if (!rgba)
		rgba = R_FindTexture (dir, tx->name, "_glow", &w, &h);
	if (rgba)
	{
		R_AllocMips (tx->glow, width, height, arena);
		R_ImagePixels (rgba, w, h, tx->glow[0], width, height, false);
		Mem_Free (rgba);
	}
	else
	{
		// the file's colors where the map's are fullbright
		index = (const byte *)tx + tx->offsets[0];
		bright = false;
		for (i = 0 ; i < width * height && !bright ; i++)
			bright = d_fullbright[index[i]] && !(fence && index[i] == 255);
		if (!bright)
			return;
		R_AllocMips (tx->glow, width, height, arena);
		for (i = 0 ; i < width * height ; i++)
			if (d_fullbright[index[i]] && !(fence && index[i] == 255))
				tx->glow[0][i] = tx->rgb[0][i] & ~PIXEL_TRANSPARENT;
	}
	R_BuildPixelMips (tx->glow, width, height, false);
}
