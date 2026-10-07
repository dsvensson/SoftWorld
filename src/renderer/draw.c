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

// draw.c -- 2D drawing. Coordinates are con units, the 320x200 layout;
// each texel covers vid.scale x vid.scale pixels.
//
// The 2D is a layer of its own, vid.hud, which the presenter lays over the 3D
// view after the view's blends and gamma. A frame's Draw_ calls are recorded,
// and Draw_Flush draws them into the layer only when they aren't the last
// frame's: most frames the 2D doesn't change.

#include "markup.h"
#include "r_local.h"

typedef struct {
	vrect_t	rect;
	int		width;
	int		height;
	byte	*ptexbytes;
	int		rowbytes;
} rectdesc_t;

static rectdesc_t	r_rectdesc;

byte		*draw_chars;				// 8*8 graphic characters
qpic_t		*draw_disc;
static qpic_t		*draw_backtile;

//=============================================================================
/* Support Routines */

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	qpic_t		*pic;			// loaded on first use
	bool		wad;			// the pic is gfx.wad's: not freed
	bool		missing;		// QuakeC asked, and there is none (until the gamedir changes)
} cachepic_t;

#define	MAX_CACHED_PICS		128
#define	MAX_QUAKEC_PICS		64		// of them those QuakeC asked for first: the engine's have room
static cachepic_t	menu_cachepics[MAX_CACHED_PICS];
static int			menu_numcachepics;
static int			draw_numquakecpics;


qpic_t	*Draw_PicFromWad (char *lumpname)
{
	return W_GetLumpName (lumpname);
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			break;

	if (i == menu_numcachepics)
	{
		if (menu_numcachepics == MAX_CACHED_PICS)
			Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
		menu_numcachepics++;
		Q_strncpyz (pic->name, path, sizeof(pic->name));
	}

	dat = pic->pic;

	if (dat)
		return dat;

//
// load the pic from disk
//
	pic->pic = (qpic_t *)FS_LoadFile (path, NULL);

	dat = pic->pic;
	if (!dat)
	{
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	}

	SwapPic (dat);

	return dat;
}

// the gfx.wad picture a path names: gfx/<lump>, with or without .lmp
static qpic_t *Draw_WadPic (const char *path)
{
	char	name[MAX_QPATH];
	size_t	len;

	if (Q_strncasecmp (path, "gfx/", 4))
		return NULL;
	Q_strncpyz (name, path + 4, sizeof(name));
	len = strlen (name);
	if (len > 4 && !Q_strcasecmp (name + len - 4, ".lmp"))
		name[len - 4] = 0;
	return W_TryGetPic (name);
}

/*
================
Draw_TryCachePic

Draw_CachePic for QuakeC's names: a loose pic, else gfx.wad's, checked, and
NULL rather than an error when there is none or no room
================
*/
qpic_t *Draw_TryCachePic (const char *path)
{
	cachepic_t	*pic;
	int			i, len;
	qpic_t		*dat;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			break;
	if (i == menu_numcachepics)
	{
		if (menu_numcachepics == MAX_CACHED_PICS || draw_numquakecpics == MAX_QUAKEC_PICS
			|| strlen (path) >= sizeof(pic->name))
			return NULL;
		menu_numcachepics++;
		draw_numquakecpics++;
		Q_strncpyz (pic->name, path, sizeof(pic->name));
	}
	if (pic->pic || pic->missing)
		return pic->pic;

	dat = (qpic_t *)FS_LoadFile ((char *)path, &len);
	if (dat)
	{
		SwapPic (dat);
		if (len < 8 || dat->width <= 0 || dat->height <= 0 || dat->width > 4096 || dat->height > 4096
			|| (int64_t)dat->width * dat->height > len - 8)
		{
			Con_DPrintf ("%s: not a pic\n", path);
			Mem_Free (dat);
			dat = NULL;
		}
	}
	else if ((dat = Draw_WadPic (path)))
		pic->wad = true;
	pic->pic = dat;
	pic->missing = !dat;
	return dat;
}

/*
===============
Draw_FlushCache

Drops the cached pics so they are reloaded from the new game directory, and
QuakeC's images.
===============
*/
static void Draw_FreeImages (void);

