// snd_pipewire.c -- sound output through PipeWire
//
// The mixer paints 16-bit stereo into a ring buffer ahead of a read position, as it
// did with DMA hardware. The stream's process callback copies from the ring on
// PipeWire's real-time thread and advances the read position, taking no locks and
// allocating nothing, as snd_coreaudio.c's render callback does. The stream asks
// for a short quantum (node.latency), and follows the default output; PipeWire
// converts from the ring's rate if the graph runs at another.

#include "mem.h"
#include "print.h"
#include "sound.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <string.h>

#define RING_FRAMES		32768			// power of two; about 0.7 s at 48 kHz
#define FRAME_BYTES		4				// 16-bit stereo
#define SND_RATE		48000
#define SND_QUANTUM		"256/48000"		// frames a callback, about 5 ms

static byte					*snd_ring;
static _Atomic uint32_t		snd_readframe;		// next ring frame the device gets
static atomic_bool			snd_silenced;		// the window is inactive
static struct pw_thread_loop	*snd_loop;
static struct pw_stream		*snd_stream;
static enum pw_stream_state	snd_state;			// the stream's, under the loop's lock
static const char			*snd_error;

static void SND_Process (void *data)
{
	struct pw_buffer	*b;
	struct spa_data		*d;
	byte				*out;
	uint32_t			frames, read, pos, n, left;

	(void)data;
	b = pw_stream_dequeue_buffer (snd_stream);
	if (!b)
		return;
	d = &b->buffer->datas[0];
	out = d->data;
	if (!out)
	{
		pw_stream_queue_buffer (snd_stream, b);
		return;
	}
	frames = d->maxsize / FRAME_BYTES;
	if (b->requested && b->requested < frames)
		frames = (uint32_t)b->requested;
	read = atomic_load_explicit (&snd_readframe, memory_order_relaxed);

	if (atomic_load_explicit (&snd_silenced, memory_order_relaxed))
		memset (out, 0, (size_t)frames * FRAME_BYTES);
	else
	{
		for (pos = read, left = frames ; left ; left -= n, pos += n)
		{
			n = RING_FRAMES - (pos & (RING_FRAMES - 1));
			if (n > left)
				n = left;
			memcpy (out, snd_ring + (size_t)(pos & (RING_FRAMES - 1)) * FRAME_BYTES, (size_t)n * FRAME_BYTES);
			out += (size_t)n * FRAME_BYTES;
		}
	}
	d->chunk->offset = 0;
	d->chunk->stride = FRAME_BYTES;
	d->chunk->size = frames * FRAME_BYTES;
	pw_stream_queue_buffer (snd_stream, b);

	atomic_store_explicit (&snd_readframe, read + frames, memory_order_release);
}

static void SND_StateChanged (void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	(void)data;
	(void)old;
	snd_state = state;
	snd_error = error;
	pw_thread_loop_signal (snd_loop, false);
}

static const struct pw_stream_events	snd_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = SND_StateChanged,
	.process = SND_Process,
};

/*
===============================================================================

BACKEND CONTRACT

===============================================================================
*/

bool SNDDMA_Init (dma_t *dma)
{
	byte					podbuffer[1024];
	struct spa_pod_builder	builder = SPA_POD_BUILDER_INIT (podbuffer, sizeof(podbuffer));
	const struct spa_pod	*params[1];
	struct spa_audio_info_raw	info = {.format = SPA_AUDIO_FORMAT_S16, .rate = SND_RATE, .channels = 2,
		.position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}};
	int						waited;

	pw_init (NULL, NULL);
	snd_ring = Mem_Calloc (RING_FRAMES, FRAME_BYTES);
	atomic_store (&snd_readframe, 0);

	snd_loop = pw_thread_loop_new ("softworld-sound", NULL);
	if (!snd_loop)
	{
		Con_Printf ("PipeWire: couldn't start\n");
		SNDDMA_Shutdown ();
		return false;
	}
	snd_stream = pw_stream_new_simple (pw_thread_loop_get_loop (snd_loop), "SoftWorld",
		pw_properties_new (
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Game",
			PW_KEY_APP_NAME, "SoftWorld",
			PW_KEY_NODE_NAME, "softworld",
			PW_KEY_NODE_LATENCY, SND_QUANTUM,
			NULL),
		&snd_events, NULL);
	if (!snd_stream)
	{
		Con_Printf ("PipeWire: couldn't make a stream\n");
		SNDDMA_Shutdown ();
		return false;
	}
	params[0] = spa_format_audio_raw_build (&builder, SPA_PARAM_EnumFormat, &info);

	// connected, and running or waiting for the graph; or not, within a second
	snd_state = PW_STREAM_STATE_CONNECTING;
	pw_thread_loop_lock (snd_loop);
	if (pw_thread_loop_start (snd_loop) < 0
		|| pw_stream_connect (snd_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0)
		snd_state = PW_STREAM_STATE_ERROR;
	for (waited = 0 ; waited < 10 && (snd_state == PW_STREAM_STATE_CONNECTING
		|| snd_state == PW_STREAM_STATE_UNCONNECTED) ; waited++)
		pw_thread_loop_timed_wait (snd_loop, 1);
	pw_thread_loop_unlock (snd_loop);
	if (snd_state == PW_STREAM_STATE_ERROR || snd_state == PW_STREAM_STATE_UNCONNECTED
		|| snd_state == PW_STREAM_STATE_CONNECTING)
	{
		Con_Printf ("PipeWire: no sound output%s%s\n", snd_error ? ": " : "", snd_error ? snd_error : "");
		SNDDMA_Shutdown ();
		return false;
	}

	memset (dma, 0, sizeof(*dma));
	dma->channels = 2;
	dma->samplebits = 16;
	dma->speed = SND_RATE;
	dma->samples = RING_FRAMES * 2;
	dma->submission_chunk = 1;
	dma->buffer = snd_ring;
	dma->soundalive = true;
	dma->gamealive = true;

	Con_Printf ("PipeWire sound: %d Hz, %s frames a period\n", SND_RATE, SND_QUANTUM);
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	return (int)(atomic_load_explicit (&snd_readframe, memory_order_acquire) * 2) & (RING_FRAMES * 2 - 1);
}

void *SNDDMA_LockBuffer (void)
{
	return snd_ring;
}

void SNDDMA_UnlockBuffer ([[maybe_unused]] void *buffer)
{
}

void SNDDMA_Submit (void)
{
}

void SNDDMA_Shutdown (void)
{
	if (snd_loop)
		pw_thread_loop_stop (snd_loop);
	if (snd_stream)
		pw_stream_destroy (snd_stream);
	if (snd_loop)
		pw_thread_loop_destroy (snd_loop);
	snd_stream = NULL;
	snd_loop = NULL;
	Mem_Free (snd_ring);
	snd_ring = NULL;
}

/*
==================
SNDDMA_SetBlocked

While blocked (the window is inactive) the device gets silence, and the read
position keeps moving so the mixer catches up when unblocked.
==================
*/
void SNDDMA_SetBlocked (bool blocked)
{
	atomic_store (&snd_silenced, blocked);
}
