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
// sv_nqmsg.c -- NetQuake's messages, in QuakeWorld's words
//
// NetQuake's progs write NetQuake's messages with the Write builtins: a gunshot
// without a count, an intermission without the view, a cd track with its loop
// track. As FTE's server does (net_preparse.c), each destination's writes are
// held as values until a message is whole, then written as QuakeWorld's, at
// the destination's precision. A message that isn't known is dropped, with
// the rest of that destination's writes until the frame's messages go out.

#include "sv_local.h"

// NetQuake's numbers where QuakeWorld's mean something else
#define	NQSVC_UPDATENAME	13
#define	NQSVC_UPDATECOLORS	17
#define	NQSVC_CUTSCENE		34		// QuakeWorld's svc_smallkick
#define	NQTE_EXPLOSION2		12		// QuakeWorld's TE_BLOOD
#define	NQTE_BEAM			13		// QuakeWorld's TE_LIGHTNINGBLOOD

#define	NQ_MAXVALUES		16		// more than any message has
#define	NQ_STRINGBYTES		4096	// the longest finale's text and more

typedef struct
{
	nqwrite_t	kind;
	float		value;
	int			string;				// NQW_STRING: where in strings
} nqvalue_t;

// the writes to a destination, since the last whole message
typedef struct
{
	nqvalue_t	values[NQ_MAXVALUES];
	int			count;
	char		strings[NQ_STRINGBYTES];
	int			stringbytes;
	client_t	*one;				// MSG_ONE's client
	bool		dropping;			// past an unknown message, until the frame's end
} nqstream_t;

static nqstream_t	nq_streams[MSG_INIT + 1];
static byte			nq_warned[256 / 8];	// the messages warned of this level

static int NQ_Int (const nqvalue_t *v)
{
	return (int)v->value;
}

/*
==============================================================================

THE MESSAGES

==============================================================================
*/

// the values the message at the start of a stream has: 0 while they aren't
// all there, -1 for a message this doesn't know
static int NQ_MessageLength (const nqstream_t *s)
{
	if (!s->count)
		return 0;
	switch (NQ_Int (&s->values[0]))
	{
	case svc_killedmonster:
	case svc_foundsecret:
	case svc_sellscreen:
	case svc_intermission:
		return 1;
	case svc_print:
	case svc_centerprint:
	case svc_stufftext:
	case svc_finale:
	case NQSVC_CUTSCENE:
		return s->count >= 2 && s->values[1].kind != NQW_STRING ? -1 : 2;
	case svc_setview:
		return 2;
	case svc_cdtrack:				// the track and the one to loop
	case svc_updatestat:			// the stat and a long
	case svc_lightstyle:
	case NQSVC_UPDATENAME:
	case NQSVC_UPDATECOLORS:
		return 3;
	case svc_setangle:
		return 4;
	case svc_spawnstaticsound:
		return 7;
	case svc_temp_entity:
		if (s->count < 2)
			return 0;
		switch (NQ_Int (&s->values[1]))
		{
		case TE_SPIKE:
		case TE_SUPERSPIKE:
		case TE_GUNSHOT:
		case TE_EXPLOSION:
		case TE_TAREXPLOSION:
		case TE_WIZSPIKE:
		case TE_KNIGHTSPIKE:
		case TE_LAVASPLASH:
		case TE_TELEPORT:
			return 5;				// the place
		case NQTE_EXPLOSION2:
			return 7;				// the place, and the colors
		case TE_LIGHTNING1:
		case TE_LIGHTNING2:
		case TE_LIGHTNING3:
		case NQTE_BEAM:
			return 9;				// the entity, the start and the end
		}
		return -1;
	}
	return -1;
}

