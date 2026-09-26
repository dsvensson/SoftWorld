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
================
Draw_Image

An 8 bit image at con x,y; texels equal to transparent (if not -1) are
skipped
================
*/
static void Draw_Image (int x, int y, const byte *src, int srcrow, int w, int h, int transparent)
{
	int		k = (int)vid.scale;
	int		v, j;
	pixel_t	*dest;

	for (v=0 ; v<h ; v++, src += srcrow)
	{
		dest = vid.buffer + (y+v)*k*vid.rowpixels + x*k;
		simd_expand8 (dest, src, d_pal30, w, k, transparent);
		// the other rows of the block: copies, or drawn again around transparent texels
		for (j=1 ; j<k ; j++)
		{
			if (transparent < 0)
				memcpy (dest + j*vid.rowpixels, dest, (size_t)w * k * sizeof(pixel_t));
			else
				simd_expand8 (dest + j*vid.rowpixels, src, d_pal30, w, k, transparent);
		}
	}
}

/*
================
Draw_Block

A w x h con unit rectangle of one color
================
*/
static void Draw_Block (int x, int y, int w, int h, pixel_t p)
{
	int		k = (int)vid.scale;
	int		u, v;
	pixel_t	*dest;

	for (v=y*k ; v<(y+h)*k ; v++)
	{
		dest = vid.buffer + v*vid.rowpixels;
		for (u=x*k ; u<(x+w)*k ; u++)
			dest[u] = p;
	}
}

/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character (int x, int y, int num)
{
	byte			*source;
	int				drawline;
	int				row, col;

	num &= 255;

	if (y <= -8)
		return;			// totally off screen

	if ((unsigned)y > vid.conheight - 8 || x < 0 || (unsigned)x > vid.conwidth - 8)
		return;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	if (y < 0)
	{	// clipped
		drawline = 8 + y;
		source -= 128*y;
		y = 0;
	}
	else
		drawline = 8;

	Draw_Image (x, y, source, 128, 8, drawline, 0);
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
	Draw_Block (x, y, 1, 1, d_pal30[color]);
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
	if ((x < 0) ||
		((unsigned)(x + width) > vid.conwidth) ||
		(y < 0) ||
		((unsigned)(y + height) > vid.conheight))
	{
		Sys_Error ("Draw_Pic: bad coordinates");
	}

	Draw_Image (x, y, pic->data + srcy * pic->width + srcx, pic->width, width, height, -1);
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	if (x < 0 || (unsigned)(x + pic->width) > vid.conwidth || y < 0 ||
		 (unsigned)(y + pic->height) > vid.conheight)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}

	Draw_Image (x, y, pic->data, pic->width, pic->width, pic->height, TRANSPARENT_COLOR);
}

void Draw_CharToConback (int num, byte *dest)
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

/*
================
Draw_ConsoleBackground

================
*/
void Draw_ConsoleBackground (int lines, bool downloading)
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
		Draw_Image (0, y, row, 0, vid.conwidth, 1, -1);
	}

	// put it back
	memcpy(conback->data + 320*186, saveback, 320*8);
}


/*
==============
R_DrawRect

A rectangle of an 8 bit image
==============
*/
static void R_DrawRect (vrect_t *prect, int rowbytes, byte *psrc)
{
	Draw_Image (prect->x, prect->y, psrc, rowbytes, prect->width, prect->height, -1);
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

			R_DrawRect (&vr, r_rectdesc.rowbytes, psrc);
		
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
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c)
{
	if (x < 0 || (unsigned)(x + w) > vid.conwidth ||
		y < 0 || (unsigned)(y + h) > vid.conheight) {
		Con_Printf("Bad Draw_Fill(%d, %d, %d, %d, %c)\n",
			x, y, w, h, c);
		return;
	}

	Draw_Block (x, y, w, h, d_pal30[c & 255]);
}
//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	int			x, y, k = (int)vid.scale;
	pixel_t		*pbuf;

	for (y=0 ; y<(int)vid.height ; y++)
	{
		int	t;

		pbuf = vid.buffer + vid.rowpixels*y;
		t = ((y / k) & 1) << 1;

		for (x=0 ; x<(int)vid.width ; x++)
		{
			if (((x / k) & 3) != t)
				pbuf[x] = d_pal30[0];
		}
	}
}

//=============================================================================

