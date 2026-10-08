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
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "snd_local.h"


#define	PAINTBUFFER_SIZE	512
static portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
static int		snd_scaletable[32][256];
static int 	*snd_p, snd_linear_count, snd_vol;
static short	*snd_out;

static void Snd_WriteLinearBlastStereo16 (void);

static void Snd_WriteLinearBlastStereo16 (void)
{
	int		i;
	int		val;

	for (i=0 ; i<snd_linear_count ; i+=2)
	{
		val = (snd_p[i]*snd_vol)>>8;
		if (val > 0x7fff)
			snd_out[i] = 0x7fff;
		else if (val < -32768)
			snd_out[i] = -32768;
		else
			snd_out[i] = (short)val;

		val = (snd_p[i+1]*snd_vol)>>8;
		if (val > 0x7fff)
			snd_out[i+1] = 0x7fff;
		else if (val < -32768)
			snd_out[i+1] = -32768;
		else
			snd_out[i+1] = (short)val;
	}
}

static void S_TransferStereo16 (int endtime)
{
	int		lpos;
	int		lpaintedtime;
	unsigned	*pbuf;

	snd_vol = (int)(volume.value*256);

	snd_p = (int *) paintbuffer;
	lpaintedtime = snd.paintedtime;

	pbuf = SNDDMA_LockBuffer ();
	if (!pbuf)
	{
		S_RestartOutput ();
		return;
	}

	while (lpaintedtime < endtime)
	{
	// handle recirculating buffer issues
		lpos = lpaintedtime & ((snd.dma.samples>>1)-1);

		snd_out = (short *) pbuf + (lpos<<1);

		snd_linear_count = (snd.dma.samples>>1) - lpos;
		if (lpaintedtime + snd_linear_count > endtime)
			snd_linear_count = endtime - lpaintedtime;

		snd_linear_count <<= 1;

	// write a linear blast of samples
		Snd_WriteLinearBlastStereo16 ();

		snd_p += snd_linear_count;
		lpaintedtime += (snd_linear_count>>1);
	}

	SNDDMA_UnlockBuffer (pbuf);
}

static void S_TransferPaintBuffer(int endtime)
{
	int 	out_idx;
	int 	count;
	int 	out_mask;
	int 	*p;
	int 	step;
	int		val;
	int		vol;
	unsigned	*pbuf;

	if (snd.dma.samplebits == 16 && snd.dma.channels == 2)
	{
		S_TransferStereo16 (endtime);
		return;
	}
	
	p = (int *) paintbuffer;
	count = (endtime - snd.paintedtime) * snd.dma.channels;
	out_mask = snd.dma.samples - 1; 
	out_idx = snd.paintedtime * snd.dma.channels & out_mask;
	step = 3 - snd.dma.channels;
	vol = (int)(volume.value*256);

	pbuf = SNDDMA_LockBuffer ();
	if (!pbuf)
	{
		S_RestartOutput ();
		return;
	}

	if (snd.dma.samplebits == 16)
	{
		short *out = (short *) pbuf;
		while (count--)
		{
			val = (*p * vol) >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < -32768)
				val = -32768;
			out[out_idx] = (short)val;
			out_idx = (out_idx + 1) & out_mask;
		}
	}
	else if (snd.dma.samplebits == 8)
	{
		unsigned char *out = (unsigned char *) pbuf;
		while (count--)
		{
			val = (*p * vol) >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < -32768)
				val = -32768;
			out[out_idx] = (unsigned char)((val>>8) + 128);
			out_idx = (out_idx + 1) & out_mask;
		}
	}

	SNDDMA_UnlockBuffer (pbuf);
}


/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

static void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int endtime);
static void SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int endtime);

/*
===============================================================================

QUAKEC'S SAMPLES

What QuakeC queues (S_RawSamples) at the output's rate, by the time it plays.
All of it is kept, as FTE keeps it, and what is queued is counted in the
stream's own frames. qcquake's drivers take the sound's position as the frames
they queued less those queued, wrapped to their ring, and count a wrap each
time it goes back: a sample dropped, or a count a frame short, puts them a ring
(1.5 s) ahead for good.

===============================================================================
*/

#define RAW_MIN		65536			// sample pairs in the ring at first: 1.5 s at 44.1 kHz
#define RAW_MAX		(1 << 21)		// and at most (47 s); a call that doesn't fit is dropped

static portable_samplepair_t	*raw_samples;
static int		raw_size;		// a power of two

// The stream: its rate, the output time it started at, and the frames queued
// before it. Output sample t is the stream's frame raw_first + (t - raw_start) *
// raw_step; raw_frames counts every frame queued.
static int		raw_hz;
static double	raw_step;
static int		raw_start;
static int64_t	raw_first, raw_frames;

// the frame output sample t comes from
static int64_t S_RawFrame (int t)
{
	return raw_first + (int64_t)floor ((double)(t - raw_start) * raw_step);
}

// room in the ring for n more queued sample pairs, the ring grown (what is
// queued kept at its times); false past RAW_MAX
static bool S_RawRoom (int n)
{
	int		need = snd.rawend - snd.paintedtime + n, size, t;
	portable_samplepair_t	*grown;

	if (need <= raw_size)
		return true;
	if (need > RAW_MAX)
		return false;
	for (size = raw_size ? raw_size : RAW_MIN ; size < need ; size *= 2)
		;
	grown = Mem_Alloc ((size_t)size * sizeof(*grown));
	for (t = snd.paintedtime ; t < snd.rawend ; t++)
		grown[t & (size - 1)] = raw_samples[t & (raw_size - 1)];
	Mem_Free (raw_samples);
	raw_samples = grown;
	raw_size = size;
	return true;
}

