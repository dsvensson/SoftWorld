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

#pragma once
// sound.h -- client sound i/o functions

#include "bspfile.h"
#include "cvar.h"
#include "mathlib.h"
#include "q_types.h"

// !!! if this is changed, it much be changed in asm_i386.h too !!!
typedef struct
{
	int left;
	int right;
} portable_samplepair_t;

typedef struct sfx_s
{
	char 	name[MAX_QPATH];
	struct sfxcache_s	*data;		// decoded samples once loaded
} sfx_t;

typedef struct sfxcache_s
{
	int 	length;
	int 	loopstart;
	int 	speed;
	int 	width;
	int 	stereo;
	byte	data[1];		// variable sized
} sfxcache_t;

typedef struct
{
	bool		gamealive;
	bool		soundalive;
	bool		splitbuffer;
	int				channels;
	int				samples;				// mono samples in buffer
	int				submission_chunk;		// don't mix less than this #
	int				samplepos;				// in mono samples
	int				samplebits;
	int				speed;
	unsigned char	*buffer;
} dma_t;

// !!! if this is changed, it much be changed in asm_i386.h too !!!
typedef struct
{
	sfx_t	*sfx;			// sfx number
	int		leftvol;		// 0-255 volume
	int		rightvol;		// 0-255 volume
	int		end;			// end time in global paintsamples
	int 	pos;			// sample position in sfx
	int		looping;		// where to loop, -1 = no looping
	int		entnum;			// to allow overriding a specific sound
	int		entchannel;		//
	vec3_t	origin;			// origin of sound effect
	vec_t	dist_mult;		// distance multiplier (attenuation/clipK)
	int		master_vol;		// 0-255 master volume
} channel_t;

typedef struct
{
	int		rate;
	int		width;
	int		channels;
	int		loopstart;
	int		samples;
	int		dataofs;		// chunk starts this many bytes from file start
} wavinfo_t;

void S_Init (void);
void S_Shutdown (void);
void S_StartSound (int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol,  float attenuation);
void S_StaticSound (sfx_t *sfx, vec3_t origin, float vol, float attenuation);
void S_StopSound (int entnum, int entchannel);
void S_StopAllSounds(bool clear);
void S_StopDynamicSounds (void);	// the entities' sounds; the ambient and static ones go on
void S_ClearBuffer (void);
// where the listener is, filled in by the client every frame
typedef struct
{
	vec3_t		origin;
	vec3_t		forward, right, up;
	int			viewentity;		// sounds from this entity play at full volume
	const byte	*ambient_levels;	// NUM_AMBIENTS levels at origin, NULL for none
	float		frametime;		// seconds since the last update
} snd_listener_t;

void S_Update (const snd_listener_t *listener);
void S_ExtraUpdate (void);

sfx_t *S_PrecacheSound (char *sample);
void S_PaintChannels(int endtime);

// opens the output and describes its ring buffer in dma
bool SNDDMA_Init (dma_t *dma);

// gets the current DMA position
int SNDDMA_GetDMAPos(void);

// the whole output ring, dma->samples samples, for writing; NULL if unavailable
void *SNDDMA_LockBuffer (void);
void SNDDMA_UnlockBuffer (void *buffer);

// shutdown the DMA xfer.
void SNDDMA_Shutdown(void);

// silence while the window is inactive
void SNDDMA_SetBlocked (bool blocked);
void S_BlockSound (void);
void S_UnblockSound (void);

// ====================================================================
// User-setable variables
// ====================================================================

#define	MAX_DYNAMIC_CHANNELS	8


extern	cvar_t bgmvolume;
extern	cvar_t volume;

void S_LocalSound (char *s);
sfxcache_t *S_LoadSound (sfx_t *s);

void SND_InitScaletable (void);
void SNDDMA_Submit(void);

