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
// snd_dma.c -- main control for any streaming sound output device

#include "snd_local.h"


void S_Play(void);
void S_PlayVol(void);
void S_SoundList(void);
void S_Update_ (void);
void S_StopAllSounds(bool clear);
void S_StopAllSoundsC(void);

// =======================================================================
// Internal sound data & structures
// =======================================================================

#define	MAX_SFX		512

snd_state_t	snd;

static void S_Startup (void);

// sound's own random numbers: how far the mixer has got depends on the
// clock, and taking from rand () then would move the particles, which draw
// from it too; frames of a timedemo must come out the same every run
static unsigned	snd_randstate = 0x2545F491;

static int S_Rand (void)
{
	snd_randstate = snd_randstate * 1664525u + 1013904223u;
	return (int)(snd_randstate >> 16);
}
static vec_t		sound_nominal_clip_dist=1000.0;

cvar_t bgmvolume = {.name = "bgmvolume", .string = "1", .archive = true};
cvar_t volume = {.name = "volume", .string = "0.7", .archive = true};

static cvar_t nosound = {.name = "nosound", .string = "0"};
static cvar_t precache = {.name = "precache", .string = "1"};
cvar_t loadas8bit = {.name = "loadas8bit", .string = "0"};
static cvar_t ambient_level = {.name = "ambient_level", .string = "0.3"};
static cvar_t ambient_fade = {.name = "ambient_fade", .string = "100"};
static cvar_t snd_noextraupdate = {.name = "snd_noextraupdate", .string = "0"};
static cvar_t snd_show = {.name = "snd_show", .string = "0"};
cvar_t _snd_mixahead = {.name = "_snd_mixahead", .string = "0.1", .archive = true};


// ====================================================================
// User-setable variables
// ====================================================================



void S_SoundInfo_f(void)
{
	if (!snd.started)
	{
		Con_Printf ("sound system not started\n");
		return;
	}
	
    Con_Printf("%5d stereo\n", snd.dma.channels - 1);
    Con_Printf("%5d samples\n", snd.dma.samples);
    Con_Printf("%5d samplepos\n", snd.dma.samplepos);
    Con_Printf("%5d samplebits\n", snd.dma.samplebits);
    Con_Printf("%5d submission_chunk\n", snd.dma.submission_chunk);
    Con_Printf("%5d speed\n", snd.dma.speed);
    Con_Printf("0x%x dma buffer\n", snd.dma.buffer);
	Con_Printf("%5d total_channels\n", snd.total_channels);
}


/*
================
S_Startup
================
*/

static void S_Startup (void)
{
	int		rc;

	if (!snd.initialized)
		return;

	rc = SNDDMA_Init (&snd.dma);
	if (!rc)
	{
		snd.started = false;
		return;
	}

	snd.started = true;
	if (snd.blocked)
		SNDDMA_SetBlocked (true);
}


/*
================
S_Init
================
*/
/*
================
S_FlushSounds

Drops decoded sounds so they are reloaded from the new game directory.
================
*/
static void S_FlushSounds (void)
{
	int		i;

	for (i = 0 ; i < snd.num_sfx ; i++)
	{
		Mem_Free (snd.known_sfx[i].data);
		snd.known_sfx[i].data = NULL;
	}
}

void S_Init (void)
{

//	Con_Printf("\nSound Initialization\n");

	if (COM_CheckParm("-nosound"))
		return;

	Cmd_AddCommand("play", S_Play);
	Cmd_AddCommand("playvol", S_PlayVol);
	Cmd_AddCommand("stopsound", S_StopAllSoundsC);
	Cmd_AddCommand("soundlist", S_SoundList);
	Cmd_AddCommand("soundinfo", S_SoundInfo_f);

	Cvar_RegisterVariable(&nosound);
	Cvar_RegisterVariable(&volume);
	Cvar_RegisterVariable(&precache);
	Cvar_RegisterVariable(&loadas8bit);
	Cvar_RegisterVariable(&bgmvolume);
	Cvar_RegisterVariable(&ambient_level);
	Cvar_RegisterVariable(&ambient_fade);
	Cvar_RegisterVariable(&snd_noextraupdate);
	Cvar_RegisterVariable(&snd_show);
	Cvar_RegisterVariable(&_snd_mixahead);




	snd.initialized = true;
	snd.max_channels = 128;
	snd.channels = Mem_Calloc ((size_t)snd.max_channels, sizeof(channel_t));

	S_Startup ();

	SND_InitScaletable ();

	snd.known_sfx = Mem_Calloc (MAX_SFX, sizeof(sfx_t));
	snd.num_sfx = 0;
	FS_AddGamedirCallback (S_FlushSounds);

//	Con_Printf ("Sound sampling rate: %i\n", shm->speed);

	snd.ambient_sfx[AMBIENT_WATER] = S_PrecacheSound ("ambience/water1.wav");
	snd.ambient_sfx[AMBIENT_SKY] = S_PrecacheSound ("ambience/wind2.wav");

	S_StopAllSounds (true);
}


