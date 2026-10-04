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
// png.c -- writes RGB images as PNG files, stored without compression: 8 bit
// sRGB, or 16 bit HDR

#include "mem.h"
#include "png.h"

#include <stdio.h>
#include <string.h>

static uint32_t	png_crctable[256];

static void PNG_InitCRC (void)
{
	uint32_t	c;
	int			n, k;

	for (n = 0 ; n < 256 ; n++)
	{
		c = (uint32_t)n;
		for (k = 0 ; k < 8 ; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		png_crctable[n] = c;
	}
}

static uint32_t PNG_CRC (uint32_t crc, const byte *data, size_t len)
{
	while (len--)
		crc = png_crctable[(crc ^ *data++) & 0xff] ^ (crc >> 8);
	return crc;
}

static void PNG_PutBE (byte *p, uint32_t v)
{
	p[0] = (byte)(v >> 24);
	p[1] = (byte)(v >> 16);
	p[2] = (byte)(v >> 8);
	p[3] = (byte)v;
}

// a chunk's length, type, data and CRC
static void PNG_Chunk (FILE *f, const char *type, const byte *data, uint32_t len)
{
	byte		head[8], tail[4];
	uint32_t	crc;

	PNG_PutBE (head, len);
	memcpy (head + 4, type, 4);
	crc = PNG_CRC (0xffffffffu, head + 4, 4);
	crc = PNG_CRC (crc, data, len);
	PNG_PutBE (tail, crc ^ 0xffffffffu);
	fwrite (head, 1, 8, f);
	fwrite (data, 1, len, f);
	fwrite (tail, 1, 4, f);
}

/*
==============
PNG_Write

An RGB image of 8 or 16 bit channels, its color space named by a cICP chunk
when given. The image data is a zlib stream of stored deflate blocks: raw's
rows, each a filter byte (none) and the row's pixels.
==============
*/
static bool PNG_Write (const char *path, int width, int height, int depth, const byte cicp[4], const byte *raw,
	size_t rawsize)
{
	static const byte	signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	byte		header[13];
	byte		*z, *out;
	size_t		zsize, done, block;
	uint32_t	a = 1, b = 0;
	FILE		*f;

	if (!png_crctable[1])
		PNG_InitCRC ();

	zsize = 2 + rawsize + (rawsize / 65535 + 1) * 5 + 4;
	z = out = Mem_Alloc (zsize);
	*out++ = 0x78;		// deflate, 32K window
	*out++ = 0x01;		// no dictionary, fastest
	done = 0;
	do
	{
		block = rawsize - done > 65535 ? 65535 : rawsize - done;
		*out++ = done + block == rawsize;	// BFINAL, BTYPE 00 (stored)
		*out++ = (byte)block;
		*out++ = (byte)(block >> 8);
		*out++ = (byte)~block;
		*out++ = (byte)(~block >> 8);
		memcpy (out, raw + done, block);
		out += block;
		done += block;
	} while (done < rawsize);
	for (done = 0 ; done < rawsize ; done++)
	{
		a = (a + raw[done]) % 65521;
		b = (b + a) % 65521;
	}
	PNG_PutBE (out, (b << 16) | a);
	out += 4;

	PNG_PutBE (header, (uint32_t)width);
	PNG_PutBE (header + 4, (uint32_t)height);
	header[8] = (byte)depth;	// bits per channel
	header[9] = 2;		// RGB
	header[10] = header[11] = header[12] = 0;	// deflate, adaptive filtering, no interlace

	f = fopen (path, "wb");
	if (f)
	{
		fwrite (signature, 1, sizeof(signature), f);
		PNG_Chunk (f, "IHDR", header, sizeof(header));
		if (cicp)
			PNG_Chunk (f, "cICP", cicp, 4);
		PNG_Chunk (f, "IDAT", z, (uint32_t)(out - z));
		PNG_Chunk (f, "IEND", NULL, 0);
		fclose (f);
	}
	Mem_Free (z);
	return f != NULL;
}

bool PNG_WriteRGB (const char *path, int width, int height, const byte *rgb, int rowbytes)
{
	size_t	rowsize = 1 + (size_t)width * 3, rawsize = (size_t)height * rowsize;
	byte	*raw = Mem_Alloc (rawsize);
	bool	written;
	int		y;

	for (y = 0 ; y < height ; y++)
	{
		raw[y * rowsize] = 0;
		memcpy (raw + y * rowsize + 1, rgb + (size_t)y * rowbytes, (size_t)width * 3);
	}
	written = PNG_Write (path, width, height, 8, NULL, raw, rawsize);
	Mem_Free (raw);
	return written;
}

bool PNG_WriteRGB16PQ (const char *path, int width, int height, const uint16_t *rgb)
{
	// cICP: BT.2020's primaries (9), PQ (16), RGB (0), full range (1)
	static const byte	cicp[4] = {9, 16, 0, 1};
	size_t	rowsize = 1 + (size_t)width * 6, rawsize = (size_t)height * rowsize, i;
	byte	*raw = Mem_Alloc (rawsize), *p;
	bool	written;
	int		y;

	for (y = 0 ; y < height ; y++)
	{
		p = raw + y * rowsize;
		*p++ = 0;
		for (i = 0 ; i < (size_t)width * 3 ; i++, rgb++)
		{
			*p++ = (byte)(*rgb >> 8);		// big-endian
			*p++ = (byte)*rgb;
		}
	}
	written = PNG_Write (path, width, height, 16, cicp, raw, rawsize);
	Mem_Free (raw);
	return written;
}
