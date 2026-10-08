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
// cl_tent.c -- client side temporary entities

#include "cl_local.h"


#define	MAX_BEAMS	8
typedef struct
{
	int		entity;
	struct model_s	*model;
	float	endtime;
	vec3_t	start, end;
} beam_t;

static beam_t		cl_beams[MAX_BEAMS];

// how far toward the current aim the beam of whose eyes the view is turns
// from where the server last put it, 0 .. 1: hides the beam's lag behind the
// view (FTE)
static cvar_t		cl_truelightning = {.name = "cl_truelightning", .string = "1", .archive = true,
	.description = "How far the lightning beam of the player whose view you see (yours, or the one a demo or "
		"spectating follows) turns from the server's aim toward the view, 0 to 1, hiding its lag; 0 draws it "
		"as sent."};

#define	MAX_EXPLOSIONS	8
typedef struct
{
	vec3_t	origin;
	float	start;
	model_t	*model;
} explosion_t;

static explosion_t	cl_explosions[MAX_EXPLOSIONS];


static sfx_t			*cl_sfx_wizhit;
static sfx_t			*cl_sfx_knighthit;
static sfx_t			*cl_sfx_tink1;
static sfx_t			*cl_sfx_ric1;
static sfx_t			*cl_sfx_ric2;
static sfx_t			*cl_sfx_ric3;
static sfx_t			*cl_sfx_r_exp3;

/*
=================
CL_ParseTEnts
=================
*/
void CL_InitTEnts (void)
{
	Cvar_RegisterVariable (&cl_truelightning);
	cl_sfx_wizhit = S_PrecacheSound ("wizard/hit.wav");
	cl_sfx_knighthit = S_PrecacheSound ("hknight/hit.wav");
	cl_sfx_tink1 = S_PrecacheSound ("weapons/tink1.wav");
	cl_sfx_ric1 = S_PrecacheSound ("weapons/ric1.wav");
	cl_sfx_ric2 = S_PrecacheSound ("weapons/ric2.wav");
	cl_sfx_ric3 = S_PrecacheSound ("weapons/ric3.wav");
	cl_sfx_r_exp3 = S_PrecacheSound ("weapons/r_exp3.wav");
}

/*
=================
CL_ClearTEnts
=================
*/
void CL_ClearTEnts (void)
{
	memset (&cl_beams, 0, sizeof(cl_beams));
	memset (&cl_explosions, 0, sizeof(cl_explosions));
}

/*
=================
CL_AllocExplosion
=================
*/
static explosion_t *CL_AllocExplosion (void)
{
	int		i;
	float	time;
	int		index;
	
	for (i=0 ; i<MAX_EXPLOSIONS ; i++)
		if (!cl_explosions[i].model)
			return &cl_explosions[i];
// find the oldest explosion
	time = (float)cl.time;
	index = 0;

	for (i=0 ; i<MAX_EXPLOSIONS ; i++)
		if (cl_explosions[i].start < time)
		{
			time = cl_explosions[i].start;
			index = i;
		}
	return &cl_explosions[index];
}

/*
=================
CL_ParseBeam
=================
*/
static void CL_ParseBeam (model_t *m)
{
	int		ent;
	vec3_t	start, end;

	ent = MSG_ReadShort ();

	start[0] = MSG_ReadCoord ();
	start[1] = MSG_ReadCoord ();
	start[2] = MSG_ReadCoord ();

	end[0] = MSG_ReadCoord ();
	end[1] = MSG_ReadCoord ();
	end[2] = MSG_ReadCoord ();

	CL_AddBeam (m, ent, start, end);
}

/*
=================
CL_AddBeam

A beam of model m from entity ent for 0.2 seconds, replacing the entity's
last; CSQC's are keyed past the server's entity numbers
=================
*/
void CL_AddBeam (model_t *m, int ent, const vec3_t start, const vec3_t end)
{
	beam_t	*b;
	int		i;

// override any beam with the same entity
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
		if (b->entity == ent)
		{
			b->entity = ent;
			b->model = m;
			b->endtime = (float)(cl.time + 0.2f);
			VectorCopy (start, b->start);
			VectorCopy (end, b->end);
			return;
		}

// find a free beam
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
	{
		if (!b->model || b->endtime < cl.time)
		{
			b->entity = ent;
			b->model = m;
			b->endtime = (float)(cl.time + 0.2f);
			VectorCopy (start, b->start);
			VectorCopy (end, b->end);
			return;
		}
	}
	Con_Printf ("beam list overflow!\n");	
}

