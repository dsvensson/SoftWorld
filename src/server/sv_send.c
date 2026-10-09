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
// sv_main.c -- server main program

#include "sv_local.h"

#define CHAN_AUTO   0
#define CHAN_WEAPON 1
#define CHAN_VOICE  2
#define CHAN_ITEM   3
#define CHAN_BODY   4

/*
=============================================================================

Con_Printf redirection

=============================================================================
*/

static char	outputbuf[8000];


extern cvar_t sv_phs;

/*
==================
SV_FlushRedirect
==================
*/
static void SV_FlushRedirect (void)
{
	char	send[8000+6];

	if (svs.redirected == RD_PACKET)
	{
		send[0] = 0xff;
		send[1] = 0xff;
		send[2] = 0xff;
		send[3] = 0xff;
		send[4] = A2C_PRINT;
		memcpy (send+5, outputbuf, strlen(outputbuf)+1);

		NET_SendPacket (NS_SERVER, (int)strlen(send)+1, send, svs.net_from);
	}
	else if (svs.redirected == RD_CLIENT && outputbuf[0])
	{	// a command that printed nothing sends nothing (nextdl comes by the dozen)
		ClientReliableWrite_Begin (host_client, svc_print, (int)strlen(outputbuf)+3);
		ClientReliableWrite_Byte (host_client, PRINT_HIGH);
		ClientReliableWrite_String (host_client, outputbuf);
	}

	// clear it
	outputbuf[0] = 0;
}


/*
==================
SV_BeginRedirect

  Send Con_Printf data to the remote client
  instead of the console
==================
*/
static void SV_RedirectPrint (const char *msg);

void SV_BeginRedirect (redirect_t rd)
{
	svs.redirected = rd;
	outputbuf[0] = 0;
	Con_SetPrintRedirect (SV_RedirectPrint);
}

void SV_EndRedirect (void)
{
	Con_SetPrintRedirect (NULL);
	SV_FlushRedirect ();
	svs.redirected = RD_NONE;
}


/*
================
SV_RedirectPrint

Collects console output for the redirect target.
================
*/
static void SV_RedirectPrint (const char *msg)
{
	if (strlen (msg) + strlen(outputbuf) > sizeof(outputbuf) - 1)
		SV_FlushRedirect ();
	Q_strncatz (outputbuf, msg, sizeof(outputbuf));
}

/*
================
SV_LogPrint

Copies console output to the server log file.
================
*/
void SV_LogPrint (const char *msg)
{
	if (svs.logfile)
	{
		fputs (msg, svs.logfile);
		fflush (svs.logfile);
	}
}

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

void SV_PrintToClient(client_t *cl, int level, const char *string)
{
	ClientReliableWrite_Begin (cl, svc_print, (int)strlen(string)+3);
	ClientReliableWrite_Byte (cl, level);
	ClientReliableWrite_String (cl, string);
}