static void NQ_PutValue (sizebuf_t *msg, const nqstream_t *s, const nqvalue_t *v)
{
	switch (v->kind)
	{
	case NQW_BYTE:		MSG_WriteByte (msg, NQ_Int (v));	break;
	case NQW_CHAR:		MSG_WriteChar (msg, NQ_Int (v));	break;
	case NQW_SHORT:		MSG_WriteShort (msg, NQ_Int (v));	break;
	case NQW_LONG:		MSG_WriteLong (msg, NQ_Int (v));	break;
	case NQW_COORD:		MSG_WriteCoord (msg, v->value);		break;
	case NQW_ANGLE:		MSG_WriteAngle (msg, v->value);		break;
	case NQW_STRING:	MSG_WriteString (msg, s->strings + v->string);	break;
	case NQW_ENTITY:	MSG_WriteShort (msg, NQ_Int (v));	break;
	}
}

// values first .. first + count - 1 of the message, as they were written
static void NQ_PutValues (sizebuf_t *msg, const nqstream_t *s, int first, int count)
{
	int		i;

	for (i = first ; i < first + count ; i++)
		NQ_PutValue (msg, s, &s->values[i]);
}

// The message in QuakeWorld's words; false when QuakeWorld's clients have none
// such. FTE's server sends NetQuake's beam and second explosion only to its
// clients with FTE_PEXT_TE_BULLET, and the explosion as a plain one to others.
static bool NQ_Translate (const nqstream_t *s, int count, sizebuf_t *msg)
{
	const nqvalue_t	*v = s->values;

	switch (NQ_Int (&v[0]))
	{
	case svc_temp_entity:
		switch (NQ_Int (&v[1]))
		{
		case TE_GUNSHOT:			// with QuakeWorld's count
			NQ_PutValues (msg, s, 0, 2);
			MSG_WriteByte (msg, 1);
			NQ_PutValues (msg, s, 2, 3);
			return true;
		case NQTE_EXPLOSION2:
			MSG_WriteByte (msg, svc_temp_entity);
			MSG_WriteByte (msg, TE_EXPLOSION);
			NQ_PutValues (msg, s, 2, 3);
			return true;
		case NQTE_BEAM:
			return false;
		}
		break;
	case svc_print:					// with QuakeWorld's level
		MSG_WriteByte (msg, svc_print);
		MSG_WriteByte (msg, PRINT_HIGH);
		NQ_PutValues (msg, s, 1, 1);
		return true;
	case svc_cdtrack:				// without the track to loop
		NQ_PutValues (msg, s, 0, 2);
		return true;
	case svc_updatestat:			// NetQuake's stats are longs
		MSG_WriteByte (msg, svc_updatestatlong);
		MSG_WriteByte (msg, NQ_Int (&v[1]));
		MSG_WriteLong (msg, NQ_Int (&v[2]));
		return true;
	case NQSVC_CUTSCENE:			// FTE's: a finale without its picture, the text dropped
		MSG_WriteByte (msg, svc_finale);
		MSG_WriteString (msg, "/.");
		return true;
	case svc_setview:
	case NQSVC_UPDATENAME:
	case NQSVC_UPDATECOLORS:
		return false;
	}
	NQ_PutValues (msg, s, 0, count);
	return true;
}

// how a temp entity from MSG_BROADCAST goes out, as FTE's server multicasts
// them; and where from (its place, or a beam's start)
static int NQ_TempEntityMulticast (const nqstream_t *s, vec3_t origin)
{
	int		te = NQ_Int (&s->values[1]), first = 2, i;

	if (te == TE_LIGHTNING1 || te == TE_LIGHTNING2 || te == TE_LIGHTNING3 || te == NQTE_BEAM)
		first = 3;
	for (i = 0 ; i < 3 ; i++)
		origin[i] = s->values[first + i].value;
	switch (te)
	{
	case TE_LAVASPLASH:
		return MULTICAST_ALL;
	case TE_SPIKE:
	case TE_SUPERSPIKE:
	case TE_EXPLOSION:
	case NQTE_EXPLOSION2:
	case TE_LIGHTNING1:
	case TE_LIGHTNING2:
	case TE_LIGHTNING3:
	case NQTE_BEAM:
		return MULTICAST_PHS;
	default:
		return MULTICAST_PVS;
	}
}

/*
==============================================================================

SENDING

==============================================================================
*/

