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
// test_tga.c -- TGA_Decode on small images made here: each kind of pixel,
// plain and run-length encoded, stored from each corner, and files cut short

#include "mem.h"
#include "sys.h"
#include "tga.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

// a file: the header, an ID field of idlength bytes, then body
static int MakeTGA (byte *out, int type, int w, int h, int depth, int desc, int idlength,
	int maptype, int mapfirst, int maplength, int mapbits, const byte *body, int bodylen)
{
	memset (out, 0, 18);
	out[0] = (byte)idlength;
	out[1] = (byte)maptype;
	out[2] = (byte)type;
	out[3] = (byte)mapfirst;
	out[4] = (byte)(mapfirst >> 8);
	out[5] = (byte)maplength;
	out[6] = (byte)(maplength >> 8);
	out[7] = (byte)mapbits;
	out[12] = (byte)w;
	out[13] = (byte)(w >> 8);
	out[14] = (byte)h;
	out[15] = (byte)(h >> 8);
	out[16] = (byte)depth;
	out[17] = (byte)desc;
	memset (out + 18, 0xAA, (size_t)idlength);
	memcpy (out + 18 + idlength, body, (size_t)bodylen);
	return 18 + idlength + bodylen;
}

// the image decodes to want, w by h RGBA from the top left
static void Expect (const char *name, const byte *file, int len, int w, int h, const byte *want)
{
	int			width, height;
	const char	*error = NULL;
	byte		*rgba = TGA_Decode (file, len, &width, &height, &error);

	if (!rgba)
	{
		printf ("FAIL %s: %s\n", name, error ? error : "no reason");
		failures++;
		return;
	}
	if (width != w || height != h || memcmp (rgba, want, (size_t)w * h * 4))
	{
		int		i;

		printf ("FAIL %s: %dx%d", name, width, height);
		for (i = 0 ; i < w * h * 4 && i < width * height * 4 ; i++)
			printf ("%s%d", i % 4 ? "," : " ", rgba[i]);
		printf ("\n");
		failures++;
	}
	else
		printf ("ok   %s\n", name);
	Mem_Free (rgba);
}

static void ExpectFail (const char *name, const byte *file, int len)
{
	int		width, height;
	byte	*rgba = TGA_Decode (file, len, &width, &height, NULL);

	if (rgba)
	{
		printf ("FAIL %s: decoded\n", name);
		failures++;
		Mem_Free (rgba);
	}
	else
		printf ("ok   %s\n", name);
}

// 2x2: red green over blue white, as RGBA from the top left
static const byte quad[16] = {
	255, 0, 0, 255,		0, 255, 0, 255,
	0, 0, 255, 255,		255, 255, 255, 255,
};