/*
=================
SV_ClientPrintf

Sends text across to be displayed if the level passes
=================
*/
void SV_ClientPrintf (client_t *cl, int level, char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	
	if (level < cl->messagelevel)
		return;
	
	va_start (argptr,fmt);
	vsnprintf (string, sizeof(string), fmt,argptr);
	va_end (argptr);

	SV_PrintToClient(cl, level, string);
	SV_MVDPrint (cl, level, string);
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients
=================
*/
void SV_BroadcastPrintf (int level, char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	client_t	*cl;
	int			i;

	va_start (argptr,fmt);
	vsnprintf (string, sizeof(string), fmt,argptr);
	va_end (argptr);
	
	Sys_Printf ("%s", string);	// print to the console
	SV_MVDPrint (NULL, level, string);

	for (i=0, cl = svs.clients ; i<MAX_CLIENTS ; i++, cl++)
	{
		if (level < cl->messagelevel)
			continue;
		if (!cl->state)
			continue;

		SV_PrintToClient(cl, level, string);
	}
}

/*
=================
SV_BroadcastCommand

Sends text to all active clients
=================
*/
void SV_BroadcastCommand (char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	
	if (!sv.state)
		return;
	va_start (argptr,fmt);
	vsnprintf (string, sizeof(string), fmt,argptr);
	va_end (argptr);

	MSG_WriteByte (&sv.reliable_datagram, svc_stufftext);
	MSG_WriteString (&sv.reliable_datagram, string);
}


/*
=================
SV_Multicast

Sends the contents of sv.multicast to a subset of the clients,
then clears sv.multicast.

MULTICAST_ALL	same as broadcast
MULTICAST_PVS	send to clients potentially visible from org
MULTICAST_PHS	send to clients potentially hearable from org
=================
*/
void SV_Multicast (vec3_t origin, int to)
{
	SV_MulticastProtExt (origin, to, 0, 0, 0);
}

void SV_MulticastExt (vec3_t origin, int to, unsigned fteext2)
{
	SV_MulticastProtExt (origin, to, 0, 0, fteext2);
}

void SV_MulticastProtExt (vec3_t origin, int to, unsigned with, unsigned without, unsigned with2)
{
	client_t	*client;
	byte		*mask;
	int			leafnum;
	int			j;
	bool	reliable;

	leafnum = CM_Leafnum (sv.map, CM_PointInLeaf (sv.map, origin));

	reliable = false;

	switch (to)
	{
	case MULTICAST_ALL_R:
		reliable = true;	// intentional fallthrough
	case MULTICAST_ALL:
		mask = SV_LeafPVS (0);		// leaf 0 is everything;
		break;

	case MULTICAST_PHS_R:
		reliable = true;	// intentional fallthrough
	case MULTICAST_PHS:
		mask = SV_LeafPHS (leafnum);
		break;

	case MULTICAST_PVS_R:
		reliable = true;	// intentional fallthrough
	case MULTICAST_PVS:
		mask = SV_LeafPVS (leafnum);
		break;

	default:
		mask = NULL;
		SV_Error ("SV_Multicast: bad to:%i", to);
	}

	// send the data to all relevent clients
	for (j = 0, client = svs.clients; j < MAX_CLIENTS; j++, client++)
	{
		if (client->state != cs_spawned || (client->fteext & with) != with || (client->fteext & without)
			|| (client->fteext2 & with2) != with2)
			continue;

		if (to == MULTICAST_PHS_R || to == MULTICAST_PHS) {
			vec3_t delta;
			VectorSubtract(origin, client->edict->v.origin, delta);
			if (Length(delta) <= 1024)
				goto inrange;
		}

		// -1 is because pvs rows are 1 based, not 0 based like leafs
		leafnum = CM_Leafnum (sv.map, CM_PointInLeaf (sv.map, client->edict->v.origin)) - 1;
		if (leafnum >= 0 && leafnum < sv.vis_rowbytes * 8 && !(mask[leafnum>>3] & (1<<(leafnum&7))))
			continue;

inrange:
		if (reliable) {
			ClientReliableCheckBlock(client, sv.multicast.cursize);
			ClientReliableWrite_SZ(client, sv.multicast.data, sv.multicast.cursize);
		} else
			SZ_Write (&client->datagram, sv.multicast.data, sv.multicast.cursize);
	}

	// QTV hears and sees it all, as mvdsv's demos do, in what its viewers'
	// protocol has
	if (!with2 && (MVD_FTE_EXTENSIONS & with) == with && !(MVD_FTE_EXTENSIONS & without))
		SV_MVDAll (sv.multicast.data, sv.multicast.cursize);
	SZ_Clear (&sv.multicast);
}


// FTE's svc_fte_soundextended: its fields, then the entity and channel, the
// sound and the place
static void SV_StartExtendedSound (int ent, int channel, int sound_num, int volume, float attenuation,
	vec3_t origin, int to)
{
	int		fields = 0, i;

	if (volume != DEFAULT_SOUND_PACKET_VOLUME)
		fields |= NQSND_VOLUME;
	if (attenuation != DEFAULT_SOUND_PACKET_ATTENUATION)
		fields |= NQSND_ATTENUATION;
	if (ent >= 8192)
		fields |= NQSND_LARGEENTITY;
	if (sound_num > 255)
		fields |= NQSND_LARGESOUND;

	MSG_WriteByte (&sv.multicast, svc_fte_soundextended);
	MSG_WriteByte (&sv.multicast, fields);
	if (fields & NQSND_VOLUME)
		MSG_WriteByte (&sv.multicast, volume);
	if (fields & NQSND_ATTENUATION)
		MSG_WriteByte (&sv.multicast, (int)(attenuation*64));
	if (fields & NQSND_LARGEENTITY)
	{
		MSG_WriteBigEntity (&sv.multicast, ent);
		MSG_WriteByte (&sv.multicast, channel);
	}
	else
		MSG_WriteShort (&sv.multicast, (ent<<3) | channel);
	if (fields & NQSND_LARGESOUND)
		MSG_WriteShort (&sv.multicast, sound_num);
	else
		MSG_WriteByte (&sv.multicast, sound_num);
	for (i=0 ; i<3 ; i++)
		MSG_WriteCoord (&sv.multicast, origin[i]);
	SV_MulticastExt (origin, to, FTE_PEXT2_REPLACEMENTDELTAS);
}

/*  
==================
SV_StartSound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
allready running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.  (max 4 attenuation)

==================
*/  
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume,
    float attenuation)
{       
    int         sound_num;
    int			i;
	int			ent;
	vec3_t		origin;
	bool	use_phs;
	bool	reliable = false;

	if (volume < 0 || volume > 255)
		SV_Error ("SV_StartSound: volume = %i", volume);

	if (attenuation < 0 || attenuation > 4)
		SV_Error ("SV_StartSound: attenuation = %f", attenuation);

	if (channel < 0 || channel > 15)
		SV_Error ("SV_StartSound: channel = %i", channel);

// find precache number for sound
    for (sound_num=1 ; sound_num<MAX_SOUNDS
        && sv.sound_precache[sound_num] ; sound_num++)
        if (!strcmp(sample, sv.sound_precache[sound_num]))
            break;
    
    if ( sound_num == MAX_SOUNDS || !sv.sound_precache[sound_num] )
    {
        Con_Printf ("SV_StartSound: %s not precacheed\n", sample);
        return;
    }
    
	ent = NUM_FOR_EDICT(entity);

	// use the entity origin unless it is a bmodel
	if (entity->v.solid == SOLID_BSP)
	{
		for (i=0 ; i<3 ; i++)
			origin[i] = entity->v.origin[i]+0.5f*(entity->v.mins[i]+entity->v.maxs[i]);
	}
	else
	{
		VectorCopy (entity->v.origin, origin);
	}

	if ((channel & 8) || !sv_phs.value)	// no PHS flag
	{
		if (channel & 8)
			reliable = true; // sounds that break the phs are reliable
		use_phs = false;
		channel &= 7;
	}
	else
		use_phs = true;

//	if (channel == CHAN_BODY || channel == CHAN_VOICE)
//		reliable = true;

	// past what svc_sound holds (sound 255, entity 1023), FTE's extended sound,
	// to the clients with replacement deltas, as FTE's server sends it
	if (sound_num >= MAX_QW_SOUNDS || ent >= 1024)
	{
		SV_StartExtendedSound (ent, channel, sound_num, volume, attenuation, origin,
			use_phs ? (reliable ? MULTICAST_PHS_R : MULTICAST_PHS) : (reliable ? MULTICAST_ALL_R : MULTICAST_ALL));
		return;
	}

	channel = (ent<<3) | channel;

	if (volume != DEFAULT_SOUND_PACKET_VOLUME)
		channel |= SND_VOLUME;
	if (attenuation != DEFAULT_SOUND_PACKET_ATTENUATION)
		channel |= SND_ATTENUATION;

	MSG_WriteByte (&sv.multicast, svc_sound);
	MSG_WriteShort (&sv.multicast, channel);
	if (channel & SND_VOLUME)
		MSG_WriteByte (&sv.multicast, volume);
	if (channel & SND_ATTENUATION)
		MSG_WriteByte (&sv.multicast, (int)(attenuation*64));
	MSG_WriteByte (&sv.multicast, sound_num);
	for (i=0 ; i<3 ; i++)
		MSG_WriteCoord (&sv.multicast, origin[i]);

	if (use_phs)
		SV_Multicast (origin, reliable ? MULTICAST_PHS_R : MULTICAST_PHS);
	else
		SV_Multicast (origin, reliable ? MULTICAST_ALL_R : MULTICAST_ALL);
}           


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/