static void Draw_FlushCache (void)
{
	int		i;

	for (i = 0 ; i < menu_numcachepics ; i++)
	{
		if (!menu_cachepics[i].wad)
			Mem_Free (menu_cachepics[i].pic);
		menu_cachepics[i].pic = NULL;
		menu_cachepics[i].wad = false;
		menu_cachepics[i].missing = false;
	}
	Draw_FreeImages ();
	Draw_Invalidate ();		// a new pic may have an old one's address
}

/*
===============
Draw_Init
===============
*/
void Draw_Init (void)
{
	draw_chars = W_GetLumpName ("conchars");
	draw_disc = W_GetLumpName ("disc");
	draw_backtile = W_GetLumpName ("backtile");
	FS_AddGamedirCallback (Draw_FlushCache);

	r_rectdesc.width = draw_backtile->width;
	r_rectdesc.height = draw_backtile->height;
	r_rectdesc.ptexbytes = draw_backtile->data;
	r_rectdesc.rowbytes = draw_backtile->width;
}

/*
===============================================================================

QUAKEC'S IMAGES

RGBA images QuakeC makes (FTE's r_uploadimage), by name, and pics read back
as RGBA (r_readimage)

===============================================================================
*/

struct drawimage_s
{
	char		name[MAX_QPATH];
	int			width, height;
	hudpixel_t	*data;			// premultiplied
};

#define	MAX_DRAW_IMAGES		32
#define	MAX_IMAGE_SIZE		4096
#define	MAX_IMAGE_BYTES		(64 << 20)		// of them all

static drawimage_t	draw_images[MAX_DRAW_IMAGES];
static int			draw_numimages;
static size_t		draw_imagebytes;

const drawimage_t *Draw_FindImage (const char *name)
{
	int		i;

	for (i=0 ; i<draw_numimages ; i++)
		if (!strcmp (draw_images[i].name, name))
			return &draw_images[i];
	return NULL;
}

/*
================
Draw_UploadImage

RGBA bytes, alpha not premultiplied, as the image name (a new one, or a new
size and pixels for one there is); false if it can't be
================
*/
bool Draw_UploadImage (const char *name, int width, int height, const byte *rgba)
{
	drawimage_t	*img = (drawimage_t *)Draw_FindImage (name);
	size_t		bytes, had = img ? (size_t)img->width * img->height * sizeof(hudpixel_t) : 0;
	unsigned	a;
	int			i;

	if (width <= 0 || height <= 0 || width > MAX_IMAGE_SIZE || height > MAX_IMAGE_SIZE
		|| strlen (name) >= sizeof(img->name))
		return false;
	bytes = (size_t)width * height * sizeof(hudpixel_t);
	if (draw_imagebytes - had + bytes > MAX_IMAGE_BYTES || (!img && draw_numimages == MAX_DRAW_IMAGES))
		return false;
	if (!img)
	{
		img = &draw_images[draw_numimages++];
		Q_strncpyz (img->name, name, sizeof(img->name));
	}
	else
		Mem_Free (img->data);
	draw_imagebytes += bytes - had;
	img->width = width;
	img->height = height;
	img->data = Mem_Alloc (bytes);
	for (i=0 ; i<width*height ; i++, rgba += 4)
	{
		a = rgba[3];
		img->data[i] = HUD_RGBA ((rgba[0] * a + 127) / 255, (rgba[1] * a + 127) / 255, (rgba[2] * a + 127) / 255, a);
	}
	Draw_Invalidate ();		// the pixels changed under the same image
	return true;
}

void Draw_ImageSize (const drawimage_t *img, int *width, int *height)
{
	*width = img->width;
	*height = img->height;
}

static void Draw_FreeImages (void)
{
	int		i;

	for (i=0 ; i<draw_numimages ; i++)
		Mem_Free (draw_images[i].data);
	draw_numimages = 0;
	draw_imagebytes = 0;
}

// 8 bit texels as RGBA through the palette, one index transparent (-1 none)
static byte *Draw_Expand (const byte *src, int width, int height, int transparent)
{
	byte	*rgba = Mem_Alloc ((size_t)width * height * 4), *out = rgba;
	int		i;

	for (i=0 ; i<width*height ; i++, out += 4)
	{
		out[0] = d_palrgb[src[i]][0];
		out[1] = d_palrgb[src[i]][1];
		out[2] = d_palrgb[src[i]][2];
		out[3] = src[i] == transparent ? 0 : 255;
	}
	return rgba;
}

