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
// q_endian.h -- conversion between host byte order and the little-endian file and
// network formats. The supported targets are little-endian.

#include <stdint.h>
#include <string.h>

static inline short LittleShort (short l)
{
	return l;
}

static inline int LittleLong (int l)
{
	return l;
}

static inline float LittleFloat (float l)
{
	return l;
}

static inline short BigShort (short l)
{
	return (short)(((l & 0xff) << 8) | ((l >> 8) & 0xff));
}

static inline int BigLong (int l)
{
	uint32_t	u = (uint32_t)l;

	return (int)((u >> 24) | ((u >> 8) & 0xff00) | ((u << 8) & 0xff0000) | (u << 24));
}

static inline float BigFloat (float l)
{
	int		i;

	memcpy (&i, &l, sizeof(i));
	i = BigLong (i);
	memcpy (&l, &i, sizeof(l));
	return l;
}
