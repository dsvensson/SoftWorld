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
// snd_wasapi.c -- sound output through WASAPI shared mode
//
// The mixer paints 16-bit stereo into a ring buffer ahead of a read position, as it
// did with DMA hardware. An event-driven thread feeds the ring to the default render
// device and advances the read position. When the default device changes, the thread
// reopens the stream on the new one; the ring's format stays the same.

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include "mem.h"
#include "print.h"
#include "sound.h"

#include <string.h>

#define RING_FRAMES		32768			// power of two; about 0.7 s at 48 kHz
#define FRAME_BYTES		4				// 16-bit stereo
#define DEVICE_BUFFER	(20 * 10000)	// 20 ms, in 100 ns units

static const GUID sw_IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID sw_CLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID sw_IID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID sw_IID_IMMNotificationClient = {0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const GUID sw_IID_IAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const GUID sw_IID_IAudioRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

static byte				*snd_ring;
static volatile LONG	snd_readframe;		// next ring frame the device gets
static HANDLE			snd_thread;
static HANDLE			snd_quitevent;		// manual reset: stop the thread
static HANDLE			snd_changeevent;	// auto reset: the default device changed
static HANDLE			snd_bufferevent;	// auto reset: the device wants samples
static HANDLE			snd_readyevent;		// manual reset: first open attempt done
static int				snd_rate;			// the ring's sample rate
static bool				snd_opened;			// result of the first open
static volatile LONG	snd_silenced;		// the window is inactive

// owned by the sound thread
static IMMDeviceEnumerator	*snd_enumerator;
static IAudioClient			*snd_client;
static IAudioRenderClient	*snd_render;
static UINT32				snd_bufferframes;

/*
===============================================================================

DEFAULT DEVICE NOTIFICATIONS

===============================================================================
*/

static HRESULT STDMETHODCALLTYPE Notify_QueryInterface (IMMNotificationClient *self, REFIID riid, void **object)
{
	if (IsEqualIID (riid, &sw_IID_IUnknown) || IsEqualIID (riid, &sw_IID_IMMNotificationClient))
	{
		*object = self;
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

// the client is static, so reference counts don't matter
static ULONG STDMETHODCALLTYPE Notify_AddRef ([[maybe_unused]] IMMNotificationClient *self)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE Notify_Release ([[maybe_unused]] IMMNotificationClient *self)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE Notify_OnDeviceStateChanged ([[maybe_unused]] IMMNotificationClient *self,
	[[maybe_unused]] LPCWSTR id, [[maybe_unused]] DWORD state)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Notify_OnDeviceAdded ([[maybe_unused]] IMMNotificationClient *self,
	[[maybe_unused]] LPCWSTR id)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Notify_OnDeviceRemoved ([[maybe_unused]] IMMNotificationClient *self,
	[[maybe_unused]] LPCWSTR id)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Notify_OnDefaultDeviceChanged ([[maybe_unused]] IMMNotificationClient *self,
	EDataFlow flow, ERole role, [[maybe_unused]] LPCWSTR id)
{
	if (flow == eRender && role == eConsole)
		SetEvent (snd_changeevent);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Notify_OnPropertyValueChanged ([[maybe_unused]] IMMNotificationClient *self,
	[[maybe_unused]] LPCWSTR id, [[maybe_unused]] const PROPERTYKEY key)
{
	return S_OK;
}

static IMMNotificationClientVtbl snd_notifyvtbl =
{
	Notify_QueryInterface,
	Notify_AddRef,
	Notify_Release,
	Notify_OnDeviceStateChanged,
	Notify_OnDeviceAdded,
	Notify_OnDeviceRemoved,
	Notify_OnDefaultDeviceChanged,
	Notify_OnPropertyValueChanged,
};
static IMMNotificationClient snd_notify = {&snd_notifyvtbl};

/*
===============================================================================

SOUND THREAD

===============================================================================
*/

static void SND_CloseDevice (void)
{
	if (snd_client)
		IAudioClient_Stop (snd_client);
	if (snd_render)
	{
		IAudioRenderClient_Release (snd_render);
		snd_render = NULL;
	}
	if (snd_client)
	{
		IAudioClient_Release (snd_client);
		snd_client = NULL;
	}
}

/*
===============
SND_OpenDevice

Opens a stream in the ring's format on the default render device
===============
*/
static bool SND_OpenDevice (void)
{
	IMMDevice		*device;
	WAVEFORMATEX	*mixformat;
	WAVEFORMATEX	format;
	HRESULT			hr;

	if (FAILED (IMMDeviceEnumerator_GetDefaultAudioEndpoint (snd_enumerator, eRender, eConsole, &device)))
		return false;
	hr = IMMDevice_Activate (device, &sw_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&snd_client);
	IMMDevice_Release (device);
	if (FAILED (hr))
	{
		snd_client = NULL;
		return false;
	}

	// the first device decides the rate; later ones convert to it
	if (!snd_rate)
	{
		if (FAILED (IAudioClient_GetMixFormat (snd_client, &mixformat)))
		{
			SND_CloseDevice ();
			return false;
		}
		snd_rate = (int)mixformat->nSamplesPerSec;
		CoTaskMemFree (mixformat);
	}

	memset (&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 2;
	format.nSamplesPerSec = (DWORD)snd_rate;
	format.wBitsPerSample = 16;
	format.nBlockAlign = FRAME_BYTES;
	format.nAvgBytesPerSec = format.nSamplesPerSec * FRAME_BYTES;

	hr = IAudioClient_Initialize (snd_client, AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
		DEVICE_BUFFER, 0, &format, NULL);
	if (SUCCEEDED (hr))
		hr = IAudioClient_SetEventHandle (snd_client, snd_bufferevent);
	if (SUCCEEDED (hr))
		hr = IAudioClient_GetBufferSize (snd_client, &snd_bufferframes);
	if (SUCCEEDED (hr))
		hr = IAudioClient_GetService (snd_client, &sw_IID_IAudioRenderClient, (void **)&snd_render);
	if (SUCCEEDED (hr))
		hr = IAudioClient_Start (snd_client);
	if (FAILED (hr))
	{
		SND_CloseDevice ();
		return false;
	}
	return true;
}

/*
===============
SND_FillDevice

Gives the device as many ring frames as it has room for
===============
*/
static HRESULT SND_FillDevice (void)
{
	UINT32	padding, frames, first;
	BYTE	*data;
	LONG	read;
	HRESULT	hr;

	hr = IAudioClient_GetCurrentPadding (snd_client, &padding);
	if (FAILED (hr))
		return hr;
	frames = snd_bufferframes - padding;
	if (!frames)
		return S_OK;
	hr = IAudioRenderClient_GetBuffer (snd_render, frames, &data);
	if (FAILED (hr))
		return hr;

	read = snd_readframe;
	if (snd_silenced)
		memset (data, 0, (size_t)frames * FRAME_BYTES);
	else
	{
		first = RING_FRAMES - (UINT32)read;
		if (first > frames)
			first = frames;
		memcpy (data, snd_ring + (size_t)read * FRAME_BYTES, (size_t)first * FRAME_BYTES);
		memcpy (data + (size_t)first * FRAME_BYTES, snd_ring, (size_t)(frames - first) * FRAME_BYTES);
	}
	hr = IAudioRenderClient_ReleaseBuffer (snd_render, frames, 0);

	// the mixer paints ahead of this position, so it never touches what the device reads
	InterlockedExchange (&snd_readframe, (read + (LONG)frames) & (RING_FRAMES - 1));
	return hr;
}

static DWORD WINAPI SND_Thread ([[maybe_unused]] LPVOID param)
{
	HANDLE	events[3];
	HANDLE	task;
	DWORD	taskindex = 0;
	DWORD	wait;

	CoInitializeEx (NULL, COINIT_MULTITHREADED);
	task = AvSetMmThreadCharacteristicsW (L"Games", &taskindex);

	if (SUCCEEDED (CoCreateInstance (&sw_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
		&sw_IID_IMMDeviceEnumerator, (void **)&snd_enumerator)))
	{
		IMMDeviceEnumerator_RegisterEndpointNotificationCallback (snd_enumerator, &snd_notify);
		snd_opened = SND_OpenDevice ();
	}
	SetEvent (snd_readyevent);

	events[0] = snd_quitevent;
	events[1] = snd_changeevent;
	events[2] = snd_bufferevent;
	while (snd_enumerator && snd_opened)
	{
		wait = WaitForMultipleObjects (snd_client ? 3 : 2, events, FALSE, snd_client ? 200 : 500);
		if (wait == WAIT_OBJECT_0)
			break;
		if (wait == WAIT_OBJECT_0 + 1 || (!snd_client && wait == WAIT_TIMEOUT))
		{	// a new default device, or retrying after the device went away
			SND_CloseDevice ();
			SND_OpenDevice ();
			continue;
		}
		if (snd_client && FAILED (SND_FillDevice ()))
			SND_CloseDevice ();		// device lost; reopen on the next timeout
	}

	SND_CloseDevice ();
	if (snd_enumerator)
	{
		IMMDeviceEnumerator_UnregisterEndpointNotificationCallback (snd_enumerator, &snd_notify);
		IMMDeviceEnumerator_Release (snd_enumerator);
		snd_enumerator = NULL;
	}
	if (task)
		AvRevertMmThreadCharacteristics (task);
	CoUninitialize ();
	return 0;
}

/*
===============================================================================

BACKEND CONTRACT

===============================================================================
*/

bool SNDDMA_Init (dma_t *dma)
{
	snd_ring = Mem_Calloc (RING_FRAMES, FRAME_BYTES);
	snd_readframe = 0;
	snd_rate = 0;
	snd_opened = false;
	snd_quitevent = CreateEventW (NULL, TRUE, FALSE, NULL);
	snd_changeevent = CreateEventW (NULL, FALSE, FALSE, NULL);
	snd_bufferevent = CreateEventW (NULL, FALSE, FALSE, NULL);
	snd_readyevent = CreateEventW (NULL, TRUE, FALSE, NULL);

	snd_thread = CreateThread (NULL, 0, SND_Thread, NULL, 0, NULL);
	if (snd_thread)
		WaitForSingleObject (snd_readyevent, INFINITE);
	if (!snd_thread || !snd_opened)
	{
		Con_Printf ("WASAPI: no sound output device\n");
		SNDDMA_Shutdown ();
		return false;
	}

	memset (dma, 0, sizeof(*dma));
	dma->channels = 2;
	dma->samplebits = 16;
	dma->speed = snd_rate;
	dma->samples = RING_FRAMES * 2;
	dma->submission_chunk = 1;
	dma->buffer = snd_ring;
	dma->soundalive = true;
	dma->gamealive = true;

	Con_Printf ("WASAPI sound: %d Hz\n", snd_rate);
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	return (int)(snd_readframe * 2) & (RING_FRAMES * 2 - 1);
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
	if (snd_thread)
	{
		SetEvent (snd_quitevent);
		WaitForSingleObject (snd_thread, INFINITE);
		CloseHandle (snd_thread);
		snd_thread = NULL;
	}
	if (snd_quitevent)
		CloseHandle (snd_quitevent);
	if (snd_changeevent)
		CloseHandle (snd_changeevent);
	if (snd_bufferevent)
		CloseHandle (snd_bufferevent);
	if (snd_readyevent)
		CloseHandle (snd_readyevent);
	snd_quitevent = snd_changeevent = snd_bufferevent = snd_readyevent = NULL;
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
	InterlockedExchange (&snd_silenced, blocked);
}
