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
// cl_ents.c -- entity parsing and management

#include "cl_local.h"

extern	cvar_t	cl_predict_players;
extern	cvar_t	cl_predict_players2;
extern	cvar_t	r_drawvweps;
extern	cvar_t	cl_solid_players;

static struct predicted_player {
	int flags;
	bool active;
	vec3_t origin;	// predicted origin
} predicted_players[MAX_CLIENTS];

//============================================================

/*
===============
CL_AllocDlight

===============
*/
dlight_t *CL_AllocDlight (int key)
{
	int		i;
	dlight_t	*dl;

// first look for an exact key match
	if (key)
	{
		dl = cl.dlights;
		for (i=0 ; i<MAX_DLIGHTS ; i++, dl++)
		{
			if (dl->key == key)
			{
				memset (dl, 0, sizeof(*dl));
				dl->key = key;
				return dl;
			}
		}
	}

// then look for anything else
	dl = cl.dlights;
	for (i=0 ; i<MAX_DLIGHTS ; i++, dl++)
	{
		if (dl->die < cl.time)
		{
			memset (dl, 0, sizeof(*dl));
			dl->key = key;
			return dl;
		}
	}

	dl = &cl.dlights[0];
	memset (dl, 0, sizeof(*dl));
	dl->key = key;
	return dl;
}

/*
===============
CL_NewDlight
===============
*/
static void CL_NewDlight (int key, float x, float y, float z, float radius, float time,
				   int type)
{
	dlight_t	*dl;

	dl = CL_AllocDlight (key);
	dl->origin[0] = x;
	dl->origin[1] = y;
	dl->origin[2] = z;
	dl->radius = radius;
	dl->die = (float)(cl.time + time);
	if (type == 0) {
		dl->color[0] = 0.2f;
		dl->color[1] = 0.1f;
		dl->color[2] = 0.05f;
		dl->color[3] = 0.7f;
	} else if (type == 1) {
		dl->color[0] = 0.05f;
		dl->color[1] = 0.05f;
		dl->color[2] = 0.3f;
		dl->color[3] = 0.7f;
	} else if (type == 2) {
		dl->color[0] = 0.5;
		dl->color[1] = 0.05f;
		dl->color[2] = 0.05f;
		dl->color[3] = 0.7f;
	} else if (type == 3) {
		dl->color[0]=0.5;
		dl->color[1] = 0.05f;
		dl->color[2] = 0.4f;
		dl->color[3] = 0.7f;
	}
}


/*
===============
CL_DecayLights

===============
*/
void CL_DecayLights (void)
{
	int			i;
	dlight_t	*dl;

	dl = cl.dlights;
	for (i=0 ; i<MAX_DLIGHTS ; i++, dl++)
	{
		if (dl->die < cl.time || !dl->radius)
			continue;
		
		dl->radius = (float)(dl->radius - cls.frametime*dl->decay);
		if (dl->radius < 0)
			dl->radius = 0;
	}
}


/*
=========================================================================

PACKET ENTITY PARSING / LINKING

=========================================================================
*/

/*
=================
FlushEntityPacket
=================
*/
static void FlushEntityPacket (void)
{
	int			word, num, bits, ext;
	entity_state_t	olde, newe;

	Con_DPrintf ("FlushEntityPacket\n");

	memset (&olde, 0, sizeof(olde));

	cl.validsequence = 0;		// can't render a frame
	cl.frames[cls.netchan.incoming_sequence&UPDATE_MASK].invalid = true;

	// read it all, but ignore it
	while (1)
	{
		word = (unsigned short)MSG_ReadShort ();
		if (msg_badread)
		{	// something didn't parse right...
			Host_EndGame ("msg_badread in packetentities");
			return;
		}

		if (!word)
			break;	// done

		num = MSG_ReadEntityHeader (word, &bits, &ext, cls.fteext);
		if (!(word & U_REMOVE))
			MSG_ReadDeltaEntity (&olde, &newe, num, bits, ext, cls.mvdext1);
	}
}

/*
===============================================================================

SMOOTHING

Everything the server places but the players is drawn a little in the past,
between two positions the server sent, so that it moves smoothly however the
updates arrive (qualia): the drawn moment trails the newest update by one and
a half times the mean gap between updates, 10 to 120 msec. A new entity,
another model or a move of over 200 units is a snap. The players are run
forward to the present through the movement code instead (CL_LinkPlayers).

===============================================================================
*/

#define	LERP_SNAP		200			// units: farther is a teleport
#define	LERP_MINTRAIL	0.010
#define	LERP_MAXTRAIL	0.120
#define	LERP_MINSPAN	0.0005		// a window shorter than this is a snap

typedef struct
{
	vec3_t	val[3];		// newest first
	double	at[3];		// when each arrived
} lerptrail_t;

typedef struct
{
	lerptrail_t	origin, angles;
	int			modelindex;
	int			stamp;		// the last update it was in
} entlerp_t;

static entlerp_t	cl_entlerp[MAX_EDICTS];
static int			lerp_update = 1;	// counts updates; 0 is never
static double		lerp_lastat;		// when the last update arrived
static double		lerp_interval;		// the mean gap between updates

static entlerp_t	cl_playerlerp[MAX_CLIENTS];	// an MVD's players
static int			playerlerp_frame = -1;		// the parse count they were aimed at
static unsigned		playerlerp_fixangle;		// players whose view the server set

void CL_ResetSmoothing (void)
{
	lerp_update += 2;	// nothing tracked carries over
	lerp_lastat = 0;
	lerp_interval = 0;
	playerlerp_frame = -1;
	playerlerp_fixangle = 0;
	memset (cl_playerlerp, 0, sizeof(cl_playerlerp));
}

static void Trail_Fix (lerptrail_t *t, const vec3_t v, double now)
{
	int		i;

	for (i=0 ; i<3 ; i++)
	{
		VectorCopy (v, t->val[i]);
		t->at[i] = now;
	}
}