void SV_FindModelNumbers (void)
{
	int		i;

	sv.nailmodel = -1;
	sv.supernailmodel = -1;
	sv.playermodel = -1;

	for (i=0 ; i<MAX_MODELS ; i++)
	{
		if (!sv.model_precache[i])
			break;
		if (!strcmp(sv.model_precache[i],"progs/spike.mdl"))
			sv.nailmodel = i;
		if (!strcmp(sv.model_precache[i],"progs/s_spike.mdl"))
			sv.supernailmodel = i;
		if (!strcmp(sv.model_precache[i],"progs/player.mdl"))
			sv.playermodel = i;
	}
}


/*
==================
SV_SendFixangle

The view the progs turned the player to (fixangle): in msg, the datagram,
which the high-lag teleport's turned moves count from; reliable with msg NULL,
the spawn's, as FTE sends that. In a datagram it is lost with a dropped packet,
or when the reliable message fills the packet, as a big level's spawn has it.
==================
*/
void SV_SendFixangle (client_t *client, sizebuf_t *msg)
{
	edict_t		*ent = client->edict;
	byte		data[16];
	bool		reliable = !msg;
	sizebuf_t	fix = {.data = data, .maxsize = sizeof(data),
		.floatcoords = reliable ? client->netchan.message.floatcoords : msg->floatcoords};
	sizebuf_t	*mvdmsg;
	int			i;

	if (client->mvdext1 & MVD_PEXT1_HIGHLAGTELEPORT)
		MSG_WriteByte (&fix, SV_NoteFixangle (client));
	for (i=0 ; i < 3 ; i++)
		MSG_WriteAngle (&fix, ent->v.angles[i] );
	if (reliable)
	{
		ClientReliableWrite_Begin (client, svc_setangle, 1 + fix.cursize);
		ClientReliableWrite_SZ (client, fix.data, fix.cursize);
	}
	else
	{
		MSG_WriteByte (msg, svc_setangle);
		SZ_Write (msg, fix.data, fix.cursize);
	}
	if (sv_mvd && !client->spectator)
	{	// QTV's names the player
		mvdmsg = SV_MVDMessage ();
		MSG_WriteByte (mvdmsg, svc_setangle);
		MSG_WriteByte (mvdmsg, (int)(client - svs.clients));
		for (i=0 ; i < 3 ; i++)
			MSG_WriteAngle (mvdmsg, ent->v.angles[i]);
		SV_MVDAll (mvdmsg->data, mvdmsg->cursize);
	}
	ent->v.fixangle = 0;
}