bool S_RawSamples (int hz, int channels, const short *data, int frames)
{
	int		n, t, i;
	portable_samplepair_t	*out;

	if (!snd.started || hz <= 0 || frames <= 0 || (channels != 1 && channels != 2))
		return false;
	// after a gap, or at another rate, the stream starts again
	if (snd.rawend < snd.paintedtime || hz != raw_hz)
	{
		snd.rawend = raw_start = snd.paintedtime;
		raw_first = raw_frames;
		raw_hz = hz;
		raw_step = (double)hz / snd.dma.speed;
	}
	// the samples up to where the frame after these begins
	for (n = 0 ; S_RawFrame (snd.rawend + n) < raw_frames + frames ; n++)
		;
	if (!S_RawRoom (n))
		return false;
	for (t = snd.rawend ; t < snd.rawend + n ; t++)
	{
		i = (int)(S_RawFrame (t) - raw_frames) * channels;
		out = &raw_samples[t & (raw_size - 1)];
		out->left = data[i];
		out->right = data[i + channels - 1];
	}
	snd.rawend += n;
	raw_frames += frames;
	return true;
}

// The frames not yet mixed, from the one the next sample to mix comes from, as
// FTE counts them (the mixing ahead of the device isn't counted): never more
// than QuakeC queued, and only down as the mixing goes on. Half a frame more,
// so a driver turning it back into frames (qcquake's) isn't one short.
float S_RawQueued (void)
{
	int64_t	left;

	if (!snd.started || snd.rawend <= snd.paintedtime)
		return 0;
	left = raw_frames - S_RawFrame (snd.paintedtime > raw_start ? snd.paintedtime : raw_start);
	return (float)(((double)left + 0.5) / raw_hz);
}

// the queued samples up to endtime into the paint buffer
static void S_PaintRaw (int endtime)
{
	int		t, stop = snd.rawend < endtime ? snd.rawend : endtime;
	portable_samplepair_t	*in;

	for (t = snd.paintedtime ; t < stop ; t++)
	{
		in = &raw_samples[t & (raw_size - 1)];
		paintbuffer[t - snd.paintedtime].left += in->left;
		paintbuffer[t - snd.paintedtime].right += in->right;
	}
}

void S_PaintChannels(int endtime)
{
	int 	i;
	int 	end;
	channel_t *ch;
	sfxcache_t	*sc;
	int		ltime, count;

	while (snd.paintedtime < endtime)
	{
	// if paintbuffer is smaller than DMA buffer
		end = endtime;
		if (endtime - snd.paintedtime > PAINTBUFFER_SIZE)
			end = snd.paintedtime + PAINTBUFFER_SIZE;

	// clear the paint buffer
		Q_memset(paintbuffer, 0, (end - snd.paintedtime) * sizeof(portable_samplepair_t));
		S_PaintRaw (end);

	// paint in the channels.
		ch = snd.channels;
		for (i=0; i<snd.total_channels ; i++, ch++)
		{
			if (!ch->sfx)
				continue;
			if (!ch->leftvol && !ch->rightvol)
				continue;
			sc = S_LoadSound (ch->sfx);
			if (!sc)
				continue;

			ltime = snd.paintedtime;

			while (ltime < end)
			{	// paint up to end
				if (ch->end < end)
					count = ch->end - ltime;
				else
					count = end - ltime;

				if (count > 0)
				{	
					if (sc->width == 1)
						SND_PaintChannelFrom8(ch, sc, count);
					else
						SND_PaintChannelFrom16(ch, sc, count);
	
					ltime += count;
				}

			// if at end of loop, restart
				if (ltime >= ch->end)
				{
					if (sc->loopstart >= 0)
					{
						ch->pos = sc->loopstart;
						ch->end = ltime + sc->length - ch->pos;
					}
					else				
					{	// channel just stopped
						ch->sfx = NULL;
						break;
					}
				}
			}
															  
		}

	// transfer out according to DMA format
		S_TransferPaintBuffer(end);
		snd.paintedtime = end;
	}
}

void SND_InitScaletable (void)
{
	int		i, j;
	
	for (i=0 ; i<32 ; i++)
		for (j=0 ; j<256 ; j++)
			snd_scaletable[i][j] = ((signed char)j) * i * 8;
}



static void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int count)
{
	int 	data;
	int		*lscale, *rscale;
	unsigned char *sfx;
	int		i;

	if (ch->leftvol > 255)
		ch->leftvol = 255;
	if (ch->rightvol > 255)
		ch->rightvol = 255;
		
	lscale = snd_scaletable[ch->leftvol >> 3];
	rscale = snd_scaletable[ch->rightvol >> 3];
	sfx = (unsigned char *)sc->data + ch->pos;

	for (i=0 ; i<count ; i++)
	{
		data = sfx[i];
		paintbuffer[i].left += lscale[data];
		paintbuffer[i].right += rscale[data];
	}
	
	ch->pos += count;
}



static void SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int count)
{
	int data;
	int left, right;
	int leftvol, rightvol;
	signed short *sfx;
	int	i;

	leftvol = ch->leftvol;
	rightvol = ch->rightvol;
	sfx = (signed short *)sc->data + ch->pos;

	for (i=0 ; i<count ; i++)
	{
		data = sfx[i];
		left = (data * leftvol) >> 8;
		right = (data * rightvol) >> 8;
		paintbuffer[i].left += left;
		paintbuffer[i].right += right;
	}

	ch->pos += count;
}

