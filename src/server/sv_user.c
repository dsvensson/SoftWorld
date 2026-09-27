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
// sv_user.c -- server code for moving users

#include "sv_local.h"

edict_t	*sv_player;

static usercmd_t	cmd;

static cvar_t	sv_spectalk = {.name = "sv_spectalk", .string = "1"};

static cvar_t	sv_mapcheck	= {.name = "sv_mapcheck", .string = "1"};


extern cvar_t pausable;

/*
============================================================

USER STRINGCMD EXECUTION

host_client and sv_player will be valid.
============================================================
*/

/*
================
SV_New_f

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each server load.
================
*/
void SV_New_f (void)
{
	char		*gamedir;
	int			playernum;
	unsigned	fteext;

	if (host_client->state == cs_spawned)
		return;

	host_client->state = cs_connected;
	host_client->connection_started = host.realtime;

	if (sv.bigcoords && !(host_client->fteext & FTE_PEXT_FLOATCOORDS))
	{
		SV_ClientPrintf (host_client, PRINT_HIGH, "%s", SV_BIGCOORDS_REFUSAL);
		SV_DropClient (host_client);
		return;
	}

	// what the client's protocol has no room for, it won't see
	if ((sv.num_edicts > 512 && !(host_client->fteext & FTE_PEXT_ENTITYDBL)) ||
		(sv.num_edicts > 1024 && !(host_client->fteext & FTE_PEXT_ENTITYDBL2)) ||
		(sv.model_precache[256] && !(host_client->fteext & FTE_PEXT_MODELDBL)))
		SV_ClientPrintf (host_client, PRINT_HIGH, "This map has more entities or models than your client's\n"
			"protocol has room for: some will be invisible to you.\n");

	// send the info about the new client to all connected clients
//	SV_FullClientUpdate (host_client, &sv.reliable_datagram);
//	host_client->sendinfo = true;

	gamedir = Info_ValueForKey (svs.info, "*gamedir");
	if (!gamedir[0])
		gamedir = "qw";

//NOTE:  This doesn't go through ClientReliableWrite since it's before the user
//spawns.  These functions are written to not overflow
	if (host_client->num_backbuf) {
		Con_Printf("WARNING %s: [SV_New] Back buffered (%d0, clearing", host_client->name, host_client->netchan.message.cursize); 
		host_client->num_backbuf = 0;
		SZ_Clear(&host_client->netchan.message);
	}

	// send the serverdata, with the protocol extensions in use this level;
	// what follows is in this level's encoding
	MSG_WriteByte (&host_client->netchan.message, svc_serverdata);
	fteext = host_client->fteext;
	if (!sv.bigcoords)
		fteext &= ~FTE_PEXT_FLOATCOORDS;
	if (fteext)
	{
		MSG_WriteLong (&host_client->netchan.message, PROTOCOL_VERSION_FTE);
		MSG_WriteLong (&host_client->netchan.message, (int)fteext);
	}
	if (host_client->mvdext1)
	{
		MSG_WriteLong (&host_client->netchan.message, PROTOCOL_VERSION_MVD1);
		MSG_WriteLong (&host_client->netchan.message, (int)host_client->mvdext1);
	}
	MSG_WriteLong (&host_client->netchan.message, PROTOCOL_VERSION);
	host_client->netchan.message.floatcoords = sv.bigcoords;
	SZ_Clear (&host_client->datagram);		// the old level's, in its encoding
	host_client->datagram.floatcoords = sv.bigcoords;
	MSG_WriteLong (&host_client->netchan.message, svs.spawncount);
	MSG_WriteString (&host_client->netchan.message, gamedir);

	playernum = NUM_FOR_EDICT(host_client->edict)-1;
	if (host_client->spectator)
		playernum |= 128;
	MSG_WriteByte (&host_client->netchan.message, playernum);

	// send full levelname
	MSG_WriteString (&host_client->netchan.message, PR_GetString(sv.edicts->v.message));

	// send the movevars
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.gravity);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.stopspeed);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.maxspeed);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.spectatormaxspeed);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.accelerate);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.airaccelerate);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.wateraccelerate);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.friction);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.waterfriction);
	MSG_WriteFloat(&host_client->netchan.message, sv.movevars.entgravity);

	// send music
	MSG_WriteByte (&host_client->netchan.message, svc_cdtrack);
	MSG_WriteByte (&host_client->netchan.message, (int)sv.edicts->v.sounds);

	// send server info string
	MSG_WriteByte (&host_client->netchan.message, svc_stufftext);
	MSG_WriteString (&host_client->netchan.message, va("fullserverinfo \"%s\"\n", svs.info) );
}

/*
==================
SV_Soundlist_f
==================
*/
void SV_Soundlist_f (void)
{
	char		**s;
	int			n;

	if (host_client->state != cs_connected)
	{
		Con_Printf ("soundlist not valid -- allready spawned\n");
		return;
	}

	// handle the case of a level changing while a client was connecting
	if ( atoi(Cmd_Argv(1)) != svs.spawncount )
	{
		Con_Printf ("SV_Soundlist_f from different level\n");
		SV_New_f ();
		return;
	}

	n = atoi(Cmd_Argv(2));
	
//NOTE:  This doesn't go through ClientReliableWrite since it's before the user
//spawns.  These functions are written to not overflow
	if (host_client->num_backbuf) {
		Con_Printf("WARNING %s: [SV_Soundlist] Back buffered (%d0, clearing", host_client->name, host_client->netchan.message.cursize); 
		host_client->num_backbuf = 0;
		SZ_Clear(&host_client->netchan.message);
	}

	MSG_WriteByte (&host_client->netchan.message, svc_soundlist);
	MSG_WriteByte (&host_client->netchan.message, n);
	for (s = sv.sound_precache+1 + n ; 
		*s && host_client->netchan.message.cursize < (MAX_MSGLEN/2); 
		s++, n++)
		MSG_WriteString (&host_client->netchan.message, *s);

	MSG_WriteByte (&host_client->netchan.message, 0);

	// next msg
	if (*s)
		MSG_WriteByte (&host_client->netchan.message, n);
	else
		MSG_WriteByte (&host_client->netchan.message, 0);
}