// a temporary entity as the server sent it
typedef struct
{
	int		type;
	int		ent;			// a beam's
	vec3_t	pos;			// its origin, a box's min, a beam's start
	vec3_t	pos2;			// a beam's end, a box's max, a color (0 to 1)
	vec3_t	vel;			// a velocity or a direction
	int		count;
	int		color, colors;	// palette colors: the first and how many from it
	float	time;			// TEDP_CUSTOMFLASH's
} tent_t;

static void CL_ReadVector (vec3_t v)
{
	v[0] = MSG_ReadCoord ();
	v[1] = MSG_ReadCoord ();
	v[2] = MSG_ReadCoord ();
}

/*
=================
CL_ReadTEnt

A temporary entity of any kind FTE's client reads: QuakeWorld's, FTE's
(FTE_PEXT_TE_BULLET) and DarkPlaces'. One of another kind can't be read past,
so it ends the game.
=================
*/
static void CL_ReadTEnt (tent_t *te)
{
	memset (te, 0, sizeof(*te));
	te->type = MSG_ReadByte ();
	te->count = 1;
	switch (te->type)
	{
	case TE_LIGHTNING1:
	case TE_LIGHTNING2:
	case TE_LIGHTNING3:
	case TE_BEAM:
		te->ent = MSG_ReadShort ();
		CL_ReadVector (te->pos);
		CL_ReadVector (te->pos2);
		break;
	case TE_GUNSHOT:
	case TE_BLOOD:
		te->count = MSG_ReadByte ();
		CL_ReadVector (te->pos);
		break;
	case TE_SPIKE:
	case TE_SUPERSPIKE:
	case TE_EXPLOSION:
	case TE_TAREXPLOSION:
	case TE_WIZSPIKE:
	case TE_KNIGHTSPIKE:
	case TE_LAVASPLASH:
	case TE_TELEPORT:
	case TE_LIGHTNINGBLOOD:
	case TE_BULLET:
	case TE_SUPERBULLET:
	case TE_NQEXPLOSION:
	case TE_NQGUNSHOT:
	case TEDP_GUNSHOTQUAD:
	case TEDP_SPIKEQUAD:
	case TEDP_SUPERSPIKEQUAD:
	case TEDP_EXPLOSIONQUAD:
	case TEDP_SMALLFLASH:
	case TEDP_PLASMABURN:
	case TEDP_TEI_BIGEXPLOSION:
		CL_ReadVector (te->pos);
		break;
	case TE_EXPLOSION3_NEH:			// the color in coordinates
	case TE_RAILTRAIL:
		CL_ReadVector (te->pos);
		CL_ReadVector (te->pos2);
		break;
	case TE_EXPLOSION2:
		CL_ReadVector (te->pos);
		te->color = MSG_ReadByte ();
		te->colors = MSG_ReadByte ();
		break;
	case TEDP_BLOOD:
	case TEDP_SPARK:
		CL_ReadVector (te->pos);
		te->vel[0] = (float)(signed char)MSG_ReadByte ();
		te->vel[1] = (float)(signed char)MSG_ReadByte ();
		te->vel[2] = (float)(signed char)MSG_ReadByte ();
		te->count = MSG_ReadByte ();
		break;
	case TEDP_BLOODSHOWER:
		CL_ReadVector (te->pos);
		CL_ReadVector (te->pos2);
		te->vel[2] = -MSG_ReadCoord ();
		te->count = MSG_ReadShort () & 0xffff;
		break;
	case TEDP_EXPLOSIONRGB:
		CL_ReadVector (te->pos);
		te->pos2[0] = MSG_ReadByte () / 255.0f;
		te->pos2[1] = MSG_ReadByte () / 255.0f;
		te->pos2[2] = MSG_ReadByte () / 255.0f;
		break;
	case TEDP_PARTICLECUBE:
		CL_ReadVector (te->pos);
		CL_ReadVector (te->pos2);
		CL_ReadVector (te->vel);
		te->count = MSG_ReadShort () & 0xffff;
		te->color = MSG_ReadByte ();
		te->colors = MSG_ReadByte ();		// the gravity flag
		te->time = MSG_ReadCoord ();		// the jitter
		break;
	case TEDP_PARTICLERAIN:
	case TEDP_PARTICLESNOW:
		CL_ReadVector (te->pos);
		CL_ReadVector (te->pos2);
		CL_ReadVector (te->vel);
		te->count = MSG_ReadShort () & 0xffff;
		te->color = MSG_ReadByte ();
		break;
	case TEDP_CUSTOMFLASH:
		CL_ReadVector (te->pos);
		te->count = MSG_ReadByte () * 8;				// the radius
		te->time = (MSG_ReadByte () + 1) / 256.0f;
		te->pos2[0] = MSG_ReadByte () / 127.0f;
		te->pos2[1] = MSG_ReadByte () / 127.5f;
		te->pos2[2] = MSG_ReadByte () / 127.0f;
		break;
	case TEDP_FLAMEJET:
	case TEDP_SMOKE:
	case TEDP_TEI_PLASMAHIT:
		CL_ReadVector (te->pos);
		CL_ReadVector (te->vel);
		te->count = MSG_ReadByte ();
		break;
	case TEDP_TEI_G3:
		CL_ReadVector (te->pos);
		CL_ReadVector (te->pos2);
		CL_ReadVector (te->vel);			// unused
		break;
	default:
		Host_EndGame ("CL_ParseTEnt: bad type %i", te->type);
	}
}

