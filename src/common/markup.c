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
// markup.c -- the colors ezQuake and FTE write into text
//
// ezQuake: &cRGB colors what follows, a hex digit per channel; &r goes back to
// no color, and so does &cfff, the way every KTX message ends its runs
// (qualia). FTE: ^0 .. ^9 are Quake 3's colors, ^xRGB is one of the hex ones,
// ^&FB a foreground and background of the CGA sixteen (the background isn't
// drawn), ^a and ^m switch to the other charset and back, ^h half
// transparency, ^b blinking, ^s and ^r push and pop all that, ^d goes back to
// the start, ^^ is a ^. Anything else is text: an & or a ^ that starts no code
// is shown.

#include "markup.h"

// FTE's console colors, the CGA sixteen, in four bits a channel
static const byte	cga[16][3] = {
	{0, 0, 0}, {0, 0, 10}, {0, 10, 0}, {0, 10, 10}, {10, 0, 0}, {10, 0, 10}, {10, 5, 0}, {10, 10, 10},
	{5, 5, 5}, {5, 5, 15}, {5, 15, 5}, {5, 15, 15}, {15, 5, 5}, {15, 5, 15}, {15, 15, 5}, {15, 15, 15}
};

// Quake 3's ^0 .. ^9 as FTE draws them: black, red, green, yellow, blue,
// cyan, magenta, white, half transparent white and grey
static const byte	q3colors[10] = {0, 12, 10, 14, 9, 11, 13, 15, 15, 7};

static int Markup_Hex (int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

// white is no color: the glyph as it is
static uint16_t Markup_Color (int r, int g, int b)
{
	return r == 15 && g == 15 && b == 15 ? 0 : TEXT_RGB (r, g, b);
}

static uint16_t Markup_CGA (int i)
{
	return Markup_Color (cga[i][0], cga[i][1], cga[i][2]);
}

// FTE's ^&: an upper case hex digit, or - for the default
static bool Markup_Extended (int c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || c == '-';
}

void Markup_Begin (markup_t *m)
{
	m->color = 0;
	m->alt = 0;
	m->depth = 0;
}

/*
==================
Markup_Code

The code at p read into m: how many characters it takes, 0 for none
==================
*/
static int Markup_Code (const char *p, markup_t *m)
{
	int		r, g, b;

	if (p[0] == '&')
	{
		if (p[1] == 'c' && (r = Markup_Hex (p[2])) >= 0 && (g = Markup_Hex (p[3])) >= 0 && (b = Markup_Hex (p[4])) >= 0)
		{
			m->color = Markup_Color (r, g, b);
			return 5;
		}
		if (p[1] == 'r')
		{
			m->color = 0;
			return 2;
		}
		return 0;
	}
	if (p[0] != '^')
		return 0;

	switch (p[1])
	{
	case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
		m->color = (m->color & TEXT_BLINK) | Markup_CGA (q3colors[p[1] - '0']) | (p[1] == '8' ? TEXT_HALF : 0);
		return 2;
	case 'x':
		if ((r = Markup_Hex (p[2])) < 0 || (g = Markup_Hex (p[3])) < 0 || (b = Markup_Hex (p[4])) < 0)
			return 0;
		m->color = (m->color & TEXT_BLINK) | Markup_Color (r, g, b);
		return 5;
	case '&':
		if (!Markup_Extended (p[2]) || !Markup_Extended (p[3]))
			return 0;
		m->color = (m->color & (TEXT_BLINK | TEXT_HALF)) | (p[2] == '-' ? 0 : Markup_CGA (Markup_Hex (p[2])));
		return 4;
	case 'a':
	case 'm':
		m->alt ^= 128;
		return 2;
	case 'h':
		m->color ^= TEXT_HALF;
		return 2;
	case 'b':
		m->color ^= TEXT_BLINK;
		return 2;
	case 'd':
		m->color = 0;
		m->alt = 0;
		return 2;
	case 's':
		if (m->depth < (int)(sizeof(m->stackcolor)/sizeof(m->stackcolor[0])))
		{
			m->stackcolor[m->depth] = m->color;
			m->stackalt[m->depth] = m->alt;
			m->depth++;
		}
		return 2;
	case 'r':
		if (m->depth)
		{
			m->depth--;
			m->color = m->stackcolor[m->depth];
			m->alt = m->stackalt[m->depth];
		}
		return 2;
	}
	return 0;
}

int Markup_Next (const char **s, markup_t *m)
{
	const char	*p = *s;
	int			n, c;

	while ((n = Markup_Code (p, m)))
		p += n;
	if (p[0] == '^' && p[1] == '^')
		p++;		// the ^ is the character
	c = (byte)*p;
	if (!c)
		return -1;
	*s = p + 1;
	return c ^ m->alt;
}

int Markup_CodeLength (const char *s)
{
	markup_t	m;

	Markup_Begin (&m);
	return Markup_Code (s, &m);
}

int Markup_Length (const char *s)
{
	markup_t	m;
	int			n;

	Markup_Begin (&m);
	for (n = 0 ; Markup_Next (&s, &m) >= 0 ; n++)
		;
	return n;
}