/*
================
Draw_ReadImage

A pic as RGBA, alpha 0 where it is transparent, through the palette the 2D
draws with: a loose .lmp (255 transparent), gfx.wad's (gfx/conchars, 0
transparent, as the characters are drawn), and gfx/palette.lmp as 16x16 of
its colors. Mem_Alloc'd; NULL if there is no such pic.
================
*/
byte *Draw_ReadImage (const char *path, int *width, int *height)
{
	byte	index[256], *rgba;
	qpic_t	*pic;
	int		i, len;

	if (!Q_strcasecmp (path, "gfx/conchars") || !Q_strcasecmp (path, "gfx/conchars.lmp"))
	{
		*width = *height = 128;
		return Draw_Expand (draw_chars, 128, 128, 0);
	}
	if (!Q_strcasecmp (path, "gfx/palette.lmp"))
	{
		for (i=0 ; i<256 ; i++)
			index[i] = (byte)i;
		*width = *height = 16;
		return Draw_Expand (index, 16, 16, -1);
	}
	pic = (qpic_t *)FS_LoadFile ((char *)path, &len);
	if (pic)
	{
		SwapPic (pic);
		rgba = NULL;
		if (len >= 8 && pic->width > 0 && pic->height > 0 && pic->width <= MAX_IMAGE_SIZE
			&& pic->height <= MAX_IMAGE_SIZE && (int64_t)pic->width * pic->height <= len - 8)
		{
			*width = pic->width;
			*height = pic->height;
			rgba = Draw_Expand (pic->data, pic->width, pic->height, TRANSPARENT_COLOR);
		}
		Mem_Free (pic);
		return rgba;
	}
	if ((pic = Draw_WadPic (path)))
	{
		*width = pic->width;
		*height = pic->height;
		return Draw_Expand (pic->data, pic->width, pic->height, TRANSPARENT_COLOR);
	}
	return NULL;
}

/*
===============================================================================

THE 2D LAYER

What the recorded calls draw, into vid.hud

===============================================================================
*/

static hudpixel_t	draw_pal[256];		// the palette, opaque

/*
================
Draw_Image

An 8 bit image at con x,y through pal; texels equal to transparent (if not
-1) are skipped
================
*/
static void Draw_Image (int x, int y, const byte *src, int srcrow, int w, int h, const hudpixel_t *pal,
	int transparent)
{
	int			k = (int)vid.scale;
	int			v, j;
	hudpixel_t	*dest;

	for (v=0 ; v<h ; v++, src += srcrow)
	{
		dest = vid.hud + (y+v)*k*vid.rowpixels + x*k;
		simd_expand8 (dest, src, pal, w, k, transparent);
		// the other rows of the block: copies, or drawn again around transparent texels
		for (j=1 ; j<k ; j++)
		{
			if (transparent < 0)
				memcpy (dest + j*vid.rowpixels, dest, (size_t)w * k * sizeof(hudpixel_t));
			else
				simd_expand8 (dest + j*vid.rowpixels, src, pal, w, k, transparent);
		}
	}
}

/*
================
Draw_Block

A w x h con unit rectangle of one color, clipped to the screen
================
*/
static void Draw_Block (int x, int y, int w, int h, hudpixel_t p)
{
	int			k = (int)vid.scale;
	int			u, v, x0, x1, y0, y1;
	hudpixel_t	*dest;

	x0 = x < 0 ? 0 : x;
	y0 = y < 0 ? 0 : y;
	x1 = x + w > (int)vid.conwidth ? (int)vid.conwidth : x + w;
	y1 = y + h > (int)vid.conheight ? (int)vid.conheight : y + h;
	for (v=y0*k ; v<y1*k ; v++)
	{
		dest = vid.hud + v*vid.rowpixels;
		for (u=x0*k ; u<x1*k ; u++)
			dest[u] = p;
	}
}

