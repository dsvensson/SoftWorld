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
// test_markup.c -- ezQuake's and FTE's colors read out of text: what shows,
// and in which color

#include "markup.h"

#include <stdio.h>
#include <string.h>

static int	failures;

/*
==================
Check

in, shown with markup read: the characters want, and the color of each as
colors (a string of letters: n none, then see Color)
==================
*/
static char Color (uint16_t c)
{
	if (c & TEXT_BLINK)
		return 'b';
	if (c & TEXT_HALF)
		return 'h';
	switch (c)
	{
	case 0:							return 'n';
	case TEXT_TINT | 0xf55:			return 'r';		// FTE's ^1
	case TEXT_TINT | 0x5f5:			return 'g';		// ^2
	case TEXT_TINT | 0xf13:			return 'k';		// KTX's &cF13
	case TEXT_TINT | 0x0af:			return 'x';		// ^x0af
	case TEXT_TINT | 0xaaa:			return 'y';		// ^9, grey
	case TEXT_TINT | 0x000:			return '0';		// ^0, black
	}
	return '?';
}

static void Check (const char *in, const char *want, const char *colors)
{
	char		got[256], gotcolors[256];
	const char	*s = in;
	markup_t	m;
	int			c, n;

	Markup_Begin (&m);
	for (n = 0 ; (c = Markup_Next (&s, &m)) >= 0 && n < 255 ; n++)
	{
		got[n] = (char)c;
		gotcolors[n] = Color (m.color);
	}
	got[n] = gotcolors[n] = 0;
	if (strcmp (got, want) || strcmp (gotcolors, colors) || Markup_Length (in) != (int)strlen (want))
	{
		printf ("FAIL \"%s\": \"%s\" %s, want \"%s\" %s\n", in, got, gotcolors, want, colors);
		failures++;
	}
}

int main (void)
{
	// ezQuake
	Check ("plain", "plain", "nnnnn");
	Check ("{&cF13RL&cFFF}", "{RL}", "nkkn");
	Check ("&cf13a&rb", "ab", "kn");
	Check ("R&D &&", "R&D &&", "nnnnnn");	// not codes
	Check ("&cxyz&cf0", "&cxyz&cf0", "nnnnnnnnn");
	Check ("&c", "&c", "nn");

	// FTE
	Check ("^1red^7white", "redwhite", "rrrnnnnn");
	Check ("^2a^9b^0c", "abc", "gy0");
	Check ("^x0afq^dz", "qz", "xn");
	Check ("^8half^1", "half", "hhhh");
	Check ("^bblink^b.", "blink.", "bbbbbn");
	Check ("^s^1a^rb", "ab", "rn");
	Check ("^^1", "^1", "nn");				// ^^ is a ^
	Check ("x^", "x^", "nn");
	Check ("^z^[", "^z^[", "nnnn");			// no such codes
	Check ("^&C-c^&--d", "cd", "rn");		// FTE's CGA 12, light red
	Check ("^&c-", "^&c-", "nnnn");			// lower case is no code

	// the other charset
	{
		const char	*s = "^aA^aB";
		markup_t	m;

		Markup_Begin (&m);
		if (Markup_Next (&s, &m) != ('A' | 128) || Markup_Next (&s, &m) != 'B' || Markup_Next (&s, &m) != -1)
		{
			printf ("FAIL ^a\n");
			failures++;
		}
	}
	{
		// a gold 0 under ^a is character 0, not the end
		const char	*s = "^a\x80";
		markup_t	m;

		Markup_Begin (&m);
		if (Markup_Next (&s, &m) != 0 || Markup_Next (&s, &m) != -1)
		{
			printf ("FAIL ^a at 128\n");
			failures++;
		}
	}

	printf ("%s: %i failures\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
