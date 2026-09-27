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
// test_msg.c -- coordinates, angles and entity deltas through a sized
// buffer, in each encoding and with each protocol extension: sizes on the
// wire, and what reads back

#include "msg.h"
#include "protocol.h"
#include "sys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// an entity delta from nothing, written and read back with some extensions
static void TestDelta (const char *what, const entity_state_t *to, unsigned fteext, unsigned mvdext1, bool floatcoords)
{
	static const entity_state_t	nullstate = {0};
	byte			data[256];
	sizebuf_t		buf = {.data = data, .maxsize = sizeof(data), .floatcoords = floatcoords};
	entity_state_t	got;
	int				word, num, bits, ext, i;
	bool			same;

	MSG_WriteDeltaEntity (&buf, &nullstate, to, true, fteext, mvdext1);
	MSG_BeginReading (&buf);
	word = MSG_ReadShort () & 0xffff;
	num = MSG_ReadEntityHeader (word, &bits, &ext, fteext);
	MSG_ReadDeltaEntity (&nullstate, &got, num, bits, ext, mvdext1);

	same = got.number == to->number && got.modelindex == to->modelindex && got.frame == to->frame &&
		got.skinnum == to->skinnum && got.effects == to->effects && got.colormap == to->colormap;
	for (i=0 ; i<3 ; i++)
		same = same && got.origin[i] == to->origin[i];
	if (fteext & FTE_PEXT_TRANS)
		same = same && got.alpha == (to->alpha ? to->alpha : 0);
	if (fteext & FTE_PEXT_COLOURMOD)
		same = same && !memcmp (got.colormod, to->colormod, 3);
	same = same && msg_readcount == buf.cursize && !msg_badread;
	if (!same)
	{
		printf ("FAIL delta %s: number %i/%i model %i/%i origin %g %g %g, read %i of %i bytes\n", what,
			got.number, to->number, got.modelindex, to->modelindex, got.origin[0], got.origin[1], got.origin[2],
			msg_readcount, buf.cursize);
		failures++;
	}
}

static void TestDeltas (void)
{
	unsigned		fte;
	entity_state_t	to = {.number = 100, .modelindex = 50, .frame = 3, .skinnum = 1, .effects = 4,
		.origin = {100.5f, -200.25f, 30}};
	byte			data[64];
	sizebuf_t		buf = {.data = data, .maxsize = sizeof(data)};
	entity_state_t	same;
	int				word, bits, ext, num;

	fte = FTE_PEXT_TRANS | FTE_PEXT_MODELDBL | FTE_PEXT_ENTITYDBL | FTE_PEXT_ENTITYDBL2 | FTE_PEXT_COLOURMOD;
	TestDelta ("vanilla", &to, 0, 0, false);
	TestDelta ("vanilla with FTE", &to, fte, 0, false);

	to.number = 700;			// + 512
	to.modelindex = 300;		// a byte + 256
	to.alpha = 127;
	to.colormod[0] = 16;
	to.colormod[1] = 32;
	to.colormod[2] = 64;
	TestDelta ("entity 700 model 300", &to, fte, 0, false);
	to.number = 1100;			// + 1024
	to.modelindex = 3000;		// a short
	TestDelta ("entity 1100 model 3000", &to, fte, 0, false);
	to.number = 2047;			// + 512 + 1024
	to.origin[0] = 5000.125f;
	TestDelta ("entity 2047, MVD1 float origin", &to, fte, MVD_PEXT1_FLOATCOORDS, false);
	TestDelta ("entity 2047, float coordinates", &to, fte | FTE_PEXT_FLOATCOORDS, 0, true);

	// nothing changed: nothing written, even where the number needs FTE's bits
	same = to;
	MSG_WriteDeltaEntity (&buf, &same, &to, false, fte, 0);
	Check (buf.cursize == 0, "unchanged entity 2047 writes nothing", buf.cursize, 0);

	// removing entities past 511 needs FTE's bits for the number
	MSG_WriteEntityRemove (&buf, 1600);
	MSG_WriteEntityRemove (&buf, 20);
	MSG_BeginReading (&buf);
	word = MSG_ReadShort () & 0xffff;
	num = MSG_ReadEntityHeader (word, &bits, &ext, fte);
	Check ((word & U_REMOVE) && num == 1600, "remove 1600", num, 1600);
	word = MSG_ReadShort () & 0xffff;
	num = MSG_ReadEntityHeader (word, &bits, &ext, fte);
	Check ((word & U_REMOVE) && num == 20, "remove 20", num, 20);
}

int main (void)
{
	TestCoords (false);
	TestCoords (true);
	TestAngles (false);
	TestAngles (true);
	TestReaderFollowsBuffer ();
	TestDeltas ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("msg: all encodings round-trip\n");
	return 0;
}