static void Trail_Push (lerptrail_t *t, const vec3_t v, double now)
{
	VectorCopy (t->val[1], t->val[2]);
	t->at[2] = t->at[1];
	VectorCopy (t->val[0], t->val[1]);
	t->at[1] = t->at[0];
	VectorCopy (v, t->val[0]);
	t->at[0] = now;
}

/*
===============
Trail_Sample

Where the trail is at time: between the two values that bracket it
===============
*/
static void Trail_Sample (const lerptrail_t *t, double time, vec3_t out)
{
	int		from, to, i;
	double	span;
	float	f;

	if (time >= t->at[1])
	{
		from = 1;
		to = 0;
	}
	else
	{
		from = 2;
		to = 1;
	}
	span = t->at[to] - t->at[from];
	if (span <= LERP_MINSPAN)
	{
		VectorCopy (t->val[to], out);
		return;
	}
	f = (float)((time - t->at[from]) / span);
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	for (i=0 ; i<3 ; i++)
		out[i] = t->val[from][i] + f * (t->val[to][i] - t->val[from][i]);
}

/*
===============
CL_LerpSnapshot

An update of the entities arrived: aim each one's trails at where it now is
===============
*/
static void CL_LerpSnapshot (const packet_entities_t *pack)
{
	const entity_state_t	*s;
	entlerp_t	*l;
	vec3_t		angles, d;
	double		now, gap;
	int			i, j, previous;

	// the mean gap, gently: one late update must not drag everything into
	// the past and let it snap forward again; a stall is not a gap. An MVD
	// stamps with its own clock.
	now = cls.mvdplayback ? CL_MVDFrameTime () : host.realtime;
	gap = now - lerp_lastat;
	if (lerp_lastat && gap >= 0 && gap < 0.5)
		lerp_interval = lerp_interval ? lerp_interval * 0.95 + gap * 0.05 : gap;
	lerp_lastat = now;

	previous = lerp_update++;
	for (i=0 ; i<pack->num_entities ; i++)
	{
		s = &pack->entities[i];
		l = &cl_entlerp[s->number];

		// the angles the short way round from the newest
		for (j=0 ; j<3 ; j++)
		{
			angles[j] = s->angles[j];
			while (angles[j] - l->angles.val[0][j] > 180)
				angles[j] -= 360;
			while (angles[j] - l->angles.val[0][j] < -180)
				angles[j] += 360;
		}

		VectorSubtract (s->origin, l->origin.val[0], d);
		if (l->stamp == previous && l->modelindex == s->modelindex && DotProduct (d, d) <= LERP_SNAP*LERP_SNAP)
		{
			if (d[0] || d[1] || d[2])
				Trail_Push (&l->origin, s->origin, now);
			if (angles[0] != l->angles.val[0][0] || angles[1] != l->angles.val[0][1] || angles[2] != l->angles.val[0][2])
				Trail_Push (&l->angles, angles, now);
		}
		else
		{	// new, teleported, or something else now: just be there
			Trail_Fix (&l->origin, s->origin, now);
			Trail_Fix (&l->angles, s->angles, now);
			l->modelindex = s->modelindex;
		}
		l->stamp = lerp_update;
	}
}

/*
===============
CL_MVDFixAngle
===============
*/
void CL_MVDFixAngle (int slot)
{
	if (slot >= 0 && slot < MAX_CLIENTS)
		playerlerp_fixangle |= 1u << slot;
}

/*
===============
CL_LerpMVDPlayers

A new frame of an MVD: aim each player's trails at where it has them, once,
after the whole frame is read. A player not in the frame before, with another
model, moved over 200 units, or a respawn snaps; a view the server set snaps
the angles.
===============
*/
void CL_LerpMVDPlayers (void)
{
	const player_state_t	*state;
	entlerp_t	*l;
	vec3_t		angles, d;
	double		now;
	int			j, k, previous;
	bool		respawn;

	if (cl.parsecount == playerlerp_frame)
		return;
	previous = playerlerp_frame;
	playerlerp_frame = cl.parsecount;
	now = CL_MVDFrameTime ();

	for (j=0 ; j<MAX_CLIENTS ; j++)
	{
		state = &cl.frames[cl.parsecountmod].playerstate[j];
		if (state->messagenum != cl.parsecount || !state->modelindex)
			continue;
		l = &cl_playerlerp[j];

		for (k=0 ; k<3 ; k++)
		{
			angles[k] = state->viewangles[k];
			while (angles[k] - l->angles.val[0][k] > 180)
				angles[k] -= 360;
			while (angles[k] - l->angles.val[0][k] < -180)
				angles[k] += 360;
		}

		VectorSubtract (state->origin, l->origin.val[0], d);
		respawn = (l->stamp & 1) && !(state->flags & PF_DEAD);	// the low bit: dead last frame
		if ((l->stamp >> 1) == previous && l->modelindex == state->modelindex && !respawn
		 && DotProduct (d, d) <= LERP_SNAP*LERP_SNAP)
		{
			if (d[0] || d[1] || d[2])
				Trail_Push (&l->origin, state->origin, now);
			if (playerlerp_fixangle & (1u << j))
				Trail_Fix (&l->angles, angles, now);
			else if (angles[0] != l->angles.val[0][0] || angles[1] != l->angles.val[0][1]
			 || angles[2] != l->angles.val[0][2])
				Trail_Push (&l->angles, angles, now);
		}
		else
		{
			Trail_Fix (&l->origin, state->origin, now);
			Trail_Fix (&l->angles, state->viewangles, now);
			l->modelindex = state->modelindex;
		}
		l->stamp = (cl.parsecount << 1) | ((state->flags & PF_DEAD) ? 1 : 0);
	}
	playerlerp_fixangle = 0;
}

/*
===============
CL_PlayerPlace

Where an MVD's player is at the moment played; false for one not in the
latest frame
===============
*/
bool CL_PlayerPlace (int slot, vec3_t origin, vec3_t angles)
{
	const entlerp_t	*l = &cl_playerlerp[slot];

	if (slot < 0 || slot >= MAX_CLIENTS || (l->stamp >> 1) != playerlerp_frame)
		return false;
	Trail_Sample (&l->origin, CL_MVDTime (), origin);
	Trail_Sample (&l->angles, CL_MVDTime (), angles);
	return true;
}

