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
// snd_webaudio.c -- sound output in a browser page, through an AudioWorklet
// (snd_webaudio.js, snd_worklet.js)
//
// The mixer paints 16-bit stereo into a ring buffer ahead of a read position, as
// it did with DMA hardware. The ring is in the page's memory, which the
// worklet shares: on the browser's audio thread it takes its frames from the
// read position and moves it, as Core Audio's callback does (snd_coreaudio.c),
// at the rate the browser's output runs at. The ring and the words shared
// with the worklet are the program's own, never freed: a worklet still
// playing after SNDDMA_Shutdown reads nothing freed.

#include "cmd.h"
#include "print.h"
#include "sound.h"
#include "web_local.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define RING_FRAMES		32768			// power of two; about 0.7 s at 48 kHz

// snd_webaudio.js
int		web_snd_init (int16_t *ring, int frames, void *shared);
int		web_snd_state (void);
void	web_snd_error (char *buf, int size);
double	web_snd_latency (void);
void	web_snd_shutdown (void);

enum { SND_STARTING, SND_WAITING, SND_PLAYING, SND_FAILED };

static alignas(16) int16_t	snd_ring[RING_FRAMES * 2];
static alignas(16) struct
{
	_Atomic uint32_t	readframe;		// next ring frame the worklet takes
	_Atomic uint32_t	silenced;		// the page is inactive
	_Atomic uint32_t	peak;			// the loudest sample of the last second
	_Atomic uint32_t	seconds;		// seconds played
} snd_shared;

static bool		snd_open;
static int		snd_rate;
static int		snd_state = -1;			// as last told

static void SND_Info_f (void)
{
	static const char	*states[] = {"starting", "waiting for a click or a key", "playing", "failed"};
	uint32_t	peak = atomic_load (&snd_shared.peak);

	if (!snd_open)
	{
		Con_Printf ("No sound output\n");
		return;
	}
	Con_Printf ("Sound: %s, %d Hz, %.0f ms of output latency\n", states[web_snd_state ()], snd_rate,
		web_snd_latency ());
	Con_Printf ("Played: %u frames, %u s; the loudest of the last second %.1f dBFS\n",
		atomic_load (&snd_shared.readframe), atomic_load (&snd_shared.seconds),
		peak ? 20 * log10 (peak / 32768.0) : -INFINITY);
}

/*
===============================================================================

BACKEND CONTRACT

===============================================================================
*/

bool SNDDMA_Init (dma_t *dma)
{
	static bool	registered;
	char		why[256];

	if (!registered)
	{
		Cmd_AddCommand ("snd_info", SND_Info_f,
			"Prints the sound output's state, rate and latency, the frames played and how loud the last second was.");
		registered = true;
	}

	memset (snd_ring, 0, sizeof(snd_ring));
	atomic_store (&snd_shared.readframe, 0);
	snd_rate = web_snd_init (snd_ring, RING_FRAMES, &snd_shared);
	if (!snd_rate)
	{
		web_snd_error (why, sizeof(why));
		Con_Printf ("No sound: %s\n", why);
		return false;
	}
	snd_open = true;
	snd_state = -1;

	memset (dma, 0, sizeof(*dma));
	dma->channels = 2;
	dma->samplebits = 16;
	dma->speed = snd_rate;
	dma->samples = RING_FRAMES * 2;
	dma->submission_chunk = 1;
	dma->buffer = (byte *)snd_ring;
	dma->soundalive = true;
	dma->gamealive = true;

	Con_Printf ("Web Audio sound: %d Hz\n", snd_rate);
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	return (int)(atomic_load_explicit (&snd_shared.readframe, memory_order_acquire) * 2) & (RING_FRAMES * 2 - 1);
}

void *SNDDMA_LockBuffer (void)
{
	return snd_open ? snd_ring : NULL;
}

void SNDDMA_UnlockBuffer ([[maybe_unused]] void *buffer)
{
}

// tells when the sound starts, waits for the browser's gesture, or fails
void SNDDMA_Submit (void)
{
	int		state;
	char	why[256];

	if (!snd_open || (state = web_snd_state ()) == snd_state)
		return;
	snd_state = state;
	web_notice ("sound", state == SND_WAITING ? "Click or press a key for sound" : NULL);
	if (state == SND_WAITING)
		Con_Printf ("Sound waits for a click or a key (the browser's rule)\n");
	else if (state == SND_PLAYING)
		Con_Printf ("Sound: playing, %d Hz\n", snd_rate);
	else if (state == SND_FAILED)
	{
		web_snd_error (why, sizeof(why));
		Con_Printf ("No sound: %s\n", why);
	}
}

void SNDDMA_Shutdown (void)
{
	if (!snd_open)
		return;
	web_snd_shutdown ();
	web_notice ("sound", NULL);
	snd_open = false;
}

/*
==================
SNDDMA_SetBlocked

While blocked (the page is inactive) the worklet plays silence, and the read
position keeps moving so the mixer catches up when unblocked.
==================
*/
void SNDDMA_SetBlocked (bool blocked)
{
	atomic_store (&snd_shared.silenced, blocked);
}