/*
==================
SV_Modellist_f
==================
*/
void SV_Modellist_f (void)
{
	char		**s;
	int			n, i, max;

	if (host_client->state != cs_connected)
	{
		Con_Printf ("modellist not valid -- allready spawned\n");
		return;
	}
	
	// handle the case of a level changing while a client was connecting
	if ( atoi(Cmd_Argv(1)) != svs.spawncount )
	{
		Con_Printf ("SV_Modellist_f from different level\n");
		SV_New_f ();
		return;
	}

	n = atoi(Cmd_Argv(2));

//NOTE:  This doesn't go through ClientReliableWrite since it's before the user
//spawns.  These functions are written to not overflow
	if (host_client->num_backbuf) {
		Con_Printf("WARNING %s: [SV_Modellist] Back buffered (%d0, clearing", host_client->name, host_client->netchan.message.cursize); 
		host_client->num_backbuf = 0;
		SZ_Clear(&host_client->netchan.message);
	}

	// past 255 models the list goes on with FTE's short start, to clients
	// that can take the model numbers; the next number goes back as a byte,
	// so a message only ends where that byte isn't 0
	max = (host_client->fteext & FTE_PEXT_MODELDBL) ? MAX_MODELS : 256;
	if (n < 0 || n >= max - 1)
		n = 0;
	if (n > 255)
	{
		MSG_WriteByte (&host_client->netchan.message, svc_fte_modellistshort);
		MSG_WriteShort (&host_client->netchan.message, n);
	}
	else
	{
		MSG_WriteByte (&host_client->netchan.message, svc_modellist);
		MSG_WriteByte (&host_client->netchan.message, n);
	}
	for (s = sv.model_precache+1+n, i = n+1 ;
		i < max && *s && (!((i-1) & 255) || host_client->netchan.message.cursize < (MAX_MSGLEN/2));
		s++, i++)
		MSG_WriteString (&host_client->netchan.message, *s);
	MSG_WriteByte (&host_client->netchan.message, 0);

	// next msg
	if (i < max && *s)
		MSG_WriteByte (&host_client->netchan.message, (i-1) & 255);
	else
		MSG_WriteByte (&host_client->netchan.message, 0);
}

/*
==================
SV_WriteStatic
==================
*/
static void SV_WriteStatic (client_t *client, const entity_state_t *s)
{
	static const entity_state_t	nullstate = {0};
	sizebuf_t	*msg = &client->netchan.message;
	int			i;

	if (!SV_EntityFits (client, s->number, s->modelindex))
		return;
	if (client->fteext & FTE_PEXT_SPAWNSTATIC2)
	{
		MSG_WriteByte (msg, svc_fte_spawnstatic2);
		SV_WriteDelta (client, &nullstate, s, msg, true);
		return;
	}
	MSG_WriteByte (msg, svc_spawnstatic);
	MSG_WriteByte (msg, s->modelindex);
	MSG_WriteByte (msg, s->frame);
	MSG_WriteByte (msg, s->colormap);
	MSG_WriteByte (msg, s->skinnum);
	for (i=0 ; i<3 ; i++)
	{
		MSG_WriteCoord (msg, s->origin[i]);
		MSG_WriteAngle (msg, s->angles[i]);
	}
}

/*
==================
SV_WriteBaseline
==================
*/
static void SV_WriteBaseline (client_t *client, int entnum)
{
	static const entity_state_t	nullstate = {0};
	sizebuf_t		*msg = &client->netchan.message;
	edict_t			*ent;
	entity_state_t	base;
	int				i;

	ent = EDICT_NUM(entnum);
	if (!entnum || !ent->baseline.modelindex || !SV_EntityFits (client, entnum, 0))
		return;
	SV_ClientBaseline (client, ent, &base);
	if (client->fteext & FTE_PEXT_SPAWNSTATIC2)
	{
		MSG_WriteByte (msg, svc_fte_spawnbaseline2);
		SV_WriteDelta (client, &nullstate, &base, msg, true);
		return;
	}
	MSG_WriteByte (msg, svc_spawnbaseline);
	MSG_WriteShort (msg, entnum);
	MSG_WriteByte (msg, base.modelindex);
	MSG_WriteByte (msg, base.frame);
	MSG_WriteByte (msg, base.colormap);
	MSG_WriteByte (msg, base.skinnum);
	for (i=0 ; i<3 ; i++)
	{
		MSG_WriteCoord (msg, base.origin[i]);
		MSG_WriteAngle (msg, base.angles[i]);
	}
}

/*
==================
SV_PreSpawn_f

The level's static entities, then its entity baselines, then its signon
buffers, as many as fit in half a message at a time; the client asks for
the rest from the number it gets back
==================
*/
void SV_PreSpawn_f (void)
{
	unsigned	buf, total, size;
	unsigned	check;
	sizebuf_t	*msg;

	if (host_client->state != cs_connected)
	{
		Con_Printf ("prespawn not valid -- allready spawned\n");
		return;
	}

	// handle the case of a level changing while a client was connecting
	if ( atoi(Cmd_Argv(1)) != svs.spawncount )
	{
		Con_Printf ("SV_PreSpawn_f from different level\n");
		SV_New_f ();
		return;
	}

	total = (unsigned)(sv.num_static_entities + sv.num_baselines + sv.num_signon_buffers);
	buf = atoi(Cmd_Argv(2));
	if (buf >= total)
		buf = 0;

	if (!buf) {
		// should be three numbers following containing checksums
		check = atoi(Cmd_Argv(3));

//		Con_DPrintf("Client check = %d\n", check);

		if (sv_mapcheck.value && check != sv.map_checksum &&
			check != sv.map_checksum2) {
			SV_ClientPrintf (host_client, PRINT_HIGH,
				"Map model file does not match (%s), %i != %i/%i.\n"
				"You may need a new version of the map, or the proper install files.\n",
				sv.modelname, check, sv.map_checksum, sv.map_checksum2);
			SV_DropClient (host_client);
			return;
		}
		host_client->checksum = check;
	}

//NOTE:  This doesn't go through ClientReliableWrite since it's before the user
//spawns.  These functions are written to not overflow
	if (host_client->num_backbuf) {
		Con_Printf("WARNING %s: [SV_PreSpawn] Back buffered (%d0, clearing", host_client->name, host_client->netchan.message.cursize);
		host_client->num_backbuf = 0;
		SZ_Clear(&host_client->netchan.message);
	}

	msg = &host_client->netchan.message;
	for ( ; buf < total && msg->cursize < msg->maxsize / 2 ; buf++)
	{
		if (buf < (unsigned)sv.num_static_entities)
			SV_WriteStatic (host_client, &sv.static_entities[buf]);
		else if (buf < (unsigned)(sv.num_static_entities + sv.num_baselines))
			SV_WriteBaseline (host_client, (int)buf - sv.num_static_entities);
		else
		{
			// a signon buffer whole, into a message with room for it
			check = buf - sv.num_static_entities - sv.num_baselines;
			size = (unsigned)sv.signon_buffer_size[check];
			if (msg->cursize && msg->cursize + size > (unsigned)msg->maxsize - 64)
				break;
			SZ_Write (msg, sv.signon_buffers[check], size);
		}
	}

	if (buf == total)
	{	// all done prespawning
		MSG_WriteByte (msg, svc_stufftext);
		MSG_WriteString (msg, va("cmd spawn %i 0\n",svs.spawncount) );
	}
	else
	{	// need to prespawn more
		MSG_WriteByte (msg, svc_stufftext);
		MSG_WriteString (msg, va("cmd prespawn %i %i\n", svs.spawncount, buf) );
	}
}

