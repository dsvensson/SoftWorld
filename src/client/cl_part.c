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
// cl_part.c -- the scripted particles' client (src/particles): what they hit,
// the client's brush entities as prediction has them (cl.pmove.physents),
// their lights and sounds, the effects the client and the server name, found
// again whenever the scripts change, and their run once a frame into the scene

#include "cl_local.h"

static const char	*cl_effectnames[PT_NUM] =
{
	"TE_GUNSHOT", "TE_GUNSHOTQUAD", "TE_QWGUNSHOT", "TE_SPIKE", "TE_SPIKEQUAD", "TE_SUPERSPIKE", "TE_SUPERSPIKEQUAD",
	"TE_BULLET", "TE_SUPERBULLET", "TE_WIZSPIKE", "TE_KNIGHTSPIKE",
	"TE_EXPLOSION", "TE_EXPLOSIONQUAD", "TE_TEI_BIGEXPLOSION", "TE_TAREXPLOSION", "TE_LAVASPLASH", "TE_TELEPORT",
	"TE_BLOOD", "TE_QWBLOOD", "TE_LIGHTNINGBLOOD", "TE_SPARK", "TE_FLAMEJET", "TE_PLASMABURN", "TE_SMALLFLASH",
	"TE_TEI_SMOKE", "TE_TEI_PLASMAHIT", "TE_TEI_G3", "te_nexbeam", "TE_RAILTRAIL",
	"TE_LIGHTNING1", "TE_LIGHTNING1_END", "TE_LIGHTNING2", "TE_LIGHTNING2_END", "TE_LIGHTNING3", "TE_LIGHTNING3_END",
	"TE_BEAM", "TE_BEAM_END",
	"TR_ROCKET", "TR_GRENADE", "TR_BLOOD", "TR_WIZSPIKE", "TR_SLIGHTBLOOD", "TR_KNIGHTSPIKE", "TR_VORESPIKE",
};

static int		cl_effects[PT_NUM];
static int		cl_particles[MAX_PARTICLE_PRECACHE];	// the server's list's
static int		cl_modeltrails[MAX_MODELS], cl_modelemits[MAX_MODELS];
static unsigned	cl_modelemitflags[MAX_MODELS];

int CL_Effect (cl_effect_t effect)
{
	return cl_effects[effect];
}

const char *CL_EffectName (cl_effect_t effect)
{
	return cl_effectnames[effect];
}

int CL_ParticleEffect (int index)
{
	return index > 0 && index < MAX_PARTICLE_PRECACHE && cl.particle_name[index][0] ? cl_particles[index] : P_INVALID;
}

void CL_PrecacheParticle (int index)
{
	cl_particles[index] = P_FindParticleType (cl.particle_name[index]);
}

void CL_PrecacheModelEffects (int index)
{
	cl_modeltrails[index] = cl.model_name[index][0] ? P_ModelTrail (cl.model_name[index]) : P_INVALID;
	cl_modelemits[index] = cl.model_name[index][0] ? P_ModelEmit (cl.model_name[index], &cl_modelemitflags[index])
		: P_INVALID;
}

int CL_ModelTrail (int modelindex)
{
	return modelindex > 0 && modelindex < MAX_MODELS ? cl_modeltrails[modelindex] : P_INVALID;
}

int CL_ModelEmit (int modelindex, unsigned *emitflags)
{
	*emitflags = modelindex > 0 && modelindex < MAX_MODELS ? cl_modelemitflags[modelindex] : 0;
	return modelindex > 0 && modelindex < MAX_MODELS ? cl_modelemits[modelindex] : P_INVALID;
}

// the scripts changed: every name found again
static void CL_PartChanged (void)
{
	int		i;

	for (i = 0 ; i < PT_NUM ; i++)
		cl_effects[i] = P_FindParticleType (cl_effectnames[i]);
	for (i = 1 ; i < MAX_PARTICLE_PRECACHE ; i++)
		cl_particles[i] = cl.particle_name[i][0] ? P_FindParticleType (cl.particle_name[i]) : P_INVALID;
	for (i = 1 ; i < MAX_MODELS ; i++)
		CL_PrecacheModelEffects (i);
	CSQC_ParticlesChanged ();
}

