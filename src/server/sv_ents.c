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

#include "sv_local.h"

//=============================================================================

// because there can be a lot of nails, there is a special
// network protocol for them
#define	MAX_NAILS	32
static edict_t	*nails[MAX_NAILS];
static int		numnails;


static bool SV_AddNailUpdate (edict_t *ent)
{
	if (sv.bigcoords)
		return false;		// the packed nail positions only reach +-4096
	if (ent->v.modelindex != sv.nailmodel
		&& ent->v.modelindex != sv.supernailmodel)
		return false;
	if (numnails == MAX_NAILS)
		return true;
	nails[numnails] = ent;
	numnails++;
	return true;
}

static void SV_EmitNailUpdate (sizebuf_t *msg)
{
	byte	bits[6];	// [48 bits] xyzpy 12 12 12 4 8 
	int		n, i;
	edict_t	*ent;
	int		x, y, z, p, yaw;

	if (!numnails)
		return;

	MSG_WriteByte (msg, svc_nails);
	MSG_WriteByte (msg, numnails);

	for (n=0 ; n<numnails ; n++)
	{
		ent = nails[n];
		x = (int)(ent->v.origin[0]+4096)>>1;
		y = (int)(ent->v.origin[1]+4096)>>1;
		z = (int)(ent->v.origin[2]+4096)>>1;
		p = (int)(16*ent->v.angles[0]/360)&15;
		yaw = (int)(256*ent->v.angles[1]/360)&255;

		bits[0] = (byte)x;
		bits[1] = (byte)((x>>8) | (y<<4));
		bits[2] = (byte)((y>>4));
		bits[3] = (byte)z;
		bits[4] = (byte)((z>>8) | (p<<4));
		bits[5] = (byte)yaw;

		for (i=0 ; i<6 ; i++)
			MSG_WriteByte (msg, bits[i]);
	}
}

//=============================================================================


/*
==================
SV_WriteDelta

Writes part of a packetentities message, or a static entity or baseline:
the fields that differ from a state the client has, in the form its
protocol extensions allow
==================
*/
void SV_WriteDelta (const client_t *client, const entity_state_t *from, const entity_state_t *to, sizebuf_t *msg,
	bool force)
{
	MSG_WriteDeltaEntity (msg, from, to, force, client->fteext, client->mvdext1);
}

/*
==================
SV_EntityFits

Whether the client's protocol has room for the entity's number and model
==================
*/
bool SV_EntityFits (const client_t *client, int number, int modelindex)
{
	if (SV_ReplacementDeltas (client))
		return true;
	if (number >= MAX_QW_EDICTS)
		return false;
	if ((number & 512) && !(client->fteext & FTE_PEXT_ENTITYDBL))
		return false;
	if ((number & 1024) && !(client->fteext & FTE_PEXT_ENTITYDBL2))
		return false;
	return modelindex < 256 || (client->fteext & FTE_PEXT_MODELDBL);
}

/*
==================
SV_EntityLook

FTE's alpha and color of an entity: from the progs' fields when they have
them, else from the map's keys; and its trail and emitted particle effects
(traileffectnum, emiteffectnum)
==================
*/
void SV_EntityLook (const edict_t *ent, entity_state_t *s)
{
	const float	*fields = (const float *)&ent->v;
	const float	*colormod;
	float		alpha;
	int			i, c;

	c = pr.fofs_traileffectnum ? (int)fields[pr.fofs_traileffectnum] : 0;
	s->traileffect = (unsigned short)(c > 0 && c < MAX_PARTICLE_PRECACHE ? c : 0);
	c = pr.fofs_emiteffectnum ? (int)fields[pr.fofs_emiteffectnum] : 0;
	s->emiteffect = (unsigned short)(c > 0 && c < MAX_PARTICLE_PRECACHE ? c : 0);

	alpha = pr.fofs_alpha ? fields[pr.fofs_alpha] : ent->alpha;
	s->alpha = 0;
	if (alpha > 0 && alpha < 1)
		s->alpha = (byte)(alpha * 254 < 1 ? 1 : alpha * 254);

	colormod = pr.fofs_colormod ? &fields[pr.fofs_colormod] : ent->colormod;
	memset (s->colormod, 0, sizeof(s->colormod));
	if ((colormod[0] || colormod[1] || colormod[2]) &&
		(colormod[0] != 1 || colormod[1] != 1 || colormod[2] != 1))
	{
		for (i=0 ; i<3 ; i++)
		{
			c = (int)(colormod[i] * 32);
			s->colormod[i] = (byte)(c < 0 ? 0 : c > 255 ? 255 : c);
		}
	}
}