/*
==================
SV_SetUpClientEdict

A client's edict, fresh for a player or a spectator
==================
*/
static void SV_SetUpClientEdict (client_t *cl)
{
	edict_t	*ent;
	eval_t	*val;

	ent = cl->edict;

	memset (&ent->v, 0, pr.progs->entityfields * 4);
	ent->alpha = 0;
	memset (ent->colormod, 0, sizeof(ent->colormod));
	ent->v.colormap = (float)NUM_FOR_EDICT(ent);
	ent->v.team = 0;	// FIXME
	ent->v.netname = PR_SetString(cl->name);

	cl->entgravity = 1.0;
	val = GetEdictFieldValue(ent, "gravity");
	if (val)
		val->_float = 1.0;
	cl->maxspeed = sv_maxspeed.value;
	val = GetEdictFieldValue(ent, "maxspeed");
	if (val)
		val->_float = sv_maxspeed.value;
}

/*
==================
SV_Spawn_f
==================
*/
void SV_Spawn_f (void)
{
	int		i;
	client_t	*client;
	int n;

	if (host_client->state != cs_connected)
	{
		Con_Printf ("Spawn not valid -- allready spawned\n");
		return;
	}

// handle the case of a level changing while a client was connecting
	if ( atoi(Cmd_Argv(1)) != svs.spawncount )
	{
		Con_Printf ("SV_Spawn_f from different level\n");
		SV_New_f ();
		return;
	}

	n = atoi(Cmd_Argv(2));

	// make sure n is valid
	if ( n < 0 || n > MAX_CLIENTS )
	{
		Con_Printf ("SV_Spawn_f invalid client start\n");
		SV_New_f ();
		return;
	}


	
// send all current names, colors, and frag counts
	// FIXME: is this a good thing?
	SZ_Clear (&host_client->netchan.message);

// send current status of all other players

	// normally this could overflow, but no need to check due to backbuf
	for (i=n, client = svs.clients + n ; i<MAX_CLIENTS ; i++, client++)
		SV_FullClientUpdateToClient (client, host_client);
	
// send all current light styles
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
	{
		ClientReliableWrite_Begin (host_client, svc_lightstyle, 
			(int)(3 + (sv.lightstyles[i] ? strlen(sv.lightstyles[i]) : 1)));
		ClientReliableWrite_Byte (host_client, (char)i);
		ClientReliableWrite_String (host_client, sv.lightstyles[i]);
	}

	SV_SetUpClientEdict (host_client);

//
// force stats to be updated
//
	memset (host_client->stats, 0, sizeof(host_client->stats));

	ClientReliableWrite_Begin (host_client, svc_updatestatlong, 6);
	ClientReliableWrite_Byte (host_client, STAT_TOTALSECRETS);
	ClientReliableWrite_Long (host_client, (int)pr.global_struct->total_secrets);

	ClientReliableWrite_Begin (host_client, svc_updatestatlong, 6);
	ClientReliableWrite_Byte (host_client, STAT_TOTALMONSTERS);
	ClientReliableWrite_Long (host_client, (int)pr.global_struct->total_monsters);

	ClientReliableWrite_Begin (host_client, svc_updatestatlong, 6);
	ClientReliableWrite_Byte (host_client, STAT_SECRETS);
	ClientReliableWrite_Long (host_client, (int)pr.global_struct->found_secrets);

	ClientReliableWrite_Begin (host_client, svc_updatestatlong, 6);
	ClientReliableWrite_Byte (host_client, STAT_MONSTERS);
	ClientReliableWrite_Long (host_client, (int)pr.global_struct->killed_monsters);

	// get the client to check and download skins
	// when that is completed, a begin command will be issued
	ClientReliableWrite_Begin (host_client, svc_stufftext, 8);
	ClientReliableWrite_String (host_client, "skins\n" );

}

/*
==================
SV_SpawnSpectator
==================
*/
void SV_SpawnSpectator (void)
{
	int		i;
	edict_t	*e;

	VectorCopy (vec3_origin, sv_player->v.origin);
	VectorCopy (vec3_origin, sv_player->v.view_ofs);
	sv_player->v.view_ofs[2] = 22;

	// search for an info_playerstart to spawn the spectator at
	for (i=MAX_CLIENTS-1 ; i<sv.num_edicts ; i++)
	{
		e = EDICT_NUM(i);
		if (!strcmp(PR_GetString(e->v.classname), "info_player_start"))
		{
			VectorCopy (e->v.origin, sv_player->v.origin);
			return;
		}
	}

}

/*
==================
SV_PutClientInGame

host_client, as a player or a spectator, through the progs
==================
*/
static void SV_PutClientInGame (void)
{
	int		i;

	if (host_client->spectator)
	{
		SV_SpawnSpectator ();

		if (pr.SpectatorConnect) {
			// copy spawn parms out of the client_t
			for (i=0 ; i< NUM_SPAWN_PARMS ; i++)
				(&pr.global_struct->parm1)[i] = host_client->spawn_parms[i];

			// call the spawn function
			pr.global_struct->time = (float)sv.time;
			pr.global_struct->self = EDICT_TO_PROG(sv_player);
			PR_ExecuteProgram (pr.SpectatorConnect);
		}
		return;
	}

	// copy spawn parms out of the client_t
	for (i=0 ; i< NUM_SPAWN_PARMS ; i++)
		(&pr.global_struct->parm1)[i] = host_client->spawn_parms[i];

	// call the spawn function
	pr.global_struct->time = (float)sv.time;
	pr.global_struct->self = EDICT_TO_PROG(sv_player);
	PR_ExecuteProgram (pr.global_struct->ClientConnect);

	// actually spawn the player
	pr.global_struct->time = (float)sv.time;
	pr.global_struct->self = EDICT_TO_PROG(sv_player);
	PR_ExecuteProgram (pr.global_struct->PutClientInServer);
}

