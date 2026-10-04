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
// png.h -- PNG images

#include "q_types.h"

// 8 bit RGB, rows rowbytes apart; false if the file can't be written
bool	PNG_WriteRGB (const char *path, int width, int height, const byte *rgb, int rowbytes);
// HDR: 16 bit RGB, PQ in BT.2020 (a cICP chunk says so), rows width pixels apart
bool	PNG_WriteRGB16PQ (const char *path, int width, int height, const uint16_t *rgb);