/*
===============
CL_EntityPlace

Where to draw an entity of the newest update
===============
*/
static void CL_EntityPlace (const entity_state_t *s, vec3_t origin, vec3_t angles)
{
	const entlerp_t	*l = &cl_entlerp[s->number];
	double			trail;

	if ((cl_nolerp.value && !cls.mvdplayback) || l->stamp != lerp_update)
	{
		VectorCopy (s->origin, origin);
		VectorCopy (s->angles, angles);
		return;
	}

	// an MVD reads a frame ahead: the moment played is between two it has
	if (cls.mvdplayback)
	{
		Trail_Sample (&l->origin, CL_MVDTime (), origin);
		Trail_Sample (&l->angles, CL_MVDTime (), angles);
		return;
	}

	trail = lerp_interval * 1.5;
	trail = trail < LERP_MINTRAIL ? LERP_MINTRAIL : trail > LERP_MAXTRAIL ? LERP_MAXTRAIL : trail;
	Trail_Sample (&l->origin, host.realtime - trail, origin);
	Trail_Sample (&l->angles, host.realtime - trail, angles);
}

/*
==================
CL_ParsePacketEntities

An svc_packetentities has just been parsed, deal with the
rest of the data stream.
==================
*/
void CL_ParsePacketEntities (bool delta)
{
	int			oldpacket, newpacket;
	packet_entities_t	*oldp, *newp, dummy;
	int			oldindex, newindex;
	int			word, newnum, oldnum, bits, ext;
	bool	full;
	byte		from;

	newpacket = cls.netchan.incoming_sequence&UPDATE_MASK;
	newp = &cl.frames[newpacket].packet_entities;
	cl.frames[newpacket].invalid = false;

	if (delta)
	{
		from = (byte)MSG_ReadByte ();

		// an MVD deltas from its frame before; the last one with entities is
		// what the client has (qualia)
		if (cls.mvdplayback)
			oldpacket = cl.validsequence ? cl.validsequence : cls.netchan.incoming_sequence - 1;
		else
			oldpacket = cl.frames[newpacket].delta_sequence;

		if (!cls.mvdplayback && (from&UPDATE_MASK) != (oldpacket&UPDATE_MASK))
			Con_DPrintf ("WARNING: from mismatch\n");
	}
	else
		oldpacket = -1;

	full = false;
	if (oldpacket != -1)
	{
		if (!cls.mvdplayback && cls.netchan.outgoing_sequence - oldpacket >= UPDATE_BACKUP-1)
		{	// we can't use this, it is too old
			FlushEntityPacket ();
			return;
		}
		cl.validsequence = cls.netchan.incoming_sequence;
		oldp = &cl.frames[oldpacket&UPDATE_MASK].packet_entities;
	}
	else
	{	// this is a full update that we can start delta compressing from now
		oldp = &dummy;
		dummy.num_entities = 0;
		cl.validsequence = cls.netchan.incoming_sequence;
		full = true;
	}

	oldindex = 0;
	newindex = 0;
	newp->num_entities = 0;

	while (1)
	{
		word = (unsigned short)MSG_ReadShort ();
		if (msg_badread)
		{	// something didn't parse right...
			Host_EndGame ("msg_badread in packetentities");
			return;
		}

		if (!word)
		{
			while (oldindex < oldp->num_entities)
			{	// copy all the rest of the entities from the old packet
//Con_Printf ("copy %i\n", oldp->entities[oldindex].number);
				if (newindex >= MAX_MVD_PACKET_ENTITIES)
					Host_EndGame ("CL_ParsePacketEntities: too many entities");
				newp->entities[newindex] = oldp->entities[oldindex];
				newindex++;
				oldindex++;
			}
			break;
		}
		newnum = MSG_ReadEntityHeader (word, &bits, &ext, cls.fteext);
		oldnum = oldindex >= oldp->num_entities ? 9999 : oldp->entities[oldindex].number;

		while (newnum > oldnum)
		{
			if (full)
			{
				Con_Printf ("WARNING: oldcopy on full update");
				FlushEntityPacket ();
				return;
			}

//Con_Printf ("copy %i\n", oldnum);
			// copy one of the old entities over to the new packet unchanged
			if (newindex >= MAX_MVD_PACKET_ENTITIES)
				Host_EndGame ("CL_ParsePacketEntities: too many entities");
			newp->entities[newindex] = oldp->entities[oldindex];
			newindex++;
			oldindex++;
			oldnum = oldindex >= oldp->num_entities ? 9999 : oldp->entities[oldindex].number;
		}

		if (newnum < oldnum)
		{	// new from baseline
//Con_Printf ("baseline %i\n", newnum);
			if (word & U_REMOVE)
			{
				if (full)
				{
					cl.validsequence = 0;
					Con_Printf ("WARNING: U_REMOVE on full update\n");
					FlushEntityPacket ();
					return;
				}
				continue;
			}
			if (newindex >= MAX_MVD_PACKET_ENTITIES)
				Host_EndGame ("CL_ParsePacketEntities: too many entities");
			MSG_ReadDeltaEntity (&cl.baselines[newnum], &newp->entities[newindex], newnum, bits, ext, cls.mvdext1);
			newindex++;
			continue;
		}

		if (newnum == oldnum)
		{	// delta from previous
			if (full)
			{
				cl.validsequence = 0;
				Con_Printf ("WARNING: delta on full update");
			}
			if (word & U_REMOVE)
			{
				oldindex++;
				continue;
			}
//Con_Printf ("delta %i\n",newnum);
			MSG_ReadDeltaEntity (&oldp->entities[oldindex], &newp->entities[newindex], newnum, bits, ext,
				cls.mvdext1);
			newindex++;
			oldindex++;
		}

	}

	newp->num_entities = newindex;
	CL_LerpSnapshot (newp);
}