int main (void)
{
	byte	file[256];
	int		len;

	{
		// bottom row first: blue white, then red green; BGR
		static const byte body[] = {255,0,0, 255,255,255, 0,0,255, 0,255,0};
		len = MakeTGA (file, 2, 2, 2, 24, 0, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("24 bit from the bottom", file, len, 2, 2, quad);
		ExpectFail ("24 bit cut short", file, len - 1);
		ExpectFail ("a header cut short", file, 17);
	}
	{
		// top row first, with an ID field and alpha
		static const byte body[] = {0,0,255,10, 0,255,0,20, 255,0,0,30, 255,255,255,40};
		static const byte want[] = {255,0,0,10, 0,255,0,20, 0,0,255,30, 255,255,255,40};
		len = MakeTGA (file, 2, 2, 2, 32, 0x28, 5, 0, 0, 0, 0, body, sizeof(body));
		Expect ("32 bit from the top, an ID field", file, len, 2, 2, want);
	}
	{
		// from the top right: each row right to left
		static const byte body[] = {0,255,0, 0,0,255, 255,255,255, 255,0,0};
		len = MakeTGA (file, 2, 2, 2, 24, 0x30, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("24 bit from the top right", file, len, 2, 2, quad);
	}
	{
		// a truecolor image with a color map it doesn't use: the map is skipped
		static const byte body[] = {9,9,9, 9,9,9, 255,0,0, 255,255,255, 0,0,255, 0,255,0};
		len = MakeTGA (file, 2, 2, 2, 24, 0, 0, 1, 0, 2, 24, body, sizeof(body));
		Expect ("24 bit with a color map", file, len, 2, 2, quad);
	}
	{
		// a run of two across the rows, then a raw packet of two, from the top
		static const byte body[] = {0x81, 0,0,255, 0x01, 255,0,0, 255,255,255};
		static const byte want[] = {255,0,0,255, 255,0,0,255, 0,0,255,255, 255,255,255,255};
		len = MakeTGA (file, 10, 2, 2, 24, 0x20, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("24 bit run-length encoded", file, len, 2, 2, want);
		ExpectFail ("run-length encoded cut short", file, len - 2);
	}
	{
		// a run of four
		static const byte body[] = {0x83, 1,2,3,4};
		static const byte want[] = {3,2,1,4, 3,2,1,4, 3,2,1,4, 3,2,1,4};
		len = MakeTGA (file, 10, 2, 2, 32, 0x28, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("32 bit one run", file, len, 2, 2, want);
	}
	{
		static const byte body[] = {10, 20, 30, 40};
		static const byte want[] = {30,30,30,255, 40,40,40,255, 10,10,10,255, 20,20,20,255};
		len = MakeTGA (file, 3, 2, 2, 8, 0, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("8 bit grey", file, len, 2, 2, want);
	}
	{
		// grey and alpha, run-length encoded, from the top
		static const byte body[] = {0x81, 50,60, 0x01, 70,80, 90,100};
		static const byte want[] = {50,50,50,60, 50,50,50,60, 70,70,70,80, 90,90,90,100};
		len = MakeTGA (file, 11, 2, 2, 16, 0x28, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("16 bit grey and alpha run-length encoded", file, len, 2, 2, want);
	}
	{
		// a map of 3 entries from index 10; index 99 is outside it (black)
		static const byte body[] = {255,0,0, 0,255,0, 0,0,255,  12, 99, 10, 11};
		static const byte want[] = {0,0,255,255, 0,255,0,255, 255,0,0,255, 0,0,0,255};
		len = MakeTGA (file, 1, 2, 2, 8, 0, 0, 1, 10, 3, 24, body, sizeof(body));
		Expect ("8 bit color-mapped", file, len, 2, 2, want);
		ExpectFail ("color map cut short", file, 18 + 5);
	}
	{
		// the same, 32 bit entries, run-length encoded
		static const byte body[] = {255,0,0,1, 0,255,0,2, 0x81, 1, 0x01, 0, 0};
		static const byte want[] = {0,255,0,2, 0,255,0,2, 0,0,255,1, 0,0,255,1};
		len = MakeTGA (file, 9, 2, 2, 8, 0x28, 0, 1, 0, 2, 32, body, sizeof(body));
		Expect ("8 bit color-mapped run-length encoded", file, len, 2, 2, want);
	}
	{
		// A1R5G5B5: the top bit is alpha only where the header gives alpha a bit
		static const byte body[] = {0x00, 0x7C, 0xE0, 0x83};
		static const byte want[] = {255,0,0,255, 0,255,0,255};
		static const byte want_alpha[] = {255,0,0,0, 0,255,0,255};
		len = MakeTGA (file, 2, 2, 1, 16, 0x20, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("16 bit without alpha", file, len, 2, 1, want);
		len = MakeTGA (file, 2, 2, 1, 16, 0x21, 0, 0, 0, 0, 0, body, sizeof(body));
		Expect ("16 bit with alpha", file, len, 2, 1, want_alpha);
	}
	{
		static const byte body[] = {0};
		len = MakeTGA (file, 2, 0, 2, 24, 0, 0, 0, 0, 0, 0, body, sizeof(body));
		ExpectFail ("no width", file, len);
		len = MakeTGA (file, 2, 1, 1, 8, 0, 0, 0, 0, 0, 0, body, sizeof(body));
		ExpectFail ("8 bit truecolor", file, len);
		len = MakeTGA (file, 1, 1, 1, 8, 0, 0, 0, 0, 0, 0, body, sizeof(body));
		ExpectFail ("color-mapped without a map", file, len);
		len = MakeTGA (file, 32, 1, 1, 8, 0, 0, 0, 0, 0, 0, body, sizeof(body));
		ExpectFail ("Huffman encoded", file, len);
	}

	if (failures)
	{
		printf ("%d failed\n", failures);
		return 1;
	}
	return 0;
}