// a spike's tink, now and then a ricochet
static void CL_SpikeSound (vec3_t pos)
{
	int		rnd;

	if ( rand() % 5 )
		S_StartSound (-1, 0, cl_sfx_tink1, pos, 1, 1);
	else
	{
		rnd = rand() & 3;
		if (rnd == 1)
			S_StartSound (-1, 0, cl_sfx_ric1, pos, 1, 1);
		else if (rnd == 2)
			S_StartSound (-1, 0, cl_sfx_ric2, pos, 1, 1);
		else
			S_StartSound (-1, 0, cl_sfx_ric3, pos, 1, 1);
	}
}

// a light at pos for time seconds; color 0 0 0 is white
static void CL_TEntLight (const vec3_t pos, float radius, float time, float decay, float r, float g, float b)
{
	dlight_t	*dl = CL_AllocDlight (0);

	VectorCopy (pos, dl->origin);
	dl->radius = radius;
	dl->die = (float)(cl.time + time);
	dl->decay = decay;
	dl->color[0] = r;
	dl->color[1] = g;
	dl->color[2] = b;
	dl->color[3] = 0.7f;
}

// an explosion's particles, light and sound, and with sprite its sprite
static void CL_Explosion (vec3_t pos, bool sprite, float r, float g, float b)
{
	explosion_t	*ex;

	R_ParticleExplosion (pos);
	CL_TEntLight (pos, 350, 0.5f, 300, r, g, b);
	S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
	if (sprite)
	{
		ex = CL_AllocExplosion ();
		VectorCopy (pos, ex->origin);
		ex->start = (float)cl.time;
		ex->model = Mod_ForName ("progs/s_explod.spr", true);
	}
}