/*
===============
CL_LinkPacketEntities

===============
*/
static void CL_LinkPacketEntities (void)
{
	entity_t			*ent;
	packet_entities_t	*pack;
	entity_state_t		*s1;
	model_t				*model;
	vec3_t				old_origin, origin, angles;
	float				autorotate;
	int					i;
	int					pnum;
	dlight_t			*dl;

	// an MVD's frame may bring no entities: the last that did has them (qualia)
	pack = &cl.frames[(cls.mvdplayback ? cl.validsequence : cls.netchan.incoming_sequence) & UPDATE_MASK].packet_entities;

	autorotate = anglemod((float)(100*cl.time));

	for (pnum=0 ; pnum<pack->num_entities ; pnum++)
	{
		s1 = &pack->entities[pnum];
		CL_EntityPlace (s1, origin, angles);

		// spawn light flashes, even ones coming from invisible objects
		if ((s1->effects & (EF_BLUE | EF_RED)) == (EF_BLUE | EF_RED))
			CL_NewDlight (s1->number, origin[0], origin[1], origin[2], (float)(200 + (rand()&31)), 0.1f, 3);
		else if (s1->effects & EF_BLUE)
			CL_NewDlight (s1->number, origin[0], origin[1], origin[2], (float)(200 + (rand()&31)), 0.1f, 1);
		else if (s1->effects & EF_RED)
			CL_NewDlight (s1->number, origin[0], origin[1], origin[2], (float)(200 + (rand()&31)), 0.1f, 2);
		else if (s1->effects & EF_BRIGHTLIGHT)
			CL_NewDlight (s1->number, origin[0], origin[1], origin[2] + 16, (float)(400 + (rand()&31)), 0.1f, 0);
		else if (s1->effects & EF_DIMLIGHT)
			CL_NewDlight (s1->number, origin[0], origin[1], origin[2], (float)(200 + (rand()&31)), 0.1f, 0);

		// if set to invisible, skip
		if (!s1->modelindex)
			continue;
		model = CL_Model (s1->modelindex);
		if (!model)
			continue;

		// create a new entity
		if (cl.numvisedicts == MAX_VISEDICTS)
			break;		// object list is full

		ent = &cl.visedicts[cl.numvisedicts];
		cl.numvisedicts++;

		ent->keynum = s1->number;
		ent->model = model;
		ent->alpha = s1->alpha;
	
		// set colormap
		if (s1->colormap && (s1->colormap < MAX_CLIENTS) 
			&& !strcmp(ent->model->name,"progs/player.mdl") )
		{
			ent->translate = cl.players[s1->colormap-1].translate;
			ent->palette = cl.players[s1->colormap-1].palette;
			ent->skin = Skin_ForPlayer (&cl.players[s1->colormap-1]);
		}
		else
		{
			ent->translate = NULL;
			ent->palette = NULL;
			ent->skin = NULL;
		}

		// set skin
		ent->skinnum = s1->skinnum;
		
		// set frame
		ent->frame = s1->frame;

		// rotate binary objects locally
		if (model->flags & EF_ROTATE)
		{
			ent->angles[0] = 0;
			ent->angles[1] = autorotate;
			ent->angles[2] = 0;
		}
		else
			VectorCopy (angles, ent->angles);

		VectorCopy (origin, ent->origin);

		// add automatic particle trails
		if (!model->flags)
			continue;

		// scan the old entity display list for a matching
		for (i=0 ; i<cl.oldnumvisedicts ; i++)
		{
			if (cl.oldvisedicts[i].keynum == ent->keynum)
			{
				VectorCopy (cl.oldvisedicts[i].origin, old_origin);
				break;
			}
		}
		if (i == cl.oldnumvisedicts)
			continue;		// not in last message

		for (i=0 ; i<3 ; i++)
			if ( abs((int)(old_origin[i] - ent->origin[i])) > 128)
			{	// no trail if too far
				VectorCopy (ent->origin, old_origin);
				break;
			}
		if (model->flags & EF_ROCKET)
		{
			R_RocketTrail (old_origin, ent->origin, 0);
			dl = CL_AllocDlight (s1->number);
			VectorCopy (ent->origin, dl->origin);
			dl->radius = 200;
			dl->die = (float)(cl.time + 0.1f);
		}
		else if (model->flags & EF_GRENADE)
			R_RocketTrail (old_origin, ent->origin, 1);
		else if (model->flags & EF_GIB)
			R_RocketTrail (old_origin, ent->origin, 2);
		else if (model->flags & EF_ZOMGIB)
			R_RocketTrail (old_origin, ent->origin, 4);
		else if (model->flags & EF_TRACER)
			R_RocketTrail (old_origin, ent->origin, 3);
		else if (model->flags & EF_TRACER2)
			R_RocketTrail (old_origin, ent->origin, 5);
		else if (model->flags & EF_TRACER3)
			R_RocketTrail (old_origin, ent->origin, 6);
	}
}


/*
=========================================================================

PROJECTILE PARSING / LINKING

=========================================================================
*/

typedef struct
{
	int		modelindex;
	vec3_t	origin;
	vec3_t	angles;
} projectile_t;

#define	MAX_PROJECTILES	32
static projectile_t	cl_projectiles[MAX_PROJECTILES];
static int				cl_num_projectiles;


void CL_ClearProjectiles (void)
{
	cl_num_projectiles = 0;
}

/*
=====================
CL_ParseProjectiles

Nails are passed as efficient temporary entities; svc_nails2 gives each its
entity number too
=====================
*/
void CL_ParseProjectiles (bool numbered)
{
	int		i, c, j;
	byte	bits[6];
	projectile_t	*pr;

	c = MSG_ReadByte ();
	for (i=0 ; i<c ; i++)
	{
		if (numbered)
			MSG_ReadByte ();
		for (j=0 ; j<6 ; j++)
			bits[j] = (byte)MSG_ReadByte ();

		if (cl_num_projectiles == MAX_PROJECTILES)
			continue;

		pr = &cl_projectiles[cl_num_projectiles];
		cl_num_projectiles++;

		pr->modelindex = cl.spikeindex;
		pr->origin[0] = (vec_t)(( ( bits[0] + ((bits[1]&15)<<8) ) <<1) - 4096);
		pr->origin[1] = (vec_t)(( ( (bits[1]>>4) + (bits[2]<<4) ) <<1) - 4096);
		pr->origin[2] = (vec_t)(( ( bits[3] + ((bits[4]&15)<<8) ) <<1) - 4096);
		pr->angles[0] = (vec_t)(360*(bits[4]>>4)/16);
		pr->angles[1] = (vec_t)(360*bits[5]/256);
	}
}