// the first brush entity a line hits, as prediction has them: hull 0 of each
float CL_PartTrace (const vec3_t start, const vec3_t end, vec3_t impact, vec3_t normal, int *entnum)
{
	const physent_t	*pe;
	trace_t			trace;
	vec3_t			s, e;
	float			frac = 1;
	int				i;

	VectorCopy (end, impact);
	normal[0] = normal[1] = 0;
	normal[2] = 1;
	*entnum = 0;
	for (i = 0 ; i < cl.pmove.numphysent ; i++)
	{
		pe = &cl.pmove.physents[i];
		if (!pe->model)
			continue;		// a player's box
		VectorSubtract (start, pe->origin, s);
		VectorSubtract (end, pe->origin, e);
		memset (&trace, 0, sizeof(trace));
		trace.fraction = 1;
		trace.allsolid = true;
		VectorCopy (e, trace.endpos);
		CM_RecursiveHullCheck (&pe->model->hulls[0], pe->model->hulls[0].firstclipnode, 0, 1, s, e, &trace);
		if (trace.fraction < frac && !trace.allsolid)
		{
			frac = trace.fraction;
			VectorAdd (trace.endpos, pe->origin, impact);
			VectorCopy (trace.plane.normal, normal);
			*entnum = pe->info;
			if (frac <= 0)
				break;
		}
	}
	return frac;
}

static int CL_PartContents (const vec3_t p)
{
	if (!cl.clipmodels[1])
		return CONTENTS_EMPTY;
	return CM_HullPointContents (&cl.clipmodels[1]->hulls[0], cl.clipmodels[1]->hulls[0].firstclipnode, p);
}

// an entity's brush model and origin in the latest packet; the world's for 0
static struct model_s *CL_PartBrushEntity (int entnum, vec3_t origin)
{
	const cl_entities_t		*pak = &cl.frames[cl.parsecountmod].packet_entities;
	const entity_state_t	*s;
	model_t					*m;
	int						i;

	origin[0] = origin[1] = origin[2] = 0;
	if (!entnum)
		return cl.worldmodel;
	for (i = 0 ; i < pak->num_entities ; i++)
	{
		s = &pak->entities[i];
		if (s->number != entnum)
			continue;
		m = CL_Model (s->modelindex);
		if (!m || m->type != mod_brush || s->angles[0] || s->angles[1] || s->angles[2])
			return NULL;
		VectorCopy (s->origin, origin);
		return m;
	}
	return NULL;
}

static void CL_PartDlight (int key, const vec3_t org, float radius, float time, float decay, const vec3_t rgb)
{
	dlight_t	*dl = CL_AllocDlight (key);

	VectorCopy (org, dl->origin);
	dl->radius = radius;
	dl->die = (float)(cl.time + time);
	dl->decay = decay;
	VectorCopy (rgb, dl->color);
}

static void CL_PartPrecacheSound (const char *sample)
{
	S_PrecacheSound ((char *)sample);
}

static void CL_PartSound (const vec3_t org, const char *sample, float vol, float attenuation)
{
	vec3_t	o;

	VectorCopy (org, o);
	S_StartSound (0, 0, S_PrecacheSound ((char *)sample), o, vol, attenuation);
}

void CL_InitParticles (void)
{
	p_host_t	ph =
	{
		.palette = cls.basepal,
		.trace = CL_PartTrace,
		.contents = CL_PartContents,
		.brushentity = CL_PartBrushEntity,
		.dlight = CL_PartDlight,
		.precachesound = CL_PartPrecacheSound,
		.sound = CL_PartSound,
		.changed = CL_PartChanged,
	};

	P_Init (&ph);
}

// a level's start: map_<name>.cfg for it
void CL_NewMapParticles (void)
{
	char	mapname[MAX_QPATH], *slash;

	slash = strrchr (cl.worldmodel->name, '/');
	COM_StripExtension (slash ? slash + 1 : cl.worldmodel->name, mapname);
	P_NewMap (mapname);
}

// a new game directory's images and scripts
void CL_ReloadParticles (void)
{
	P_ClearParticles ();
	R_FlushParticleImages ();
	P_ReloadScripts ();
}

// the frame's particles, into the scene
void CL_RunParticles (void)
{
	p_frame_t	frame = {.time = cl.time, .realtime = host.realtime, .frametime = (float)cls.frametime};

	r_scene.particles = P_RunFrame (&frame);
}
