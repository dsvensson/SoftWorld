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
// snd_mem.c: sound caching

#include "snd_local.h"
static bool GetWavinfo (const char *name, byte *wav, int wavlength, wavinfo_t *info);



/*
================
ResampleSfx

The samples at data into sc, at the output's rate (speed), 8 bits a sample
if as8bit
================
*/
static void ResampleSfx (sfxcache_t *sc, int inrate, int inwidth, const byte *data, int speed, bool as8bit)
{
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i;
	int		sample, samplefrac, fracstep;

	stepscale = (float)inrate / speed;	// this is usually 0.5, 1, or 2

	outcount = (int)(sc->length / stepscale);
	sc->length = outcount;
	if (sc->loopstart != -1)
		sc->loopstart = (int)(sc->loopstart / stepscale);

	sc->speed = speed;
	if (as8bit)
		sc->width = 1;
	else
		sc->width = inwidth;
	sc->stereo = 0;

// resample / decimate to the current source rate

	if (stepscale == 1 && inwidth == 1 && sc->width == 1)
	{
// fast special case
		for (i=0 ; i<outcount ; i++)
			((signed char *)sc->data)[i]
			= (int)( (unsigned char)(data[i]) - 128);
	}
	else
	{
// general case
		samplefrac = 0;
		fracstep = (int)(stepscale*256);
		for (i=0 ; i<outcount ; i++)
		{
			srcsample = samplefrac >> 8;
			samplefrac += fracstep;
			if (inwidth == 2)
				sample = LittleShort ( ((const short *)data)[srcsample] );
			else
				sample = ((unsigned char)data[srcsample] - 128) * 256;	// a negative can't be shifted
			if (sc->width == 2)
				((short *)sc->data)[i] = (short)sample;
			else
				((signed char *)sc->data)[i] = (signed char)(sample >> 8);
		}
	}
}

//=============================================================================

/*
==============
S_DecodeSound

A sound's file (sound/<name>) decoded at the output's rate (speed), 8 bits a
sample if as8bit, from Mem_Alloc; on any thread (a loader's). NULL, said on
the console, if it can't be.
==============
*/
sfxcache_t *S_DecodeSound (const char *name, int speed, bool as8bit)
{
    char	namebuffer[256];
	byte	*data;
	wavinfo_t	info;
	int		len;
	float	stepscale;
	sfxcache_t	*sc;
	int		filelen;

    Q_strncpyz(namebuffer, "sound/", sizeof(namebuffer));
    Q_strncatz(namebuffer, name, sizeof(namebuffer));

	data = FS_LoadFile (namebuffer, &filelen);

	if (!data)
	{
		Con_Printf ("Couldn't load %s\n", namebuffer);
		return NULL;
	}

	if (!GetWavinfo (name, data, filelen, &info))
	{
		Mem_Free (data);
		return NULL;
	}
	if (info.channels != 1)
	{
		Con_Printf ("%s is a stereo sample\n", name);
		Mem_Free (data);
		return NULL;
	}

	stepscale = (float)info.rate / speed;
	len = (int)(info.samples / stepscale);

	len = len * info.width * info.channels;

	sc = Mem_Calloc (1, (size_t)len + sizeof(sfxcache_t));

	sc->length = info.samples;
	sc->loopstart = info.loopstart;
	sc->speed = info.rate;
	sc->width = info.width;
	sc->stereo = info.channels;

	ResampleSfx (sc, info.rate, info.width, data + info.dataofs, speed, as8bit);
	Mem_Free (data);

	return sc;
}

/*
==============
S_LoadSound

The sound's samples, decoded now if they aren't yet; NULL while a loader
decodes them, and for a sound that couldn't be (until it is precached again)
==============
*/
sfxcache_t *S_LoadSound (sfx_t *s)
{
	if (s->data)
		return s->data;
	if (s->loading || s->failed)
		return NULL;
	s->data = S_DecodeSound (s->name, snd.dma.speed, loadas8bit.value != 0);
	s->failed = !s->data;
	return s->data;
}



/*
===============================================================================

WAV loading

===============================================================================
*/


// the file being read, on the thread reading it
static thread_local byte	*data_p;
static thread_local byte 	*iff_end;
static thread_local byte 	*last_chunk;
static thread_local byte 	*iff_data;
static thread_local int 	iff_chunk_len;