/*
=============
CL_LinkProjectiles

=============
*/
static void CL_LinkProjectiles (void)
{
	int		i;
	projectile_t	*pr;
	entity_t		*ent;

	for (i=0, pr=cl_projectiles ; i<cl_num_projectiles ; i++, pr++)
	{
		// grab an entity to fill in
		if (cl.numvisedicts == MAX_VISEDICTS)
			break;		// object list is full
		ent = &cl.visedicts[cl.numvisedicts];
		cl.numvisedicts++;
		ent->keynum = 0;

		if (pr->modelindex < 1)
			continue;
		ent->model = cl.model_precache[pr->modelindex];
		ent->alpha = 0;
		ent->skinnum = 0;
		ent->frame = 0;
		ent->translate = NULL;
		ent->palette = NULL;
		ent->skin = NULL;
		VectorCopy (pr->origin, ent->origin);
		VectorCopy (pr->angles, ent->angles);
	}
}

//========================================


entity_t *CL_NewTempEntity (void);

/*
===================
CL_MVDWeaponModel

An MVD's visible weapon, from the view model the player's weapon stat names:
v_axe 1 to v_light 8, as the default list has them (ezQuake)
===================
*/
static int CL_MVDWeaponModel (int modelindex)
{
	static const char *const	views[] = {
		"progs/v_axe.mdl", "progs/v_shot.mdl", "progs/v_shot2.mdl", "progs/v_nail.mdl",
		"progs/v_nail2.mdl", "progs/v_rock.mdl", "progs/v_rock2.mdl", "progs/v_light.mdl"
	};
	int		i;

	if (modelindex <= 0 || modelindex >= MAX_MODELS)
		return 0;
	for (i=0 ; i<8 ; i++)
		if (!strcmp (cl.model_name[modelindex], views[i]))
			return i + 1;
	return 0;
}

/*
===================
CL_ParseMVDPlayerinfo

An MVD's player: what the message leaves out is as the last message about the
player said, however long ago (qualia's per-player bases)
===================
*/
static void CL_ParseMVDPlayerinfo (int num)
{
	static const struct { int df, pf; }	flagmap[] = {
		{DF_EFFECTS, PF_EFFECTS}, {DF_SKINNUM, PF_SKINNUM}, {DF_DEAD, PF_DEAD}, {DF_GIB, PF_GIB},
		{DF_WEAPONFRAME, PF_WEAPONFRAME}, {DF_MODEL, PF_MODEL}
	};
	player_state_t	*state, *prev;
	int		flags, i;

	state = &cl.frames[cl.parsecountmod].playerstate[num];
	prev = &cl.mvd_prev[num];
	*state = *prev;

	flags = MSG_ReadShort () & 0xffff;
	state->frame = MSG_ReadByte ();
	for (i=0 ; i<3 ; i++)
		if (flags & (DF_ORIGIN << i))
			state->origin[i] = MSG_ReadCoord ();
	for (i=0 ; i<3 ; i++)
		if (flags & (DF_ANGLES << i))
			state->command.angles[i] = MSG_ReadAngle16 ();
	if (flags & DF_MODEL)
		state->modelindex = MSG_ReadByte ();
	if (flags & DF_SKINNUM)
		state->skinnum = MSG_ReadByte ();
	if (flags & DF_EFFECTS)
		state->effects = MSG_ReadByte ();
	if (flags & DF_WEAPONFRAME)
		state->weaponframe = MSG_ReadByte ();

	// a model past 255 borrows the skin's top bit
	if ((flags & DF_MODEL) && (flags & DF_SKINNUM) && (state->skinnum & 128) && (cls.fteext & FTE_PEXT_MODELDBL))
	{
		state->modelindex += 256;
		state->skinnum &= 127;
	}
	// an old recording has none for players who joined while it recorded
	if (!state->modelindex && !cl.players[num].spectator)
		state->modelindex = cl.playerindex;

	state->flags = 0;
	for (i=0 ; i<(int)(sizeof(flagmap)/sizeof(flagmap[0])) ; i++)
		if (flags & flagmap[i].df)
			state->flags |= flagmap[i].pf;
	state->messagenum = cl.parsecount;
	state->state_time = cl.parsecounttime;
	state->command.msec = 0;
	VectorCopy (state->command.angles, state->viewangles);
	VectorCopy (vec3_origin, state->velocity);
	state->pm_type = cl.players[num].spectator ? PM_OLD_SPECTATOR : (state->flags & PF_DEAD) ? PM_DEAD : PM_NORMAL;
	state->alpha = 0;
	memset (state->colormod, 0, sizeof(state->colormod));
	state->vw_index = !(state->flags & PF_GIB) && state->modelindex == cl.playerindex
		? CL_MVDWeaponModel (cl.players[num].stats[STAT_WEAPON]) : 0;

	*prev = *state;
}

