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
//
// r_max_size halves the world's textures as ezQuake's gl_max_size does,
// averaging in linear light, and r_saturation_world takes their color away;
// they are drawn as a file's are, and lit through a colormap as gray in
// r_lightmode 0.

#include "r_local.h"

#define LIQUID_SIZE		64		// the turbulent span drawers' texture

cvar_t	r_externaltextures = {.name = "r_externaltextures", .string = "1", .archive = true,
	.description = "Draws textures from TGA files in place of the map's own, where there are any: "
		"textures/<map>/<name>.tga or textures/<name>.tga, with the fullbright light of <name>_luma.tga or "
		"<name>_glow.tga. They are read as the map loads; walls are drawn with them in r_lightmode 1.",
	.values = (const cvar_value_t[]){{"0", "The map's own"}, {"1", "The files'"}, {0}}};

static cvar_t	r_max_size = {.name = "r_max_size", .string = "0", .archive = true, .min = 0, .max = 4096,
	.description = "The most texels a side the world's textures are drawn with, its doors' and lifts' too, as "
		"ezQuake's gl_max_size: a larger one is halved until it fits, each texel the average of four in linear "
		"light, so 1 draws a wall in its texture's average color. Walls take it in r_lightmode 1, liquids in "
		"either; fences, the sky, other brush models, models and attract mode are drawn as they are. 0 is no limit."};

static cvar_t	r_saturation_world = {.name = "r_saturation_world", .string = "1", .archive = true, .min = 0, .max = 1,
	.description = "How much of their color the world's textures keep, its doors' and lifts' too, its liquids' and "
		"fences': 1 all of it, 0 none, a gray world under whatever color its light has (r_saturation_lightmaps). "
		"The sky, other brush models, models and attract mode keep theirs."};

// d_cm30 with r_saturation_world's color: the colormap the world's textures
// are lit through in r_lightmode 0
const pixel_t	*r_worldcolormap = d_cm30;
static pixel_t	r_graycolormap[VID_GRADES * 256];
static float	r_colormapgray;		// 1 - the r_saturation_world it has