/*
=================
CL_RunTEnt

What a temporary entity shows: id's particles for each, as FTE's classic
particles have them; those without stay unseen
=================
*/
static void CL_RunTEnt (const tent_t *te)
{
	vec3_t		pos, vel, mid;
	model_t		*m;

	VectorCopy (te->pos, pos);
	VectorCopy (te->vel, vel);
	switch (te->type)
	{
	case TE_WIZSPIKE:			// spike hitting wall
		R_RunParticleEffect (pos, vec3_origin, 20, 30);
		S_StartSound (-1, 0, cl_sfx_wizhit, pos, 1, 1);
		break;

	case TE_KNIGHTSPIKE:			// spike hitting wall
		R_RunParticleEffect (pos, vec3_origin, 226, 20);
		S_StartSound (-1, 0, cl_sfx_knighthit, pos, 1, 1);
		break;

	case TE_SPIKE:			// spike hitting wall
	case TE_BULLET:
	case TEDP_SPIKEQUAD:
		R_RunParticleEffect (pos, vec3_origin, 0, 10);
		CL_SpikeSound (pos);
		break;

	case TE_SUPERSPIKE:			// super spike hitting wall
	case TE_SUPERBULLET:
	case TEDP_SUPERSPIKEQUAD:
		R_RunParticleEffect (pos, vec3_origin, 0, 20);
		CL_SpikeSound (pos);
		break;

	case TE_EXPLOSION:			// rocket explosion
		CL_Explosion (pos, true, 0.2f, 0.1f, 0.05f);
		break;
	case TE_NQEXPLOSION:
		CL_Explosion (pos, false, 0.2f, 0.1f, 0.05f);
		break;
	case TEDP_EXPLOSIONQUAD:
		CL_Explosion (pos, true, 0.25f, 0.25f, 1);
		break;
	case TE_EXPLOSION3_NEH:
	case TEDP_EXPLOSIONRGB:
		CL_Explosion (pos, false, te->pos2[0], te->pos2[1], te->pos2[2]);
		break;
	case TEDP_TEI_BIGEXPLOSION:
		CL_Explosion (pos, false, 2, 1.5f, 0.75f);
		break;

	case TE_EXPLOSION2:			// NetQuake's, in a color range
		R_ParticleExplosion2 (pos, te->color, te->colors);
		CL_TEntLight (pos, 350, 0.5f, 300, 0, 0, 0);
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	case TE_TAREXPLOSION:			// tarbaby explosion
		R_BlobExplosion (pos);
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	case TE_LIGHTNING1:				// lightning bolts
		CL_AddBeam (Mod_ForName ("progs/bolt.mdl", true), te->ent, te->pos, te->pos2);
		break;
	case TE_LIGHTNING2:
		CL_AddBeam (Mod_ForName ("progs/bolt2.mdl", true), te->ent, te->pos, te->pos2);
		break;
	case TE_LIGHTNING3:
		CL_AddBeam (Mod_ForName ("progs/bolt3.mdl", true), te->ent, te->pos, te->pos2);
		break;
	case TE_BEAM:					// the mission packs' beam.mdl, where the game has one
		if ((m = Mod_ForName ("progs/beam.mdl", false)))
			CL_AddBeam (m, te->ent, te->pos, te->pos2);
		break;

	case TE_LAVASPLASH:
		R_LavaSplash (pos);
		break;

	case TE_TELEPORT:
		R_TeleportSplash (pos);
		break;

	case TE_GUNSHOT:			// bullet hitting wall
	case TE_NQGUNSHOT:
	case TEDP_GUNSHOTQUAD:
		R_RunParticleEffect (pos, vec3_origin, 0, 20*te->count);
		break;

	case TE_BLOOD:				// bullets hitting body
		R_RunParticleEffect (pos, vec3_origin, 73, 20*te->count);
		break;
	case TEDP_BLOOD:
		R_RunParticleEffect (pos, vel, 73, te->count);
		break;
	case TEDP_BLOODSHOWER:
		VectorAdd (te->pos, te->pos2, mid);
		VectorScale (mid, 0.5f, mid);
		R_RunParticleEffect (mid, vel, 73, te->count);
		break;

	case TE_LIGHTNINGBLOOD:		// lightning hitting body
		R_RunParticleEffect (pos, vec3_origin, 225, 50);
		break;

	case TEDP_SPARK:
		R_RunParticleEffect (pos, vel, 224, te->count);
		break;
	case TEDP_FLAMEJET:
		R_RunParticleEffect (pos, vel, 232, te->count);
		break;
	case TEDP_PLASMABURN:
		R_RunParticleEffect (pos, vec3_origin, 15, 50);
		break;

	case TEDP_SMALLFLASH:
		CL_TEntLight (pos, 200, 0.2f, 1000, 0, 0, 0);
		break;
	case TEDP_CUSTOMFLASH:
		CL_TEntLight (pos, (float)te->count, te->time, te->count / te->time, te->pos2[0], te->pos2[1], te->pos2[2]);
		break;

	default:	// rails, cubes, weather and smoke: none of id's
		break;
	}
}

/*
=================
CL_ParseTEnt
=================
*/
void CL_ParseTEnt (void)
{
	tent_t	te;

	CL_ReadTEnt (&te);
	if (!CL_MVDQuiet ())	// not a scan's or a seek's, long over
		CL_RunTEnt (&te);
}


/*
=================
CL_NewTempEntity
=================
*/
entity_t *CL_NewTempEntity (void)
{
	entity_t	*ent;

	if (cl.numvisedicts == MAX_VISEDICTS)
		return NULL;
	ent = &cl.visedicts[cl.numvisedicts];
	cl.numvisedicts++;
	ent->keynum = 0;
	
	memset (ent, 0, sizeof(*ent));

	ent->translate = NULL;
	ent->palette = NULL;
	return ent;
}