/*
==================
SV_ClientBaseline

An entity's baseline as the client got it at prespawn, without what its
protocol extensions have no room for; new entities are sent as deltas from it
==================
*/
void SV_ClientBaseline (const client_t *client, const edict_t *ent, entity_state_t *base)
{
	*base = ent->baseline;
	if (SV_ReplacementDeltas (client))
		return;
	if (base->modelindex > 255 && !(client->fteext & FTE_PEXT_MODELDBL))
		base->modelindex = 0;
	if (!(client->fteext & FTE_PEXT_SPAWNSTATIC2))
	{
		base->alpha = 0;
		memset (base->colormod, 0, sizeof(base->colormod));
	}
}

/*
=============
SV_EntityState

An entity as the clients see it
=============
*/
void SV_EntityState (const edict_t *ent, int number, entity_state_t *state)
{
	*state = (entity_state_t){.number = number};
	// FTE's: what steps, or doesn't move by itself, is drawn from step to step
	// (with replacement deltas)
	if (!ent->v.movetype || ent->v.movetype == MOVETYPE_STEP)
		state->dpflags = RENDER_STEP;
	VectorCopy (ent->v.origin, state->origin);
	VectorCopy (ent->v.angles, state->angles);
	state->modelindex = (int)ent->v.modelindex;
	state->frame = (int)ent->v.frame;
	state->colormap = (int)ent->v.colormap;
	state->skinnum = (int)ent->v.skin;
	state->effects = (int)ent->v.effects;
	SV_EntityLook (ent, state);
}

/*
=============
SV_EmitPacketEntities

Writes a delta update of a packet_entities_t to the message, from a frame the
client has (its delta_sequence's), or the whole of it without one.

=============
*/
void SV_EmitPacketEntities (const client_t *client, const packet_entities_t *from, const packet_entities_t *to,
	sizebuf_t *msg)
{
	edict_t	*ent;
	int		oldindex, newindex;
	int		oldnum, newnum;
	int		oldmax;
	entity_state_t	base;

	if (from)
	{
		oldmax = from->num_entities;
		MSG_WriteByte (msg, svc_deltapacketentities);
		MSG_WriteByte (msg, client->delta_sequence);
	}
	else
	{
		oldmax = 0;	// no delta update
		MSG_WriteByte (msg, svc_packetentities);
	}

	newindex = 0;
	oldindex = 0;
//Con_Printf ("---%i to %i ----\n", client->delta_sequence & UPDATE_MASK
//			, client->netchan.outgoing_sequence & UPDATE_MASK);
	while (newindex < to->num_entities || oldindex < oldmax)
	{
		newnum = newindex >= to->num_entities ? 9999 : to->entities[newindex].number;
		oldnum = oldindex >= oldmax ? 9999 : from->entities[oldindex].number;

		if (newnum == oldnum)
		{	// delta update from old position
//Con_Printf ("delta %i\n", newnum);
			SV_WriteDelta (client, &from->entities[oldindex], &to->entities[newindex], msg, false);
			oldindex++;
			newindex++;
			continue;
		}

		if (newnum < oldnum)
		{	// this is a new entity, send it from the baseline
			ent = EDICT_NUM(newnum);
//Con_Printf ("baseline %i\n", newnum);
			SV_ClientBaseline (client, ent, &base);
			SV_WriteDelta (client, &base, &to->entities[newindex], msg, true);
			newindex++;
			continue;
		}

		if (newnum > oldnum)
		{	// the old entity isn't present in the new message
//Con_Printf ("remove %i\n", oldnum);
			MSG_WriteEntityRemove (msg, oldnum);
			oldindex++;
			continue;
		}
	}

	MSG_WriteShort (msg, 0);	// end of packetentities
}