// whether the file has n bytes at data_p
static bool InWav (int n)
{
	return data_p && data_p <= iff_end && iff_end - data_p >= n;
}

static short GetLittleShort(void)
{
	short val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	data_p += 2;
	return val;
}

static int GetLittleLong(void)
{
	int val;

	// unsigned: the top byte's high bit would overflow an int's shift
	val = (int)((uint32_t)data_p[0] | (uint32_t)data_p[1] << 8 | (uint32_t)data_p[2] << 16
		| (uint32_t)data_p[3] << 24);
	data_p += 4;
	return val;
}

static void FindNextChunk(char *chunkname)
{
	while (1)
	{
		data_p=last_chunk;

		if (!InWav (8))
		{	// didn't find the chunk
			data_p = NULL;
			return;
		}

		data_p += 4;
		iff_chunk_len = GetLittleLong();
		if (iff_chunk_len < 0)
		{
			data_p = NULL;
			return;
		}
//		if (iff_chunk_len > 1024*1024)
//			Sys_Error ("FindNextChunk: %i length is past the 1 meg sanity limit", iff_chunk_len);
		data_p -= 8;
		if (iff_end - data_p - 8 < ((iff_chunk_len + 1) & ~1))
			last_chunk = iff_end;		// the last, cut short
		else
			last_chunk = data_p + 8 + ( (iff_chunk_len + 1) & ~1 );
		if (!Q_strncmp((char *)data_p, chunkname, 4))
			return;
	}
}

static void FindChunk(char *chunkname)
{
	last_chunk = iff_data;
	FindNextChunk (chunkname);
}



/*
============
GetWavinfo

False, said on the console, if the file isn't a WAV file that can be used
============
*/
static bool GetWavinfo (const char *sndname, byte *wav, int wavlength, wavinfo_t *info)
{
	int     i;
	int     format;
	int		samples;

	memset (info, 0, sizeof(*info));

	iff_data = wav;
	iff_end = wav + wavlength;

// find "RIFF" chunk
	FindChunk("RIFF");
	if (!(InWav (12) && !Q_strncmp((char *)(data_p+8), "WAVE", 4)))
	{
		Con_Printf("%s: missing RIFF/WAVE chunks\n", sndname);
		return false;
	}

// get "fmt " chunk
	iff_data = data_p + 12;
// DumpChunks ();

	FindChunk("fmt ");
	if (!InWav (24))
	{
		Con_Printf("%s: missing fmt chunk\n", sndname);
		return false;
	}
	data_p += 8;
	format = GetLittleShort();
	if (format != 1)
	{
		Con_Printf("%s: Microsoft PCM format only\n", sndname);
		return false;
	}

	info->channels = GetLittleShort();
	info->rate = GetLittleLong();
	data_p += 4+2;
	info->width = GetLittleShort() / 8;
	if (info->rate <= 0 || (info->width != 1 && info->width != 2))
	{
		Con_Printf ("%s: %i bytes a sample at %i Hz\n", sndname, info->width, info->rate);
		return false;
	}

// get cue chunk
	FindChunk("cue ");
	if (InWav (36))
	{
		data_p += 32;
		info->loopstart = GetLittleLong();
//		Con_Printf("loopstart=%d\n", sfx->loopstart);

	// if the next chunk is a LIST chunk, look for a cue length marker
		FindNextChunk ("LIST");
		if (InWav (32))
		{
			if (!strncmp ((char *)(data_p + 28), "mark", 4))
			{	// this is not a proper parse, but it works with cooledit...
				data_p += 24;
				i = GetLittleLong ();	// samples in loop
				info->samples = info->loopstart + i;
//				Con_Printf("looped length: %i\n", i);
			}
		}
	}
	else
		info->loopstart = -1;

// find data chunk
	FindChunk("data");
	if (!InWav (8))
	{
		Con_Printf("%s: missing data chunk\n", sndname);
		return false;
	}

	data_p += 4;
	samples = GetLittleLong () / info->width;
	// as many as the file has
	if (samples < 0 || samples > (iff_end - data_p) / info->width)
		samples = (int)((iff_end - data_p) / info->width);

	if (info->samples)
	{
		if (samples < info->samples)
		{
			Con_Printf ("%s: a bad loop length\n", sndname);
			return false;
		}
	}
	else
		info->samples = samples;

	info->dataofs = (int)(data_p - wav);

	return true;
}
