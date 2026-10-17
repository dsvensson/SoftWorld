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
// test_glob.c -- Q_GlobMatch, the menu's searches' patterns: '*' and '?'
// within a directory, the rest as it is, case aside

#include "q_string.h"

#include <stdio.h>

static int	failures;

static void Expect (const char *pattern, const char *text, bool want)
{
	if (Q_GlobMatch (pattern, text) == want)
		return;
	printf ("FAIL: \"%s\" %s \"%s\"\n", pattern, want ? "should match" : "shouldn't match", text);
	failures++;
}

int main (void)
{
	Expect ("*.mvd", "foo.mvd", true);
	Expect ("*.mvd", "FOO.MVD", true);
	Expect ("*.mvd", "demos/foo.mvd", false);		// not into a directory
	Expect ("demos/*.mvd", "demos/foo.mvd", true);
	Expect ("demos/*.mvd", "Demos/x.Mvd", true);
	Expect ("demos/*.mvd", "foo.mvd", false);
	Expect ("*.mvd", "a.mvd.gz", false);
	Expect ("*.mvd.gz", "a.mvd.gz", true);
	Expect ("*.gz", "a.mvd.gz", true);
	Expect ("?.qwd", "a.qwd", true);
	Expect ("?.qwd", "ab.qwd", false);
	Expect ("a?b", "a/b", false);
	Expect ("a*b*c", "axxbyyc", true);
	Expect ("a*b*c", "axxbyy", false);
	Expect ("*a", "bba", true);
	Expect ("*", "", true);
	Expect ("*", "anything", true);
	Expect ("*", "dir/file", false);
	Expect ("", "", true);
	Expect ("", "a", false);
	Expect ("x", "", false);
	Expect ("**.qwd", "x.qwd", true);
	Expect ("*[ab]*", "x[ab]y", true);
	if (failures)
	{
		printf ("%d failed\n", failures);
		return 1;
	}
	printf ("glob: all passed\n");
	return 0;
}