// s over d, both premultiplied
static hudpixel_t Draw_Over (hudpixel_t s, hudpixel_t d)
{
	unsigned	keep = 255 - HUD_A (s), c, out = 0;
	int			shift;

	for (shift=0 ; shift<32 ; shift += 8)
	{
		c = ((s >> shift) & 255) + (((d >> shift) & 255) * keep + 127) / 255;
		out |= (c > 255 ? 255 : c) << shift;
	}
	return out;
}

/*
================
Draw_BlendNow

A w x h con unit rectangle of a premultiplied color over what is there; on
the screen. What is under it keeps (255 - alpha) / 255 of each channel, a
table's for the rectangle (the browser's are wide, and change each frame).
================
*/
static void Draw_BlendNow (int x, int y, int w, int h, hudpixel_t p)
{
	int			k = (int)vid.scale;
	unsigned	keep = 255 - HUD_A (p), v;
	byte		kept[256];
	hudpixel_t	*dest;
	int			u, row;

	if (HUD_A (p) == 255)
	{
		Draw_Block (x, y, w, h, p);
		return;
	}
	for (v=0 ; v<256 ; v++)
		kept[v] = (byte)((v * keep + 127) / 255);
	// premultiplied: a channel is at most alpha, and what is kept at most the rest
	for (row=y*k ; row<(y+h)*k ; row++)
	{
		dest = vid.hud + row*vid.rowpixels + x*k;
		for (u=0 ; u<w*k ; u++)
			dest[u] = p + (hudpixel_t)(kept[dest[u] & 255] | kept[(dest[u] >> 8) & 255] << 8
				| kept[(dest[u] >> 16) & 255] << 16 | (hudpixel_t)kept[dest[u] >> 24] << 24);
	}
}

/*
================
Draw_RGBANow

A premultiplied RGBA image at con x,y: opaque texels copied, clear ones
skipped, the others over what is there
================
*/
static void Draw_RGBANow (int x, int y, const hudpixel_t *src, int srcrow, int w, int h)
{
	int			k = (int)vid.scale;
	int			u, v, i, j;
	unsigned	a;
	hudpixel_t	*dest, *block;

	for (v=0 ; v<h ; v++, src += srcrow)
		for (j=0 ; j<k ; j++)
		{
			dest = vid.hud + ((y+v)*k + j)*vid.rowpixels + x*k;
			for (u=0 ; u<w ; u++)
			{
				if (!(a = HUD_A (src[u])))
					continue;
				block = dest + u*k;
				if (a == 255)
					for (i=0 ; i<k ; i++)
						block[i] = src[u];
				else
					for (i=0 ; i<k ; i++)
						block[i] = Draw_Over (src[u], block[i]);
			}
		}
}

/*
================
Draw_ImageHalf

An 8 bit image through pal, half over what is there; texels of 0 are skipped
================
*/
static void Draw_ImageHalf (int x, int y, const byte *src, int srcrow, int w, int h, const hudpixel_t *pal)
{
	const hudpixel_t	color = 0x00fefefeu;	// the color's bits but each channel's lowest
	int					k = (int)vid.scale;
	int					u, v, i, j;
	hudpixel_t			*dest, p, d;

	for (v=0 ; v<h ; v++, src += srcrow)
		for (j=0 ; j<k ; j++)
		{
			dest = vid.hud + ((y+v)*k + j)*vid.rowpixels + x*k;
			for (u=0 ; u<w ; u++)
			{
				if (!src[u])
					continue;
				p = (pal[src[u]] & color) >> 1;
				for (i=0 ; i<k ; i++)
				{
					d = dest[u*k + i];
					dest[u*k + i] = (((d & color) >> 1) + p) | ((128 + (HUD_A (d) >> 1)) << 24);
				}
			}
		}
}

/*
================
Draw_TintPalette

The palette multiplied by a text color's four bits a channel (markup.h),
kept for the last few colors: text goes through the same ones again and
again
================
*/
#define	DRAW_TINTS	8