// =======================================================================
// Shutdown sound engine
// =======================================================================

void S_Shutdown (void)
{
	if (snd.started)
		SNDDMA_Shutdown ();
	if (snd.known_sfx)
	{
		S_FlushSounds ();
		Mem_Free (snd.known_sfx);
	}
	FS_RemoveGamedirCallback (S_FlushSounds);
	memset (&snd, 0, sizeof(snd));
}

/*
================
S_RestartOutput
================
*/
void S_RestartOutput (void)
{
	if (snd.started)
		SNDDMA_Shutdown ();
	snd.started = false;
	S_Startup ();
}

/*
==================
S_BlockSound / S_UnblockSound

The window became inactive or active again
==================
*/
void S_BlockSound (void)
{
	if (++snd.blocked == 1 && snd.started)
		SNDDMA_SetBlocked (true);
}

void S_UnblockSound (void)
{
	if (snd.blocked > 0 && --snd.blocked == 0 && snd.started)
		SNDDMA_SetBlocked (false);
}


// =======================================================================
// Load a sound
// =======================================================================

/*
==================
S_FindName

==================
*/
sfx_t *S_FindName (char *sndname)
{
	int		i;
	sfx_t	*sfx;

	if (!sndname)
		Sys_Error ("S_FindName: NULL\n");

	if (Q_strlen(sndname) >= MAX_QPATH)
		Sys_Error ("Sound name too long: %s", sndname);

// see if already loaded
	for (i=0 ; i < snd.num_sfx ; i++)
		if (!Q_strcmp(snd.known_sfx[i].name, sndname))
		{
			return &snd.known_sfx[i];
		}

	if (snd.num_sfx == MAX_SFX)
		Sys_Error ("S_FindName: out of sfx_t");

	sfx = &snd.known_sfx[i];
	Q_strncpyz (sfx->name, sndname, sizeof(sfx->name));

	snd.num_sfx++;
	
	return sfx;
}

/*
==================
S_PrecacheSound

==================
*/
sfx_t *S_PrecacheSound (char *sndname)
{
	sfx_t	*sfx;

	if (!snd.started || nosound.value)
		return NULL;

	sfx = S_FindName (sndname);
	
// cache it in
	if (precache.value)
		S_LoadSound (sfx);
	
	return sfx;
}


//=============================================================================

/*
=================
SND_PickChannel
=================
*/
channel_t *SND_PickChannel(int entnum, int entchannel)
{
    int ch_idx;
    int first_to_die;
    int life_left;

// Check for replacement sound, or find the best one to replace
    first_to_die = -1;
    life_left = 0x7fffffff;
    for (ch_idx=NUM_AMBIENTS ; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS ; ch_idx++)
    {
		if (entchannel != 0		// channel 0 never overrides
		&& snd.channels[ch_idx].entnum == entnum
		&& (snd.channels[ch_idx].entchannel == entchannel || entchannel == -1) )
		{	// allways override sound from same entity
			first_to_die = ch_idx;
			break;
		}

		// don't let monster sounds override player sounds
		if (snd.channels[ch_idx].entnum == snd.viewentity && entnum != snd.viewentity && snd.channels[ch_idx].sfx)
			continue;

		if (snd.channels[ch_idx].end - snd.paintedtime < life_left)
		{
			life_left = snd.channels[ch_idx].end - snd.paintedtime;
			first_to_die = ch_idx;
		}
   }

	if (first_to_die == -1)
		return NULL;

	if (snd.channels[first_to_die].sfx)
		snd.channels[first_to_die].sfx = NULL;

    return &snd.channels[first_to_die];    
}       

