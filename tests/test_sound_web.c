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
// test_sound_web.c -- the web's sound output in a browser (emrun) that lets sound
// play without a gesture: snd_webaudio.c with its AudioWorklet (snd_worklet.js)
// playing the ring from the page's memory. A ring of a square wave at half scale
// plays: the read position goes on at the output's rate, the loudest sample the
// worklet tells is the wave's (the ring's place, its samples' format and byte
// order as the worklet takes them), and what it played is cleared. Blocked, it
// plays silence while the position goes on. Without Web Audio it is skipped.

#include "snd_webaudio.c"

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define SKIPPED		77
#define AMPLITUDE	16384

static int		failures;
static dma_t	dma;
static int		step;
static double	started;
static uint32_t	startframe;

//
// the engine, as far as snd_webaudio.c needs it
//

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

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}

// sys_web.js's, which the test has none of
void web_notice (const char *slot, const char *text)
{
	(void)slot;
	(void)text;
}

static void Finish (void)
{
	SNDDMA_Shutdown ();
	if (failures)
		printf ("%d failures\n", failures);
	else
		printf ("sound_web: the worklet plays the ring\n");
	emscripten_force_exit (failures ? 1 : 0);
}

// a 1 kHz square, half scale, over the whole ring
static void Paint (void)
{
	int		i, period = dma.speed / 1000;

	for (i = 0 ; i < RING_FRAMES ; i++)
		snd_ring[i * 2] = snd_ring[i * 2 + 1] = (int16_t)((i / (period / 2)) & 1 ? AMPLITUDE : -AMPLITUDE);
}

static void Step (void *unused)
{
	double		now = emscripten_get_now () / 1000, rate;
	uint32_t	frames, read, i;
	int			zeros;

	(void)unused;
	switch (step)
	{
	case 0:		// the worklet loaded and the context running
		if (web_snd_state () == SND_FAILED)
		{
			printf ("sound_web: the worklet didn't load\n");
			failures++;
			Finish ();
		}
		if (web_snd_state () != SND_PLAYING)
		{
			if (now - started > 10)
			{
				printf ("sound_web: the context never ran (the browser asks for a gesture)\n");
				SNDDMA_Shutdown ();
				emscripten_force_exit (SKIPPED);
			}
			break;
		}
		Paint ();
		started = now;
		startframe = atomic_load (&snd_shared.readframe);
		step++;
		break;

	case 1:		// a second and a half of it: the second the worklet counts the peak of
		if (now - started < 1.5)
			break;
		frames = atomic_load (&snd_shared.readframe) - startframe;
		rate = frames / (now - started);
		printf ("sound_web: %u frames in %.2f s, %.0f a second at %d Hz\n", frames, now - started, rate, dma.speed);
		if (rate < dma.speed * 0.8 || rate > dma.speed * 1.2)
			failures++;
		printf ("sound_web: the loudest sample %u, the wave's %d\n", atomic_load (&snd_shared.peak), AMPLITUDE);
		if (atomic_load (&snd_shared.peak) < AMPLITUDE - 1 || atomic_load (&snd_shared.peak) > AMPLITUDE + 1)
			failures++;
		// what was played (and the ring's once round, more than a second's) is cleared
		read = atomic_load (&snd_shared.readframe);
		for (zeros = 0, i = 1 ; i <= 1024 ; i++)
			zeros += !snd_ring[((read - i) & (RING_FRAMES - 1)) * 2];
		printf ("sound_web: %d of the 1024 frames played last cleared\n", zeros);
		if (zeros != 1024)
			failures++;
		Paint ();
		SNDDMA_SetBlocked (true);
		started = now;
		startframe = read;
		step++;
		break;

	case 2:		// blocked: silence, and the position going on
		if (now - started < 2.2)
			break;
		frames = atomic_load (&snd_shared.readframe) - startframe;
		printf ("sound_web: blocked, %u frames in %.2f s, the loudest sample %u\n", frames, now - started,
			atomic_load (&snd_shared.peak));
		if (frames < dma.speed * 0.8 * (now - started) || atomic_load (&snd_shared.peak))
			failures++;
		Finish ();
	}
	emscripten_set_timeout (Step, 50, NULL);
}

int main (void)
{
	if (!SNDDMA_Init (&dma))
	{
		printf ("sound_web: no Web Audio, skipped\n");
		return SKIPPED;
	}
	if (dma.samples != RING_FRAMES * 2 || dma.samplebits != 16 || dma.channels != 2)
		failures++;
	started = emscripten_get_now () / 1000;
	emscripten_set_timeout (Step, 50, NULL);
	emscripten_exit_with_live_runtime ();
}