/*
==================
SV_WriteClientdataToMessage

==================
*/
static void SV_WriteClientdataToMessage (client_t *client, sizebuf_t *msg)
{
	int		i, start;
	edict_t	*other;
	edict_t	*ent;

	ent = client->edict;

	// send the chokecount for r_netgraph
	if (client->chokecount)
	{
		MSG_WriteByte (msg, svc_chokecount);
		MSG_WriteByte (msg, client->chokecount);
		client->chokecount = 0;
	}

	// send a damage message if the player got hit this frame
	if (ent->v.dmg_take || ent->v.dmg_save)
	{
		other = PROG_TO_EDICT(ent->v.dmg_inflictor);
		start = msg->cursize;
		MSG_WriteByte (msg, svc_damage);
		MSG_WriteByte (msg, (int)ent->v.dmg_save);
		MSG_WriteByte (msg, (int)ent->v.dmg_take);
		for (i=0 ; i<3 ; i++)
			MSG_WriteCoord (msg, other->v.origin[i] + 0.5f*(other->v.mins[i] + other->v.maxs[i]));
		SV_MVDSingle (client, msg->data + start, msg->cursize - start);
	
		ent->v.dmg_take = 0;
		ent->v.dmg_save = 0;
	}

	if (ent->v.fixangle)
		SV_SendFixangle (client, msg);

	// NetQuake's progs kick the view with punchangle, which QuakeWorld's
	// clients take as kicks that they drop themselves, as FTE's server sends it
	if (pr.nq)
	{
		while (ent->v.punchangle[0] < -3)
		{
			ent->v.punchangle[0] += 4;
			MSG_WriteByte (msg, svc_bigkick);
		}
		while (ent->v.punchangle[0] < -1)
		{
			ent->v.punchangle[0] += 2;
			MSG_WriteByte (msg, svc_smallkick);
		}
		ent->v.punchangle[1] = ent->v.punchangle[2] = 0;
	}

	// the server's time: in every datagram with FTE_PEXT_ACCURATETIMINGS,
	// every few seconds with Z_EXT_SERVERTIME, which only keeps a clock near
	if ((client->fteext & FTE_PEXT_ACCURATETIMINGS) ||
		((client->z_ext & Z_EXT_SERVERTIME) && host.realtime - client->lastservertime >= 5))
	{
		MSG_WriteByte (msg, svc_updatestatlong);
		MSG_WriteByte (msg, STAT_TIME);
		MSG_WriteLong (msg, (int)(sv.time * 1000));
		client->lastservertime = host.realtime;
	}
}