/*
=================
CL_TrueLightningEnd

The end of the viewed player's beam (the server's, in end) turned toward the
view by fraction f, at the length the server gave it; it leaves from 16 above
the origin, as the server's does
=================
*/
static void CL_TrueLightningEnd (const vec3_t start, float f, vec3_t end)
{
	vec3_t	from, dir, ang, right, up;
	float	len, delta, pitch, yaw;

	VectorCopy (start, from);
	from[2] += 16;
	VectorSubtract (end, from, dir);
	len = Length (dir);
	if (len < 1)
		return;

	// the server's direction as view angles: pitch positive down
	yaw = atan2f (dir[1], dir[0]) * 180 / (float)Q_PI;
	pitch = -atan2f (dir[2], sqrtf (dir[0]*dir[0] + dir[1]*dir[1])) * 180 / (float)Q_PI;

	delta = anglemod (cl.simangles[PITCH] - pitch);
	if (delta > 180)
		delta -= 360;
	ang[PITCH] = pitch + delta * f;
	delta = anglemod (cl.simangles[YAW] - yaw);
	if (delta > 180)
		delta -= 360;
	ang[YAW] = yaw + delta * f;
	ang[ROLL] = 0;

	AngleVectors (ang, dir, right, up);		// writes all three
	VectorMA (from, len, dir, end);
}

/*
=================
CL_UpdateBeams
=================
*/
static void CL_UpdateBeams (void)
{
	int			i, j;
	beam_t		*b;
	vec3_t		dist, org, end;
	float		d, f;
	entity_t	*ent;
	float		yaw, pitch;
	float		forward;
	int			viewed = Cam_ViewEntity ();

// update lightning
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
	{
		if (!b->model || b->endtime < cl.time)
			continue;

	// if coming from whose eyes the view is (the player, or the one a demo
	// or spectating follows), update the start position, and maybe the end
		VectorCopy (b->end, end);
		if (viewed && b->entity == viewed)	// entity 0 is the world
		{
			VectorCopy (cl.simorg, b->start);
			f = cl_truelightning.value;
			if (f > 0)
				CL_TrueLightningEnd (b->start, f > 1 ? 1 : f, end);
		}

	// calculate pitch and yaw
		VectorSubtract (end, b->start, dist);

		if (dist[1] == 0 && dist[0] == 0)
		{
			yaw = 0;
			if (dist[2] > 0)
				pitch = 90;
			else
				pitch = 270;
		}
		else
		{
			yaw = (float)((int) (atan2(dist[1], dist[0]) * 180 / Q_PI));
			if (yaw < 0)
				yaw += 360;
	
			forward = sqrtf(dist[0]*dist[0] + dist[1]*dist[1]);
			pitch = (float)((int) (atan2(dist[2], forward) * 180 / Q_PI));
			if (pitch < 0)
				pitch += 360;
		}

	// add new entities for the lightning
		VectorCopy (b->start, org);
		d = VectorNormalize(dist);
		while (d > 0)
		{
			ent = CL_NewTempEntity ();
			if (!ent)
				return;
			VectorCopy (org, ent->origin);
			ent->model = b->model;
			ent->angles[0] = pitch;
			ent->angles[1] = yaw;
			ent->angles[2] = (vec_t)(rand()%360);

			for (j=0 ; j<3 ; j++)		// not i: that walks the beams
				org[j] += dist[j]*30;
			d -= 30;
		}
	}
	
}

/*
=================
CL_UpdateExplosions
=================
*/
static void CL_UpdateExplosions (void)
{
	int			i;
	int			f;
	explosion_t	*ex;
	entity_t	*ent;

	for (i=0, ex=cl_explosions ; i< MAX_EXPLOSIONS ; i++, ex++)
	{
		if (!ex->model)
			continue;
		f = (int)(10*(cl.time - ex->start));
		if (f >= ex->model->numframes)
		{
			ex->model = NULL;
			continue;
		}

		ent = CL_NewTempEntity ();
		if (!ent)
			return;
		VectorCopy (ex->origin, ent->origin);
		ent->model = ex->model;
		ent->frame = f;
	}
}

/*
=================
CL_UpdateTEnts
=================
*/
void CL_UpdateTEnts (void)
{
	CL_UpdateBeams ();
	CL_UpdateExplosions ();
}