/*
==================
SV_Join_f / SV_Observe_f

A spectator becomes a player, or the other way, without reconnecting
(ZQuake's Z_EXT_JOIN_OBSERVE)
==================
*/
static void SV_SwitchSide (bool spectator)
{
	int		i;

	if (host_client->state != cs_spawned || host_client->spectator == spectator)
		return;
	if (!(host_client->z_ext & Z_EXT_JOIN_OBSERVE))
	{
		SV_ClientPrintf (host_client, PRINT_HIGH, "Your client doesn't support this command.\n");
		return;
	}
	if (!SV_CanSwitchSide (host_client, spectator))
		return;

	// the old side leaves, as SV_DropClient has it
	pr.global_struct->self = EDICT_TO_PROG(sv_player);
	if (!host_client->spectator)
		PR_ExecuteProgram (pr.global_struct->ClientDisconnect);
	else if (pr.SpectatorDisconnect)
		PR_ExecuteProgram (pr.SpectatorDisconnect);

	host_client->old_frags = 0;
	host_client->spectator = spectator;
	host_client->spec_track = 0;
	if (spectator)
		Info_SetValueForStarKey (host_client->userinfo, "*spectator", "1", MAX_INFO_STRING, SV_InfoCharset ());
	else
		Info_RemoveKey (host_client->userinfo, "*spectator");

	// and comes in on the new one, as a new client would
	SV_SetUpClientEdict (host_client);
	PR_ExecuteProgram (pr.global_struct->SetNewParms);
	for (i=0 ; i<NUM_SPAWN_PARMS ; i++)
		host_client->spawn_parms[i] = (&pr.global_struct->parm1)[i];
	SV_PutClientInGame ();
	host_client->sendinfo = true;
}

static void SV_Join_f (void)
{
	SV_SwitchSide (false);
}

static void SV_Observe_f (void)
{
	SV_SwitchSide (true);
}

/*
==================
SV_Begin_f
==================
*/
void SV_Begin_f (void)
{
	unsigned pmodel = 0, emodel = 0;

	if (host_client->state == cs_spawned)
		return; // don't begin again

	host_client->state = cs_spawned;
	
	// handle the case of a level changing while a client was connecting
	if ( atoi(Cmd_Argv(1)) != svs.spawncount )
	{
		Con_Printf ("SV_Begin_f from different level\n");
		SV_New_f ();
		return;
	}

	SV_PutClientInGame ();

	// clear the net statistics, because connecting gives a bogus picture
	host_client->netchan.frame_latency = 0;
	host_client->netchan.frame_rate = 0;
	host_client->netchan.drop_count = 0;
	host_client->netchan.good_count = 0;

	//check he's not cheating

	pmodel = atoi(Info_ValueForKey (host_client->userinfo, "pmodel"));
	emodel = atoi(Info_ValueForKey (host_client->userinfo, "emodel"));

	if (pmodel != sv.model_player_checksum ||
		emodel != sv.eyes_player_checksum)
		SV_BroadcastPrintf (PRINT_HIGH, "%s WARNING: non standard player/eyes model detected\n", host_client->name);

	// if we are paused, tell the client
	if (sv.paused) {
		ClientReliableWrite_Begin (host_client, svc_setpause, 2);
		ClientReliableWrite_Byte (host_client, sv.paused);
		SV_ClientPrintf(host_client, PRINT_HIGH, "Server is paused.\n");
	}

}

//=============================================================================

/*
==================
SV_NextDownload_f
==================
*/
void SV_NextDownload_f (void)
{
	byte	buffer[1024];
	int		r;
	int		percent;
	int		size;

	if (!host_client->download)
		return;

	r = host_client->downloadsize - host_client->downloadcount;
	if (r > 768)
		r = 768;
	r = (int)fread (buffer, 1, r, host_client->download);
	ClientReliableWrite_Begin (host_client, svc_download, 6+r);
	ClientReliableWrite_Short (host_client, r);

	host_client->downloadcount += r;
	size = host_client->downloadsize;
	if (!size)
		size = 1;
	percent = host_client->downloadcount*100/size;
	ClientReliableWrite_Byte (host_client, percent);
	ClientReliableWrite_SZ (host_client, buffer, r);

	if (host_client->downloadcount != host_client->downloadsize)
		return;

	fclose (host_client->download);
	host_client->download = NULL;

}

void OutofBandPrintf(netadr_t where, char *fmt, ...)
{
	va_list		argptr;
	char	send[1024];
	
	send[0] = 0xff;
	send[1] = 0xff;
	send[2] = 0xff;
	send[3] = 0xff;
	send[4] = A2C_PRINT;
	va_start (argptr, fmt);
	vsnprintf (send+5, sizeof(send)-5, fmt, argptr);
	va_end (argptr);

	NET_SendPacket (NS_SERVER, (int)strlen(send)+1, send, where);
}

/*
==================
SV_NextUpload
==================
*/
void SV_NextUpload (void)
{
	int		percent;
	int		size;

	if (!*host_client->uploadfn) {
		SV_ClientPrintf(host_client, PRINT_HIGH, "Upload denied\n");
		ClientReliableWrite_Begin (host_client, svc_stufftext, 8);
		ClientReliableWrite_String (host_client, "stopul");

		// suck out rest of packet
		size = MSG_ReadShort ();	MSG_ReadByte ();
		msg_readcount += size;
		return;
	}

	size = MSG_ReadShort ();
	percent = MSG_ReadByte ();

	if (!host_client->upload)
	{
		host_client->upload = fopen(host_client->uploadfn, "wb");
		if (!host_client->upload) {
			Sys_Printf("Can't create %s\n", host_client->uploadfn);
			ClientReliableWrite_Begin (host_client, svc_stufftext, 8);
			ClientReliableWrite_String (host_client, "stopul");
			*host_client->uploadfn = 0;
			return;
		}
		Sys_Printf("Receiving %s from %d...\n", host_client->uploadfn, host_client->userid);
		if (host_client->remote_snap)
			OutofBandPrintf(host_client->snap_from, "Server receiving %s from %d...\n", host_client->uploadfn, host_client->userid);
	}

	fwrite (svs.net_message.data + msg_readcount, 1, size, host_client->upload);
	msg_readcount += size;

Con_DPrintf ("UPLOAD: %d received\n", size);

	if (percent != 100) {
		ClientReliableWrite_Begin (host_client, svc_stufftext, 8);
		ClientReliableWrite_String (host_client, "nextul\n");
	} else {
		fclose (host_client->upload);
		host_client->upload = NULL;

		Sys_Printf("%s upload completed.\n", host_client->uploadfn);

		if (host_client->remote_snap) {
			char *p;

			if ((p = strchr(host_client->uploadfn, '/')) != NULL)
				p++;
			else
				p = host_client->uploadfn;
			OutofBandPrintf(host_client->snap_from, "%s upload completed.\nTo download, enter:\ndownload %s\n", 
				host_client->uploadfn, p);
		}
	}

}