/*
=================
SND_Spatialize
=================
*/
void SND_Spatialize(channel_t *ch)
{
    vec_t dot;
    vec_t dist;
    vec_t lscale, rscale, scale;
    vec3_t source_vec;

// anything coming from the view entity will allways be full volume
	if (ch->entnum == snd.viewentity)
	{
		ch->leftvol = ch->master_vol;
		ch->rightvol = ch->master_vol;
		return;
	}

// calculate stereo seperation and distance attenuation

	VectorSubtract(ch->origin, snd.listener_origin, source_vec);
	
	dist = VectorNormalize(source_vec) * ch->dist_mult;
	
	dot = DotProduct(snd.listener_right, source_vec);

	if (snd.dma.channels == 1)
	{
		rscale = 1.0;
		lscale = 1.0;
	}
	else
	{
		rscale = 1.0f + dot;
		lscale = 1.0f - dot;
	}

// add in distance effect
	scale = (1.0f - dist) * rscale;
	ch->rightvol = (int) (ch->master_vol * scale);
	if (ch->rightvol < 0)
		ch->rightvol = 0;

	scale = (1.0f - dist) * lscale;
	ch->leftvol = (int) (ch->master_vol * scale);
	if (ch->leftvol < 0)
		ch->leftvol = 0;
}           


// =======================================================================
// Start a sound effect
// =======================================================================

void S_StartSound(int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol, float attenuation)
{
	channel_t *target_chan, *check;
	sfxcache_t	*sc;
	int		vol;
	int		ch_idx;
	int		skip;

	if (!snd.started)
		return;

	if (!sfx)
		return;

	if (nosound.value)
		return;

	vol = (int)(fvol*255);

// pick a channel to play on
	target_chan = SND_PickChannel(entnum, entchannel);
	if (!target_chan)
		return;
		
// spatialize
	memset (target_chan, 0, sizeof(*target_chan));
	VectorCopy(origin, target_chan->origin);
	target_chan->dist_mult = attenuation / sound_nominal_clip_dist;
	target_chan->master_vol = vol;
	target_chan->entnum = entnum;
	target_chan->entchannel = entchannel;
	SND_Spatialize(target_chan);

	if (!target_chan->leftvol && !target_chan->rightvol)
		return;		// not audible at all

// new channel
	sc = S_LoadSound (sfx);
	if (!sc)
	{
		target_chan->sfx = NULL;
		return;		// couldn't load the sound's data
	}

	target_chan->sfx = sfx;
	target_chan->pos = (int)0.0;
    target_chan->end = snd.paintedtime + sc->length;	

// if an identical sound has also been started this frame, offset the pos
// a bit to keep it from just making the first one louder
	check = &snd.channels[NUM_AMBIENTS];
    for (ch_idx=NUM_AMBIENTS ; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS ; ch_idx++, check++)
    {
		if (check == target_chan)
			continue;
		if (check->sfx == sfx && !check->pos)
		{
			skip = S_Rand () % (int)(0.1*snd.dma.speed);
			if (skip >= target_chan->end)
				skip = target_chan->end - 1;
			target_chan->pos += skip;
			target_chan->end -= skip;
			break;
		}
		
	}
}

void S_StopSound(int entnum, int entchannel)
{
	int i;

	// the entities' channels, after the ambient ones (id's looked at the first
	// eight, the ambients among them)
	for (i=NUM_AMBIENTS ; i<NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS ; i++)
	{
		if (snd.channels[i].entnum == entnum
			&& snd.channels[i].entchannel == entchannel)
		{
			snd.channels[i].end = 0;
			snd.channels[i].sfx = NULL;
			return;
		}
	}
}

void S_StopAllSounds(bool clear)
{
	int		i;

	if (!snd.started)
		return;

	snd.total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;	// no statics

	for (i=0 ; i<snd.max_channels ; i++)
		if (snd.channels[i].sfx)
			snd.channels[i].sfx = NULL;

	Q_memset(snd.channels, 0, (size_t)snd.max_channels * sizeof(channel_t));

	if (clear)
		S_ClearBuffer ();
}

void S_StopAllSoundsC (void)
{
	S_StopAllSounds (true);
}

void S_StopDynamicSounds (void)
{
	if (!snd.started)
		return;
	memset (snd.channels + NUM_AMBIENTS, 0, MAX_DYNAMIC_CHANNELS * sizeof(channel_t));
}

void S_ClearBuffer (void)
{
	int		clear;
	byte	*buffer;

	if (!snd.started)
		return;

	buffer = SNDDMA_LockBuffer ();
	if (!buffer)
		return;

	if (snd.dma.samplebits == 8)
		clear = 0x80;
	else
		clear = 0;
	memset (buffer, clear, (size_t)(snd.dma.samples * snd.dma.samplebits/8));

	SNDDMA_UnlockBuffer (buffer);
}


