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
// test_md4.c -- Com_BlockChecksum on RFC 1320's test strings: the four words of
// each digest xored, as servers compare map checksums with

#include "md4.h"

#include <stdio.h>
#include <string.h>

static const struct
{
	const char	*text;
	unsigned	checksum;
} tests[] = {
	{"", 0xc6f640b7},					// 31d6cfe0d16ae931b73c59d7e0c089c0
	{"abc", 0x5da10e2e},				// a448017aaf21d8525fc10ae87aa6729d
	{"message digest", 0x24dc0744},		// d9130a8164549fe818874806e1c7014b
};

int main (void)
{
	unsigned	checksum;
	int			failures = 0;

	for (size_t i = 0 ; i < sizeof(tests) / sizeof(tests[0]) ; i++)
	{
		checksum = Com_BlockChecksum (tests[i].text, (int)strlen (tests[i].text));
		if (checksum != tests[i].checksum)
		{
			printf ("\"%s\": 0x%08x, not 0x%08x\n", tests[i].text, checksum, tests[i].checksum);
			failures++;
		}
	}
	if (failures)
		return 1;
	printf ("md4: the checksums are RFC 1320's\n");
	return 0;
}
