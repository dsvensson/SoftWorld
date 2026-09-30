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

#pragma once
// tga.h -- Truevision TGA images

#include "q_types.h"

#define	TGA_MAXSIZE		8192		// the widest and tallest image read

// a TGA file of len bytes as 8 bit RGBA, top row first, from Mem_Alloc; NULL if
// it isn't an image this reads, with why in *error (if error isn't NULL).
// Truecolor (15, 16, 24 and 32 bit), greyscale (8 bit, and 16 with alpha) and
// color-mapped (8 bit indices) images are read, plain or run-length encoded,
// stored in any corner.
byte	*TGA_Decode (const byte *data, int len, int *width, int *height, const char **error);