// the message for one client only: to its reliable stream, and QTV's view of it
static void NQ_SendOne (client_t *cl, const nqstream_t *s, int count)
{
	byte		data[MAX_MSGLEN];
	sizebuf_t	msg = {.data = data, .maxsize = sizeof(data), .allowoverflow = true,
		.floatcoords = cl->netchan.message.floatcoords};

	if (!NQ_Translate (s, count, &msg) || msg.overflowed)
		return;
	ClientReliableCheckBlock (cl, msg.cursize);
	ClientReliableWrite_SZ (cl, msg.data, msg.cursize);
	SV_MVDSingle (cl, msg.data, msg.cursize);
}

// QuakeWorld's intermission carries the view, where id1 has just put the
// player: the place and angles of each client's entity, as FTE's server sends
// it; and an angle set is the entity's angles turned at once
static void NQ_ForClient (client_t *cl, int svc, const nqstream_t *s)
{
	edict_t	*ent = cl->edict;
	int		i;

	if (svc == svc_setangle)
	{
		for (i = 0 ; i < 3 ; i++)
			ent->v.angles[i] = s->values[1 + i].value;
		ent->v.fixangle = 1;
		return;
	}
	ClientReliableWrite_Begin (cl, svc_intermission, 1 + 3 * 4 + 3 * 2);
	for (i = 0 ; i < 3 ; i++)
		ClientReliableWrite_Coord (cl, ent->v.origin[i] + (i == 2 ? ent->v.view_ofs[2] : 0));
	for (i = 0 ; i < 3 ; i++)
		ClientReliableWrite_Angle (cl, ent->v.angles[i]);
}

// A message to a buffer all share, with room made for it, as FTE's server
// makes it: the broadcasts go out at once when it wouldn't fit
// (SV_FlushBroadcasts), the signon goes on in its next buffer. Copper's
// monsters send their count to all as each spawns, a level of them at once.
static void NQ_SendAll (const nqstream_t *s, int count, sizebuf_t *dest)
{
	byte		data[MAX_MSGLEN];
	sizebuf_t	msg = {.data = data, .maxsize = sizeof(data), .allowoverflow = true,
		.floatcoords = dest->floatcoords};

	if (!NQ_Translate (s, count, &msg) || msg.overflowed)
		return;
	if (dest == &sv.signon)
		SV_SignonRoom (msg.cursize);
	else if (dest->cursize + msg.cursize > dest->maxsize)
		SV_FlushBroadcasts ();
	SZ_Write (dest, msg.data, msg.cursize);
}

// the whole message at the start of the stream for dest, to where it goes
static void NQ_Send (int dest, const nqstream_t *s, int count)
{
	int			svc = NQ_Int (&s->values[0]), to, i;
	client_t	*cl;
	vec3_t		origin;

	if (svc == svc_intermission || svc == svc_setangle)
	{
		if (dest == MSG_ONE)
			NQ_ForClient (s->one, svc, s);
		else if (dest != MSG_INIT)
			for (i = 0, cl = svs.clients ; i < MAX_CLIENTS ; i++, cl++)
				if (cl->state == cs_spawned && !cl->spectator)
					NQ_ForClient (cl, svc, s);
		return;
	}

	switch (dest)
	{
	case MSG_ONE:
		NQ_SendOne (s->one, s, count);
		break;
	case MSG_ALL:
		NQ_SendAll (s, count, &sv.reliable_datagram);
		break;
	case MSG_INIT:
		NQ_SendAll (s, count, &sv.signon);
		break;
	case MSG_BROADCAST:
		if (svc != svc_temp_entity)
		{
			NQ_SendAll (s, count, &sv.datagram);
			break;
		}
		to = NQ_TempEntityMulticast (s, origin);
		if (NQ_Translate (s, count, &sv.multicast))
			SV_Multicast (origin, to);
		break;
	}
}

static void NQ_Warn (const char *what, int number)
{
	if (nq_warned[number / 8] & (1 << (number % 8)))
		return;
	nq_warned[number / 8] |= (byte)(1 << (number % 8));
	Con_Printf ("%s sent %s %i, which QuakeWorld's protocol hasn't: dropped\n", pr.name, what, number);
}