/*
===================
CL_PlayerMoveType

How the player moves, for prediction: the server says with Z_EXT_PM_TYPE, and
whether the player is on the ground with Z_EXT_PF_ONGROUND; else a guess
(ezQuake)
===================
*/
static void CL_PlayerMoveType (player_state_t *state, int num, int flags)
{
	static const int	types[] = {
		[PMC_OLD_SPECTATOR] = PM_OLD_SPECTATOR, [PMC_SPECTATOR] = PM_SPECTATOR,
		[PMC_FLY] = PM_FLY, [PMC_NONE] = PM_NONE, [PMC_LOCK] = PM_LOCK
	};
	int		code = (flags >> PF_PMC_SHIFT) & PF_PMC_MASK;
	bool	spectator = num == cl.playernum ? cl.spectator : cl.players[num].spectator != 0;

	if (cl.z_ext & Z_EXT_PF_ONGROUND)
		state->onground = (flags & PF_ONGROUND) != 0;

	// another player's water jump timer would be whatever an earlier
	// prediction left in the frame: a stale one throws the player (qualia)
	if (num != cl.playernum)
		state->waterjumptime = 0;

	if (!(cl.z_ext & Z_EXT_PM_TYPE) || code > PMC_LOCK
	 || (code > PMC_OLD_SPECTATOR && !(cl.z_ext & Z_EXT_PM_TYPE_NEW)))
		state->pm_type = spectator ? PM_OLD_SPECTATOR : (flags & PF_DEAD) ? PM_DEAD : PM_NORMAL;
	else if (code == PMC_NORMAL || code == PMC_NORMAL_JUMP_HELD)
	{
		state->pm_type = (flags & PF_DEAD) ? PM_DEAD : PM_NORMAL;
		if (!(flags & PF_DEAD))
			state->jump_held = code == PMC_NORMAL_JUMP_HELD;
	}
	else
		state->pm_type = types[code];
}

/*
===================
CL_ParsePlayerinfo
===================
*/
void CL_ParsePlayerinfo (void)
{
	int			msec;
	int			flags;
	player_state_t	*state;
	int			num;
	int			i;

	num = MSG_ReadByte ();
	if (num >= MAX_CLIENTS)
		Host_EndGame ("CL_ParsePlayerinfo: bad num %i", num);
	if (cls.mvdplayback)
	{
		CL_ParseMVDPlayerinfo (num);
		return;
	}

	state = &cl.frames[cl.parsecountmod].playerstate[num];

	// with FTE_PEXT_TRANS a third byte of flags can follow; without it
	// ZQuake's two flags of that byte sit at the top of the word
	flags = MSG_ReadShort () & 0xffff;
	if (cls.fteext & FTE_PEXT_TRANS)
	{
		if (flags & PF_EXTRA_PFS)
			flags |= (MSG_ReadByte () & 255) << 16;
	}
	else
		flags = (flags & 0x3fff) | ((flags & 0xc000) << 8);
	state->flags = flags;
	CL_PlayerMoveType (state, num, flags);

	state->messagenum = cl.parsecount;
	state->origin[0] = MSG_ReadOrigin (cls.mvdext1);
	state->origin[1] = MSG_ReadOrigin (cls.mvdext1);
	state->origin[2] = MSG_ReadOrigin (cls.mvdext1);

	state->frame = MSG_ReadByte ();

	// the other player's last move was likely some time
	// before the packet was sent out, so accurately track
	// the exact time it was valid at
	if (flags & PF_MSEC)
	{
		msec = MSG_ReadByte ();
		state->state_time = cl.parsecounttime - msec*0.001;
	}
	else
		state->state_time = cl.parsecounttime;

	if (flags & PF_COMMAND)
		MSG_ReadDeltaUsercmd (&nullcmd, &state->command);

	// Z_EXT_VWEP: the command's impulse is the weapon's model
	state->vw_index = (cl.z_ext & Z_EXT_VWEP) && !(flags & PF_GIB) ? state->command.impulse : 0;

	for (i=0 ; i<3 ; i++)
	{
		if (flags & (PF_VELOCITY1<<i) )
			state->velocity[i] = (vec_t)MSG_ReadShort();
		else
			state->velocity[i] = 0;
	}
	if (flags & PF_MODEL)
		state->modelindex = MSG_ReadByte ();
	else
		state->modelindex = cl.playerindex;

	if (flags & PF_SKINNUM)
	{
		state->skinnum = MSG_ReadByte ();
		// with a model, the skin's top bit is the model number's ninth
		if ((state->skinnum & 128) && (flags & PF_MODEL))
		{
			state->modelindex += 256;
			state->skinnum &= 127;
		}
	}
	else
		state->skinnum = 0;

	if (flags & PF_EFFECTS)
		state->effects = MSG_ReadByte ();
	else
		state->effects = 0;

	if (flags & PF_WEAPONFRAME)
		state->weaponframe = MSG_ReadByte ();
	else
		state->weaponframe = 0;

	if ((flags & PF_TRANS) && (cls.fteext & FTE_PEXT_TRANS))
		state->alpha = (byte)MSG_ReadByte ();
	else
		state->alpha = 0;

	memset (state->colormod, 0, sizeof(state->colormod));
	if ((flags & PF_COLOURMOD) && (cls.fteext & FTE_PEXT_COLOURMOD))
	{
		for (i=0 ; i<3 ; i++)
			state->colormod[i] = (byte)MSG_ReadByte ();
	}

	VectorCopy (state->command.angles, state->viewangles);
}


/*
================
CL_AddFlagModels

Called when the CTF flags are set
================
*/
static void CL_AddFlagModels (entity_t *ent, int team)
{
	int		i;
	float	f;
	vec3_t	v_forward, v_right, v_up;
	entity_t	*newent;

	if (cl.flagindex == -1)
		return;

	f = 14;
	if (ent->frame >= 29 && ent->frame <= 40) {
		if (ent->frame >= 29 && ent->frame <= 34) { //axpain
			if      (ent->frame == 29) f = f + 2; 
			else if (ent->frame == 30) f = f + 8;
			else if (ent->frame == 31) f = f + 12;
			else if (ent->frame == 32) f = f + 11;
			else if (ent->frame == 33) f = f + 10;
			else if (ent->frame == 34) f = f + 4;
		} else if (ent->frame >= 35 && ent->frame <= 40) { // pain
			if      (ent->frame == 35) f = f + 2; 
			else if (ent->frame == 36) f = f + 10;
			else if (ent->frame == 37) f = f + 10;
			else if (ent->frame == 38) f = f + 8;
			else if (ent->frame == 39) f = f + 4;
			else if (ent->frame == 40) f = f + 2;
		}
	} else if (ent->frame >= 103 && ent->frame <= 118) {
		if      (ent->frame >= 103 && ent->frame <= 104) f = f + 6;  //nailattack
		else if (ent->frame >= 105 && ent->frame <= 106) f = f + 6;  //light 
		else if (ent->frame >= 107 && ent->frame <= 112) f = f + 7;  //rocketattack
		else if (ent->frame >= 112 && ent->frame <= 118) f = f + 7;  //shotattack
	}

	newent = CL_NewTempEntity ();
	newent->model = cl.model_precache[cl.flagindex];
	newent->skinnum = team;

	AngleVectors (ent->angles, v_forward, v_right, v_up);
	v_forward[2] = -v_forward[2]; // reverse z component
	for (i=0 ; i<3 ; i++)
		newent->origin[i] = ent->origin[i] - f*v_forward[i] + 22*v_right[i];
	newent->origin[2] -= 16;

	VectorCopy (ent->angles, newent->angles)
	newent->angles[2] -= 45;
}