/*
=============
SV_PMCode

The PF_PMC code for how the player moves; dead is normal, with PF_DEAD
=============
*/
static int SV_PMCode (const client_t *cl)
{
	switch (SV_PMTypeForClient (cl))
	{
	case PM_OLD_SPECTATOR:
		return PMC_OLD_SPECTATOR;
	case PM_SPECTATOR:
		return PMC_SPECTATOR;
	case PM_FLY:
		return PMC_FLY;
	case PM_NONE:
		return PMC_NONE;
	case PM_LOCK:
		return PMC_LOCK;
	case PM_NORMAL:
		return cl->jump_held ? PMC_NORMAL_JUMP_HELD : PMC_NORMAL;
	}
	return PMC_NORMAL;
}

/*
=============
SV_WritePlayersToClient

=============
*/
static void SV_WritePlayersToClient (client_t *client, edict_t *clent, byte *pvs, sizebuf_t *msg)
{
	int			i, j;
	client_t	*cl;
	edict_t		*ent;
	int			msec;
	usercmd_t	cmd;
	int			pflags;
	entity_state_t	look;

	for (j=0,cl=svs.clients ; j<MAX_CLIENTS ; j++,cl++)
	{
		if (cl->state != cs_spawned)
			continue;

		ent = cl->edict;

		// ZOID visibility tracking
		if (ent != clent &&
			!(client->spec_track && client->spec_track - 1 == j)) 
		{
			if (cl->spectator)
				continue;

			// ignore if not touching a PV leaf
			for (i=0 ; i < ent->num_leafs ; i++)
				if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i]&7) ))
					break;
			if (i == ent->num_leafs)
				continue;		// not visible
		}
		
		pflags = PF_MSEC | PF_COMMAND;
		
		if (ent->v.modelindex != sv.playermodel)
			pflags |= PF_MODEL;
		for (i=0 ; i<3 ; i++)
			if (ent->v.velocity[i])
				pflags |= PF_VELOCITY1<<i;
		if (ent->v.effects)
			pflags |= PF_EFFECTS;
		// a model past 255 borrows the skin's top bit
		if (ent->v.skin || ((pflags & PF_MODEL) && ent->v.modelindex > 255))
			pflags |= PF_SKINNUM;
		SV_EntityLook (ent, &look);
		if (look.alpha && (client->fteext & FTE_PEXT_TRANS))
			pflags |= PF_TRANS;
		// ZQuake's, to clients that asked for them
		if ((client->z_ext & Z_EXT_PF_ONGROUND) && ((int)ent->v.flags & FL_ONGROUND))
			pflags |= PF_ONGROUND;
		if ((client->z_ext & Z_EXT_PF_SOLID) && (ent->v.solid == SOLID_BBOX || ent->v.solid == SOLID_SLIDEBOX))
			pflags |= PF_SOLID;
		// the flag rides in the byte that comes with FTE_PEXT_TRANS
		if ((look.colormod[0] | look.colormod[1] | look.colormod[2]) &&
			(client->fteext & FTE_PEXT_TRANS) && (client->fteext & FTE_PEXT_COLOURMOD))
			pflags |= PF_COLOURMOD;
		if (ent->v.health <= 0)
			pflags |= PF_DEAD;
		if (ent->v.mins[2] != -24)
			pflags |= PF_GIB;

		if (cl->spectator)
		{	// only sent origin and velocity to spectators
			pflags &= PF_VELOCITY1 | PF_VELOCITY2 | PF_VELOCITY3;
		}
		else if (ent == clent)
		{	// don't send a lot of data on personal entity
			pflags &= ~(PF_MSEC|PF_COMMAND);
			if (ent->v.weaponframe)
				pflags |= PF_WEAPONFRAME;
		}

		if (client->spec_track && client->spec_track - 1 == j &&
			ent->v.weaponframe)
			pflags |= PF_WEAPONFRAME;

		// how the player moves (Z_EXT_PM_TYPE), for prediction
		if (client->z_ext & Z_EXT_PM_TYPE)
			pflags |= SV_PMCode (cl) << PF_PMC_SHIFT;
		if (SV_PMTypeForClient (cl) == PM_LOCK && ent == clent)
			pflags |= PF_COMMAND;	// the view angles the server sets

		MSG_WriteByte (msg, svc_playerinfo);
		MSG_WriteByte (msg, j);
		// with FTE_PEXT_TRANS, flags 16-23 go in a byte of their own; without
		// it, the two ZQuake flags there go at the top of the word
		if (client->fteext & FTE_PEXT_TRANS)
		{
			if (pflags & 0xff0000)
				pflags |= PF_EXTRA_PFS;
			MSG_WriteShort (msg, pflags & 0xffff);
			if (pflags & PF_EXTRA_PFS)
				MSG_WriteByte (msg, pflags >> 16);
		}
		else
			MSG_WriteShort (msg, (pflags & 0x3fff) | ((pflags & 0xc00000) >> 8));

		for (i=0 ; i<3 ; i++)
			MSG_WriteOrigin (msg, ent->v.origin[i], client->mvdext1);

		MSG_WriteByte (msg, (int)ent->v.frame);

		if (pflags & PF_MSEC)
		{
			msec = (int)(1000*(sv.time - cl->localtime));
			if (msec > 255)
				msec = 255;
			MSG_WriteByte (msg, msec);
		}
		
		if (pflags & PF_COMMAND)
		{
			cmd = cl->lastcmd;

			if (ent->v.health <= 0)
			{	// don't show the corpse looking around...
				cmd.angles[0] = 0;
				cmd.angles[1] = ent->v.angles[1];
				cmd.angles[0] = 0;
			}

			cmd.buttons = 0;	// never send buttons
			cmd.impulse = 0;	// never send impulses

			if (ent == clent)
			{	// PM_LOCK: only the view angles
				VectorCopy (ent->v.v_angle, cmd.angles);
				cmd.forwardmove = cmd.sidemove = cmd.upmove = 0;
			}

			MSG_WriteDeltaUsercmd (msg, &nullcmd, &cmd);
		}

		for (i=0 ; i<3 ; i++)
			if (pflags & (PF_VELOCITY1<<i) )
				MSG_WriteShort (msg, (int)ent->v.velocity[i]);

		if (pflags & PF_MODEL)
			MSG_WriteByte (msg, (int)ent->v.modelindex & 255);

		if (pflags & PF_SKINNUM)
			MSG_WriteByte (msg, (int)ent->v.skin | ((pflags & PF_MODEL) && ent->v.modelindex > 255 ? 128 : 0));

		if (pflags & PF_EFFECTS)
			MSG_WriteByte (msg, (int)ent->v.effects);

		if (pflags & PF_WEAPONFRAME)
			MSG_WriteByte (msg, (int)ent->v.weaponframe);

		if (pflags & PF_TRANS)
			MSG_WriteByte (msg, look.alpha);

		if (pflags & PF_COLOURMOD)
		{
			for (i=0 ; i<3 ; i++)
				MSG_WriteByte (msg, look.colormod[i]);
		}
	}
}