static const hudpixel_t *Draw_TintPalette (unsigned rgb)
{
	static struct { unsigned rgb; hudpixel_t pal[256]; }	tints[DRAW_TINTS];
	static int		used, next;
	unsigned		r, g, b;
	int				i, j;

	for (i=0 ; i<used ; i++)
		if (tints[i].rgb == rgb)
			return tints[i].pal;

	i = used < DRAW_TINTS ? used++ : next++ % DRAW_TINTS;
	tints[i].rgb = rgb;
	r = (rgb >> 8) & 15;
	g = (rgb >> 4) & 15;
	b = rgb & 15;
	for (j=0 ; j<256 ; j++)
		tints[i].pal[j] = HUD_RGBA (d_palrgb[j][0] * r / 15, d_palrgb[j][1] * g / 15, d_palrgb[j][2] * b / 15, 255);
	return tints[i].pal;
}

// a character, clipped to the top of the screen so the console scrolls off
static void Draw_CharacterNow (int x, int y, int num, unsigned color)
{
	byte				*source;
	const hudpixel_t	*pal;
	int					drawline;

	source = draw_chars + ((num>>4)<<10) + ((num&15)<<3);
	if (y < 0)
	{	// clipped
		drawline = 8 + y;
		source -= 128*y;
		y = 0;
	}
	else
		drawline = 8;

	pal = (color & TEXT_TINT) ? Draw_TintPalette (color & TEXT_RGBMASK) : draw_pal;
	if (color & TEXT_HALF)
		Draw_ImageHalf (x, y, source, 128, 8, drawline, pal);
	else
		Draw_Image (x, y, source, 128, 8, drawline, pal, 0);
}

static void Draw_CharToConback (int num, byte *dest)
{
	int		row, col;
	byte	*source;
	int		drawline;
	int		x;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x])
				dest[x] = 0x60 + source[x];
		source += 128;
		dest += 320;
	}

}

static void Draw_ConsoleBackgroundNow (int lines, bool downloading)
{
	int				x, y, v;
	byte			*src;
	byte			row[MAX_CONWIDTH];
	int				f, fstep;
	qpic_t			*conback;
	char			ver[100];
	static			char saveback[320*8];

	conback = Draw_CachePic ("gfx/conback.lmp");

// hack the version number directly into the pic
	if (downloading) {
		snprintf (ver, sizeof(ver), "%4.2f", VERSION);
		src = conback->data + 320 + 320*186 - 11 - 8*strlen(ver);
	} else {
		snprintf (ver, sizeof(ver), "SoftWorld %4.2f", VERSION);
		src = conback->data + 320 - (strlen(ver)*8 + 11) + 320*186;
	}

	memcpy(saveback, conback->data + 320*186, 320*8);
	for (x=0 ; x<(int)strlen(ver) ; x++)
		Draw_CharToConback (ver[x], src+(x<<3));

// draw the pic, stretched to the con width
	fstep = 320*0x10000/vid.conwidth;

	for (y=0 ; y<lines ; y++)
	{
		v = (vid.conheight - lines + y)*200/vid.conheight;
		src = conback->data + v*320;
		f = 0;
		for (x=0 ; x<(int)vid.conwidth ; x++)
		{
			row[x] = src[f>>16];
			f += fstep;
		}
		Draw_Image (0, y, row, 0, vid.conwidth, 1, draw_pal, -1);
	}

	// put it back
	memcpy(conback->data + 320*186, saveback, 320*8);
}

/*
=============
Draw_TileClearNow

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
static void Draw_TileClearNow (int x, int y, int w, int h)
{
	int				width, height, tileoffsetx, tileoffsety;
	byte			*psrc;
	vrect_t			vr;

	r_rectdesc.rect.x = x;
	r_rectdesc.rect.y = y;
	r_rectdesc.rect.width = w;
	r_rectdesc.rect.height = h;

	vr.y = r_rectdesc.rect.y;
	height = r_rectdesc.rect.height;

	tileoffsety = vr.y % r_rectdesc.height;

	while (height > 0)
	{
		vr.x = r_rectdesc.rect.x;
		width = r_rectdesc.rect.width;

		if (tileoffsety != 0)
			vr.height = r_rectdesc.height - tileoffsety;
		else
			vr.height = r_rectdesc.height;

		if (vr.height > height)
			vr.height = height;

		tileoffsetx = vr.x % r_rectdesc.width;

		while (width > 0)
		{
			if (tileoffsetx != 0)
				vr.width = r_rectdesc.width - tileoffsetx;
			else
				vr.width = r_rectdesc.width;

			if (vr.width > width)
				vr.width = width;

			psrc = r_rectdesc.ptexbytes +
					(tileoffsety * r_rectdesc.rowbytes) + tileoffsetx;

			Draw_Image (vr.x, vr.y, psrc, r_rectdesc.rowbytes, vr.width, vr.height, draw_pal, -1);

			vr.x += vr.width;
			width -= vr.width;
			tileoffsetx = 0;	// only the left tile can be left-clipped
		}

		vr.y += vr.height;
		height -= vr.height;
		tileoffsety = 0;		// only the top tile can be top-clipped
	}
}

/*
===============================================================================

THE CALLS OF A FRAME

===============================================================================
*/