/*
=============
CL_AddVWep

A player holding a visible weapon: the player model without one, and the
weapon's model beside it, in the same frame. A weapon whose model is missing
leaves the plain player; a "-" player model is the weapon alone (ezQuake).
=============
*/
static void CL_AddVWep (entity_t *ent, int vw_index)
{
	struct model_s	*weapon;
	entity_t		*e;

	if (vw_index >= MAX_VWEP_MODELS)
		return;
	weapon = NULL;
	if (strcmp (cl.vw_model_name[vw_index], "-"))
	{
		weapon = cl.vw_model_precache[vw_index];
		if (!weapon)
			return;
	}

	if (!strcmp (cl.vw_model_name[0], "-"))
	{	// no body
		if (weapon)
		{
			ent->model = weapon;
			ent->skinnum = 0;
			ent->translate = NULL;
			ent->palette = NULL;
			ent->skin = NULL;
		}
		return;
	}

	ent->model = cl.vw_model_precache[0];
	if (!weapon || cl.numvisedicts == MAX_VISEDICTS)
		return;
	e = &cl.visedicts[cl.numvisedicts++];
	*e = *ent;
	e->model = weapon;
	e->skinnum = 0;
	e->translate = NULL;
	e->palette = NULL;
	e->skin = NULL;
}

/*
=============
CL_LinkPlayers

Create visible entities in the correct position
for all current players
=============
*/
static void CL_LinkPlayers (void)
{
	int				j;
	player_info_t	*info;
	player_state_t	*state;
	player_state_t	exact;
	double			playertime;
	entity_t		*ent;
	int				msec;
	frame_t			*frame;
	int				oldphysent;
	vec3_t			origin, angles;

	playertime = host.realtime - cls.latency + 0.02;
	if (playertime > host.realtime)
		playertime = host.realtime;

	frame = &cl.frames[cl.parsecount&UPDATE_MASK];

	for (j=0, info=cl.players, state=frame->playerstate ; j < MAX_CLIENTS 
		; j++, info++, state++)
	{
		if (state->messagenum != cl.parsecount)
			continue;	// not present this frame

		// spawn light flashes, even ones coming from invisible objects
		if ((state->effects & (EF_BLUE | EF_RED)) == (EF_BLUE | EF_RED))
			CL_NewDlight (j, state->origin[0], state->origin[1], state->origin[2], (float)(200 + (rand()&31)), 0.1f, 3);
		else if (state->effects & EF_BLUE)
			CL_NewDlight (j, state->origin[0], state->origin[1], state->origin[2], (float)(200 + (rand()&31)), 0.1f, 1);
		else if (state->effects & EF_RED)
			CL_NewDlight (j, state->origin[0], state->origin[1], state->origin[2], (float)(200 + (rand()&31)), 0.1f, 2);
		else if (state->effects & EF_BRIGHTLIGHT)
			CL_NewDlight (j, state->origin[0], state->origin[1], state->origin[2] + 16, (float)(400 + (rand()&31)), 0.1f, 0);
		else if (state->effects & EF_DIMLIGHT)
			CL_NewDlight (j, state->origin[0], state->origin[1], state->origin[2], (float)(200 + (rand()&31)), 0.1f, 0);

		// the player object never gets added; in an MVD the one followed
		if (j == cl.viewplayer)
			continue;

		if (!state->modelindex || !CL_Model (state->modelindex))
			continue;

		if (!Cam_DrawPlayer(j))
			continue;

		// grab an entity to fill in
		if (cl.numvisedicts == MAX_VISEDICTS)
			break;		// object list is full
		ent = &cl.visedicts[cl.numvisedicts];
		cl.numvisedicts++;
		ent->keynum = 0;

		ent->model = CL_Model (state->modelindex);
		ent->alpha = state->alpha;
		ent->skinnum = state->skinnum;
		ent->frame = state->frame;
		ent->translate = info->translate;
		ent->palette = info->palette;
		if (state->modelindex == cl.playerindex)
			ent->skin = Skin_ForPlayer (info);		// use custom skin
		else
			ent->skin = NULL;

		// an MVD's players where the recording has them at the moment played
		VectorCopy (state->origin, origin);
		VectorCopy (state->viewangles, angles);
		if (cls.mvdplayback)
			CL_PlayerPlace (j, origin, angles);

		//
		// angles
		//
		ent->angles[PITCH] = -angles[PITCH]/3;
		ent->angles[YAW] = angles[YAW];
		ent->angles[ROLL] = 0;
		ent->angles[ROLL] = PM_CalcRoll (ent->angles, state->velocity)*4;

		// run forward to the present (qualia), where vanilla ran half of it;
		// an MVD's players are where the recording has them
		msec = (int)(1000*(playertime - state->state_time));
		if (msec <= 0 || cls.mvdplayback || (!cl_predict_players.value && !cl_predict_players2.value))
		{
			VectorCopy (origin, ent->origin);
//Con_DPrintf ("nopredict\n");
		}
		else
		{
			// predict players movement
			if (msec > 255)
				msec = 255;
			state->command.msec = (byte)msec;
//Con_DPrintf ("predict: %i\n", msec);

			oldphysent = cl.pmove.numphysent;
			CL_SetSolidPlayers (j);
			CL_PredictUsercmd (state, &exact, &state->command);
			cl.pmove.numphysent = oldphysent;
			VectorCopy (exact.origin, ent->origin);
		}

		if (state->effects & EF_FLAG1)
			CL_AddFlagModels (ent, 0);
		else if (state->effects & EF_FLAG2)
			CL_AddFlagModels (ent, 1);

		if (cl.vwep_enabled && r_drawvweps.value && state->vw_index && state->modelindex == cl.playerindex)
			CL_AddVWep (ent, state->vw_index);

	}
}