/*
==============================================================================

FTE'S REPLACEMENT DELTAS

A client with FTE_PEXT2_REPLACEMENTDELTAS, on a level that has them, gets
what changed of each entity it sees, the players among them, as FTE's server
sends it (SVFTE_EmitPacketEntities): the server keeps what it has sent the
client and what is still to send, by entity. What doesn't fit in a packet
goes in the next, which goes on from there; what a lost packet carried is
sent again.

==============================================================================
*/

#define	UF_SV_REMOVE	UF_16BIT		// pending: it is gone (the writer works UF_16BIT out itself)

typedef struct
{
	int			number;
	unsigned	bits;
} svresend_t;

// what a packet carried, to send again if it is lost
typedef struct
{
	int			sequence;		// the packet's
	int			num, max;
	svresend_t	*resend;
} svsentpacket_t;

typedef struct svdeltas_s
{
	entity_state_t	*sent;			// by number: what the client has, or will once it gets what went; 0 none
	unsigned		*pending;		// by number: what is still to send of it (pending[0]: forget them all)
	int				max;			// the numbers both have room for
	int				num;			// past the highest number in use
	int				next;			// where the last packet that didn't hold everything stopped
	int				acked;			// the last packet the client acknowledged
	svsentpacket_t	packets[UPDATE_BACKUP];
} svdeltas_t;

static entity_state_t	sv_seen[MAX_EDICTS];		// a client's view, built each packet

bool SV_ReplacementDeltas (const client_t *client)
{
	return sv.replacementdeltas && (client->fteext2 & FTE_PEXT2_REPLACEMENTDELTAS);
}

void SV_FreeDeltas (client_t *client)
{
	svdeltas_t	*d = client->deltas;
	int			i;

	if (!d)
		return;
	for (i=0 ; i<UPDATE_BACKUP ; i++)
		Mem_Free (d->packets[i].resend);
	Mem_Free (d->sent);
	Mem_Free (d->pending);
	Mem_Free (d);
	client->deltas = NULL;
}