/*
==================
SV_BeginDownload_f
==================
*/
void SV_BeginDownload_f(void)
{
	char	*name;
	extern	cvar_t	allow_download;
	extern	cvar_t	allow_download_skins;
	extern	cvar_t	allow_download_models;
	extern	cvar_t	allow_download_sounds;
	extern	cvar_t	allow_download_maps;
	extern	int		file_from_pak; // ZOID did file come from pak?

	name = Cmd_Argv(1);
// hacked by zoid to allow more conrol over download
		// first off, no .. or global allow check
	if (strstr (name, "..") || !allow_download.value
		// leading dot is no good
		|| *name == '.' 
		// leading slash bad as well, must be in subdir
		|| *name == '/'
		// next up, skin check
		|| (strncmp(name, "skins/", 6) == 0 && !allow_download_skins.value)
		// now models
		|| (strncmp(name, "progs/", 6) == 0 && !allow_download_models.value)
		// now sounds
		|| (strncmp(name, "sound/", 6) == 0 && !allow_download_sounds.value)
		// now maps (note special case for maps, must not be in pak)
		|| (strncmp(name, "maps/", 6) == 0 && !allow_download_maps.value)
		// MUST be in a subdirectory	
		|| !strstr (name, "/") )	
	{	// don't allow anything with .. path
		ClientReliableWrite_Begin (host_client, svc_download, 4);
		ClientReliableWrite_Short (host_client, -1);
		ClientReliableWrite_Byte (host_client, 0);
		return;
	}

	if (host_client->download) {
		fclose (host_client->download);
		host_client->download = NULL;
	}

	// lowercase name (needed for casesen file systems)
	{
		char *p;

		for (p = name; *p; p++)
			*p = (char)tolower(*p);
	}


	host_client->downloadsize = COM_FOpenFile (name, &host_client->download);
	host_client->downloadcount = 0;

	if (!host_client->download
		// special check for maps, if it came from a pak file, don't allow
		// download  ZOID
		|| (strncmp(name, "maps/", 5) == 0 && file_from_pak))
	{
		if (host_client->download) {
			fclose(host_client->download);
			host_client->download = NULL;
		}

		Sys_Printf ("Couldn't download %s to %s\n", name, host_client->name);
		ClientReliableWrite_Begin (host_client, svc_download, 4);
		ClientReliableWrite_Short (host_client, -1);
		ClientReliableWrite_Byte (host_client, 0);
		return;
	}

	SV_NextDownload_f ();
	Sys_Printf ("Downloading %s to %s\n", name, host_client->name);
}

//=============================================================================

/*
==================
SV_Say
==================
*/
void SV_Say (bool team)
{
	client_t *client;
	int		j, tmp;
	char	*p;
	char	text[2048];
	char	t1[32], *t2;

	if (Cmd_Argc () < 2)
		return;

	if (team)
	{
		strncpy (t1, Info_ValueForKey (host_client->userinfo, "team"), 31);
		t1[31] = 0;
	}

	if (host_client->spectator && (!sv_spectalk.value || team))
		snprintf (text, sizeof(text), "[SPEC] %s: ", host_client->name);
	else if (team)
		snprintf (text, sizeof(text), "(%s): ", host_client->name);
	else {
		snprintf (text, sizeof(text), "%s: ", host_client->name);
	}

	if (svs.floodprot.messages) {
		if (!sv.paused && host.realtime<host_client->lockedtill) {
			SV_ClientPrintf(host_client, PRINT_CHAT,
				"You can't talk for %d more seconds\n", 
					(int) (host_client->lockedtill - host.realtime));
			return;
		}
		tmp = host_client->whensaidhead - svs.floodprot.messages + 1;
		if (tmp < 0)
			tmp = 10+tmp;
		if (!sv.paused &&
			host_client->whensaid[tmp] && (host.realtime-host_client->whensaid[tmp] < svs.floodprot.persecond)) {
			host_client->lockedtill = host.realtime + svs.floodprot.secondsdead;
			if (svs.floodprot.msg[0])
				SV_ClientPrintf(host_client, PRINT_CHAT,
					"FloodProt: %s\n", svs.floodprot.msg);
			else
				SV_ClientPrintf(host_client, PRINT_CHAT,
					"FloodProt: You can't talk for %d seconds.\n", svs.floodprot.secondsdead);
			return;
		}
		host_client->whensaidhead++;
		if (host_client->whensaidhead > 9)
			host_client->whensaidhead = 0;
		host_client->whensaid[host_client->whensaidhead] = host.realtime;
	}

	p = Cmd_Args();

	if (*p == '"')
	{
		p++;
		p[Q_strlen(p)-1] = 0;
	}

	Q_strncatz(text, p, sizeof(text));
	Q_strncatz(text, "\n", sizeof(text));

	Sys_Printf ("%s", text);

	for (j = 0, client = svs.clients; j < MAX_CLIENTS; j++, client++)
	{
		if (client->state != cs_spawned)
			continue;
		if (host_client->spectator && !sv_spectalk.value)
			if (!client->spectator)
				continue;

		if (team)
		{
			// the spectator team
			if (host_client->spectator) {
				if (!client->spectator)
					continue;
			} else {
				t2 = Info_ValueForKey (client->userinfo, "team");
				if (strcmp(t1, t2) || client->spectator)
					continue;	// on different teams
			}
		}
		SV_ClientPrintf(client, PRINT_CHAT, "%s", text);
	}
}


/*
==================
SV_Say_f
==================
*/
void SV_Say_f(void)
{
	SV_Say (false);
}
/*
==================
SV_Say_Team_f
==================
*/
void SV_Say_Team_f(void)
{
	SV_Say (true);
}



//============================================================================

/*
=================
SV_Pings_f

The client is showing the scoreboard, so send new ping times for all
clients
=================
*/
void SV_Pings_f (void)
{
	client_t *client;
	int		j;

	for (j = 0, client = svs.clients; j < MAX_CLIENTS; j++, client++)
	{
		if (client->state != cs_spawned)
			continue;

		ClientReliableWrite_Begin (host_client, svc_updateping, 4);
		ClientReliableWrite_Byte (host_client, j);
		ClientReliableWrite_Short (host_client, SV_CalcPing(client));
		ClientReliableWrite_Begin (host_client, svc_updatepl, 4);
		ClientReliableWrite_Byte (host_client, j);
		ClientReliableWrite_Byte (host_client, client->lossage);
	}
}



/*
==================
SV_Kill_f
==================
*/
void SV_Kill_f (void)
{
	if (sv_player->v.health <= 0)
	{
		SV_ClientPrintf (host_client, PRINT_HIGH, "Can't suicide -- allready dead!\n");
		return;
	}
	
	pr.global_struct->time = (float)sv.time;
	pr.global_struct->self = EDICT_TO_PROG(sv_player);
	PR_ExecuteProgram (pr.global_struct->ClientKill);
}

/*
==================
SV_TogglePause
==================
*/
void SV_TogglePause (const char *msg)
{
	int i;
	client_t *cl;

	sv.paused ^= 1;

	if (msg)
		SV_BroadcastPrintf (PRINT_HIGH, "%s", msg);

	// send notification to all clients
	for (i=0, cl = svs.clients ; i<MAX_CLIENTS ; i++, cl++)
	{
		if (!cl->state)
			continue;
		ClientReliableWrite_Begin (cl, svc_setpause, 2);
		ClientReliableWrite_Byte (cl, sv.paused);
	}
}