/*
=======================
SV_ClientStats
=======================
*/
void SV_ClientStats (const client_t *client, int stats[MAX_STATS])
{
	edict_t	*ent;

	ent = client->edict;
	memset (stats, 0, sizeof(stats[0]) * MAX_STATS);

	// if we are a spectator and we are tracking a player, we get his stats
	// so our status bar reflects his
	if (client->spectator && client->spec_track > 0)
		ent = svs.clients[client->spec_track - 1].edict;

	stats[STAT_HEALTH] = (int)ent->v.health;
	stats[STAT_WEAPON] = SV_ModelIndex(PR_GetString(ent->v.weaponmodel));
	stats[STAT_AMMO] = (int)ent->v.currentammo;
	stats[STAT_ARMOR] = (int)ent->v.armorvalue;
	stats[STAT_SHELLS] = (int)ent->v.ammo_shells;
	stats[STAT_NAILS] = (int)ent->v.ammo_nails;
	stats[STAT_ROCKETS] = (int)ent->v.ammo_rockets;
	stats[STAT_CELLS] = (int)ent->v.ammo_cells;
	if (!client->spectator)
		stats[STAT_ACTIVEWEAPON] = (int)ent->v.weapon;
	// stuff the sigil bits into the high bits of items for sbar
	stats[STAT_ITEMS] = (int)ent->v.items | ((int)PR_GLOBAL(serverflags) << 28);
	if (client->z_ext & Z_EXT_VIEWHEIGHT)
		stats[STAT_VIEWHEIGHT] = (int)ent->v.view_ofs[2];
}

