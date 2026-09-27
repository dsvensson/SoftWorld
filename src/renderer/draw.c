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
} cachepic_t;

#define	MAX_CACHED_PICS		128
static cachepic_t	menu_cachepics[MAX_CACHED_PICS];
static int			menu_numcachepics;


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



/*
===============
Draw_FlushCache

Drops the cached pics so they are reloaded from the new game directory.
===============
*/
static void Draw_FlushCache (void)
{
	int		i;

	for (i = 0 ; i < menu_numcachepics ; i++)
	{
		Mem_Free (menu_cachepics[i].pic);
		menu_cachepics[i].pic = NULL;
	}
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
	pixel_t			p;
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
	{
		p = d_pal30[j];
		tints[i].pal[j] = HUD_RGBA (RGB30_R (p) * r / 15, RGB30_G (p) * g / 15, RGB30_B (p) * b / 15, 255);
	}
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
		snprintf (ver, sizeof(ver), "QuakeWorld %4.2f", VERSION);
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

// three of four pixels black, in a pattern of the 320x200 layout's pixels
static void Draw_FadeScreenNow (void)
{
	int			x, y, t, k = (int)vid.scale;
	hudpixel_t	*pbuf;

	for (y=0 ; y<(int)vid.height ; y++)
	{
		pbuf = vid.hud + vid.rowpixels*y;
		t = ((y / k) & 1) << 1;

		for (x=0 ; x<(int)vid.width ; x++)
		{
			if (((x / k) & 3) != t)
				pbuf[x] = draw_pal[0];
		}
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
	DC_CONBACK,			// lines, downloading
	DC_TILE,			// x, y, width, height
	DC_FILL,			// x, y, width, height, palette index
	DC_FADE
} drawop_t;

typedef struct
{
	const qpic_t	*pic;
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
		draw_pal[i] = HUD_RGBA (RGB30_R (d_pal30[i]), RGB30_G (d_pal30[i]), RGB30_B (d_pal30[i]), 255);
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
		case DC_CONBACK:
			Draw_ConsoleBackgroundNow (c->arg[0], c->arg[1] != 0);
			break;
		case DC_TILE:
			Draw_TileClearNow (c->arg[0], c->arg[1], c->arg[2], c->arg[3]);
			break;
		case DC_FILL:
			Draw_Block (c->arg[0], c->arg[1], c->arg[2], c->arg[3], draw_pal[c->arg[4] & 255]);
			break;
		case DC_FADE:
			Draw_FadeScreenNow ();
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
//=============================================================================

/*
================
Draw_FadeScreen

Darkens what is under the menus
================
*/
void Draw_FadeScreen (void)
{
	Draw_Record (DC_FADE, NULL);
}

//=============================================================================