/*
==================
SV_Pause_f
==================
*/
void SV_Pause_f (void)
{
	char st[sizeof(host_client->name) + 32];

	if (!pausable.value) {
		SV_ClientPrintf (host_client, PRINT_HIGH, "Pause not allowed.\n");
		return;
	}

	if (host_client->spectator) {
		SV_ClientPrintf (host_client, PRINT_HIGH, "Spectators can not pause.\n");
		return;
	}

	if (sv.paused)
		snprintf (st, sizeof(st), "%s paused the game\n", host_client->name);
	else
		snprintf (st, sizeof(st), "%s unpaused the game\n", host_client->name);

	SV_TogglePause(st);
}


/*
=================
SV_Drop_f

The client is going to disconnect, so remove the connection immediately
=================
*/
void SV_Drop_f (void)
{
	SV_EndRedirect ();
	if (!host_client->spectator)
		SV_BroadcastPrintf (PRINT_HIGH, "%s dropped\n", host_client->name);
	SV_DropClient (host_client);	
}

/*
=================
SV_PTrack_f

Change the bandwidth estimate for a client
=================
*/
void SV_PTrack_f (void)
{
	int		i;
	edict_t *ent, *tent;
	
	if (!host_client->spectator)
		return;

	if (Cmd_Argc() != 2)
	{
		// turn off tracking
		host_client->spec_track = 0;
		ent = EDICT_NUM((int)(host_client - svs.clients) + 1);
		tent = EDICT_NUM(0);
		ent->v.goalentity = EDICT_TO_PROG(tent);
		return;
	}
	
	i = atoi(Cmd_Argv(1));
	if (i < 0 || i >= MAX_CLIENTS || svs.clients[i].state != cs_spawned ||
		svs.clients[i].spectator) {
		SV_ClientPrintf (host_client, PRINT_HIGH, "Invalid client to track\n");
		host_client->spec_track = 0;
		ent = EDICT_NUM((int)(host_client - svs.clients) + 1);
		tent = EDICT_NUM(0);
		ent->v.goalentity = EDICT_TO_PROG(tent);
		return;
	}
	host_client->spec_track = i + 1; // now tracking

	ent = EDICT_NUM((int)(host_client - svs.clients) + 1);
	tent = EDICT_NUM(i + 1);
	ent->v.goalentity = EDICT_TO_PROG(tent);
}


/*
=================
SV_Rate_f

Change the bandwidth estimate for a client
=================
*/
void SV_Rate_f (void)
{
	int		rate;
	
	if (Cmd_Argc() != 2)
	{
		SV_ClientPrintf (host_client, PRINT_HIGH, "Current rate is %i\n",
			(int)(1.0/host_client->netchan.rate + 0.5));
		return;
	}
	
	rate = atoi(Cmd_Argv(1));
	if (rate < 500)
		rate = 500;
	if (rate > 10000)
		rate = 10000;

	SV_ClientPrintf (host_client, PRINT_HIGH, "Net rate set to %i\n", rate);
	host_client->netchan.rate = 1.0/rate;
}


/*
=================
SV_Msg_f

Change the message level for a client
=================
*/
void SV_Msg_f (void)
{	
	if (Cmd_Argc() != 2)
	{
		SV_ClientPrintf (host_client, PRINT_HIGH, "Current msg level is %i\n",
			host_client->messagelevel);
		return;
	}
	
	host_client->messagelevel = atoi(Cmd_Argv(1));

	SV_ClientPrintf (host_client, PRINT_HIGH, "Msg level set to %i\n", host_client->messagelevel);
}

/*
==================
SV_SetInfo_f

Allow clients to change userinfo
==================
*/
void SV_SetInfo_f (void)
{
	int i;
	char oldval[MAX_INFO_STRING];


	if (Cmd_Argc() == 1)
	{
		Con_Printf ("User info settings:\n");
		Info_Print (host_client->userinfo);
		return;
	}

	if (Cmd_Argc() != 3)
	{
		Con_Printf ("usage: setinfo [ <key> <value> ]\n");
		return;
	}

	if (Cmd_Argv(1)[0] == '*')
		return;		// don't set priveledged values

	Q_strncpyz(oldval, Info_ValueForKey(host_client->userinfo, Cmd_Argv(1)), sizeof(oldval));

	Info_SetValueForKey (host_client->userinfo, Cmd_Argv(1), Cmd_Argv(2), MAX_INFO_STRING, SV_InfoCharset ());
// name is extracted below in ExtractFromUserInfo
//	strncpy (host_client->name, Info_ValueForKey (host_client->userinfo, "name")
//		, sizeof(host_client->name)-1);	
//	SV_FullClientUpdate (host_client, &sv.reliable_datagram);
//	host_client->sendinfo = true;

	if (!strcmp(Info_ValueForKey(host_client->userinfo, Cmd_Argv(1)), oldval))
		return; // key hasn't changed

	// process any changed values
	SV_ExtractFromUserinfo (host_client);

	i = (int)(host_client - svs.clients);
	MSG_WriteByte (&sv.reliable_datagram, svc_setinfo);
	MSG_WriteByte (&sv.reliable_datagram, i);
	MSG_WriteString (&sv.reliable_datagram, Cmd_Argv(1));
	MSG_WriteString (&sv.reliable_datagram, Info_ValueForKey(host_client->userinfo, Cmd_Argv(1)));
}

/*
==================
SV_ShowServerinfo_f

Dumps the serverinfo info string
==================
*/
void SV_ShowServerinfo_f (void)
{
	Info_Print (svs.info);
}

void SV_NoSnap_f(void)
{
	if (*host_client->uploadfn) {
		*host_client->uploadfn = 0;
		SV_BroadcastPrintf (PRINT_HIGH, "%s refused remote screenshot\n", host_client->name);
	}
}

typedef struct
{
	char	*name;
	void	(*func) (void);
} ucmd_t;

static ucmd_t ucmds[] =
{
	{"new", SV_New_f},
	{"modellist", SV_Modellist_f},
	{"soundlist", SV_Soundlist_f},
	{"prespawn", SV_PreSpawn_f},
	{"spawn", SV_Spawn_f},
	{"begin", SV_Begin_f},
	{"join", SV_Join_f},
	{"observe", SV_Observe_f},

	{"drop", SV_Drop_f},
	{"pings", SV_Pings_f},

// issued by hand at client consoles	
	{"rate", SV_Rate_f},
	{"kill", SV_Kill_f},
	{"pause", SV_Pause_f},
	{"msg", SV_Msg_f},

	{"say", SV_Say_f},
	{"say_team", SV_Say_Team_f},

	{"setinfo", SV_SetInfo_f},

	{"serverinfo", SV_ShowServerinfo_f},

	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},

	{"ptrack", SV_PTrack_f}, //ZOID - used with autocam

	{"snap", SV_NoSnap_f},
	
	{NULL, NULL}
};