typedef enum
{
	DC_CHAR,			// x, y, character, text color
	DC_PIC,				// pic: x, y, srcx, srcy, width, height
	DC_TRANSPIC,		// pic: x, y
	DC_TRANSSUBPIC,		// pic: x, y, srcx, srcy, width, height
	DC_IMAGE,			// image: x, y, srcx, srcy, width, height
	DC_CONBACK,			// lines, downloading
	DC_TILE,			// x, y, width, height
	DC_FILL,			// x, y, width, height, palette index
	DC_BLEND			// x, y, width, height, premultiplied RGBA (on screen)
} drawop_t;

typedef struct
{
	union
	{
		const qpic_t		*pic;
		const drawimage_t	*image;
	};
	int				op;
	int				arg[7];
} drawcmd_t;

// two frames' calls are compared byte for byte
static_assert (sizeof(drawcmd_t) == sizeof(void *) + 8 * sizeof(int), "drawcmd_t has padding");

static drawcmd_t	*draw_calls[2];			// this frame's, and the last one's
static int			draw_numcalls[2], draw_maxcalls[2];
static int			draw_now;				// which is this frame's
static bool			draw_stale = true;		// the layer is drawn whatever the calls

static drawcmd_t *Draw_Record (drawop_t op, const qpic_t *pic)
{
	int		n = draw_now;

	if (draw_numcalls[n] == draw_maxcalls[n])
	{
		draw_maxcalls[n] = draw_maxcalls[n] ? draw_maxcalls[n] * 2 : 1024;
		draw_calls[n] = Mem_Realloc (draw_calls[n], (size_t)draw_maxcalls[n] * sizeof(drawcmd_t));
	}
	draw_calls[n][draw_numcalls[n]] = (drawcmd_t){.pic = pic, .op = op};
	return &draw_calls[n][draw_numcalls[n]++];
}

void Draw_Invalidate (void)
{
	draw_stale = true;
}

/*
================
Draw_Flush

The frame's 2D into vid.hud, unless it is the last frame's
================
*/
void Draw_Flush (void)
{
	const drawcmd_t	*c;
	int				now = draw_now, last = now ^ 1;
	int				i;

	draw_now = last;
	if (!draw_stale && draw_numcalls[now] == draw_numcalls[last]
		&& (!draw_numcalls[now] || !memcmp (draw_calls[now], draw_calls[last], (size_t)draw_numcalls[now] * sizeof(drawcmd_t))))
	{
		draw_numcalls[last] = 0;
		return;
	}
	draw_numcalls[last] = 0;
	draw_stale = false;
	vid.huddirty = true;
	R_ProfCount (PROFN_HUD, 1);

	for (i=0 ; i<256 ; i++)
		draw_pal[i] = HUD_RGBA (d_palrgb[i][0], d_palrgb[i][1], d_palrgb[i][2], 255);
	memset (vid.hud, 0, (size_t)vid.rowpixels * vid.height * sizeof(hudpixel_t));

	for (i=0, c = draw_calls[now] ; i<draw_numcalls[now] ; i++, c++)
	{
		switch ((drawop_t)c->op)
		{
		case DC_CHAR:
			Draw_CharacterNow (c->arg[0], c->arg[1], c->arg[2], (unsigned)c->arg[3]);
			break;
		case DC_PIC:
			Draw_Image (c->arg[0], c->arg[1], c->pic->data + c->arg[3] * c->pic->width + c->arg[2], c->pic->width,
				c->arg[4], c->arg[5], draw_pal, -1);
			break;
		case DC_TRANSPIC:
			Draw_Image (c->arg[0], c->arg[1], c->pic->data, c->pic->width, c->pic->width, c->pic->height, draw_pal,
				TRANSPARENT_COLOR);
			break;
		case DC_TRANSSUBPIC:
			Draw_Image (c->arg[0], c->arg[1], c->pic->data + c->arg[3] * c->pic->width + c->arg[2], c->pic->width,
				c->arg[4], c->arg[5], draw_pal, TRANSPARENT_COLOR);
			break;
		case DC_IMAGE:
			Draw_RGBANow (c->arg[0], c->arg[1], c->image->data + c->arg[3] * c->image->width + c->arg[2],
				c->image->width, c->arg[4], c->arg[5]);
			break;
		case DC_CONBACK:
			Draw_ConsoleBackgroundNow (c->arg[0], c->arg[1] != 0);
			break;
		case DC_TILE:
			Draw_TileClearNow (c->arg[0], c->arg[1], c->arg[2], c->arg[3]);
			break;
		case DC_FILL:
			Draw_Block (c->arg[0], c->arg[1], c->arg[2], c->arg[3], draw_pal[c->arg[4] & 255]);
			break;
		case DC_BLEND:
			Draw_BlendNow (c->arg[0], c->arg[1], c->arg[2], c->arg[3], (hudpixel_t)c->arg[4]);
			break;
		}
	}
}

