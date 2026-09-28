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
// snd_coreaudio.c -- sound output through Core Audio
//
// The mixer paints 16-bit stereo into a ring buffer ahead of a read position, as it
// did with DMA hardware. The default output unit's render callback copies from the
// ring on Core Audio's real-time thread and advances the read position, taking no
// locks and allocating nothing. The unit follows the system's default device, and
// converts from the ring's rate if a device runs at another.

#include "mem.h"
#include "print.h"
#include "sound.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <string.h>

#define RING_FRAMES		32768			// power of two; about 0.7 s at 48 kHz
#define FRAME_BYTES		4				// 16-bit stereo

static byte					*snd_ring;
static _Atomic uint32_t		snd_readframe;		// next ring frame the device gets
static atomic_bool			snd_silenced;		// the window is inactive
static AudioComponentInstance	snd_unit;

static OSStatus SND_Render (void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
	UInt32 bus, UInt32 frames, AudioBufferList *data)
{
	byte		*out = data->mBuffers[0].mData;
	uint32_t	read, pos, n, left;

	(void)ref;
	(void)timestamp;
	(void)bus;
	if (frames > data->mBuffers[0].mDataByteSize / FRAME_BYTES)
		frames = data->mBuffers[0].mDataByteSize / FRAME_BYTES;
	read = atomic_load_explicit (&snd_readframe, memory_order_relaxed);

	if (atomic_load_explicit (&snd_silenced, memory_order_relaxed))
	{
		memset (out, 0, (size_t)frames * FRAME_BYTES);
		*flags |= kAudioUnitRenderAction_OutputIsSilence;
	}
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

	atomic_store_explicit (&snd_readframe, read + frames, memory_order_release);
	return noErr;
}

/*
===============================================================================

BACKEND CONTRACT

===============================================================================
*/

bool SNDDMA_Init (dma_t *dma)
{
	AudioComponentDescription	description = {
		.componentType = kAudioUnitType_Output,
		.componentSubType = kAudioUnitSubType_DefaultOutput,
		.componentManufacturer = kAudioUnitManufacturer_Apple,
	};
	AudioStreamBasicDescription	device, format;
	AURenderCallbackStruct		callback = {.inputProc = SND_Render};
	AudioComponent				component;
	UInt32						size = sizeof(device);
	int							rate;

	component = AudioComponentFindNext (NULL, &description);
	if (!component || AudioComponentInstanceNew (component, &snd_unit) != noErr)
	{
		snd_unit = NULL;
		Con_Printf ("Core Audio: no sound output device\n");
		return false;
	}

	// the ring at the device's rate, so the unit converts nothing
	rate = 48000;
	if (AudioUnitGetProperty (snd_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
			&device, &size) == noErr && device.mSampleRate >= 8000)
		rate = (int)device.mSampleRate;

	format = (AudioStreamBasicDescription){
		.mSampleRate = rate,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = FRAME_BYTES,
		.mFramesPerPacket = 1,
		.mBytesPerFrame = FRAME_BYTES,
		.mChannelsPerFrame = 2,
		.mBitsPerChannel = 16,
	};

	snd_ring = Mem_Calloc (RING_FRAMES, FRAME_BYTES);
	atomic_store (&snd_readframe, 0);
	if (AudioUnitSetProperty (snd_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
			&format, sizeof(format)) != noErr
		|| AudioUnitSetProperty (snd_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
			&callback, sizeof(callback)) != noErr
		|| AudioUnitInitialize (snd_unit) != noErr
		|| AudioOutputUnitStart (snd_unit) != noErr)
	{
		Con_Printf ("Core Audio: couldn't start the output\n");
		SNDDMA_Shutdown ();
		return false;
	}

	memset (dma, 0, sizeof(*dma));
	dma->channels = 2;
	dma->samplebits = 16;
	dma->speed = rate;
	dma->samples = RING_FRAMES * 2;
	dma->submission_chunk = 1;
	dma->buffer = snd_ring;
	dma->soundalive = true;
	dma->gamealive = true;

	Con_Printf ("Core Audio sound: %d Hz\n", rate);
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
	if (snd_unit)
	{
		AudioOutputUnitStop (snd_unit);
		AudioUnitUninitialize (snd_unit);
		AudioComponentInstanceDispose (snd_unit);
		snd_unit = NULL;
	}
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