void R_TexturesInit (void)
{
	Cvar_RegisterVariable (&r_externaltextures);
	Cvar_RegisterVariable (&r_max_size);
	Cvar_RegisterVariable (&r_saturation_world);
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

	if (Q_snprintfz (path, sizeof(path), "textures/%s/%s%s.tga", dir, file, suffix))
	{
		rgba = R_LoadTGA (path, w, h);
		if (rgba)
			return rgba;
	}
	if (!Q_snprintfz (path, sizeof(path), "textures/%s%s.tga", file, suffix))
		return NULL;
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
R_BuildLiquidPixels

A liquid's texels as pixels, 64x64 as the turbulent span drawers read them,
each fullbright one in pixelglow too: what a lit liquid is drawn from in
r_lightmode 1
===============
*/
static void R_BuildLiquidPixels (texture_t *tx, struct arena_s *arena)
{
	const byte	*index = (const byte *)tx + tx->offsets[0];
	int			x, y, width = (int)tx->width, height = (int)tx->height;
	bool		bright = false;
	byte		texel;

	tx->pixels[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
	for (y = 0 ; y < LIQUID_SIZE ; y++)
		for (x = 0 ; x < LIQUID_SIZE ; x++)
		{
			texel = index[y * height / LIQUID_SIZE * width + x * width / LIQUID_SIZE];
			tx->pixels[0][y * LIQUID_SIZE + x] = d_pal30[texel];
			bright |= d_fullbright[texel];
		}
	if (!bright)
		return;

	tx->pixelglow[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
	for (y = 0 ; y < LIQUID_SIZE ; y++)
		for (x = 0 ; x < LIQUID_SIZE ; x++)
		{
			texel = index[y * height / LIQUID_SIZE * width + x * width / LIQUID_SIZE];
			tx->pixelglow[0][y * LIQUID_SIZE + x] = d_fullbright[texel] ? d_pal30[texel] : 0;
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

	if (!Q_strncmp (tx->name, "sky", 3))
		return;		// drawn from its bytes
	if (tx->name[0] == '*')
	{
		R_BuildLiquidPixels (tx, arena);
		return;
	}

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
palette's fullbrights have theirs (d_pal30_unlit); the world's custom ones the
same
===============
*/
static void R_LightLiquidTexels (pixel_t *drawn, const pixel_t *texels, const pixel_t *glow)
{
	int		i;
	pixel_t	g;

	for (i = 0 ; i < LIQUID_SIZE * LIQUID_SIZE ; i++)
	{
		if (!d_unlitfloors || !glow || !(g = glow[i]))
		{
			drawn[i] = texels[i];
			continue;
		}
		drawn[i] = R_LitColor (texels[i], R_GlowPixel (g), LIGHT_ONE, LIGHT_ONE, LIGHT_ONE);
	}
}

static void R_LightLiquid (texture_t *tx)
{
	if (tx->name[0] != '*')
		return;
	if (tx->rgb[1])
		R_LightLiquidTexels (tx->rgb[0], tx->rgb[1], tx->glow[0]);
	if (tx->customrgb[1])
		R_LightLiquidTexels (tx->customrgb[0], tx->customrgb[1], tx->customglow[0]);
}

// the liquids loaded, after r_fullbright_scale or r_lightmode changed
void R_LightLiquids (void)
{
	Mod_ForEachTexture (R_LightLiquid);
}

// a model's liquids, for the light as it is now (a model loaded on a loader's
// thread, as the light changed: Mod_Install)
void R_LightModelLiquids (model_t *mod)
{
	int		i;

	for (i = 0 ; i < mod->numtextures ; i++)
		if (mod->textures[i])
			R_LightLiquid (mod->textures[i]);
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

/*
==============================================================================

r_max_size: the world's textures halved as ezQuake's gl_max_size halves them

==============================================================================
*/

/*
===============
R_HalvePixels

An image of pixels (vid.h) halved in place each way it is more than a texel,
as ezQuake's Image_MipReduce halves a texture: each pixel the average of the
four or two it covers, in linear light as the map's own mip levels are
(R_AveragePixels), and an odd last column or row left out
===============
*/
static void R_HalvePixels (pixel_t *pix, int *width, int *height)
{
	int				w = *width, h = *height, nw = w > 1 ? w >> 1 : 1, nh = h > 1 ? h >> 1 : 1;
	int				x, y, right = w > 1 ? 1 : 0, below = h > 1 ? w : 0;
	pixel_t			block[4], *out = pix;
	const pixel_t	*in;

	for (y = 0 ; y < nh ; y++)
	{
		in = pix + (size_t)(below ? y * 2 : y) * w;
		for (x = 0 ; x < nw ; x++, in += right ? 2 : 1)
		{
			block[0] = in[0];
			if (right && below)
			{
				block[1] = in[1];
				block[2] = in[w];
				block[3] = in[w + 1];
				*out++ = R_AveragePixels (block, 4);
			}
			else
			{
				block[1] = in[right + below];
				*out++ = R_AveragePixels (block, 2);
			}
		}
	}
	*width = nw;
	*height = nh;
}

// halved until it is no larger than maxw x maxh
static void R_HalveTo (pixel_t *pix, int *w, int *h, int maxw, int maxh)
{
	while (*w > maxw || *h > maxh)
		R_HalvePixels (pix, w, h);
}

/*
===============
R_IndexImages

A map's texels as pixels, as ezQuake uploads them (R_Upload8): the palette's
colors, 255 black as its d_8to24table has it; and with fullbright colors its
fullbright mask, those colors (255 its own) and the rest black, else glow
NULL. Both from Mem_Alloc.
===============
*/
static void R_IndexImages (const byte *index, int n, pixel_t **pix, pixel_t **glow)
{
	int		i;
	bool	bright = false;

	*pix = Mem_Alloc ((size_t)n * sizeof(pixel_t));
	for (i = 0 ; i < n ; i++)
	{
		(*pix)[i] = index[i] == 255 ? 0 : d_pal30[index[i]];
		bright |= d_fullbright[index[i]];
	}

	*glow = NULL;
	if (!bright)
		return;
	*glow = Mem_Alloc ((size_t)n * sizeof(pixel_t));
	for (i = 0 ; i < n ; i++)
		(*glow)[i] = d_fullbright[index[i]] ? d_pal30[index[i]] : 0;
}

// an RGBA image (freed) as pixels from Mem_Alloc, alpha left out
static pixel_t *R_RGBAPixels (byte *rgba, int n)
{
	pixel_t	*pix = Mem_Alloc ((size_t)n * sizeof(*pix));
	int		i;

	for (i = 0 ; i < n ; i++)
		pix[i] = R_RGBA8Pixel (rgba + (size_t)i * 4);
	Mem_Free (rgba);
	return pix;
}

/*
===============
R_FileImages

A texture's TGA file as pixels at its own size, and its fullbright light as
R_LoadTextureOverride finds it (<name>_luma.tga's or <name>_glow.tga's, else
the file's texels where the map's under them are fullbright; glow NULL if
none), both from Mem_Alloc; false if the file can't be read
===============
*/
static bool R_FileImages (const texture_t *tx, const char *dir, pixel_t **pix, int *w, int *h,
	pixel_t **glow, int *gw, int *gh)
{
	const byte	*index = (const byte *)tx + tx->offsets[0];
	int			x, y, i, width = (int)tx->width, height = (int)tx->height;
	bool		bright = false, under;
	byte		*rgba;

	rgba = R_FindTexture (dir, tx->name, "", w, h);
	if (!rgba)
		return false;
	*pix = R_RGBAPixels (rgba, *w * *h);
	rgba = R_FindTexture (dir, tx->name, "_luma", gw, gh);
	if (!rgba)
		rgba = R_FindTexture (dir, tx->name, "_glow", gw, gh);
	if (rgba)
	{
		*glow = R_RGBAPixels (rgba, *gw * *gh);
		return true;
	}

	*glow = Mem_Alloc ((size_t)*w * *h * sizeof(pixel_t));
	*gw = *w;
	*gh = *h;
	for (y = 0 ; y < *h ; y++)
		for (x = 0 ; x < *w ; x++)
		{
			i = y * *w + x;
			under = d_fullbright[index[y * height / *h * width + x * width / *w]];
			(*glow)[i] = under ? (*pix)[i] : 0;
			bright |= under;
		}
	if (!bright)
	{
		Mem_Free (*glow);
		*glow = NULL;
	}
	return true;
}

/*
===============
R_SmallTexels

An image as r_max_size has it, halved until no side is larger than size;
then mips levels of texels from it, outw x outh the first: each the image's
texel it falls in, and a level smaller than the image from it halved again,
as ezQuake's levels are made. The image is halved in place.
===============
*/
static void R_SmallTexels (pixel_t *pix, int w, int h, int size, pixel_t **out, int mips, int outw, int outh)
{
	int		mip, x, y, tw, th;

	R_HalveTo (pix, &w, &h, size, size);
	for (mip = 0 ; mip < mips ; mip++)
	{
		tw = outw >> mip;
		th = outh >> mip;
		R_HalveTo (pix, &w, &h, tw, th);
		for (y = 0 ; y < th ; y++)
			for (x = 0 ; x < tw ; x++)
				out[mip][y * tw + x] = pix[y * h / th * w + x * w / tw];
	}
}

/*
===============
R_CustomTexture

One of the world's textures as r_max_size and r_saturation_world have it
(tx->custom): its TGA file's texels where files has them drawn and it has one,
else the map's, halved until no side is larger than size, then gray of the
way to the gray of their luminance. As it is drawn without them where it is no
larger than size already, or size is 0, and gray is 0; the sky is drawn as it
is, and fences aren't halved, which ezQuake leaves as they are
(gl_scaleAlphaTextures 0).
===============
*/
static void R_CustomTexture (texture_t *tx, const char *dir, int size, bool files, float gray,
	struct arena_s *arena)
{
	int		width = (int)tx->width, height = (int)tx->height, w, h, gw, gh, mip, mips, outw, outh, i, n;
	bool	liquid = tx->name[0] == '*', halve, file;
	pixel_t	*pix, *glow, **out;

	tx->custom = false;
	halve = size && (width > size || height > size) && tx->name[0] != '{';
	if ((!halve && gray <= 0) || !Q_strncmp (tx->name, "sky", 3))
		return;

	// laid out as rgb and glow: a wall's four mip levels, a liquid's 64x64
	// texels at customrgb[1], lit into customrgb[0]
	mips = liquid ? 1 : MIPLEVELS;
	outw = liquid ? LIQUID_SIZE : width;
	outh = liquid ? LIQUID_SIZE : height;
	if (!tx->customrgb[0])
	{
		if (liquid)
		{
			tx->customrgb[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
			tx->customrgb[1] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
		}
		else
			R_AllocMips (tx->customrgb, width, height, arena);
	}
	out = liquid ? tx->customrgb + 1 : tx->customrgb;

	// halved from the texels at their size, or as they are drawn without
	file = files && tx->rgb[0];
	pix = glow = NULL;
	gw = gh = 0;
	if (halve)
	{
		if (!file || !R_FileImages (tx, dir, &pix, &w, &h, &glow, &gw, &gh))
		{
			R_IndexImages ((const byte *)tx + tx->offsets[0], width * height, &pix, &glow);
			w = gw = width;
			h = gh = height;
		}
		R_SmallTexels (pix, w, h, size, out, mips, outw, outh);
		Mem_Free (pix);
	}
	else
	{
		for (mip = 0 ; mip < mips ; mip++)
			memcpy (out[mip], !file ? tx->pixels[mip] : tx->rgb[mip + liquid],
				(size_t)(outw >> mip) * (outh >> mip) * sizeof(pixel_t));
		glow = file ? tx->glow[0] : tx->pixelglow[0];
	}

	// their fullbright light the same, all 0 where there is none
	if (glow && !tx->customglow[0])
	{
		if (liquid)
			tx->customglow[0] = Arena_Alloc (arena, LIQUID_SIZE * LIQUID_SIZE * sizeof(pixel_t));
		else
			R_AllocMips (tx->customglow, width, height, arena);
	}
	for (mip = 0 ; mip < mips && tx->customglow[0] ; mip++)
	{
		n = (outw >> mip) * (outh >> mip);
		if (!glow)
			memset (tx->customglow[mip], 0, (size_t)n * sizeof(pixel_t));
		else if (!halve)
			memcpy (tx->customglow[mip], file ? tx->glow[mip] : tx->pixelglow[mip], (size_t)n * sizeof(pixel_t));
	}
	if (glow && halve)
	{
		R_SmallTexels (glow, gw, gh, size, tx->customglow, mips, outw, outh);
		Mem_Free (glow);
	}

	if (gray > 0)
		for (mip = 0 ; mip < mips ; mip++)
		{
			n = (outw >> mip) * (outh >> mip);
			for (i = 0 ; i < n ; i++)
				out[mip][i] = R_SaturatePixel (out[mip][i], 1 - gray);
			for (i = 0 ; i < n && tx->customglow[0] ; i++)
				tx->customglow[mip][i] = R_SaturatePixel (tx->customglow[mip][i], 1 - gray);
		}
	if (liquid)
		R_LightLiquidTexels (tx->customrgb[0], tx->customrgb[1], tx->customglow[0]);
	tx->custom = true;
}

/*
===============
R_CheckWorldTextures

The world's textures, its doors' and lifts' with them, as r_max_size and
r_saturation_world have them, again whenever they or r_externaltextures
change, as ezQuake reloads its textures for gl_max_size. Other brush models'
are drawn as they are, as ezQuake's gl_scaleModelTextures 0 leaves them.
===============
*/
void R_CheckWorldTextures (void)
{
	model_t	*world = r_scene.worldmodel;
	int		size = (int)r_max_size.value, i;
	float	gray = 1 - r_saturation_world.value;
	bool	files = r_externaltextures.value != 0;
	char	dir[MAX_QPATH];

	if (size < 0)
		size = 0;
	if (gray != r_colormapgray)
	{
		r_colormapgray = gray;
		for (i = 0 ; i < VID_GRADES * 256 && gray > 0 ; i++)
			r_graycolormap[i] = R_SaturatePixel (d_cm30[i], 1 - gray);
		r_worldcolormap = gray > 0 ? r_graycolormap : d_cm30;
		D_FlushCaches ();
	}

	if (!world || world->type != mod_brush || (world->texmaxsize == size && world->texgray == gray
		&& ((!size && gray <= 0) || world->texfiles == files)))
		return;
	world->texmaxsize = size;
	world->texgray = gray;
	world->texfiles = files;
	R_TextureDir (world->name, dir, sizeof(dir));
	for (i = 0 ; i < world->numtextures ; i++)
		if (world->textures[i])
			R_CustomTexture (world->textures[i], dir, size, files, gray, world->arena);
	D_FlushCaches ();
}