/*
===============================================================================

DRAWING

Each call is checked and recorded, to be drawn by Draw_Flush

===============================================================================
*/

/*
================
Draw_ColoredCharacter

Draws one 8*8 graphics character with 0 being transparent, in a text color
(markup.h). It can be clipped to the top of the screen to allow the console
to be smoothly scrolled off.
================
*/
void Draw_ColoredCharacter (int x, int y, int num, unsigned color)
{
	drawcmd_t	*c;

	if (y <= -8)
		return;			// totally off screen

	if ((unsigned)y > vid.conheight - 8 || x < 0 || (unsigned)x > vid.conwidth - 8)
		return;

	if ((color & TEXT_BLINK) && ((int)(Sys_DoubleTime () * 2) & 1))
		return;

	c = Draw_Record (DC_CHAR, NULL);
	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = num & 255;
	c->arg[3] = (int)(color & (TEXT_TINT | TEXT_RGBMASK | TEXT_HALF));
}

void Draw_Character (int x, int y, int num)
{
	Draw_ColoredCharacter (x, y, num, 0);
}

/*
================
Draw_MarkupString

A string with its colors read out of it (markup.h)
================
*/
void Draw_MarkupString (int x, int y, const char *str)
{
	markup_t	m;
	int			c;

	Markup_Begin (&m);
	while ((c = Markup_Next (&str, &m)) >= 0)
	{
		Draw_ColoredCharacter (x, y, c, m.color);
		x += 8;
	}
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, char *str)
{
	while (*str)
	{
		Draw_Character (x, y, *str);
		str++;
		x += 8;
	}
}

/*
================
Draw_Alt_String
================
*/
void Draw_Alt_String (int x, int y, char *str)
{
	while (*str)
	{
		Draw_Character (x, y, (*str) | 0x80);
		str++;
		x += 8;
	}
}

void Draw_Pixel (int x, int y, byte color)
{
	drawcmd_t	*c = Draw_Record (DC_FILL, NULL);

	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = 1;
	c->arg[3] = 1;
	c->arg[4] = color;
}

/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	Draw_SubPic (x, y, pic, 0, 0, pic->width, pic->height);
}


/*
=============
Draw_SubPic
=============
*/
void Draw_SubPic (int x, int y, qpic_t *pic, int srcx, int srcy, int width, int height)
{
	drawcmd_t	*c;

	if ((x < 0) ||
		((unsigned)(x + width) > vid.conwidth) ||
		(y < 0) ||
		((unsigned)(y + height) > vid.conheight))
	{
		Sys_Error ("Draw_Pic: bad coordinates");
	}

	c = Draw_Record (DC_PIC, pic);
	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = srcx;
	c->arg[3] = srcy;
	c->arg[4] = width;
	c->arg[5] = height;
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	drawcmd_t	*c;

	if (x < 0 || (unsigned)(x + pic->width) > vid.conwidth || y < 0 ||
		 (unsigned)(y + pic->height) > vid.conheight)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}

	c = Draw_Record (DC_TRANSPIC, pic);
	c->arg[0] = x;
	c->arg[1] = y;
}