/*
=======================
SV_UpdateClientStats

Performs a delta update of the stats array.  This should only be performed
when a reliable message can be delivered this frame.
=======================
*/
// an integer stat's change, as id sends it
static void SV_WriteStat (client_t *client, int i, int value)
{
	if (value >=0 && value <= 255)
	{
		ClientReliableWrite_Begin(client, svc_updatestat, 3);
		ClientReliableWrite_Byte(client, i);
		ClientReliableWrite_Byte(client, value);
	}
	else
	{
		ClientReliableWrite_Begin(client, svc_updatestatlong, 6);
		ClientReliableWrite_Byte(client, i);
		ClientReliableWrite_Long(client, value);
	}
}

// the word QuakeC's stat i is for a client
static const int *SV_QCStat (const client_t *client, int i)
{
	if (sv.qcstats[i].global)
		return (const int *)&pr.globals[sv.qcstats[i].ofs];
	return (const int *)&client->edict->v + sv.qcstats[i].ofs;
}

static void SV_UpdateClientStats (client_t *client)
{
	int			stats[MAX_STATS];
	int			i;
	const int	*v;

	SV_ClientStats (client, stats);
	for (i=0 ; i<MAX_STATS ; i++)
		if (stats[i] != client->stats[i])
		{
			client->stats[i] = stats[i];
			SV_WriteStat (client, i, stats[i]);
		}

	// QuakeC's (FTE's clientstat and globalstat), which only CSQC reads
	if (!(client->fteext & FTE_PEXT_CSQC))
		return;
	for (i=MAX_STATS ; i<MAX_CL_STATS ; i++)
	{
		if (!sv.qcstats[i].type || *(v = SV_QCStat (client, i)) == client->stats[i])
			continue;
		client->stats[i] = *v;
		if (sv.qcstats[i].type == ev_float)
		{
			ClientReliableWrite_Begin(client, svc_fte_updatestatfloat, 6);
			ClientReliableWrite_Byte(client, i);
			ClientReliableWrite_Float(client, *(const float *)v);
		}
		else
			SV_WriteStat (client, i, *v);
	}
}

/*
=======================
SV_SendClientDatagram
=======================
*/
static bool SV_SendClientDatagram (client_t *client)
{
	byte		buf[MAX_DATAGRAM];
	sizebuf_t	msg;

	msg.data = buf;
	msg.maxsize = sizeof(buf);
	msg.cursize = 0;
	msg.allowoverflow = true;
	msg.overflowed = false;
	msg.floatcoords = client->datagram.floatcoords;

	// add the client specific data to the datagram
	SV_WriteClientdataToMessage (client, &msg);

	// send over all the objects that are in the PVS
	// this will include clients, a packetentities, and
	// possibly a nails update
	SV_WriteEntitiesToClient (client, &msg);

	// copy the accumulated multicast datagram
	// for this client out to the message
	if (client->datagram.overflowed)
		Con_Printf ("WARNING: datagram overflowed for %s\n", client->name);
	else
		SZ_Write (&msg, client->datagram.data, client->datagram.cursize);
	SZ_Clear (&client->datagram);
	SV_DownloadDatagram (client, &msg);

	// send deltas over reliable stream
	if (Netchan_CanReliable (&client->netchan))
		SV_UpdateClientStats (client);

	if (msg.overflowed)
	{
		Con_Printf ("WARNING: msg overflowed for %s\n", client->name);
		SZ_Clear (&msg);
		SV_DeltasUnsent (client);
	}

	// send the datagram
	Netchan_Transmit (&client->netchan, msg.cursize, buf);

	return true;
}

