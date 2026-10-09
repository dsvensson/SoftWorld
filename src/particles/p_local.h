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
// p_local.h -- the scripted particles' own: particle types (an effect is a
// chain of them, each the next's assoc), their particles, beam segments,
// decals and trail states. p_script.c reads the scripts into types, p_spawn.c
// starts effects, p_run.c runs them each frame.

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bspfile.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"
#include "mem.h"
#include "print.h"
#include "q_string.h"
#include "particles.h"

#define	P_MAXPARTICLES		(1 << 18)	// r_part_maxparticles at most
#define	P_MAXDECALS			(1 << 18)	// r_part_maxdecals at most
#define	P_MAXBEAMSEGS		(1 << 11)
#define	P_MAXTRAILSTATES	(1 << 10)

// the contents particles tell apart, as bits (FTE's)
#define	P_CONT_SOLID		1
#define	P_CONT_WATER		2
#define	P_CONT_SLIME		4
#define	P_CONT_LAVA			8
#define	P_CONT_SKY			16
#define	P_CONT_FLUID		(P_CONT_WATER | P_CONT_SLIME | P_CONT_LAVA | P_CONT_SKY)

typedef enum
{
	PT_NORMAL,			// a textured square facing the view
	PT_SPARK,			// a line along its motion
	PT_SPARKFAN,		// a triangle along its motion
	PT_TEXTUREDSPARK,	// a textured quad along its motion
	PT_BEAM,			// quads between the particles of a trail
	PT_CDECAL,			// cut to the surfaces it lies on
	PT_UDECAL,			// a square lying flat
	PT_INVISIBLE
} p_shape_t;

// how a type draws: types that draw alike are drawn together
typedef struct
{
	p_shape_t		type;
	r_partblend_t	blendmode;
	int		image;					// R_ParticleImage's
	int		premul;					// r_partbatch_t's
	float	scalefactor, invscalefactor;
	float	stretch, minstretch;	// a textured spark's length, by its speed (< 0: fixed)
} p_looks_t;

typedef enum
{
	SM_BOX,				// evenly in a box
	SM_CIRCLE,			// on a circle's edge
	SM_BALL,			// in a sphere
	SM_SPIRAL,			// round a trail
	SM_TRACER,			// either side of a trail, by turns
	SM_TELEBOX,			// id's teleport splash
	SM_LAVASPLASH,		// id's lava splash
	SM_UNICIRCLE,		// evenly round a circle
	SM_FIELD,			// id's bright field
	SM_DISTBALL			// a ball, thicker inside
} p_spawnmode_t;

typedef enum {RAMP_NONE, RAMP_DELTA, RAMP_NEAREST, RAMP_LERP} p_rampmode_t;

// a step of a ramp: absolute or a change a second, by the ramp mode
typedef struct
{
	vec3_t	rgb;
	float	alpha;
	float	scale;
} p_ramp_t;

typedef struct
{
	char	name[MAX_QPATH];
	float	vol;
	float	atten;
	float	weight;				// the chance of this one of the type's
} p_sound_t;

typedef struct p_particle_s
{
	struct p_particle_s	*next;
	float		die;			// p_time it goes; -1 at once
	vec3_t		org;
	vec3_t		oldorg;			// where it was last traced from
	vec3_t		vel;
	float		rgba[4];
	float		scale;
	float		angle, rotationspeed;
	float		s1, t1, s2, t2;
	union
	{
		float			nextemit;	// an emitter's next time
		p_trailstate_t	*trailstate;	// an emitter of trails'
	} state;
} p_particle_t;

#define	BS_LASTSEG	1			// a trail's last: not drawn to the next, not deleted
#define	BS_DEAD		2
#define	BS_NODRAW	4			// not drawn to the next

typedef struct p_beamseg_s
{
	struct p_beamseg_s	*next;
	p_particle_t	*p;
	int				flags;
	vec3_t			dir;
	float			texture_s;
} p_beamseg_t;

typedef struct p_decal_s
{
	struct p_decal_s	*next;
	float		die;
	int			entity;			// stuck on it (its coordinates), 0 the world
	vec3_t		vertex[3];
	float		texcoords[3][2];
	float		valpha[3];
	float		rgba[4];
} p_decal_t;

struct p_trailstate_s
{
	p_trailstate_t	**key;		// who has it: someone else took it when not them
	p_trailstate_t	*assoc;		// the next type's of the chain
	p_beamseg_t		*lastbeam;	// the trail's last beam segment (BS_LASTSEG)
	union
	{
		float	lastdist;		// how far along the trail's step it left off
		float	statetime;		// spawntime: when it may run again
	} state1;
	union
	{
		float	laststop;		// where it left off
		float	emittime;		// particles it owes
	} state2;
};

// a type's flags
#define	PT_VELOCITY			0x0001	// it moves
#define	PT_FRICTION			0x0002
#define	PT_CITRACER			0x0008	// id's tracers' colors
#define	PT_INVFRAMETIME		0x0010	// count is a second's, made each frame
#define	PT_AVERAGETRAIL		0x0020	// a trail's particles spread to end at its end
#define	PT_NOSTATE			0x0040	// no trail state
#define	PT_NOSPREADFIRST	0x0080	// a trail's first particle is not spread
#define	PT_NOSPREADLAST		0x0100	// nor its last
#define	PT_TROVERWATER		0x0200	// none in fluidmask
#define	PT_TRUNDERWATER		0x0400	// none out of fluidmask
#define	PT_WORLDSPACERAND	0x1000	// orgwrand or velwrand