/*
==================
SV_ExecuteUserCommand
==================
*/
void SV_ExecuteUserCommand (char *s)
{
	ucmd_t	*u;
	
	Cmd_TokenizeString (s);
	sv_player = host_client->edict;

	SV_BeginRedirect (RD_CLIENT);

	for (u=ucmds ; u->name ; u++)
		if (!strcmp (Cmd_Argv(0), u->name) )
		{
			u->func ();
			break;
		}

	if (!u->name)
		Con_Printf ("Bad user command: %s\n", Cmd_Argv(0));

	SV_EndRedirect ();
}

/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/





//============================================================================

static vec3_t	pmove_mins, pmove_maxs;

static playermove_t	sv_pmove;

/*
====================
AddLinksToPmove

====================
*/
void AddLinksToPmove ( areanode_t *node )
{
	link_t		*l, *next;
	edict_t		*check;
	int			pl;
	int			i;
	physent_t	*pe;

	pl = EDICT_TO_PROG(sv_player);

	// touch linked edicts
	for (l = node->solid_edicts.next ; l != &node->solid_edicts ; l = next)
	{
		next = l->next;
		check = EDICT_FROM_AREA(l);

		if (check->v.owner == pl)
			continue;		// player's own missile
		if (check->v.solid == SOLID_BSP 
			|| check->v.solid == SOLID_BBOX 
			|| check->v.solid == SOLID_SLIDEBOX)
		{
			if (check == sv_player)
				continue;

			for (i=0 ; i<3 ; i++)
				if (check->v.absmin[i] > pmove_maxs[i]
				|| check->v.absmax[i] < pmove_mins[i])
					break;
			if (i != 3)
				continue;
			if (sv_pmove.numphysent == MAX_PHYSENTS)
				return;
			pe = &sv_pmove.physents[sv_pmove.numphysent];
			sv_pmove.numphysent++;

			VectorCopy (check->v.origin, pe->origin);
			pe->info = NUM_FOR_EDICT(check);
			if (check->v.solid == SOLID_BSP)
				pe->model = sv.models[(int)(check->v.modelindex)];
			else
			{
				pe->model = NULL;
				VectorCopy (check->v.mins, pe->mins);
				VectorCopy (check->v.maxs, pe->maxs);
			}
		}
	}
	
// recurse down both sides
	if (node->axis == -1)
		return;

	if ( pmove_maxs[node->axis] > node->dist )
		AddLinksToPmove ( node->children[0] );
	if ( pmove_mins[node->axis] < node->dist )
		AddLinksToPmove ( node->children[1] );
}

/*
===========
SV_PreRunCmd
===========
Done before running a player command.  Clears the touch array
*/
static byte playertouch[(MAX_EDICTS+7)/8];

void SV_PreRunCmd(void)
{
	memset(playertouch, 0, sizeof(playertouch));
}

/*
===========
SV_RunCmd
===========
*/
void SV_RunCmd (usercmd_t *ucmd)
{
	movevars_t	movevars;
	edict_t		*ent;
	int			i, n;
	int			oldmsec;

	cmd = *ucmd;

	// the pitch limits in the serverinfo (Z_EXT_PITCHLIMITS)
	if (cmd.angles[PITCH] > sv_maxpitch.value)
		cmd.angles[PITCH] = sv_maxpitch.value;
	if (cmd.angles[PITCH] < sv_minpitch.value)
		cmd.angles[PITCH] = sv_minpitch.value;

	// chop up very long commands
	if (cmd.msec > 50)
	{
		oldmsec = ucmd->msec;
		cmd.msec = (byte)(oldmsec/2);
		SV_RunCmd (&cmd);
		cmd.msec = (byte)(oldmsec/2);
		cmd.impulse = 0;
		SV_RunCmd (&cmd);
		return;
	}

	if (!sv_player->v.fixangle)
		VectorCopy (ucmd->angles, sv_player->v.v_angle);

	sv_player->v.button0 = (float)(ucmd->buttons & 1);
	sv_player->v.button2 = (float)((ucmd->buttons & 2)>>1);
	if (ucmd->impulse)
		sv_player->v.impulse = ucmd->impulse;

//
// angles
// show 1/3 the pitch angle and all the roll angle	
	if (sv_player->v.health > 0)
	{
		if (!sv_player->v.fixangle)
		{
			sv_player->v.angles[PITCH] = -sv_player->v.v_angle[PITCH]/3;
			sv_player->v.angles[YAW] = sv_player->v.v_angle[YAW];
		}
		sv_player->v.angles[ROLL] = 
			PM_CalcRoll (sv_player->v.angles, sv_player->v.velocity)*4;
	}

	sv.frametime = ucmd->msec * 0.001;
	if (sv.frametime > 0.1)
		sv.frametime = 0.1;

	if (!host_client->spectator)
	{
		pr.global_struct->frametime = (float)sv.frametime;

		pr.global_struct->time = (float)sv.time;
		pr.global_struct->self = EDICT_TO_PROG(sv_player);
		PR_ExecuteProgram (pr.global_struct->PlayerPreThink);

		SV_RunThink (sv_player);
	}

	for (i=0 ; i<3 ; i++)
		sv_pmove.origin[i] = sv_player->v.origin[i] + (sv_player->v.mins[i] - player_mins[i]);
	VectorCopy (sv_player->v.velocity, sv_pmove.velocity);
	VectorCopy (sv_player->v.v_angle, sv_pmove.angles);

	sv_pmove.spectator = host_client->spectator;
	sv_pmove.waterjumptime = sv_player->v.teleport_time;
	sv_pmove.numphysent = 1;
	sv_pmove.physents[0].model = sv.worldmodel;
	sv_pmove.cmd = *ucmd;
	sv_pmove.dead = sv_player->v.health <= 0;
	sv_pmove.oldbuttons = host_client->oldbuttons;

	movevars = sv.movevars;
	movevars.entgravity = host_client->entgravity;
	movevars.maxspeed = host_client->maxspeed;

	for (i=0 ; i<3 ; i++)
	{
		pmove_mins[i] = sv_pmove.origin[i] - 256;
		pmove_maxs[i] = sv_pmove.origin[i] + 256;
	}
	AddLinksToPmove ( sv.areanodes );

	PM_PlayerMove (&sv_pmove, &movevars);

	host_client->oldbuttons = sv_pmove.oldbuttons;
	sv_player->v.teleport_time = sv_pmove.waterjumptime;
	sv_player->v.waterlevel = (float)sv_pmove.waterlevel;
	sv_player->v.watertype = (float)sv_pmove.watertype;
	if (sv_pmove.onground != -1)
	{
		sv_player->v.flags = (float)((int)sv_player->v.flags | FL_ONGROUND);
		sv_player->v.groundentity = EDICT_TO_PROG(EDICT_NUM(sv_pmove.physents[sv_pmove.onground].info));
	}
	else
		sv_player->v.flags = (float)((int)sv_player->v.flags & ~FL_ONGROUND);
	for (i=0 ; i<3 ; i++)
		sv_player->v.origin[i] = sv_pmove.origin[i] - (sv_player->v.mins[i] - player_mins[i]);

	VectorCopy (sv_pmove.velocity, sv_player->v.velocity);

	VectorCopy (sv_pmove.angles, sv_player->v.v_angle);

	if (!host_client->spectator)
	{
		// link into place and touch triggers
		SV_LinkEdict (sv_player, true);

		// touch other objects
		for (i=0 ; i<sv_pmove.numtouch ; i++)
		{
			n = sv_pmove.physents[sv_pmove.touchindex[i]].info;
			ent = EDICT_NUM(n);
			if (!ent->v.touch || (playertouch[n/8]&(1<<(n%8))))
				continue;
			pr.global_struct->self = EDICT_TO_PROG(ent);
			pr.global_struct->other = EDICT_TO_PROG(sv_player);
			PR_ExecuteProgram (ent->v.touch);
			playertouch[n/8] |= 1 << (n%8);
		}
	}
}