/*
=======================
SV_FlushBroadcasts

The broadcasts to the clients and QTV: each frame, and when another
message wouldn't fit in them (FTE's). A level that is loading has no one to
send them to: its clients get it whole at its serverdata, and what piled up
for them before is dropped then (SV_New_f).
=======================
*/
void SV_FlushBroadcasts (void)
{
	client_t	*client;
	int			j;

	if (sv.state == ss_loading)
	{
		SZ_Clear (&sv.reliable_datagram);
		SZ_Clear (&sv.datagram);
		return;
	}

	// append the broadcast messages to each client messages
	for (j=0, client = svs.clients ; j<MAX_CLIENTS ; j++, client++)
	{
		if (client->state < cs_connected)
			continue;	// reliables go to all connected or spawned

		ClientReliableCheckBlock(client, sv.reliable_datagram.cursize);
		ClientReliableWrite_SZ(client, sv.reliable_datagram.data, sv.reliable_datagram.cursize);

		if (client->state != cs_spawned)
			continue;	// datagrams only go to spawned
		SZ_Write (&client->datagram
			, sv.datagram.data
			, sv.datagram.cursize);
	}

	// and to QTV
	SV_MVDAll (sv.reliable_datagram.data, sv.reliable_datagram.cursize);
	SV_MVDAll (sv.datagram.data, sv.datagram.cursize);
	SZ_Clear (&sv.reliable_datagram);
	SZ_Clear (&sv.datagram);
}

/*
=======================
SV_UpdateToReliableMessages
=======================
*/
static void SV_UpdateToReliableMessages (void)
{
	int			i, j;
	client_t *client;
	edict_t *ent;
	sizebuf_t	*msg;

// check for changes to be sent over the reliable streams to all clients
	for (i=0, host_client = svs.clients ; i<MAX_CLIENTS ; i++, host_client++)
	{
		if (host_client->state != cs_spawned)
			continue;
		if (host_client->sendinfo)
		{
			host_client->sendinfo = false;
			SV_FullClientUpdate (host_client, &sv.reliable_datagram);
		}
		if (host_client->old_frags != host_client->edict->v.frags)
		{
			for (j=0, client = svs.clients ; j<MAX_CLIENTS ; j++, client++)
			{
				if (client->state < cs_connected)
					continue;
				ClientReliableWrite_Begin(client, svc_updatefrags, 4);
				ClientReliableWrite_Byte(client, i);
				ClientReliableWrite_Short(client, (int)host_client->edict->v.frags);
			}

			host_client->old_frags = (int)host_client->edict->v.frags;
			if (sv_mvd)
			{
				msg = SV_MVDMessage ();
				MSG_WriteByte (msg, svc_updatefrags);
				MSG_WriteByte (msg, i);
				MSG_WriteShort (msg, host_client->old_frags);
				SV_MVDAll (msg->data, msg->cursize);
			}
		}

		// maxspeed/entgravity changes
		ent = host_client->edict;

		if (pr.fofs_gravity && host_client->entgravity != E_FLOAT(ent, pr.fofs_gravity)) {
			host_client->entgravity = E_FLOAT(ent, pr.fofs_gravity);
			ClientReliableWrite_Begin(host_client, svc_entgravity, 5);
			ClientReliableWrite_Float(host_client, host_client->entgravity);
			if (sv_mvd)
			{
				msg = SV_MVDMessage ();
				MSG_WriteByte (msg, svc_entgravity);
				MSG_WriteFloat (msg, host_client->entgravity);
				SV_MVDSingle (host_client, msg->data, msg->cursize);
			}
		}
		if (pr.fofs_maxspeed && host_client->maxspeed != E_FLOAT(ent, pr.fofs_maxspeed)) {
			host_client->maxspeed = E_FLOAT(ent, pr.fofs_maxspeed);
			ClientReliableWrite_Begin(host_client, svc_maxspeed, 5);
			ClientReliableWrite_Float(host_client, host_client->maxspeed);
			if (sv_mvd)
			{
				msg = SV_MVDMessage ();
				MSG_WriteByte (msg, svc_maxspeed);
				MSG_WriteFloat (msg, host_client->maxspeed);
				SV_MVDSingle (host_client, msg->data, msg->cursize);
			}
		}

	}

	if (sv.datagram.overflowed)
		SZ_Clear (&sv.datagram);

	SV_FlushBroadcasts ();
}




