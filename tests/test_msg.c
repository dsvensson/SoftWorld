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
// test_msg.c -- coordinates and angles through a sized buffer, in each
// encoding: sizes on the wire, and what reads back

#include "msg.h"
#include "sys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int	failures;

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}

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

static void Check (bool ok, const char *what, double got, double want)
{
	if (ok)
		return;
	printf ("FAIL %s: got %.9g, want %.9g\n", what, got, want);
	failures++;
}

// coordinates: 1/8 unit shorts, truncated toward zero, or floats as they are
static void TestCoords (bool floatcoords)
{
	static const float	values[] = {0, 1, -1, 0.125f, -0.125f, 0.2f, -0.2f, 4095.875f, -4096,
		1234.5678f, -3000.0625f, 5000, -70000.25f, 1e6f};
	byte		data[256];
	sizebuf_t	buf = {.data = data, .maxsize = sizeof(data), .floatcoords = floatcoords};
	int			i, n;
	float		want, got;

	n = (int)(sizeof(values) / sizeof(values[0]));
	if (!floatcoords)
		n -= 4;			// past the short range
	for (i=0 ; i<n ; i++)
		MSG_WriteCoord (&buf, values[i]);
	Check (buf.cursize == n * (floatcoords ? 4 : 2), "coord size", buf.cursize, n * (floatcoords ? 4 : 2));

	MSG_BeginReading (&buf);
	for (i=0 ; i<n ; i++)
	{
		got = MSG_ReadCoord ();
		want = floatcoords ? values[i] : (int)(values[i] * 8) / 8.0f;
		Check (got == want, floatcoords ? "float coord" : "short coord", got, want);
	}
	Check (!msg_badread, "coords read past the end", 0, 0);
}

// angles: a byte of 360/256 or a short of 360/65536 degrees, wrapping
static void TestAngles (bool floatcoords)
{
	static const float	values[] = {0, 1.40625f, 45, 90, 180, 270, 359, -90, 123.456f, 720.5f};
	byte		data[256];
	sizebuf_t	buf = {.data = data, .maxsize = sizeof(data), .floatcoords = floatcoords};
	int			i, n, steps;
	float		got, want, step;

	n = (int)(sizeof(values) / sizeof(values[0]));
	for (i=0 ; i<n ; i++)
		MSG_WriteAngle (&buf, values[i]);
	Check (buf.cursize == n * (floatcoords ? 2 : 1), "angle size", buf.cursize, n * (floatcoords ? 2 : 1));

	steps = floatcoords ? 65536 : 256;
	step = 360.0f / steps;
	MSG_BeginReading (&buf);
	for (i=0 ; i<n ; i++)
	{
		got = MSG_ReadAngle ();
		// the reader gives signed angles, -180 .. 180
		want = (float)(((int)(values[i] * steps / 360) & (steps - 1)) * (double)step);
		if (want >= 180)
			want -= 360;
		Check (fabsf (got - want) < 1e-4f, floatcoords ? "angle16" : "angle8", got, want);
	}
}

// the reader follows the buffer being read, not the one last written
static void TestReaderFollowsBuffer (void)
{
	byte		d1[16], d2[16];
	sizebuf_t	shorts = {.data = d1, .maxsize = sizeof(d1)};
	sizebuf_t	floats = {.data = d2, .maxsize = sizeof(d2), .floatcoords = true};
	float		got;

	MSG_WriteCoord (&floats, 5000.25f);
	MSG_WriteCoord (&shorts, 100.5f);

	MSG_BeginReading (&floats);
	got = MSG_ReadCoord ();
	Check (got == 5000.25f, "float buffer read", got, 5000.25f);
	MSG_BeginReading (&shorts);
	got = MSG_ReadCoord ();
	Check (got == 100.5f, "short buffer read", got, 100.5f);
}

int main (void)
{
	TestCoords (false);
	TestCoords (true);
	TestAngles (false);
	TestAngles (true);
	TestReaderFollowsBuffer ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("msg: all encodings round-trip\n");
	return 0;
}