typedef struct p_type_s
{
	char	name[MAX_QPATH];
	char	config[MAX_QPATH];		// the script (namespace) it is of
	char	texname[MAX_QPATH];

	int		numsounds;
	p_sound_t	*sounds;

	vec3_t	rgb;
	float	alpha;
	vec3_t	rgbchange;				// a second
	float	alphachange;
	vec3_t	rgbrand;				// up to this much more
	float	alpharand;
	int		colorindex;				// palette colors instead, -1 none
	int		colorrand;				// and up to this many after it
	float	rgbchangetime;			// the color stops changing after this
	vec3_t	rgbrandsync;			// how much of rgbrand is one random number
	float	scale, scalerand;
	float	die, randdie;			// how long it lasts, up to randdie less
	float	veladd, randomveladd;	// of the effect's own speed (its dir's length)
	float	orgadd, randomorgadd;	// along it
	float	spawnvel, spawnvelvert;	// outwards, by the spawn mode
	vec3_t	orgbias, velbias;		// added in the world's axes
	vec3_t	orgwrand, velwrand;		// and as much at random
	float	flurry;					// snow's random gusts

	float	s1, t1, s2, t2;
	float	texsstride;				// s added for each of randsmax images
	int		randsmax;

	p_looks_t	looks;

	float	spawntime;				// a trail runs at most once in this long
	float	spawnchance;

	float	rotationstartmin, rotationstartrand;
	float	rotationmin, rotationrand;	// a second

	float	scaledelta;
	float	countextra;				// particles besides count's
	float	count;
	float	countrand;
	float	countspacing;			// a trail's: units a particle
	float	countoverflow;			// a trail's part of a particle left over
	float	rainfrequency;			// surface effects' rate

	int		assoc;					// the next type of the effect
	int		cliptype;				// what it becomes hitting something
	int		inwater;				// the effect in water instead
	float	clipcount;
	int		emit;					// what its particles emit
	float	emittime;				// how often (< 0 trails)
	float	emitrand;
	float	emitstart;

	float	areaspread, areaspreadvert;
	float	spawnparam1, spawnparam2;
	p_spawnmode_t	spawnmode;

	float	gravity;
	vec3_t	friction;
	float	clipbounce;				// < 0 dies hitting: -2 as a decal

	vec3_t	dl_rgb;
	float	dl_radius[2];			// and up to [1] more, flickering
	float	dl_time;
	float	dl_decay;				// radius a second

	p_rampmode_t	rampmode;
	int		rampindexes;
	p_ramp_t	*ramp;

	int		loaded;					// 0 not, 1 by a namespace (weak), 2 by r_particledesc
	p_particle_t	*particles;
	p_decal_t		*clippeddecals;
	p_beamseg_t		*beams;
	struct p_type_s	*nexttorun;
	bool	inrunlist;

	unsigned	flags;				// PT_
	unsigned	fluidmask;			// P_CONT_ of underwater and notunderwater
} p_type_t;

// p_script.c
extern p_host_t		p_host;
extern p_type_t		*p_types;
extern int			p_numtypes;
extern p_type_t		*p_runlist;
extern bool			p_looksdirty;	// a type was changed: P_UpdateLooks
extern int			pe_default, pe_size2, pe_size3, pe_defaulttrail;

extern cvar_t	r_part_rain, r_part_rain_quantity, r_part_density, r_part_maxparticles, r_part_maxdecals, r_part_contentswitch,
				r_bouncysparks, r_particle_tracelimit, r_decal_noperpendicular, r_lightflicker;

int		P_AllocateParticleType (const char *config, const char *name);	// found or made
void	P_UpdateLooks (void);
unsigned	P_PointContents (const vec3_t p);	// P_CONT_
void	P_PaletteColor (int index, float *rgb);	// 0 to 1

// p_spawn.c
extern p_particle_t	*p_particles, *p_freeparticles;
extern int			p_numparticles, p_particlerecycle;
extern p_beamseg_t	*p_beams, *p_freebeams;
extern int			p_numbeams;
extern p_decal_t	*p_decals, *p_freedecals;
extern int			p_numdecals, p_decalrecycle;
extern float		p_time;			// the particles', the client's as they run
extern double		p_clienttime, p_realtime;
extern float		p_frametime;

void	P_AllocParticles (float maxparticles, float maxdecals);
void	P_FreeParticles (void);
void	P_ClearTrailStates (void);
void	P_AddToRunList (p_type_t *type);
void	P_SplatDecal (p_type_t *type, const vec3_t org, const vec3_t normal, int entnum, float size);

// random numbers: 0 to 1, -1 to 1, -0.5 to 0.5
static inline float P_Random (void)
{
	return rand () * (1.0f / (float)RAND_MAX);
}

static inline float P_CRandom (void)
{
	return rand () * (2.0f / (float)RAND_MAX) - 1;
}

static inline float P_HRandom (void)
{
	return rand () * (1.0f / (float)RAND_MAX) - 0.5f;
}

#define	VectorSet(v, x, y, z)	((v)[0] = (x), (v)[1] = (y), (v)[2] = (z))
#define	VectorClear(v)			((v)[0] = (v)[1] = (v)[2] = 0)

// mathlib's take no const vectors
static inline void P_VectorMA (const vec3_t a, float scale, const vec3_t b, vec3_t out)
{
	out[0] = a[0] + scale * b[0];
	out[1] = a[1] + scale * b[1];
	out[2] = a[2] + scale * b[2];
}

static inline void P_CrossProduct (const vec3_t a, const vec3_t b, vec3_t out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline float P_Normalize (vec3_t v)
{
	float	len = sqrtf (DotProduct (v, v));

	if (len)
	{
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
	return len;
}