// a width x height source rectangle drawn at x,y, clipped to the screen:
// false if none of it shows
static bool Draw_Clip (int *x, int *y, int *srcx, int *srcy, int *width, int *height)
{
	if (*x < 0)
	{
		*srcx -= *x;
		*width += *x;
		*x = 0;
	}
	if (*y < 0)
	{
		*srcy -= *y;
		*height += *y;
		*y = 0;
	}
	if (*width > (int)vid.conwidth - *x)
		*width = (int)vid.conwidth - *x;
	if (*height > (int)vid.conheight - *y)
		*height = (int)vid.conheight - *y;
	return *width > 0 && *height > 0;
}

/*
=============
Draw_ClippedPic

QuakeC's pics, anywhere: 255 transparent (as FTE draws them), and clipped
to the screen
=============
*/
void Draw_ClippedPic (int x, int y, const qpic_t *pic)
{
	drawcmd_t	*c;
	int			srcx = 0, srcy = 0, width = pic->width, height = pic->height;

	if (!Draw_Clip (&x, &y, &srcx, &srcy, &width, &height))
		return;
	c = Draw_Record (DC_TRANSSUBPIC, pic);
	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = srcx;
	c->arg[3] = srcy;
	c->arg[4] = width;
	c->arg[5] = height;
}

/*
=============
Draw_ClippedImage

QuakeC's images (Draw_UploadImage), anywhere, clipped to the screen
=============
*/
void Draw_ClippedImage (int x, int y, const drawimage_t *img)
{
	drawcmd_t	*c;
	int			srcx = 0, srcy = 0, width = img->width, height = img->height;

	if (!Draw_Clip (&x, &y, &srcx, &srcy, &width, &height))
		return;
	c = Draw_Record (DC_IMAGE, NULL);
	c->image = img;
	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = srcx;
	c->arg[3] = srcy;
	c->arg[4] = width;
	c->arg[5] = height;
}

/*
================
Draw_ConsoleBackground

The console's pic, its bottom lines lines down the screen, with the version
================
*/
void Draw_ConsoleBackground (int lines, bool downloading)
{
	drawcmd_t	*c;

	if (lines <= 0)
		return;
	c = Draw_Record (DC_CONBACK, NULL);
	c->arg[0] = lines;
	c->arg[1] = downloading;
}

/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	drawcmd_t	*c = Draw_Record (DC_TILE, NULL);

	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = w;
	c->arg[3] = h;
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int color)
{
	drawcmd_t	*c;

	if (x < 0 || (unsigned)(x + w) > vid.conwidth ||
		y < 0 || (unsigned)(y + h) > vid.conheight) {
		Con_Printf("Bad Draw_Fill(%d, %d, %d, %d, %c)\n",
			x, y, w, h, color);
		return;
	}

	c = Draw_Record (DC_FILL, NULL);
	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = w;
	c->arg[3] = h;
	c->arg[4] = color & 255;
}

/*
=============
Draw_BlendFill

QuakeC's fills (the menu's): clipped to the screen as recorded
=============
*/
void Draw_BlendFill (int x, int y, int w, int h, int r, int g, int b, int alpha)
{
	drawcmd_t	*c;

	if (x < 0)
	{
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		h += y;
		y = 0;
	}
	if (x + w > (int)vid.conwidth)
		w = (int)vid.conwidth - x;
	if (y + h > (int)vid.conheight)
		h = (int)vid.conheight - y;
	alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
	if (w <= 0 || h <= 0 || !alpha)
		return;
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;

	c = Draw_Record (DC_BLEND, NULL);
	c->arg[0] = x;
	c->arg[1] = y;
	c->arg[2] = w;
	c->arg[3] = h;
	c->arg[4] = (int)HUD_RGBA ((unsigned)(r * alpha + 127) / 255, (unsigned)(g * alpha + 127) / 255,
		(unsigned)(b * alpha + 127) / 255, (unsigned)alpha);
}
//=============================================================================