/*
=================
S_StaticSound
=================
*/
void S_StaticSound (sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
	channel_t	*ss;
	sfxcache_t		*sc;

	if (!sfx)
		return;

	if (snd.total_channels == snd.max_channels)
	{	// no channel pointer is kept between frames, so the array may move
		snd.channels = Mem_Realloc (snd.channels, (size_t)snd.max_channels * 2 * sizeof(channel_t));
		memset (snd.channels + snd.max_channels, 0, (size_t)snd.max_channels * sizeof(channel_t));
		snd.max_channels *= 2;
	}

	ss = &snd.channels[snd.total_channels];
	snd.total_channels++;

	sc = S_LoadSound (sfx);
	if (!sc)
		return;

	if (sc->loopstart == -1)
	{
		Con_Printf ("Sound %s not looped\n", sfx->name);
		return;
	}
	
	ss->sfx = sfx;
	VectorCopy (origin, ss->origin);
	ss->master_vol = (int)vol;
	ss->dist_mult = (attenuation/64) / sound_nominal_clip_dist;
    ss->end = snd.paintedtime + sc->length;	
	
	SND_Spatialize (ss);
}


//=============================================================================

/*
===================
S_UpdateAmbientSounds
===================
*/
static void S_UpdateAmbientSounds (const byte *levels, float frametime)
{
	float		vol;
	int			ambient_channel;
	channel_t	*chan;

	if (!snd.ambient)
		return;

// calc ambient sound levels
	if (!levels || !ambient_level.value)
	{
		for (ambient_channel = 0 ; ambient_channel< NUM_AMBIENTS ; ambient_channel++)
			snd.channels[ambient_channel].sfx = NULL;
		return;
	}

	for (ambient_channel = 0 ; ambient_channel< NUM_AMBIENTS ; ambient_channel++)
	{
		chan = &snd.channels[ambient_channel];	
		chan->sfx = snd.ambient_sfx[ambient_channel];
	
		vol = ambient_level.value * levels[ambient_channel];
		if (vol < 8)
			vol = 0;

	// don't adjust volume too fast
		if (chan->master_vol < vol)
		{
			chan->master_vol = (int)(chan->master_vol + frametime * ambient_fade.value);
			if (chan->master_vol > vol)
				chan->master_vol = (int)vol;
		}
		else if (chan->master_vol > vol)
		{
			chan->master_vol = (int)(chan->master_vol - frametime * ambient_fade.value);
			if (chan->master_vol < vol)
				chan->master_vol = (int)vol;
		}
		
		chan->leftvol = chan->rightvol = chan->master_vol;
	}
}


/*
============
S_Update

Called once each time through the main loop
============
*/
void S_Update (const snd_listener_t *listener)
{
	int			i, j;
	int			total;
	channel_t	*ch;
	channel_t	*combine;

	if (!snd.started || (snd.blocked > 0))
		return;

	VectorCopy(listener->origin, snd.listener_origin);
	VectorCopy(listener->forward, snd.listener_forward);
	VectorCopy(listener->right, snd.listener_right);
	VectorCopy(listener->up, snd.listener_up);
	snd.viewentity = listener->viewentity;

// update general area ambient sound sources
	S_UpdateAmbientSounds (listener->ambient_levels, listener->frametime);

	combine = NULL;

// update spatialization for static and dynamic sounds	
	ch = snd.channels+NUM_AMBIENTS;
	for (i=NUM_AMBIENTS ; i<snd.total_channels; i++, ch++)
	{
		if (!ch->sfx)
			continue;
		SND_Spatialize(ch);         // respatialize channel
		if (!ch->leftvol && !ch->rightvol)
			continue;

	// try to combine static sounds with a previous channel of the same
	// sound effect so we don't mix five torches every frame
	
		if (i >= MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS)
		{
		// see if it can just use the last one
			if (combine && combine->sfx == ch->sfx)
			{
				combine->leftvol += ch->leftvol;
				combine->rightvol += ch->rightvol;
				ch->leftvol = ch->rightvol = 0;
				continue;
			}
		// search for one
			combine = snd.channels+MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;
			for (j=MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS ; j<i; j++, combine++)
				if (combine->sfx == ch->sfx)
					break;
					
			if (j == snd.total_channels)
			{
				combine = NULL;
			}
			else
			{
				if (combine != ch)
				{
					combine->leftvol += ch->leftvol;
					combine->rightvol += ch->rightvol;
					ch->leftvol = ch->rightvol = 0;
				}
				continue;
			}
		}
		
		
	}

//
// debugging output
//
	if (snd_show.value)
	{
		total = 0;
		ch = snd.channels;
		for (i=0 ; i<snd.total_channels; i++, ch++)
			if (ch->sfx && (ch->leftvol || ch->rightvol) )
			{
				//Con_Printf ("%3i %3i %s\n", ch->leftvol, ch->rightvol, ch->sfx->name);
				total++;
			}
		
		Con_Printf ("----(%i)----\n", total);
	}

// mix some sound
	S_Update_();
}