//======================================================================

/*
===============
CL_SetSolid

Builds all the pmove physents for the current frame
===============
*/
void CL_SetSolidEntities (void)
{
	int		i;
	frame_t	*frame;
	packet_entities_t	*pak;
	entity_state_t		*state;

	cl.pmove.physents[0].model = cl.clipmodels[1];
	VectorCopy (vec3_origin, cl.pmove.physents[0].origin);
	cl.pmove.physents[0].info = 0;
	cl.pmove.numphysent = 1;

	frame = &cl.frames[cl.parsecountmod];
	pak = &frame->packet_entities;

	for (i=0 ; i<pak->num_entities && cl.pmove.numphysent < MAX_PHYSENTS ; i++)
	{
		state = &pak->entities[i];

		if (!state->modelindex)
			continue;
		if (!cl.clipmodels[state->modelindex])		// below MAX_MODELS, see MSG_ReadDeltaEntity
			continue;
		if (cl.clipmodels[state->modelindex]->hulls[1].firstclipnode)
		{
			cl.pmove.physents[cl.pmove.numphysent].model = cl.clipmodels[state->modelindex];
			VectorCopy (state->origin, cl.pmove.physents[cl.pmove.numphysent].origin);
			cl.pmove.numphysent++;
		}
	}

}

/*
===
Calculate the new position of players, without other player clipping

We do this to set up real player prediction.
Players are predicted twice, first without clipping other players,
then with clipping against them.
This sets up the first phase.
===
*/
void CL_SetUpPlayerPrediction(bool dopred)
{
	int				j;
	player_state_t	*state;
	player_state_t	exact;
	double			playertime;
	int				msec;
	frame_t			*frame;
	struct predicted_player *pplayer;

	playertime = host.realtime - cls.latency + 0.02;
	if (playertime > host.realtime)
		playertime = host.realtime;

	frame = &cl.frames[cl.parsecount&UPDATE_MASK];

	for (j=0, pplayer = predicted_players, state=frame->playerstate; 
		j < MAX_CLIENTS;
		j++, pplayer++, state++) {

		pplayer->active = false;

		if (state->messagenum != cl.parsecount)
			continue;	// not present this frame

		if (!state->modelindex)
			continue;

		pplayer->active = true;
		pplayer->flags = state->flags;

		// note that the local player is special, since he moves locally
		// we use his last predicted postition
		if (j == cl.playernum) {
			VectorCopy(cl.frames[cls.netchan.outgoing_sequence&UPDATE_MASK].playerstate[cl.playernum].origin,
				pplayer->origin);
		} else {
			// only predict half the move to minimize overruns
			msec = (int)(500*(playertime - state->state_time));
			if (msec <= 0 ||
				(!cl_predict_players.value && !cl_predict_players2.value) ||
				!dopred)
			{
				VectorCopy (state->origin, pplayer->origin);
	//Con_DPrintf ("nopredict\n");
			}
			else
			{
				// predict players movement
				if (msec > 255)
					msec = 255;
				state->command.msec = (byte)msec;
	//Con_DPrintf ("predict: %i\n", msec);

				CL_PredictUsercmd (state, &exact, &state->command);
				VectorCopy (exact.origin, pplayer->origin);
			}
		}
	}
}

/*
===============
CL_SetSolid

Builds all the pmove physents for the current frame
Note that CL_SetUpPlayerPrediction() must be called first!
pmove must be setup with world and solid entity hulls before calling
(via CL_PredictMove)
===============
*/
void CL_SetSolidPlayers (int playernum)
{
	int		j;
	struct predicted_player *pplayer;
	physent_t *pent;

	if (!cl_solid_players.value)
		return;

	pent = cl.pmove.physents + cl.pmove.numphysent;

	for (j=0, pplayer = predicted_players; j < MAX_CLIENTS && cl.pmove.numphysent < MAX_PHYSENTS; j++, pplayer++) {

		if (!pplayer->active)
			continue;	// not present this frame

		// the player object never gets added
		if (j == playernum)
			continue;

		// dead players aren't solid; the server says who is with Z_EXT_PF_SOLID
		if ((cl.z_ext & Z_EXT_PF_SOLID) ? !(pplayer->flags & PF_SOLID) : (pplayer->flags & PF_DEAD))
			continue;

		pent->model = 0;
		VectorCopy(pplayer->origin, pent->origin);
		VectorCopy(player_mins, pent->mins);
		VectorCopy(player_maxs, pent->maxs);
		cl.pmove.numphysent++;
		pent++;
	}
}


/*
===============
CL_EmitEntities

Builds the visedicts array for cl.time

Made up of: clients, packet_entities, nails, and tents
===============
*/
void CL_EmitEntities (void)
{
	if (cls.state != ca_active)
		return;
	if (!cl.validsequence)
		return;

	cl.oldnumvisedicts = cl.numvisedicts;
	cl.oldvisedicts = cl.visedicts_list[(cls.netchan.incoming_sequence-1)&1];
	cl.visedicts = cl.visedicts_list[cls.netchan.incoming_sequence&1];

	cl.numvisedicts = 0;

	CL_LinkPlayers ();
	CL_LinkPacketEntities ();
	CL_LinkProjectiles ();
	CL_UpdateTEnts ();
}