/*
===========
SV_PostRunCmd
===========
Done after running a player command.
*/
void SV_PostRunCmd(void)
{
	// run post-think

	if (!host_client->spectator) {
		pr.global_struct->time = (float)sv.time;
		pr.global_struct->self = EDICT_TO_PROG(sv_player);
		PR_ExecuteProgram (pr.global_struct->PlayerPostThink);
		SV_RunNewmis ();
	} else if (pr.SpectatorThink) {
		pr.global_struct->time = (float)sv.time;
		pr.global_struct->self = EDICT_TO_PROG(sv_player);
		PR_ExecuteProgram (pr.SpectatorThink);
	}
}


/*
===================
SV_ExecuteClientMessage

The current net_message is parsed for the given client
===================
*/
void SV_ExecuteClientMessage (client_t *cl)
{
	int		c;
	char	*s;
	usercmd_t	oldest, oldcmd, newcmd;
	client_frame_t	*frame;
	vec3_t o;
	bool	move_issued = false; //only allow one move command
	int		checksumIndex;
	byte	checksum, calculatedChecksum;
	int		seq_hash;

	// calc ping time
	frame = &cl->frames[cl->netchan.incoming_acknowledged & UPDATE_MASK];
	frame->ping_time = (float)(host.realtime - frame->senttime);

	// make sure the reply sequence number matches the incoming
	// sequence number 
	if (cl->netchan.incoming_sequence >= cl->netchan.outgoing_sequence)
		cl->netchan.outgoing_sequence = cl->netchan.incoming_sequence;
	else
		cl->send_message = false;	// don't reply, sequences have slipped		

	// save time for ping calculations
	cl->frames[cl->netchan.outgoing_sequence & UPDATE_MASK].senttime = host.realtime;
	cl->frames[cl->netchan.outgoing_sequence & UPDATE_MASK].ping_time = -1;

	host_client = cl;
	sv_player = host_client->edict;

//	seq_hash = (cl->netchan.incoming_sequence & 0xffff) ; // ^ QW_CHECK_HASH;
	seq_hash = cl->netchan.incoming_sequence;
	
	// mark time so clients will know how much to predict
	// other players
 	cl->localtime = sv.time;
	cl->delta_sequence = -1;	// no delta unless requested
	while (1)
	{
		if (msg_badread)
		{
			Con_Printf ("SV_ReadClientMessage: badread\n");
			SV_DropClient (cl);
			return;
		}	

		c = MSG_ReadByte ();
		if (c == -1)
			break;
				
		switch (c)
		{
		default:
			Con_Printf ("SV_ReadClientMessage: unknown command char\n");
			SV_DropClient (cl);
			return;
						
		case clc_nop:
			break;

		case clc_delta:
			cl->delta_sequence = MSG_ReadByte ();
			break;

		case clc_move:
			if (move_issued)
				return;		// someone is trying to cheat...

			move_issued = true;

			checksumIndex = MSG_GetReadCount();
			checksum = (byte)MSG_ReadByte ();

			// read loss percentage
			cl->lossage = MSG_ReadByte();

			MSG_ReadDeltaUsercmd (&nullcmd, &oldest);
			MSG_ReadDeltaUsercmd (&oldest, &oldcmd);
			MSG_ReadDeltaUsercmd (&oldcmd, &newcmd);

			if ( cl->state != cs_spawned )
				break;

			// if the checksum fails, ignore the rest of the packet
			calculatedChecksum = COM_BlockSequenceCRCByte(
				svs.net_message.data + checksumIndex + 1,
				MSG_GetReadCount() - checksumIndex - 1,
				seq_hash);

			if (calculatedChecksum != checksum)
			{
				Con_DPrintf ("Failed command checksum for %s(%d) (%d != %d)\n", 
					cl->name, cl->netchan.incoming_sequence, checksum, calculatedChecksum);
				return;
			}

			if (!sv.paused) {
				SV_PreRunCmd();

				int		dropped = cl->netchan.dropped;

				if (dropped < 20)
				{
					while (dropped > 2)
					{
						SV_RunCmd (&cl->lastcmd);
						dropped--;
					}
					if (dropped > 1)
						SV_RunCmd (&oldest);
					if (dropped > 0)
						SV_RunCmd (&oldcmd);
				}
				SV_RunCmd (&newcmd);

				SV_PostRunCmd();
			}

			cl->lastcmd = newcmd;
			cl->lastcmd.buttons = 0; // avoid multiple fires on lag
			break;


		case clc_stringcmd:	
			s = MSG_ReadString ();
			SV_ExecuteUserCommand (s);
			break;

		case clc_tmove:
			o[0] = MSG_ReadCoord();
			o[1] = MSG_ReadCoord();
			o[2] = MSG_ReadCoord();
			// only allowed by spectators
			if (host_client->spectator) {
				VectorCopy(o, sv_player->v.origin);
				SV_LinkEdict(sv_player, false);
			}
			break;

		case clc_upload:
			SV_NextUpload();
			break;

		}
	}
}

/*
==============
SV_UserInit
==============
*/
void SV_UserInit (void)
{
	PM_Init ();
	Cvar_RegisterVariable (&sv_spectalk);
	Cvar_RegisterVariable (&sv_mapcheck);
}