// room for entity numbers below n
static void SV_GrowDeltas (svdeltas_t *d, int n)
{
	int		max = d->max;

	if (n > d->num)
		d->num = n;
	if (n <= d->max)
		return;
	while (max < n)
		max = max ? max * 2 : 512;
	d->sent = Mem_Realloc (d->sent, (size_t)max * sizeof(*d->sent));
	d->pending = Mem_Realloc (d->pending, (size_t)max * sizeof(*d->pending));
	memset (d->sent + d->max, 0, (size_t)(max - d->max) * sizeof(*d->sent));
	memset (d->pending + d->max, 0, (size_t)(max - d->max) * sizeof(*d->pending));
	d->max = max;
}

// what a packet carried of an entity
static void SV_NoteSent (svdeltas_t *d, int packet, int number, unsigned bits)
{
	svsentpacket_t	*p = &d->packets[packet];

	if (p->num == p->max)
	{
		p->max = p->max ? p->max * 2 : 64;
		p->resend = Mem_Realloc (p->resend, (size_t)p->max * sizeof(*p->resend));
	}
	p->resend[p->num++] = (svresend_t){number, bits};
}

// what a packet the client won't have carried, to be sent again as it is now:
// one gone since, gone (the world, all of them: forget them all again); one
// gone and back, whole
static void SV_Resend (svdeltas_t *d, int packet)
{
	svsentpacket_t	*p = &d->packets[packet];
	int		i, e;

	for (i=0 ; i<p->num ; i++)
	{
		e = p->resend[i].number;
		if (!d->sent[e].number)
			d->pending[e] = UF_SV_REMOVE;
		else if (p->resend[i].bits & UF_SV_REMOVE)
			d->pending[e] = UF_RESET;
		else
			d->pending[e] |= p->resend[i].bits;
	}
	p->num = 0;
}

void SV_DeltasUnsent (client_t *client)
{
	if (client->deltas)
		SV_Resend (client->deltas, (client->netchan.outgoing_sequence + 1) & UPDATE_MASK);
}

void SV_DeltasAcked (client_t *client)
{
	svdeltas_t	*d = client->deltas;
	int			acked = client->netchan.incoming_acknowledged, seq;

	if (!d || acked <= d->acked)
		return;
	// the client acknowledges the newest it got: those before it were lost
	seq = d->acked + 1 > acked - UPDATE_BACKUP + 1 ? d->acked + 1 : acked - UPDATE_BACKUP + 1;
	for ( ; seq < acked ; seq++)
		if (d->packets[seq & UPDATE_MASK].sequence == seq)
			SV_Resend (d, seq & UPDATE_MASK);
	d->packets[acked & UPDATE_MASK].num = 0;
	d->acked = acked;
}

// whether the entity touches a leaf the visibility set has
static bool SV_EdictVisible (const edict_t *ent, const byte *pvs)
{
	int		i;

	for (i=0 ; i < ent->num_leafs ; i++)
		if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i]&7)))
			return true;
	return false;
}

