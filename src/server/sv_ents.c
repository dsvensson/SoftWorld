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


bool SV_AddNailUpdate (edict_t *ent)
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

void SV_EmitNailUpdate (sizebuf_t *msg)
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
them, else from the map's keys
==================
*/
void SV_EntityLook (const edict_t *ent, entity_state_t *s)
{
	const float	*fields = (const float *)&ent->v;
	const float	*colormod;
	float		alpha;
	int			i, c;

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
SV_EmitPacketEntities

Writes a delta update of a packet_entities_t to the message.

=============
*/
void SV_EmitPacketEntities (client_t *client, packet_entities_t *to, sizebuf_t *msg)
{
	edict_t	*ent;
	client_frame_t	*fromframe;
	packet_entities_t *from;
	int		oldindex, newindex;
	int		oldnum, newnum;
	int		oldmax;
	entity_state_t	base;

	// this is the frame that we are going to delta update from
	if (client->delta_sequence != -1)
	{
		fromframe = &client->frames[client->delta_sequence & UPDATE_MASK];
		from = &fromframe->entities;
		oldmax = from->num_entities;

		MSG_WriteByte (msg, svc_deltapacketentities);
		MSG_WriteByte (msg, client->delta_sequence);
	}
	else
	{
		oldmax = 0;	// no delta update
		from = NULL;

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
SV_WritePlayersToClient

=============
*/
void SV_WritePlayersToClient (client_t *client, edict_t *clent, byte *pvs, sizebuf_t *msg)
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
	entity_state_t	*state;
	int		maxentities;

	// this is the frame we are creating
	frame = &client->frames[client->netchan.incoming_sequence & UPDATE_MASK];

	// find the client's PVS
	clent = client->edict;
	VectorAdd (clent->v.origin, clent->v.view_ofs, org);
	pvs = CM_FatPVS (sv.map, org);

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

		state = &pack->entities[pack->num_entities];
		pack->num_entities++;

		state->number = e;
		state->flags = 0;
		VectorCopy (ent->v.origin, state->origin);
		VectorCopy (ent->v.angles, state->angles);
		state->modelindex = (int)ent->v.modelindex;
		state->frame = (int)ent->v.frame;
		state->colormap = (int)ent->v.colormap;
		state->skinnum = (int)ent->v.skin;
		state->effects = (int)ent->v.effects;
		SV_EntityLook (ent, state);
	}

	// encode the packet entities as a delta from the
	// last packetentities acknowledged by the client

	SV_EmitPacketEntities (client, pack, msg);

	// now add the specialized nail update
	SV_EmitNailUpdate (msg);
}