void GetSoundtime(void)
{
	int		samplepos;
	static	int		buffers;
	static	int		oldsamplepos;
	int		fullsamples;
	
	fullsamples = snd.dma.samples / snd.dma.channels;

// it is possible to miscount buffers if it has wrapped twice between
// calls to S_Update.  Oh well.
	samplepos = SNDDMA_GetDMAPos();

	if (samplepos < oldsamplepos)
	{
		buffers++;					// buffer wrapped
		
		if (snd.paintedtime > 0x40000000)
		{	// time to chop things off to avoid 32 bit limits
			buffers = 0;
			snd.paintedtime = fullsamples;
			S_StopAllSounds (true);
		}
	}
	oldsamplepos = samplepos;

	snd.soundtime = buffers*fullsamples + samplepos/snd.dma.channels;
}

void S_ExtraUpdate (void)
{
	if (snd_noextraupdate.value)
		return;		// don't pollute timings
	S_Update_();
}



void S_Update_(void)
{
	unsigned        endtime;
	int				samps;
	
	if (!snd.started || (snd.blocked > 0))
		return;

// Updates DMA time
	GetSoundtime();

// check to make sure that we haven't overshot
	if (snd.paintedtime < snd.soundtime)
	{
		//Con_Printf ("S_Update_ : overflow\n");
		snd.paintedtime = snd.soundtime;
	}

// mix ahead of current position
	endtime = (unsigned int)(snd.soundtime + _snd_mixahead.value * snd.dma.speed);
	samps = snd.dma.samples >> (snd.dma.channels-1);
	if (endtime - snd.soundtime > (unsigned)samps)
		endtime = snd.soundtime + samps;

	S_PaintChannels (endtime);

	SNDDMA_Submit ();
}

/*
===============================================================================

console functions

===============================================================================
*/

void S_Play(void)
{
	static int hash=345;
	int 	i;
	char sndname[256];
	sfx_t	*sfx;

	i = 1;
	while (i<Cmd_Argc())
	{
		if (!Q_strrchr(Cmd_Argv(i), '.'))
		{
			Q_strncpyz(sndname, Cmd_Argv(i), sizeof(sndname));
			Q_strncatz(sndname, ".wav", sizeof(sndname));
		}
		else
			Q_strncpyz(sndname, Cmd_Argv(i), sizeof(sndname));
		sfx = S_PrecacheSound(sndname);
		S_StartSound(hash++, 0, sfx, snd.listener_origin, 1.0, 1.0);
		i++;
	}
}

void S_PlayVol(void)
{
	static int hash=543;
	int i;
	float vol;
	char sndname[256];
	sfx_t	*sfx;

	i = 1;
	while (i<Cmd_Argc())
	{
		if (!Q_strrchr(Cmd_Argv(i), '.'))
		{
			Q_strncpyz(sndname, Cmd_Argv(i), sizeof(sndname));
			Q_strncatz(sndname, ".wav", sizeof(sndname));
		}
		else
			Q_strncpyz(sndname, Cmd_Argv(i), sizeof(sndname));
		sfx = S_PrecacheSound(sndname);
		vol = Q_atof(Cmd_Argv(i+1));
		S_StartSound(hash++, 0, sfx, snd.listener_origin, vol, 1.0);
		i+=2;
	}
}

void S_SoundList(void)
{
	int		i;
	sfx_t	*sfx;
	sfxcache_t	*sc;
	int		size, total;

	total = 0;
	for (sfx=snd.known_sfx, i=0 ; i<snd.num_sfx ; i++, sfx++)
	{
		sc = sfx->data;
		if (!sc)
			continue;
		size = sc->length*sc->width*(sc->stereo+1);
		total += size;
		if (sc->loopstart >= 0)
			Con_Printf ("L");
		else
			Con_Printf (" ");
		Con_Printf("(%2db) %6i : %s\n",sc->width*8,  size, sfx->name);
	}
	Con_Printf ("Total resident: %i\n", total);
}


void S_LocalSound (char *sound)
{
	sfx_t	*sfx;

	if (nosound.value)
		return;
	if (!snd.started)
		return;
		
	sfx = S_PrecacheSound (sound);
	if (!sfx)
	{
		Con_Printf ("S_LocalSound: can't cache %s\n", sound);
		return;
	}
	S_StartSound (snd.viewentity, -1, sfx, vec3_origin, 1, 1);
}