// A player as the client sees it, with what predicting it takes (FTE's): how
// it moves (its movetype, 0x80 on the ground, 0x40 jump held), its velocity,
// and the others' moves; 0 for one the server moves itself, NetQuake's
static void SV_PlayerState (const client_t *client, const client_t *cl, int j, entity_state_t *s)
{
	const edict_t	*ent = cl->edict;
	int				i, v;

	SV_EntityState (ent, j + 1, s);
	s->dpflags = 0;
	if (!SV_NQPhysics (cl))
	{
		// the movetype the client takes for the server's pm_type (FTE's
		// client: a dead player tosses)
		switch (SV_PMTypeForClient (cl))
		{
		case PM_OLD_SPECTATOR:
		case PM_SPECTATOR:	s->pmovetype = MOVETYPE_NOCLIP; break;
		case PM_FLY:		s->pmovetype = MOVETYPE_FLY; break;
		case PM_NONE:		s->pmovetype = MOVETYPE_NONE; break;
		case PM_LOCK:		s->pmovetype = MOVETYPE_LOCK; break;
		case PM_DEAD:		s->pmovetype = MOVETYPE_TOSS; break;
		default:			s->pmovetype = MOVETYPE_WALK; break;
		}
		if ((int)ent->v.flags & FL_ONGROUND)
			s->pmovetype |= 0x80;
		if (cl->jump_held)
			s->pmovetype |= 0x40;
	}
	if (s->pmovetype || cl == client)
	{	// the client's own for its view's bob too
		for (i=0 ; i<3 ; i++)
		{
			v = (int)(ent->v.velocity[i] * 8);
			s->velocity[i] = (short)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
		}
	}
	if (cl != client && s->pmovetype)
	{
		s->movement[0] = cl->lastcmd.forwardmove;
		s->movement[1] = cl->lastcmd.sidemove;
		s->movement[2] = cl->lastcmd.upmove;
		v = (int)(1000*(sv.time - cl->localtime));
		s->msec = (byte)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
	if (cl == client || (client->spec_track && client->spec_track - 1 == j))
		s->weaponframe = (int)ent->v.weaponframe;
}

// what the client sees, by number: the players, then the other entities,
// nails among them (svc_nails' coordinates are short of big levels; FTE's)
static int SV_SeenStates (client_t *client, edict_t *clent, const byte *pvs)
{
	client_t	*cl;
	edict_t		*ent;
	int			e, j, n = 0;

	for (j=0,cl=svs.clients ; j<MAX_CLIENTS ; j++,cl++)
	{
		if (cl->state != cs_spawned)
			continue;
		ent = cl->edict;
		if (ent != clent && !(client->spec_track && client->spec_track - 1 == j)
			&& (cl->spectator || !SV_EdictVisible (ent, pvs)))
			continue;
		SV_PlayerState (client, cl, j, &sv_seen[n++]);
	}

	for (e=MAX_CLIENTS+1, ent=EDICT_NUM(e) ; e<sv.num_edicts ; e++, ent = NEXT_EDICT(ent))
	{
		if (!ent->v.modelindex || !*PR_GetString(ent->v.model) || !SV_EdictVisible (ent, pvs))
			continue;
		SV_EntityState (ent, e, &sv_seen[n++]);
	}
	return n;
}

/*
=============
SV_EmitDeltas

The client's view into what it is to be sent, and what fits of that into the
message, short of the room the rest of the packet takes (reserve)
=============
*/
static void SV_EmitDeltas (client_t *client, const entity_state_t *seen, int count, sizebuf_t *msg,
	int reserve)
{
	svdeltas_t			*d = client->deltas;
	const entity_state_t	*n;
	entity_state_t		*o, base;
	unsigned			bits, sentbits;
	int					i, j, packet, sequence;

	if (!d)
		d = client->deltas = Mem_Calloc (1, sizeof(*d));
	SV_GrowDeltas (d, count ? seen[count-1].number + 1 : 1);

	// this packet's record; one that is still unacknowledged is taken as lost
	sequence = client->netchan.outgoing_sequence + 1;
	packet = sequence & UPDATE_MASK;
	if (d->packets[packet].sequence > d->acked)
		SV_Resend (d, packet);
	d->packets[packet].sequence = sequence;
	d->packets[packet].num = 0;

	// a client without the frames it was sent (a new level, a lost reset)
	// starts over from nothing
	if (client->delta_sequence == -1)
		d->pending[0] = UF_SV_REMOVE;
	if (d->pending[0] & UF_SV_REMOVE)
	{
		for (j=0 ; j<d->num ; j++)
		{
			d->sent[j].number = 0;
			d->pending[j] = 0;
		}
		d->pending[0] = UF_SV_REMOVE;
	}

	// what changed: new ones whole, gone ones removed
	for (i=0, j=1 ; i<count ; i++, j++)
	{
		n = &seen[i];
		for ( ; j < n->number ; j++)
			if (d->sent[j].number)
			{
				d->pending[j] = UF_SV_REMOVE;
				d->sent[j].number = 0;
			}
		o = &d->sent[j];
		if (!o->number || (d->pending[j] & UF_SV_REMOVE))
			d->pending[j] = UF_RESET;	// new, or gone and back before the client was told
		else
			d->pending[j] |= MSG_ReplacementBits (o, n);
		*o = *n;
	}
	for ( ; j < d->num ; j++)
		if (d->sent[j].number)
		{
			d->pending[j] = UF_SV_REMOVE;
			d->sent[j].number = 0;
		}

	// an update is at most 59 bytes, the end 2
	if (msg->cursize + 64 + reserve > msg->maxsize)
		return;
	MSG_WriteByte (msg, svc_fte_updateentities);
	MSG_WriteFloat (msg, (float)sv.physicstime);
	if (d->pending[0] & UF_SV_REMOVE)
	{	// forget them all
		MSG_WriteEntityIndex (msg, 0, true);
		SV_NoteSent (d, packet, 0, UF_SV_REMOVE);
		d->pending[0] = 0;
	}
	for (j=1 ; j<d->num ; j++)
	{
		bits = d->pending[j];
		if (!bits)
			continue;
		if (msg->cursize + 64 + reserve > msg->maxsize)
			break;		// the rest next packet
		if (bits & UF_SV_REMOVE)
		{
			MSG_WriteEntityIndex (msg, j, true);
			sentbits = UF_SV_REMOVE;
		}
		else
		{
			// players in every packet, the rest on from where it stopped
			if (j < d->next && j > MAX_CLIENTS)
				continue;
			sentbits = bits;
			if (bits & UF_RESET)
			{	// from the baseline the client got at prespawn
				SV_ClientBaseline (client, EDICT_NUM (j), &base);
				if (!base.modelindex)
					base = (entity_state_t){0};
				bits = UF_RESET | MSG_ReplacementBits (&base, &d->sent[j]);
				sentbits = UF_RESET;
			}
			MSG_WriteEntityIndex (msg, j, false);
			MSG_WriteReplacement (msg, bits, &d->sent[j], client->mvdext1);
		}
		SV_NoteSent (d, packet, j, sentbits);
		d->pending[j] = 0;
	}
	MSG_WriteShort (msg, 0);
	d->next = j < d->num ? j : 0;
}

/*
=============
SV_WriteEntitiesToClient

Encodes the current state of the world as
a svc_packetentities messages and possibly
a svc_nails message and
svc_playerinfo messages
=============
*/
void SV_WriteEntitiesToClient (client_t *client, sizebuf_t *msg)
{
	int		e, i;
	byte	*pvs;
	vec3_t	org;
	edict_t	*ent;
	packet_entities_t	*pack;
	edict_t	*clent;
	client_frame_t	*frame;
	int		maxentities;

	// this is the frame we are creating
	frame = &client->frames[client->netchan.incoming_sequence & UPDATE_MASK];

	// find the client's PVS
	clent = client->edict;
	VectorAdd (clent->v.origin, clent->v.view_ofs, org);
	pvs = CM_FatPVS (sv.map, org);

	if (SV_ReplacementDeltas (client))
	{	// the players with the rest, before the datagram's multicasts
		SV_EmitDeltas (client, sv_seen, SV_SeenStates (client, clent, pvs), msg,
			client->datagram.overflowed ? 0 : client->datagram.cursize);
		return;
	}

	// send over the players in the PVS
	SV_WritePlayersToClient (client, clent, pvs, msg);
	
	// put other visible entities into either a packet_entities or a nails message
	pack = &frame->entities;
	pack->num_entities = 0;

	numnails = 0;
	maxentities = (client->fteext & FTE_PEXT_256PACKETENTITIES) ? MAX_PACKET_ENTITIES : STD_PACKET_ENTITIES;

	for (e=MAX_CLIENTS+1, ent=EDICT_NUM(e) ; e<sv.num_edicts ; e++, ent = NEXT_EDICT(ent))
	{
		// ignore ents without visible models
		if (!ent->v.modelindex || !*PR_GetString(ent->v.model))
			continue;

		// ignore if not touching a PV leaf
		for (i=0 ; i < ent->num_leafs ; i++)
			if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i]&7) ))
				break;
			
		if (i == ent->num_leafs)
			continue;		// not visible

		if (SV_AddNailUpdate (ent))
			continue;	// added to the special update list

		// add to the packetentities, if the client's protocol has room for it
		if (pack->num_entities == maxentities)
			continue;	// all full
		if (!SV_EntityFits (client, e, (int)ent->v.modelindex))
			continue;

		SV_EntityState (ent, e, &pack->entities[pack->num_entities++]);
	}

	// encode the packet entities as a delta from the
	// last packetentities acknowledged by the client

	SV_EmitPacketEntities (client, client->delta_sequence != -1
		? &client->frames[client->delta_sequence & UPDATE_MASK].entities : NULL, pack, msg);

	// now add the specialized nail update
	SV_EmitNailUpdate (msg);
}
