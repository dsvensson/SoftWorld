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
// markup.h -- the colors ezQuake and FTE write into text

#pragma once

#include "q_types.h"

// a character's color: 0 is none, the glyph as it is; else TEXT_TINT and four
// bits each of red, green and blue the glyph is multiplied by. TEXT_HALF
// draws it half transparent, TEXT_BLINK every other half second.
#define	TEXT_TINT		0x8000
#define	TEXT_HALF		0x4000
#define	TEXT_BLINK		0x2000
#define	TEXT_RGBMASK	0x0fff

static inline uint16_t TEXT_RGB (int r, int g, int b)
{
	return (uint16_t)(TEXT_TINT | r << 8 | g << 4 | b);
}

typedef struct
{
	uint16_t	color;			// of the characters that follow
	int			alt;			// 128 while FTE's ^a has the other charset on
	int			depth;			// FTE's ^s pushes, ^r pops
	uint16_t	stackcolor[4];
	int			stackalt[4];
} markup_t;

void Markup_Begin (markup_t *m);

// the next character of *s, which is moved past it, or -1 at its end; the
// codes before it are read into m
int Markup_Next (const char **s, markup_t *m);

// how many characters s shows
int Markup_Length (const char *s);

// how long the code s starts with is, 0 for none
int Markup_CodeLength (const char *s);
