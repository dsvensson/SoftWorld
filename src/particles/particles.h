/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

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
// particles.h -- scripted particle effects, FTE's as QuakeSpasm-Spiked ports
// them. An effect is named in a script: FTE's (particles/<name>.cfg, r_part
// blocks) or DarkPlaces' (effectinfo.txt). It is started at a point, along a
// trail, or by surfaces of the map, and the particles run once a frame into
// the renderer's list (r_partscene_t). r_particledesc names the scripts
// loaded; "classic", the default, loads none, and an effect a game's QuakeC
// names as "<script>.<effect>" loads its script. Where no script has an
// effect, the client draws id's particles.

#include "mathlib.h"
#include "render.h"

#define	P_INVALID	-1

// where a trail left off, and an emitter's time: kept by whoever runs one,
// NULL at first; P_DelinkTrailstate lets it go
typedef struct p_trailstate_s	p_trailstate_t;

// what the module asks of the client
typedef struct
{
	const byte	*palette;		// gfx/palette.lmp, for effects colored by index

	// the first thing a line hits from start to end: the fraction of the way,
	// where and the surface's normal, and the entity hit (0 the world)
	float	(*trace) (const vec3_t start, const vec3_t end, vec3_t impact, vec3_t normal, int *entnum);
	int		(*contents) (const vec3_t p);			// the world's CONTENTS_ at p

	// an entity's brush model and where it is, for decals stuck on it; NULL
	// if it has none, or turns
	struct model_s	*(*brushentity) (int entnum, vec3_t origin);

	// a light at org: key reuses a light (0 none), lasting time seconds, its
	// radius shrinking by decay a second
	void	(*dlight) (int key, const vec3_t org, float radius, float time, float decay, const vec3_t rgb);
	void	(*precachesound) (const char *name);
	void	(*sound) (const vec3_t org, const char *name, float volume, float attenuation);

	// the effects changed: indices the client found are to be found again
	void	(*changed) (void);
} p_host_t;

// a frame's particles: the time they run to, and where surfaces make them
typedef struct
{
	double	time;			// the client's, which effects are timed by
	double	realtime;		// lights' flicker
	float	frametime;		// the frame's, for effects made each frame ("perframe")
	struct model_s	*worldmodel;	// whose faces make effects
	vec3_t	vieworg;		// they make them near the view
	int		visframe;		// in the leafs of the view's PVS (r_scene.visframe)
} p_frame_t;

void	P_Init (const p_host_t *host);		// cvars and commands, then r_particledesc's scripts
void	P_Shutdown (void);
void	P_NewMap (const char *mapname);		// all particles gone, the scripts again with map_<mapname>.cfg
void	P_ClearParticles (void);			// all particles and decals gone (a demo's seek)
void	P_ReloadScripts (void);				// r_particledesc's scripts again: a new game directory

// the effect of a name, "<script>.<effect>" for one of a script, which is
// loaded if it isn't; P_INVALID if no script has it
int		P_FindParticleType (const char *name);

// an effect at org, count times, toward dir (NULL none); false if there is no
// such effect
bool	P_RunEffect (const vec3_t org, const vec3_t dir, float count, int type, p_trailstate_t **ts);
bool	P_RunEffectName (const vec3_t org, const vec3_t dir, float count, const char *name);

// an effect along a trail from start to end, over frametime seconds; dlkey
// keys its light and axis turns it (NULL for none)
bool	P_Trail (const vec3_t start, const vec3_t end, int type, float frametime, int dlkey,
			const vec3_t axis[3], p_trailstate_t **ts);
void	P_DelinkTrailstate (p_trailstate_t **ts);

// id's particle effect of a palette color: pe_<color>, else PE_SIZE3, PE_SIZE2
// or PE_DEFAULT by count; false if the scripts have none
bool	P_RunPaletteEffect (const vec3_t org, const vec3_t dir, int color, int count);

// count particles of te_<name>_<colour> or te_<name> anywhere in a box
// (DarkPlaces' te_particlerain and te_particlesnow), moving by dir
void	P_RunWeather (const vec3_t mins, const vec3_t maxs, const vec3_t dir, float count, int colour,
			const char *name);

// the effects scripts give a model (r_trail and r_effect): P_INVALID none;
// emitflags P_EMIT*
#define	P_EMITREPLACE	1		// the effect is drawn in place of the model
#define	P_EMITFORWARDS	2		// toward the model's forward, else up
int		P_ModelTrail (const char *modelname);
int		P_ModelEmit (const char *modelname, unsigned *emitflags);

// the frame's particles run, into the renderer's list
const r_partscene_t	*P_RunFrame (const p_frame_t *frame);