/*
=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages (void)
{
	int			i, j;
	client_t	*c;

// NetQuake's progs' messages of the frame, all out
	if (pr.nq && sv.state == ss_active)
		SV_NQEndFrame ();

// update frags, names, etc
	SV_UpdateToReliableMessages ();

// build individual updates
	for (i=0, c = svs.clients ; i<MAX_CLIENTS ; i++, c++)
	{
		if (!c->state)
			continue;

		if (c->drop) {
			SV_DropClient(c);
			c->drop = false;
			continue;
		}

		// check to see if we have a backbuf to stick in the reliable
		if (c->num_backbuf) {
			// will it fit?
			if (c->netchan.message.cursize + c->backbuf_size[0] <
				c->netchan.message.maxsize) {

				Con_DPrintf("%s: backbuf %d bytes\n",
					c->name, c->backbuf_size[0]);

				// it'll fit
				SZ_Write(&c->netchan.message, c->backbuf_data[0],
					c->backbuf_size[0]);
				
				//move along, move along
				for (j = 1; j < c->num_backbuf; j++) {
					memcpy(c->backbuf_data[j - 1], c->backbuf_data[j],
						c->backbuf_size[j]);
					c->backbuf_size[j - 1] = c->backbuf_size[j];
				}

				c->num_backbuf--;
				if (c->num_backbuf) {
					memset(&c->backbuf, 0, sizeof(c->backbuf));
					c->backbuf.floatcoords = c->netchan.message.floatcoords;
					c->backbuf.data = c->backbuf_data[c->num_backbuf - 1];
					c->backbuf.cursize = c->backbuf_size[c->num_backbuf - 1];
					c->backbuf.maxsize = sizeof(c->backbuf_data[c->num_backbuf - 1]);
				}
			}
		}

		// if the reliable message overflowed,
		// drop the client
		if (c->netchan.message.overflowed)
		{
			SZ_Clear (&c->netchan.message);
			SZ_Clear (&c->datagram);
			SV_BroadcastPrintf (PRINT_HIGH, "%s overflowed\n", c->name);
			Con_Printf ("WARNING: reliable overflow for %s\n",c->name);
			SV_DropClient (c);
			c->send_message = true;
			c->netchan.cleartime = 0;	// don't choke this message
		}

		// only send messages if the client has sent one
		// and the bandwidth is not choked
		if (!c->send_message)
			continue;
		c->send_message = false;	// try putting this after choke?
		SV_SetChannelRate (c);
		if (!sv.paused && !Netchan_CanPacket (&c->netchan))
		{
			c->chokecount++;
			continue;		// bandwidth choke
		}

		if (c->state == cs_spawned)
			SV_SendClientDatagram (c);
		else
		{	// the reliable, and a chunk for a download during the signon
			byte		buf[MAX_DATAGRAM];
			sizebuf_t	msg = {.data = buf, .maxsize = sizeof(buf)};

			SV_DownloadDatagram (c, &msg);
			Netchan_Transmit (&c->netchan, msg.cursize, buf);
		}

		// don't let rate limiting build up while the game is paused
		if (sv.paused)
			c->netchan.cleartime = host.realtime;
	}

	// and the frame to QTV's viewers, when one is due
	SV_MVDFrame ();
}




/*
=======================
SV_SendMessagesToAll

FIXME: does this sequence right?
=======================
*/
void SV_SendMessagesToAll (void)
{
	int			i;
	client_t	*c;

	for (i=0, c = svs.clients ; i<MAX_CLIENTS ; i++, c++)
		if (c->state)		// FIXME: should this only send to active?
			c->send_message = true;
	
	SV_SendClientMessages ();
}

