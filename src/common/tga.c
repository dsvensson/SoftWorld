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
// tga.c -- reads Truevision TGA images as RGBA
//
// A file is an 18 byte header, an ID field, the color map if there is one, and
// the pixels: rows from the bottom unless the header says from the top, each
// from the left unless it says from the right. A run-length encoded image is
// packets of a count and one pixel repeated, or of a count and that many pixels,
// and a packet may run on into the next row.

#include "mem.h"
#include "tga.h"

#include <string.h>

enum
{
	TGA_MAPPED = 1,
	TGA_TRUECOLOR = 2,
	TGA_GREY = 3,
	TGA_RLE = 8,			// added to the above
};

typedef struct
{
	const byte	*p, *end;
	int			bytes;			// of a pixel in the file
	int			type;			// without TGA_RLE
	bool		alpha;			// 16 bit truecolor: the top bit is alpha
	const byte	*map;			// color-mapped: the map's entries
	int			mapfirst, maplength, mapbytes;
	int			run, raw;		// pixels left in the packet: repeated, or each read
	byte		runpixel[4];
} tgaread_t;

// 15/16, 24 or 32 bit BGR(A) as RGBA
static void TGA_Color (const byte *in, int bytes, bool alpha, byte out[4])
{
	unsigned	v;

	if (bytes == 2)
	{
		v = (unsigned)in[0] | (unsigned)in[1] << 8;
		out[0] = (byte)(((v >> 10) & 31) << 3 | ((v >> 10) & 31) >> 2);
		out[1] = (byte)(((v >> 5) & 31) << 3 | ((v >> 5) & 31) >> 2);
		out[2] = (byte)((v & 31) << 3 | (v & 31) >> 2);
		out[3] = (byte)(!alpha || (v & 0x8000) ? 255 : 0);
		return;
	}
	out[0] = in[2];
	out[1] = in[1];
	out[2] = in[0];
	out[3] = bytes == 4 ? in[3] : (byte)255;
}

// the next pixel of the file as it is stored, as RGBA; false if the file ends
static bool TGA_FilePixel (tgaread_t *t, byte out[4])
{
	const byte	*in = t->p;
	int			index;

	if (t->end - t->p < t->bytes)
		return false;
	t->p += t->bytes;
	switch (t->type)
	{
	case TGA_MAPPED:
		index = in[0] - t->mapfirst;
		if (index < 0 || index >= t->maplength)
		{
			out[0] = out[1] = out[2] = 0;
			out[3] = 255;
		}
		else
			TGA_Color (t->map + index * t->mapbytes, t->mapbytes, t->alpha, out);
		break;
	case TGA_GREY:
		out[0] = out[1] = out[2] = in[0];
		out[3] = t->bytes == 2 ? in[1] : (byte)255;
		break;
	default:
		TGA_Color (in, t->bytes, t->alpha, out);
		break;
	}
	return true;
}

// the next pixel of the image, through the packets of an encoded one
static bool TGA_NextPixel (tgaread_t *t, bool rle, byte out[4])
{
	if (!rle)
		return TGA_FilePixel (t, out);
	if (!t->run && !t->raw)
	{
		if (t->p >= t->end)
			return false;
		if (*t->p & 0x80)
		{
			t->run = (*t->p++ & 0x7f) + 1;
			if (!TGA_FilePixel (t, t->runpixel))
				return false;
		}
		else
			t->raw = (*t->p++ & 0x7f) + 1;
	}
	if (t->run)
	{
		t->run--;
		memcpy (out, t->runpixel, 4);
		return true;
	}
	t->raw--;
	return TGA_FilePixel (t, out);
}

static int TGA_Short (const byte *p)
{
	return p[0] | p[1] << 8;
}

byte *TGA_Decode (const byte *data, int len, int *width, int *height, const char **error)
{
	tgaread_t	t = {0};
	const char	*why;
	int			idlength, maptype, type, mapbits, depth, desc, w, h, i, x, y;
	bool		rle, topdown, rightleft;
	byte		*out, *pixel;

	if (!error)
		error = &why;
	if (len < 18)
	{
		*error = "shorter than a header";
		return NULL;
	}
	idlength = data[0];
	maptype = data[1];
	type = data[2];
	t.mapfirst = TGA_Short (data + 3);
	t.maplength = TGA_Short (data + 5);
	mapbits = data[7];
	w = TGA_Short (data + 12);
	h = TGA_Short (data + 14);
	depth = data[16];
	desc = data[17];

	rle = (type & TGA_RLE) != 0;
	t.type = type & ~TGA_RLE;
	if (type & ~(TGA_RLE | 3) || !t.type)
	{
		*error = "not an image type this reads";
		return NULL;
	}
	if (!w || !h || w > TGA_MAXSIZE || h > TGA_MAXSIZE)
	{
		*error = "too large or empty";
		return NULL;
	}
	if (maptype > 1 || (t.type == TGA_MAPPED && maptype != 1))
	{
		*error = "a bad color map";
		return NULL;
	}

	switch (t.type)
	{
	case TGA_MAPPED:
		if (depth != 8 || (mapbits != 15 && mapbits != 16 && mapbits != 24 && mapbits != 32))
		{
			*error = "not an 8 bit color-mapped image";
			return NULL;
		}
		break;
	case TGA_TRUECOLOR:
		if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
		{
			*error = "not a 15, 16, 24 or 32 bit image";
			return NULL;
		}
		break;
	case TGA_GREY:
		if (depth != 8 && depth != 16)
		{
			*error = "not an 8 or 16 bit greyscale image";
			return NULL;
		}
		break;
	}
	t.bytes = (depth + 7) / 8;
	// the top bit of a 16 bit color is alpha where the header gives it a bit
	t.alpha = (desc & 15) != 0;

	t.p = data + 18 + idlength;
	t.end = data + len;
	if (maptype == 1)
	{
		// a color map is skipped even where the pixels don't use it
		t.mapbytes = (mapbits + 7) / 8;
		if (t.end - t.p < (ptrdiff_t)t.maplength * t.mapbytes)
		{
			*error = "cut short";
			return NULL;
		}
		t.map = t.p;
		t.p += t.maplength * t.mapbytes;
	}

	topdown = (desc & 0x20) != 0;
	rightleft = (desc & 0x10) != 0;
	out = Mem_Alloc ((size_t)w * h * 4);
	for (i = 0 ; i < w * h ; i++)
	{
		x = i % w;
		y = i / w;
		if (rightleft)
			x = w - 1 - x;
		if (!topdown)
			y = h - 1 - y;
		pixel = out + ((size_t)y * w + x) * 4;
		if (!TGA_NextPixel (&t, rle, pixel))
		{
			Mem_Free (out);
			*error = "cut short";
			return NULL;
		}
	}

	*width = w;
	*height = h;
	return out;
}