// sends the stream's whole messages, and drops it past an unknown one
static void NQ_Process (int dest, nqstream_t *s)
{
	int		count;

	while ((count = NQ_MessageLength (s)) > 0 && count <= s->count)
	{
		NQ_Send (dest, s, count);
		s->count -= count;
		memmove (s->values, s->values + count, (size_t)s->count * sizeof(s->values[0]));
		if (!s->count)
			s->stringbytes = 0;
	}
	if (count < 0)
	{
		if (NQ_Int (&s->values[0]) == svc_temp_entity)
			NQ_Warn ("temp entity", NQ_Int (&s->values[1]) & 255);
		else
			NQ_Warn ("message", NQ_Int (&s->values[0]) & 255);
		s->count = s->stringbytes = 0;
		s->dropping = true;
	}
}

/*
=================
SV_NQWrite

One of a NetQuake progs' writes, to dest (MSG_BROADCAST, MSG_ONE to one,
MSG_ALL or MSG_INIT): a number of a kind, or a string
=================
*/
void SV_NQWrite (int dest, client_t *one, nqwrite_t kind, float value, const char *string)
{
	nqstream_t	*s = &nq_streams[dest];
	nqvalue_t	*v;
	size_t		len;

	if (s->dropping)
		return;
	if (s->count && dest == MSG_ONE && one != s->one)
	{	// msg_entity changed in the middle of a message
		Con_DPrintf ("%s: an unfinished message (%i) to another client dropped\n", pr.name,
			NQ_Int (&s->values[0]));
		s->count = s->stringbytes = 0;
	}
	if (s->count == NQ_MAXVALUES)
	{
		s->count = s->stringbytes = 0;
		s->dropping = true;
		return;
	}
	if (!s->count)
		s->one = one;

	v = &s->values[s->count];
	*v = (nqvalue_t){.kind = kind, .value = value};
	if (kind == NQW_STRING)
	{
		len = strlen (string) + 1;
		if (len > sizeof(s->strings) - (size_t)s->stringbytes)
		{
			s->count = s->stringbytes = 0;
			s->dropping = true;
			return;
		}
		memcpy (s->strings + s->stringbytes, string, len);
		v->string = s->stringbytes;
		s->stringbytes += (int)len;
	}
	s->count++;
	NQ_Process (dest, s);
}

/*
=================
SV_NQEndFrame

Before the frame's messages go out: an unfinished message is dropped, writes
are taken again where an unknown message dropped them, and NetQuake's muzzle
flashes (EF_MUZZLEFLASH, for a frame) go as QuakeWorld's svc_muzzleflash
=================
*/
void SV_NQEndFrame (void)
{
	edict_t	*ent;
	int		i;

	for (i = 1 ; i < sv.num_edicts ; i++)
	{
		ent = EDICT_NUM (i);
		if (ent->free || !((int)ent->v.effects & EF_MUZZLEFLASH))
			continue;
		ent->v.effects = (float)((int)ent->v.effects & ~EF_MUZZLEFLASH);
		MSG_WriteByte (&sv.multicast, svc_muzzleflash);
		MSG_WriteShort (&sv.multicast, i);
		SV_Multicast (ent->v.origin, MULTICAST_PVS);
	}

	for (i = 0 ; i <= MSG_INIT ; i++)
	{
		if (nq_streams[i].count)
			Con_DPrintf ("%s: an unfinished message (%i) dropped\n", pr.name, NQ_Int (&nq_streams[i].values[0]));
		nq_streams[i].count = nq_streams[i].stringbytes = 0;
		nq_streams[i].dropping = false;
	}
}

/*
=================
SV_NQNewLevel

A level's start: nothing held, nothing warned of yet
=================
*/
void SV_NQNewLevel (void)
{
	memset (nq_streams, 0, sizeof(nq_streams));
	memset (nq_warned, 0, sizeof(nq_warned));
}
