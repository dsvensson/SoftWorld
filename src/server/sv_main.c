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

static void SV_FinalMessage (char *message);
static void Master_Heartbeat (void);
static void SV_WebRTCInfo (void);






client_t	*host_client;			// current client

cvar_t	sv_mintic = {.name = "sv_mintic", .string = "0.03",	// bound the size of the
	.description = "Shortest step of the world's physics, in seconds: entities (not players) move no more often "
		"than this."};
cvar_t	sv_maxtic = {.name = "sv_maxtic", .string = "0.1",	// physics time tic
	.description = "Longest step of the world's physics, in seconds: a longer gap is cut to it, and an empty server "
		"steps this seldom."};


static cvar_t	timeout = {.name = "timeout", .string = "65",		// seconds without any message
	.description = "Seconds without a packet from a client before it is dropped; the local client never times out."};
static cvar_t	zombietime = {.name = "zombietime", .string = "2",	// seconds to sink messages
											// after disconnect
	.description = "Seconds a dropped client's slot is held, so its last reliable message can be resent, "
		"before it is reused."};

static cvar_t	spectator_password = {.name = "spectator_password", .string = "",	// password for entering as a sepctator
	.description = "Password spectators must give as their spectator userinfo key; empty or \"none\" for none. "
		"Sets needpass in the serverinfo."};

cvar_t	sv_csqc_progname = {.name = "sv_csqc_progname", .string = "csprogs.dat",
	.description = "The client-side QuakeC (CSQC) the server offers from the game directory, published with "
		"its checksum and size in the serverinfo; empty for none."};
cvar_t	allow_download = {.name = "allow_download", .string = "1",
	.description = "Lets clients download the files they lack from the server; 0 turns off all downloads.",
	.values = (const cvar_value_t[]){{"0", "No downloads"}, {"1", "Downloads, as the allow_download_ cvars permit"},
		{0}}};
cvar_t	allow_download_skins = {.name = "allow_download_skins", .string = "1",
	.description = "Lets clients download skins (files under skins/), when allow_download is on.",
	.values = (const cvar_value_t[]){{"0", "Refused"}, {"1", "Allowed"}, {0}}};
cvar_t	allow_download_models = {.name = "allow_download_models", .string = "1",
	.description = "Lets clients download models (files under progs/), when allow_download is on.",
	.values = (const cvar_value_t[]){{"0", "Refused"}, {"1", "Allowed"}, {0}}};
cvar_t	allow_download_sounds = {.name = "allow_download_sounds", .string = "1",
	.description = "Lets clients download sounds (files under sound/), when allow_download is on.",
	.values = (const cvar_value_t[]){{"0", "Refused"}, {"1", "Allowed"}, {0}}};
cvar_t	allow_download_maps = {.name = "allow_download_maps", .string = "1",
	.description = "Lets clients download maps (files under maps/), when allow_download is on; "
		"maps in pak files are never sent.",
	.values = (const cvar_value_t[]){{"0", "Refused"}, {"1", "Allowed"}, {0}}};

static cvar_t sv_highchars = {.name = "sv_highchars", .string = "1",
	.description = "Lets names and other info strings keep high-bit (colored) and control characters; 0 strips them.",
	.values = (const cvar_value_t[]){{"0", "Plain ASCII only"}, {"1", "Any characters"}, {0}}};

cvar_t sv_phs = {.name = "sv_phs", .string = "1",
	.description = "Sends a sound only to the clients that could hear it (the map's PHS); 0 sends every sound to all.",
	.values = (const cvar_value_t[]){{"0", "Every sound to every client"},
		{"1", "Sounds only to clients that may hear them"}, {0}}};

// float coordinates (FTE_PEXT_FLOATCOORDS) for every map, not just those
// past the standard +-4096; clients without them can't join
cvar_t sv_bigcoords = {.name = "sv_bigcoords", .string = "0",
	.description = "Uses float coordinates on every map, not only those past +-4096; clients without them can't join. "
		"Read as a map loads.",
	.values = (const cvar_value_t[]){{"0", "Only on maps past +-4096"}, {"1", "On every map"}, {0}}};
// FTE's replacement deltas (FTE_PEXT2_REPLACEMENTDELTAS) for every level, not
// just those that need them
cvar_t sv_replacementdeltas = {.name = "sv_replacementdeltas", .string = "0",
	.description = "Sends FTE's replacement deltas on every level to the clients that read them, not only on "
		"NetQuake's progs and levels with more entities or sounds than QuakeWorld's deltas hold. Read as a map loads.",
	.values = (const cvar_value_t[]){{"0", "Only where they are needed"}, {"1", "On every level"}, {0}}};
// browsers' clients, which can't send UDP: WebSocket on TCP at the port's
// number, a packet a binary message, as FTE's (net_ws.c)
cvar_t	sv_websocket = {.name = "sv_websocket", .string = "1",
	.description = "Lets browsers' clients join over WebSocket (ws://), on TCP at the server's port number, a packet "
		"a binary message as FTE's. Read as a map opens the port; wss:// is a TLS proxy's in front.",
	.values = (const cvar_value_t[]){{"0", "UDP only"}, {"1", "UDP, and WebSocket on TCP"}, {0}}};
// clients over WebRTC, through a broker's room, as FTE's servers take them
// (net_rtc.c); and the game to QTV's viewers (sv_mvd.c)
cvar_t	sv_public = {.name = "sv_public", .string = "0",
	.description = "Hosts the game at the WebRTC broker (net_webrtc_broker), on its list of servers, for clients "
		"over WebRTC: in the room sv_webrtc_room, or else under an invitation code made once a run, which the "
		"console tells and clients connect to (connect 1234-5678). And streams it to QTV's viewers, on TCP at "
		"the server's port number (qtvplay host:port), qtv_delay seconds behind. Read as a map opens the port.",
	.values = (const cvar_value_t[]){{"0", "Not at the broker, no QTV"}, {"1", "At the broker, and QTV"}, {0}}};
cvar_t	sv_webrtc_room = {.name = "sv_webrtc_room", .string = "",
	.description = "The room a public server (sv_public) hosts at the WebRTC broker, which clients connect to as "
		"rtc://broker/room (rtcs:// over TLS); empty for the invitation code. Read as a map opens the port."};
// the most bytes per second a client's rate may ask for, 0 no limit (FTE's)
static cvar_t	sv_maxrate = {.name = "sv_maxrate", .string = "50000",
	.description = "Most bytes per second a client's rate may ask for; 0 for no limit."};
// bytes per second to a client downloading, 0 no limit (FTE's)
cvar_t	sv_maxdrate = {.name = "sv_maxdrate", .string = "10000000",
	.description = "Bytes per second sent to a client while it downloads; 0 for no limit."};

// player movement: serverinfo keys, so the clients predict the same (mvdsv's
// names and defaults); pm_pground follows pm_airstep
cvar_t	pm_ktjump = {.name = "pm_ktjump", .string = "1", .serverinfo = true,
	.description = "How far a jump made while moving down is raised toward a full jump, 0 to 1; 0 turns it off. "
		"Serverinfo."};
cvar_t	pm_bunnyspeedcap = {.name = "pm_bunnyspeedcap", .string = "", .serverinfo = true,
	.description = "Stops speed gained in the air at this many times the player's top speed; 0 or empty for no cap. "
		"Serverinfo."};
cvar_t	pm_slidefix = {.name = "pm_slidefix", .string = "", .serverinfo = true,
	.description = "Applies gravity to players on the ground too, so they go down ramps as in NetQuake. Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "Gravity on the ground too"}, {0}}};
cvar_t	pm_airstep = {.name = "pm_airstep", .string = "", .serverinfo = true,
	.description = "Lets players step up in the air, onto a step with ground under it, for some of their speed; "
		"pm_pground follows it. Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "Steps in the air"}, {0}}};
cvar_t	pm_pground = {.name = "pm_pground", .string = "", .serverinfo = true,
	.description = "Players find the ground only by landing on it, and keep it; set to follow pm_airstep. Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Ground traced on every move"}, {"1", "Ground found by landing"}, {0}}};
cvar_t	pm_rampjump = {.name = "pm_rampjump", .string = "", .serverinfo = true,
	.description = "The ground holds players moving up a steep ramp longer, and the jump fix applies even when "
		"not falling. Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "On"}, {0}}};

// how far players may look up and down: serverinfo keys for the clients
// (Z_EXT_PITCHLIMITS), and the server holds commands to them
cvar_t sv_maxpitch = {.name = "maxpitch", .string = "80", .serverinfo = true,
	.description = "How far down players may look, in degrees; clients take 0 to 89.9. Serverinfo, and the server "
		"holds commands to it."};
cvar_t sv_minpitch = {.name = "minpitch", .string = "-70", .serverinfo = true,
	.description = "How far up players may look, in negative degrees; clients take -89.9 to 0. Serverinfo, and the "
		"server holds commands to it."};

cvar_t pausable	= {.name = "pausable", .string = "1",
	.description = "Lets players pause the game with the pause command; spectators never can.",
	.values = (const cvar_value_t[]){{"0", "No pausing"}, {"1", "Players may pause"}, {0}}};


//
// game rules mirrored in svs.info
//
static cvar_t	fraglimit = {.name = "fraglimit", .string = "0", .serverinfo = true,
	.description = "Frags at which the level ends, read by the game code; 0 for no limit. Serverinfo."};
static cvar_t	timelimit = {.name = "timelimit", .string = "0", .serverinfo = true,
	.description = "Minutes after which the level ends, read by the game code; 0 for no limit. Serverinfo."};
cvar_t	teamplay = {.name = "teamplay", .string = "0", .serverinfo = true,
	.description = "Team rules, read by the game code (the meanings are the stock game's); clients show team scores "
		"when nonzero. Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "No teams"}, {"1", "No damage to teammates or yourself"},
		{"2", "Teammates take damage; killing one costs a frag"}, {"3", "No damage to teammates, but to yourself"},
		{0}}};
static cvar_t	samelevel = {.name = "samelevel", .string = "0", .serverinfo = true,
	.description = "What a level's end and exits do, read by the game code (the meanings are the stock game's). "
		"Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Go on to the next map"}, {"1", "Stay on the same map"},
		{"2", "Exits kill whoever touches them"}, {"3", "Exits kill, except on start"}, {0}}};
static cvar_t	maxclients = {.name = "maxclients", .string = "8", .serverinfo = true,
	.description = "Most players the server takes at once, up to 32. Serverinfo."};
static cvar_t	maxspectators = {.name = "maxspectators", .string = "8", .serverinfo = true,
	.description = "Most spectators the server takes at once, up to 32. Serverinfo."};
cvar_t	deathmatch = {.name = "deathmatch", .string = "1", .serverinfo = true,
	.description = "Deathmatch rules, read by the game code (the meanings are the stock game's). 0 is single "
		"player or coop, which runs NetQuake's progs.dat unless the game directory has only a qwprogs.dat. "
		"Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Single player or coop (NetQuake's progs.dat)"},
		{"1", "Weapons are picked up; items respawn"},
		{"2", "Weapons stay; armor, ammo and health don't respawn"},
		{"3", "Weapons stay; items respawn, ammo in half the time"},
		{"4", "Spawn with all weapons and full ammo; no weapons or ammo on the map"},
		{"5", "Spawn with all weapons and some ammo; no weapons on the map"}, {0}}};
cvar_t	coop = {.name = "coop", .string = "0",
	.description = "Cooperative play, read by NetQuake's game code when deathmatch is 0: players respawn where "
		"they died and keep their weapons.",
	.values = (const cvar_value_t[]){{"0", "Single player"}, {"1", "Coop"}, {0}}};
cvar_t	skill = {.name = "skill", .string = "1",
	.description = "The difficulty of NetQuake's game (deathmatch 0): which monsters a map spawns, and how they "
		"fight. Takes effect at the next map.",
	.values = (const cvar_value_t[]){{"0", "Easy"}, {"1", "Normal"}, {"2", "Hard"}, {"3", "Nightmare"}, {0}}};
static cvar_t	spawn = {.name = "spawn", .string = "0", .serverinfo = true,
	.description = "A serverinfo key left for the game code; neither the engine nor the stock game reads it."};
static cvar_t	watervis = {.name = "watervis", .string = "0", .serverinfo = true,
	.description = "Tells clients whether they may see through water (r_wateralpha); this engine's client doesn't "
		"check it. Serverinfo.",
	.values = (const cvar_value_t[]){{"0", "Water opaque"}, {"1", "Translucent water allowed"}, {0}}};

static cvar_t	hostname = {.name = "hostname", .string = "SoftWorld", .serverinfo = true,
	.description = "The server's name, as server browsers show it. Serverinfo."};


static void Master_Shutdown (void);

//============================================================================


/*
================
SV_Active
================
*/
bool SV_Active (void)
{
	return sv.state != ss_dead;
}

/*
================
SV_Kill

Ends the game: the clients are told and forgotten. A listen server closes
its port until the next map.
================
*/
void SV_Kill (void)
{
	int			i;
	client_t	*cl;

	if (sv.state == ss_dead)
		return;

	SV_FinalMessage ("server shutdown\n");
	for (i=0, cl = svs.clients ; i<MAX_CLIENTS ; i++, cl++)
	{
		if (cl->download)
		{
			fclose (cl->download);
			cl->download = NULL;
		}
		if (cl->upload)
		{
			fclose (cl->upload);
			cl->upload = NULL;
		}
		cl->state = cs_free;
	}

	PR_ResetStack ();
	PR_FreeProgs ();
	sv.edicts = NULL;
	sv.num_edicts = 0;
	sv.state = ss_dead;
	if (sv.map)
		CM_FreeMap (sv.map);
	sv.map = NULL;
	SV_FreeBrushModels ();
	if (!host.dedicated)
		NET_CloseSocket (NS_SERVER);
	Con_Printf ("Server stopped.\n");
	SV_AttractQuiet (false);	// an error's said
}

/*
================
SV_Shutdown

At exit
================
*/
void SV_Shutdown (void)
{
	SV_Kill ();
	Master_Shutdown ();
	if (svs.logfile)
	{
		fclose (svs.logfile);
		svs.logfile = NULL;
	}
	if (svs.fraglogfile)
	{
		fclose (svs.fraglogfile);
		svs.fraglogfile = NULL;
	}
}

/*
================
SV_Error

Tells the clients the server crashed and ends the game; the host decides
whether the program goes on
================
*/
void SV_Error (char *error, ...)
{
	va_list		argptr;
	static	char		string[1024];
	static	bool inerror = false;

	if (inerror)
		Sys_Error ("SV_Error: recursively entered (%s)", string);

	inerror = true;

	va_start (argptr,error);
	vsnprintf (string,sizeof(string),error,argptr);
	va_end (argptr);

	Con_Printf ("SV_Error: %s\n",string);

	SV_FinalMessage (va("server crashed: %s\n", string));
	SV_Kill ();

	inerror = false;
	Host_Error ("SV_Error: %s\n",string);
}

/*
==================
SV_FinalMessage

Used by SV_Error and SV_Quit_f to send a final message to all connected
clients before the server goes down.  The messages are sent immediately,
not just stuck on the outgoing message list, because the server is going
to totally exit after returning from this function.
==================
*/
static void SV_FinalMessage (char *message)
{
	int			i;
	client_t	*cl;
	
	SZ_Clear (&svs.net_message);
	MSG_WriteByte (&svs.net_message, svc_print);
	MSG_WriteByte (&svs.net_message, PRINT_HIGH);
	MSG_WriteString (&svs.net_message, message);
	MSG_WriteByte (&svs.net_message, svc_disconnect);

	for (i=0, cl = svs.clients ; i<MAX_CLIENTS ; i++, cl++)
		if (cl->state >= cs_connected)
			Netchan_Transmit (&cl->netchan, svs.net_message.cursize
			, svs.net_message.data);
}



/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing.
=====================
*/
void SV_DropClient (client_t *drop)
{
	// add the disconnect
	MSG_WriteByte (&drop->netchan.message, svc_disconnect);
	SV_FreeDeltas (drop);

	if (drop->state == cs_spawned)
	{
		if (!drop->spectator)
		{
			// call the prog function for removing a client
			// this will set the body to a dead frame, among other things
			PR_GLOBAL(self) = EDICT_TO_PROG(drop->edict);
			PR_ExecuteProgram (PR_GLOBAL(ClientDisconnect));
		}
		else if (pr.SpectatorDisconnect)
		{
			// call the prog function for removing a client
			// this will set the body to a dead frame, among other things
			PR_GLOBAL(self) = EDICT_TO_PROG(drop->edict);
			PR_ExecuteProgram (pr.SpectatorDisconnect);
		}
	}

	if (drop->spectator)
		Con_Printf ("Spectator %s removed\n",drop->name);
	else
		Con_Printf ("Client %s removed\n",drop->name);

	if (drop->download)
	{
		fclose (drop->download);
		drop->download = NULL;
	}
	if (drop->upload)
	{
		fclose (drop->upload);
		drop->upload = NULL;
	}
	*drop->uploadfn = 0;

	drop->state = cs_zombie;		// become free in a few seconds
	drop->connection_started = host.realtime;	// for zombie timeout

	drop->old_frags = 0;
	drop->edict->v.frags = 0;
	if (pr.nq && pr.vm)
	{	// cleared for the next in the slot (SV_SetUpClientEdict), as FTE does
		SV_UnlinkEdict (drop->edict);
		memset (&drop->edict->v, 0, QC_FieldWords (pr.vm) * 4);
	}
	drop->name[0] = 0;
	memset (drop->userinfo, 0, sizeof(drop->userinfo));

// send notification to all remaining clients
	SV_FullClientUpdate (drop, &sv.reliable_datagram);
}


//====================================================================

/*
===================
SV_CalcPing

===================
*/
int SV_CalcPing (client_t *cl)
{
	float		ping;
	int			i;
	int			count;
	register	client_frame_t *frame;

	ping = 0;
	count = 0;
	for (frame = cl->frames, i=0 ; i<UPDATE_BACKUP ; i++, frame++)
	{
		if (frame->ping_time > 0)
		{
			ping += frame->ping_time;
			count++;
		}
	}
	if (!count)
		return 9999;
	ping /= count;

	return (int)(ping*1000);
}

/*
===================
SV_FullClientUpdate

Writes all update values to a sizebuf
===================
*/
void SV_FullClientUpdate (client_t *client, sizebuf_t *buf)
{
	int		i;
	char	info[MAX_INFO_STRING];

	i = (int)(client - svs.clients);

//Sys_Printf("SV_FullClientUpdate:  Updated frags for client %d\n", i);

	MSG_WriteByte (buf, svc_updatefrags);
	MSG_WriteByte (buf, i);
	MSG_WriteShort (buf, client->old_frags);
	
	MSG_WriteByte (buf, svc_updateping);
	MSG_WriteByte (buf, i);
	MSG_WriteShort (buf, SV_CalcPing (client));
	
	MSG_WriteByte (buf, svc_updatepl);
	MSG_WriteByte (buf, i);
	MSG_WriteByte (buf, client->lossage);
	
	MSG_WriteByte (buf, svc_updateentertime);
	MSG_WriteByte (buf, i);
	MSG_WriteFloat (buf, (float)(host.realtime - client->connection_started));

	Q_strncpyz (info, client->userinfo, sizeof(info));
	Info_RemovePrefixedKeys (info, '_');	// server passwords, etc

	MSG_WriteByte (buf, svc_updateuserinfo);
	MSG_WriteByte (buf, i);
	MSG_WriteLong (buf, client->userid);
	MSG_WriteString (buf, info);
}

/*
===================
SV_FullClientUpdateToClient

Writes all update values to a client's reliable stream
===================
*/
void SV_FullClientUpdateToClient (client_t *client, client_t *cl)
{
	ClientReliableCheckBlock(cl, 24 + (int)strlen(client->userinfo));
	if (cl->num_backbuf) {
		SV_FullClientUpdate (client, &cl->backbuf);
		ClientReliable_FinishWrite(cl);
	} else
		SV_FullClientUpdate (client, &cl->netchan.message);
}


/*
==============================================================================

CONNECTIONLESS COMMANDS

==============================================================================
*/

/*
================
SVC_Status

Responds with all the info that qplug or qspy can see
This message can be up to around 5k with worst case string lengths.
================
*/
static void SVC_Status (void)
{
	int		i;
	client_t	*cl;
	int		ping;
	int		top, bottom;

	Cmd_TokenizeString ("status");
	SV_BeginRedirect (RD_PACKET);
	Con_Printf ("%s\n", svs.info);
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		cl = &svs.clients[i];
		if ((cl->state == cs_connected || cl->state == cs_spawned ) && !cl->spectator)
		{
			top = atoi(Info_ValueForKey (cl->userinfo, "topcolor"));
			bottom = atoi(Info_ValueForKey (cl->userinfo, "bottomcolor"));
			top = (top < 0) ? 0 : ((top > 13) ? 13 : top);
			bottom = (bottom < 0) ? 0 : ((bottom > 13) ? 13 : bottom);
			ping = SV_CalcPing (cl);
			Con_Printf ("%i %i %i %i \"%s\" \"%s\" %i %i\n", cl->userid, 
				cl->old_frags, (int)(host.realtime - cl->connection_started)/60,
				ping, cl->name, Info_ValueForKey (cl->userinfo, "skin"), top, bottom);
		}
	}
	SV_EndRedirect ();
}

/*
===================
SV_CheckLog

===================
*/
#define	LOG_HIGHWATER	4096
#define	LOG_FLUSH		10*60
static void SV_CheckLog (void)
{
	sizebuf_t	*sz;

	sz = &svs.log[svs.logsequence&1];

	// bump sequence if allmost full, or ten minutes have passed and
	// there is something still sitting there
	if (sz->cursize > LOG_HIGHWATER
	|| (host.realtime - svs.logtime > LOG_FLUSH && sz->cursize) )
	{
		// swap buffers and bump sequence
		svs.logtime = host.realtime;
		svs.logsequence++;
		sz = &svs.log[svs.logsequence&1];
		sz->cursize = 0;
		Con_Printf ("beginning fraglog sequence %i\n", svs.logsequence);
	}

}

/*
================
SVC_Log

Responds with all the logged frags for ranking programs.
If a sequence number is passed as a parameter and it is
the same as the current sequence, an A2A_NACK will be returned
instead of the data.
================
*/
static void SVC_Log (void)
{
	int		seq;
	char	data[MAX_DATAGRAM+64];

	if (Cmd_Argc() == 2)
		seq = atoi(Cmd_Argv(1));
	else
		seq = -1;

	if (seq == svs.logsequence-1 || !svs.fraglogfile)
	{	// they allready have this data, or we aren't logging frags
		data[0] = A2A_NACK;
		NET_SendPacket (NS_SERVER, 1, data, svs.net_from);
		return;
	}

	Con_DPrintf ("sending log %i to %s\n", svs.logsequence-1, NET_AdrToString(svs.net_from));

	snprintf (data, sizeof(data), "stdlog %i\n", svs.logsequence-1);
	Q_strncatz (data, (char *)svs.log_buf[((svs.logsequence-1)&1)], sizeof(data));

	NET_SendPacket (NS_SERVER, (int)strlen(data)+1, data, svs.net_from);
}

/*
================
SVC_Ping

Just responds with an acknowledgement
================
*/
static void SVC_Ping (void)
{
	char	data;

	data = A2A_ACK;

	NET_SendPacket (NS_SERVER, 1, &data, svs.net_from);
}

/*
=================
SVC_GetChallenge

Returns a challenge number that can be used
in a subsequent client_connect command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.
=================
*/
static void SVC_GetChallenge (void)
{
	int		i;
	int		oldest;
	int		oldestTime;
	byte		buf[64];
	sizebuf_t	msg = {0};

	oldest = 0;
	oldestTime = 0x7fffffff;

	// see if we already have a challenge for this ip
	for (i = 0 ; i < MAX_CHALLENGES ; i++)
	{
		if (NET_CompareBaseAdr (svs.net_from, svs.challenges[i].adr))
			break;
		if (svs.challenges[i].time < oldestTime)
		{
			oldestTime = svs.challenges[i].time;
			oldest = i;
		}
	}

	if (i == MAX_CHALLENGES)
	{
		// overwrite the oldest
		svs.challenges[oldest].challenge = ((rand() & 0x7fff) << 16) | (rand() & 0xffff);
		svs.challenges[oldest].adr = svs.net_from;
		svs.challenges[oldest].time = (int)host.realtime;
		i = oldest;
	}

	// send it back, with the protocol extensions this server knows
	msg.data = buf;
	msg.maxsize = sizeof(buf);
	MSG_WriteByte (&msg, S2C_CHALLENGE);
	MSG_WriteString (&msg, va("%i", svs.challenges[i].challenge));
	MSG_WriteLong (&msg, PROTOCOL_VERSION_FTE);
	MSG_WriteLong (&msg, SV_FTE_EXTENSIONS);
	MSG_WriteLong (&msg, PROTOCOL_VERSION_FTE2);
	MSG_WriteLong (&msg, SV_FTE2_EXTENSIONS);
	MSG_WriteLong (&msg, PROTOCOL_VERSION_MVD1);
	MSG_WriteLong (&msg, SV_MVD1_EXTENSIONS);
	Netchan_OutOfBand (NS_SERVER, svs.net_from, msg.cursize, msg.data);
}

/*
==================
SVC_DirectConnect

A connection request that did not come from the master
==================
*/
static void SVC_DirectConnect (void)
{
	char		userinfo[1024];
	static		int	userid;
	netadr_t	adr;
	int			i;
	client_t	*cl, *newcl;
	char		info[MAX_INFO_STRING];
	edict_t		*ent;
	int			edictnum;
	char		*s;
	int			clients, spectators;
	bool	spectator;
	int			qport;
	int			version;
	int			challenge;
	unsigned	magic, fteext, fteext2, mvdext1;

	version = atoi(Cmd_Argv(1));
	if (version != PROTOCOL_VERSION)
	{
		Netchan_OutOfBandPrint (NS_SERVER, svs.net_from, "%c\nServer is version %4.2f.\n", A2C_PRINT, VERSION);
		Con_Printf ("* rejected connect from version %i\n", version);
		return;
	}

	qport = atoi(Cmd_Argv(2));

	challenge = atoi(Cmd_Argv(3));

	// note an extra byte is needed to replace spectator key
	strncpy (userinfo, Cmd_Argv(4), sizeof(userinfo)-2);
	userinfo[sizeof(userinfo) - 2] = 0;

	// the protocol extensions the client asks for, a "0x<magic> 0x<mask>"
	// line per family after the userinfo; the ones this server knows are kept
	fteext = fteext2 = mvdext1 = 0;
	while (!msg_badread)
	{
		Cmd_TokenizeString (MSG_ReadStringLine ());
		magic = (unsigned)strtoul (Cmd_Argv(0), NULL, 0);
		if (magic == PROTOCOL_VERSION_FTE)
			fteext = (unsigned)strtoul (Cmd_Argv(1), NULL, 0) & SV_FTE_EXTENSIONS;
		else if (magic == PROTOCOL_VERSION_FTE2)
			fteext2 = (unsigned)strtoul (Cmd_Argv(1), NULL, 0) & SV_FTE2_EXTENSIONS;
		else if (magic == PROTOCOL_VERSION_MVD1)
			mvdext1 = (unsigned)strtoul (Cmd_Argv(1), NULL, 0) & SV_MVD1_EXTENSIONS;
	}
	msg_badread = false;
	Con_DPrintf ("%s asks for protocol extensions FTE 0x%x, FTE2 0x%x, MVD1 0x%x\n",
		NET_AdrToString (svs.net_from), fteext, fteext2, mvdext1);

	// see if the challenge is valid
	for (i=0 ; i<MAX_CHALLENGES ; i++)
	{
		if (NET_CompareBaseAdr (svs.net_from, svs.challenges[i].adr))
		{
			if (challenge == svs.challenges[i].challenge)
				break;		// good
			Netchan_OutOfBandPrint (NS_SERVER, svs.net_from, "%c\nBad challenge.\n", A2C_PRINT);
			return;
		}
	}
	if (i == MAX_CHALLENGES)
	{
		Netchan_OutOfBandPrint (NS_SERVER, svs.net_from, "%c\nNo challenge for address.\n", A2C_PRINT);
		return;
	}

	if (sv.bigcoords && !(fteext & FTE_PEXT_FLOATCOORDS))
	{
		Con_Printf ("%s: refused, no float coordinates\n", NET_AdrToString (svs.net_from));
		Netchan_OutOfBandPrint (NS_SERVER, svs.net_from, "%c\n%s", A2C_PRINT, SV_BIGCOORDS_REFUSAL);
		return;
	}

	// check for password or spectator_password; a showcase's client watches,
	// whatever it asked (sv_attract.c)
	s = Info_ValueForKey (userinfo, "spectator");
	if (svs.attract && svs.net_from.type == NA_LOOPBACK)
	{
		Info_RemoveKey (userinfo, "spectator");
		Info_SetValueForStarKey (userinfo, "*spectator", "1", MAX_INFO_STRING, SV_InfoCharset ());
		spectator = true;
	}
	else if (s[0] && strcmp(s, "0"))
	{
		if (spectator_password.string[0] && 
			Q_strcasecmp (spectator_password.string, "none") &&
			strcmp(spectator_password.string, s) )
		{	// failed
			Con_Printf ("%s:spectator password failed\n", NET_AdrToString (svs.net_from));
			Netchan_OutOfBandPrint (NS_SERVER, svs.net_from, "%c\nrequires a spectator password\n\n", A2C_PRINT);
			return;
		}
		Info_RemoveKey (userinfo, "spectator"); // remove passwd
		Info_SetValueForStarKey (userinfo, "*spectator", "1", MAX_INFO_STRING, SV_InfoCharset ());
		spectator = true;
	}
	else
	{
		s = Info_ValueForKey (userinfo, "password");
		if (password.string[0] && 
			Q_strcasecmp (password.string, "none") &&
			strcmp(password.string, s) )
		{
			Con_Printf ("%s:password failed\n", NET_AdrToString (svs.net_from));
			Netchan_OutOfBandPrint (NS_SERVER, svs.net_from, "%c\nserver requires a password\n\n", A2C_PRINT);
			return;
		}
		spectator = false;
		Info_RemoveKey (userinfo, "password"); // remove passwd
	}

	adr = svs.net_from;
	userid++;	// so every client gets a unique id

	// works properly
	memset (info, 0, sizeof(info));
	if (!sv_highchars.value) {
		byte *p, *q;

		for (p = (byte *)info, q = (byte *)userinfo;
			*q && p < (byte *)info + sizeof(info)-1; q++)
			if (*q > 31 && *q <= 127)
				*p++ = *q;
	} else
		strncpy (info, userinfo, sizeof(info)-1);

	// if there is allready a slot for this ip, drop it
	for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
	{
		if (cl->state == cs_free)
			continue;
		if (NET_CompareBaseAdr (adr, cl->netchan.remote_address)
			&& ( cl->netchan.qport == qport 
			|| adr.port == cl->netchan.remote_address.port ))
		{
			if (cl->state == cs_connected) {
				Con_Printf("%s:dup connect\n", NET_AdrToString (adr));
				userid--;
				return;
			}

			Con_Printf ("%s:reconnect\n", NET_AdrToString (adr));
			SV_DropClient (cl);
			break;
		}
	}

	// count up the clients and spectators
	clients = 0;
	spectators = 0;
	for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
	{
		if (cl->state == cs_free)
			continue;
		if (cl->spectator)
			spectators++;
		else
			clients++;
	}

	// if at server limits, refuse connection
	if ( maxclients.value > MAX_CLIENTS )
		Cvar_SetValue ("maxclients", MAX_CLIENTS);
	if (maxspectators.value > MAX_CLIENTS)
		Cvar_SetValue ("maxspectators", MAX_CLIENTS);
	if (maxspectators.value + maxclients.value > MAX_CLIENTS)
		Cvar_SetValue ("maxspectators", MAX_CLIENTS - maxspectators.value + maxclients.value);
	if (!svs.attract && ((spectator && spectators >= (int)maxspectators.value)
		|| (!spectator && clients >= (int)maxclients.value)))
	{
		Con_Printf ("%s:full connect\n", NET_AdrToString (adr));
		Netchan_OutOfBandPrint (NS_SERVER, adr, "%c\nserver is full\n\n", A2C_PRINT);
		return;
	}

	// find a client slot
	newcl = NULL;
	for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
	{
		if (cl->state == cs_free)
		{
			newcl = cl;
			break;
		}
	}
	if (!newcl)
	{
		Con_Printf ("WARNING: miscounted available clients\n");
		return;
	}

	
	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	SV_FreeDeltas (newcl);
	memset (newcl, 0, sizeof(*newcl));
	newcl->userid = userid;
	newcl->fteext = fteext;
	newcl->fteext2 = fteext2;
	newcl->mvdext1 = mvdext1;
	memcpy (newcl->userinfo, info, sizeof(newcl->userinfo));
	newcl->z_ext = atoi (Info_ValueForKey (newcl->userinfo, "*z_ext")) & SV_Z_EXTENSIONS;

	Netchan_OutOfBandPrint (NS_SERVER, adr, "%c", S2C_CONNECTION );

	edictnum = (int)((newcl-svs.clients)+1);
	
	Netchan_Setup (&newcl->netchan , adr, qport, NS_SERVER);

	newcl->state = cs_connected;

	newcl->datagram.allowoverflow = true;
	newcl->datagram.data = newcl->datagram_buf;
	newcl->datagram.maxsize = sizeof(newcl->datagram_buf);
	// the encoding changes with serverdata, see SV_New_f

	// spectator mode can ONLY be set at join time
	newcl->spectator = spectator;

	ent = EDICT_NUM(edictnum);	
	newcl->edict = ent;
	
	// parse some info from the info strings
	SV_ExtractFromUserinfo (newcl);

	// JACK: Init the floodprot stuff.
	for (i=0; i<10; i++)
		newcl->whensaid[i] = 0.0;
	newcl->whensaidhead = 0;
	newcl->lockedtill = 0;

	// call the progs to get default spawn parms for the new client
	PR_ExecuteProgram (PR_GLOBAL(SetNewParms));
	for (i=0 ; i<NUM_SPAWN_PARMS ; i++)
		newcl->spawn_parms[i] = PR_PARM(i);

	if (newcl->spectator)
		Con_Printf ("Spectator %s connected\n", newcl->name);
	else
		Con_DPrintf ("Client %s connected\n", newcl->name);
	newcl->sendinfo = true;
}

/*
==================
SV_CanSwitchSide
==================
*/
bool SV_CanSwitchSide (client_t *cl, bool spectator)
{
	client_t	*c;
	int			i, clients, spectators;
	const char	*pw;

	// a password we can't check now: the client must reconnect with it
	pw = spectator ? spectator_password.string : password.string;
	if (pw[0] && Q_strcasecmp (pw, "none"))
	{
		SV_ClientPrintf (cl, PRINT_HIGH, "This server needs a %s password: reconnect as a %s with it set.\n",
			spectator ? "spectator" : "player", spectator ? "spectator" : "player");
		return false;
	}

	clients = spectators = 0;
	for (i=0, c=svs.clients ; i<MAX_CLIENTS ; i++, c++)
	{
		if (c == cl || c->state == cs_free)
			continue;
		if (c->spectator)
			spectators++;
		else
			clients++;
	}
	if (spectator ? spectators >= (int)maxspectators.value : clients >= (int)maxclients.value)
	{
		SV_ClientPrintf (cl, PRINT_HIGH, "All %s slots are taken.\n", spectator ? "spectator" : "player");
		return false;
	}
	return true;
}

static int Rcon_Validate (void)
{
	if (!strlen (rcon_password.string))
		return 0;

	if (strcmp (Cmd_Argv(1), rcon_password.string) )
		return 0;

	return 1;
}

/*
===============
SVC_RemoteCommand

A client issued an rcon command.
Shift down the remaining args
Redirect all printfs
===============
*/
static void SVC_RemoteCommand (void)
{
	int		i;
	char	remaining[1024];


	if (!Rcon_Validate ()) {
		Con_Printf ("Bad rcon from %s:\n%s\n"
			, NET_AdrToString (svs.net_from), svs.net_message.data+4);

		SV_BeginRedirect (RD_PACKET);

		Con_Printf ("Bad rcon_password.\n");

	} else {

		Con_Printf ("Rcon from %s:\n%s\n"
			, NET_AdrToString (svs.net_from), svs.net_message.data+4);

		SV_BeginRedirect (RD_PACKET);

		remaining[0] = 0;

		for (i=2 ; i<Cmd_Argc() ; i++)
		{
			Q_strncatz (remaining, Cmd_Argv(i), sizeof(remaining));
			Q_strncatz (remaining, " ", sizeof(remaining));
		}

		Cmd_ExecuteString (remaining);

	}

	SV_EndRedirect ();
}


/*
=================
SV_ConnectionlessPacket

A connectionless packet has four leading 0xff
characters to distinguish it from a game channel.
Clients that are in the game can still send
connectionless packets.
=================
*/
static void SV_ConnectionlessPacket (void)
{
	char	*s;
	char	*c;

	MSG_BeginReading (&svs.net_message);
	MSG_ReadLong ();		// skip the -1 marker

	s = MSG_ReadStringLine ();

	Cmd_TokenizeString (s);

	c = Cmd_Argv(0);
	Con_DPrintf ("%s: %s\n", NET_AdrToString (svs.net_from), c);

	if (!strcmp(c, "ping") || ( c[0] == A2A_PING && (c[1] == 0 || c[1] == '\n')) )
	{
		SVC_Ping ();
		return;
	}
	if (c[0] == A2A_ACK && (c[1] == 0 || c[1] == '\n') )
	{
		Con_Printf ("A2A_ACK from %s\n", NET_AdrToString (svs.net_from));
		return;
	}
	else if (!strcmp(c,"status"))
	{
		SVC_Status ();
		return;
	}
	else if (!strcmp(c,"log"))
	{
		SVC_Log ();
		return;
	}
	else if (!strcmp(c,"connect"))
	{
		SVC_DirectConnect ();
		return;
	}
	else if (!strcmp(c,"getchallenge"))
	{
		SVC_GetChallenge ();
		return;
	}
	else if (!strcmp(c, "rcon"))
		SVC_RemoteCommand ();
	else
		Con_Printf ("bad connectionless packet from %s:\n%s\n"
		, NET_AdrToString (svs.net_from), s);
}

/*
==============================================================================

PACKET FILTERING
 

You can add or remove addresses from the filter list with:

addip <ip>
removeip <ip>

The ip address is specified in dot format, and any unspecified digits will match any value, so you can specify an entire class C network with "addip 192.246.40".

An address with a prefix length matches its first bits: "addip 2001:db8::/32", "addip 10.0.0.0/8". An IPv6 address without one is the one address.

Removeip will only remove an address specified exactly the same way.  You cannot addip a subnet, then removeip a single host.

listip
Prints the current list of filters.

writeip
Dumps "addip <ip>" commands to listip.cfg so it can be execed at a later date.  The filter lists are not saved and restored by default, because I beleive it would cause too much confusion.

filterban <0 or 1>

If 1 (the default), then ip addresses matching the current list will be prohibited from entering the game.  This is the default setting.

If 0, then only addresses matching the list will be allowed.  This lets you easily set up a private game, or a game that only allows players from your local network.


==============================================================================
*/


// an address matches when its bits under the mask are compare's; an IPv4
// filter's mask holds all of ::ffff: before the address
typedef struct
{
	byte	mask[16];
	byte	compare[16];
} ipfilter_t;

#define	MAX_IPFILTERS	1024

static ipfilter_t	ipfilters[MAX_IPFILTERS];
static int			numipfilters;

static cvar_t	filterban = {.name = "filterban", .string = "1",
	.description = "Whether the IP filters (addip) ban the addresses they match, or let only those in.",
	.values = (const cvar_value_t[]){{"0", "Only matching addresses are heard"},
		{"1", "Matching addresses are banned"}, {0}}};

/*
=================
StringToFilter

IPv4's dotted, its numbers that are 0 or left out matching any (192.246.40);
or an address and the bits of it to match, a.b.c.d/n or IPv6's
2001:db8::/32 (all of an IPv6 address without them)
=================
*/
static bool StringToFilter (const char *s, ipfilter_t *f)
{
	char		copy[64], *slash;
	netadr_t	a = {.type = NA_IP};
	int			bits, value, octet, i;

	memset (f, 0, sizeof(*f));
	Q_strncpyz (copy, s, sizeof(copy));
	slash = strchr (copy, '/');
	if (slash)
		*slash++ = 0;

	if (slash || strchr (copy, ':'))
	{
		if (!NET_ParseIP (copy, a.ip) || (slash && (*slash < '0' || *slash > '9')))
			return false;
		bits = slash ? atoi (slash) + (NET_IsIPv4 (a) ? 96 : 0) : 128;
		if (bits > 128 || (slash && NET_IsIPv4 (a) && bits < 96))
			return false;
		for (i = 0 ; i < 16 ; i++, bits -= 8)
		{
			f->mask[i] = (byte)(bits >= 8 ? 0xff : bits > 0 ? 0xff << (8 - bits) : 0);
			f->compare[i] = a.ip[i] & f->mask[i];
		}
		return true;
	}

	// id's: up to four numbers, those 0 or left out matching any
	NET_SetIPv4 (&a, (const byte[4]){0});
	memset (f->mask, 0xff, 12);
	memcpy (f->compare, a.ip, 12);
	for (s = copy, octet = 0 ; octet < 4 ; octet++)
	{
		if (*s < '0' || *s > '9')
			return false;
		for (value = 0 ; *s >= '0' && *s <= '9' ; s++)
			if (value <= 255)
				value = value * 10 + *s - '0';
		if (value > 255)
			return false;
		f->compare[12 + octet] = (byte)value;
		f->mask[12 + octet] = value ? 0xff : 0;
		if (!*s)
			return true;
		if (*s++ != '.')
			return false;
	}
	return false;
}

// a filter as addip takes it
static const char *FilterToString (const ipfilter_t *f)
{
	static char	s[64];
	netadr_t	a = {.type = NA_IP};
	int			bits = 0, i, b;
	bool		octets = true;		// every mask byte all or none: id's form

	memcpy (a.ip, f->compare, sizeof(a.ip));
	for (i = 0 ; i < 16 ; i++)
	{
		for (b = f->mask[i] ; b ; b &= b - 1)
			bits++;
		if (f->mask[i] && f->mask[i] != 0xff)
			octets = false;
	}
	if (NET_IsIPv4 (a) && bits >= 96 && f->mask[11] == 0xff)
		snprintf (s, sizeof(s), octets ? "%s" : "%s/%i", NET_BaseAdrToString (a), bits - 96);
	else
		snprintf (s, sizeof(s), bits == 128 ? "%s" : "%s/%i", NET_BaseAdrToString (a), bits);
	return s;
}

/*
=================
SV_AddIP_f
=================
*/
static void SV_AddIP_f (void)
{
	ipfilter_t	f;

	if (!StringToFilter (Cmd_Argv(1), &f))
	{
		Con_Printf ("Bad filter address: %s\n", Cmd_Argv(1));
		return;
	}
	if (numipfilters == MAX_IPFILTERS)
	{
		Con_Printf ("IP filter list is full\n");
		return;
	}
	ipfilters[numipfilters++] = f;
}

/*
=================
SV_RemoveIP_f
=================
*/
static void SV_RemoveIP_f (void)
{
	ipfilter_t	f;
	int			i, j;

	if (!StringToFilter (Cmd_Argv(1), &f))
	{
		Con_Printf ("Bad filter address: %s\n", Cmd_Argv(1));
		return;
	}
	for (i=0 ; i<numipfilters ; i++)
		if (!memcmp (&ipfilters[i], &f, sizeof(f)))
		{
			for (j=i+1 ; j<numipfilters ; j++)
				ipfilters[j-1] = ipfilters[j];
			numipfilters--;
			Con_Printf ("Removed.\n");
			return;
		}
	Con_Printf ("Didn't find %s.\n", Cmd_Argv(1));
}

/*
=================
SV_ListIP_f
=================
*/
static void SV_ListIP_f (void)
{
	int		i;

	Con_Printf ("Filter list:\n");
	for (i=0 ; i<numipfilters ; i++)
		Con_Printf ("%s\n", FilterToString (&ipfilters[i]));
}

/*
=================
SV_WriteIP_f
=================
*/
static void SV_WriteIP_f (void)
{
	FILE	*f;
	char	name[MAX_OSPATH];
	int		i;

	snprintf (name, sizeof(name), "%s/listip.cfg", com_gamedir);

	Con_Printf ("Writing %s.\n", name);

	f = fopen (name, "wb");
	if (!f)
	{
		Con_Printf ("Couldn't open %s\n", name);
		return;
	}
	
	for (i=0 ; i<numipfilters ; i++)
		fprintf (f, "addip %s\n", FilterToString (&ipfilters[i]));
	
	fclose (f);
}

/*
=================
SV_SendBan
=================
*/
static void SV_SendBan (void)
{
	char		data[128];

	data[0] = data[1] = data[2] = data[3] = 0xff;
	data[4] = A2C_PRINT;
	data[5] = 0;
	Q_strncatz (data, "\nbanned.\n", sizeof(data));

	NET_SendPacket (NS_SERVER, (int)strlen(data), data, svs.net_from);
}

/*
=================
SV_FilterPacket
=================
*/
static bool SV_FilterPacket (void)
{
	int		i, j;

	for (i=0 ; i<numipfilters ; i++)
	{
		for (j = 0 ; j < 16 && (svs.net_from.ip[j] & ipfilters[i].mask[j]) == ipfilters[i].compare[j] ; j++)
			;
		if (j == 16)
			return filterban.value;
	}

	return !filterban.value;
}

//============================================================================

/*
=================
SV_ReadPackets
=================
*/
static void SV_ReadPackets (void)
{
	int			i;
	client_t	*cl;
	int			qport;

	while (NET_GetPacket (NS_SERVER, &svs.net_from, &svs.net_message))
	{
		if (SV_FilterPacket ())
		{
			SV_SendBan ();	// tell them we aren't listening...
			continue;
		}

		// check for connectionless packet (0xffffffff) first
		if (*(int *)svs.net_message.data == -1)
		{
			SV_ConnectionlessPacket ();
			continue;
		}
		
		// read the qport out of the message so we can fix up
		// stupid address translating routers
		MSG_BeginReading (&svs.net_message);
		MSG_ReadLong ();		// sequence number
		MSG_ReadLong ();		// sequence number
		qport = MSG_ReadShort () & 0xffff;

		// check for packets from connected clients
		for (i=0, cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
		{
			if (cl->state == cs_free)
				continue;
			if (!NET_CompareBaseAdr (svs.net_from, cl->netchan.remote_address))
				continue;
			if (cl->netchan.qport != qport)
				continue;
			if (cl->netchan.remote_address.port != svs.net_from.port)
			{
				Con_DPrintf ("SV_ReadPackets: fixing up a translated port\n");
				cl->netchan.remote_address.port = svs.net_from.port;
			}
			if (Netchan_Process (&cl->netchan, svs.net_from, &svs.net_message))
			{	// this is a valid, sequenced packet, so process it
				svs.stats.packets++;
				cl->send_message = true;	// reply at end of frame
				if (cl->state != cs_zombie)
					SV_ExecuteClientMessage (cl);
			}
			break;
		}
		
		if (i != MAX_CLIENTS)
			continue;
	
		// packet is not from a known client
		//	Con_Printf ("%s:sequenced packet without connection\n"
		// ,NET_AdrToString(net_from));
	}
}

/*
==================
SV_CheckTimeouts

If a packet has not been received from a client in timeout.value
seconds, drop the conneciton.

When a client is normally dropped, the client_t goes into a zombie state
for a few seconds to make sure any final reliable message gets resent
if necessary
==================
*/
static void SV_CheckTimeouts (void)
{
	int		i;
	client_t	*cl;
	float	droptime;
	int	nclients;
	
	droptime = (float)(host.realtime - timeout.value);
	nclients = 0;

	for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
	{
		if (cl->state == cs_connected || cl->state == cs_spawned) {
			if (!cl->spectator)
				nclients++;
			if (cl->netchan.last_received < droptime
				&& cl->netchan.remote_address.type != NA_LOOPBACK) {
				SV_BroadcastPrintf (PRINT_HIGH, "%s timed out\n", cl->name);
				SV_DropClient (cl); 
				cl->state = cs_free;	// don't bother with zombie state
			}
		}
		if (cl->state == cs_zombie && 
			host.realtime - cl->connection_started > zombietime.value)
		{
			cl->state = cs_free;	// can now be reused
		}
	}
	if (sv.paused && !nclients) {
		// nobody left, unpause the server
		SV_TogglePause("Pause released since no players are left.\n");
	}
}

/*
===================
SV_CheckVars

===================
*/
static void SV_CheckVars (void)
{
	static char *pw, *spw;
	int			v;

	// the air step works best with the ground found by landing (mvdsv)
	if (!pm_airstep.value != !pm_pground.value)
		Cvar_Set ("pm_pground", pm_airstep.value ? "1" : "");

	if (password.string == pw && spectator_password.string == spw)
		return;
	pw = password.string;
	spw = spectator_password.string;

	v = 0;
	if (pw && pw[0] && strcmp(pw, "none"))
		v |= 1;
	if (spw && spw[0] && strcmp(spw, "none"))
		v |= 2;

	Con_Printf ("Updated needpass.\n");
	if (!v)
		Info_SetValueForKey (svs.info, "needpass", "", MAX_SERVERINFO_STRING, SV_InfoCharset ());
	else
		Info_SetValueForKey (svs.info, "needpass", va("%i",v), MAX_SERVERINFO_STRING, SV_InfoCharset ());
}

/*
==================
SV_HeldStill

A game of one, a listen server's for its own player alone, holds still
while that player is away (the menu or the console has the keys), as FTE's
PAUSE_AUTO: no plaque, nothing sent of it
==================
*/
static bool SV_HeldStill (bool away)
{
	client_t	*cl;
	int			i, players = 0;

	if (!away || host.dedicated || maxclients.value != 1 || svs.attract)
		return false;	// a showcase plays on under the menu
	for (i = 0, cl = svs.clients ; i < MAX_CLIENTS ; i++, cl++)
	{
		if (cl->state == cs_free)
			continue;
		if (cl->state != cs_spawned || cl->netchan.remote_address.type != NA_LOOPBACK)
			return false;
		players++;
	}
	return players == 1;
}

/*
==================
SV_Frame

==================
*/
void SV_Frame (double time, bool away)
{
	static double	start, end;
	bool			still;

	start = Sys_DoubleTime ();
	svs.stats.idle += start - end;
	SV_AttractQuiet (true);

// keep the random time dependent
	rand ();

// decide the simulation time
	still = sv.paused || SV_HeldStill (away);
	if (!still)
		sv.time += time;

// check timeouts
	SV_CheckTimeouts ();

// toggle the log buffer if full
	SV_CheckLog ();

// NetQuake's players move in the world's frame, by the moves just read, as
// NetQuake has it; QuakeWorld's as their moves come
	if (pr.nq)
		SV_ReadPackets ();

// move autonomous things around if enough time has passed
	if (!still)
		SV_Physics ();

// get packets
	if (!pr.nq)
		SV_ReadPackets ();

	SV_CheckVars ();

// send messages back to the clients that had packets read this frame
	SV_SendClientMessages ();

// send a heartbeat to the master if needed; a showcase is no one's to join
	if (!svs.attract)
	{
		Master_Heartbeat ();
		SV_WebRTCInfo ();
	}
	SV_AttractQuiet (false);

// collect timing statistics
	end = Sys_DoubleTime ();
	svs.stats.active += end-start;
	if (++svs.stats.count == STATFRAMES)
	{
		svs.stats.latched_active = svs.stats.active;
		svs.stats.latched_idle = svs.stats.idle;
		svs.stats.latched_packets = svs.stats.packets;
		svs.stats.active = 0;
		svs.stats.idle = 0;
		svs.stats.packets = 0;
		svs.stats.count = 0;
	}
}

/*
===============
SV_InitLocal
===============
*/
/*
===============
SV_InfoCharset

Which characters values stored in server-side info strings may use.
===============
*/
info_charset_t SV_InfoCharset (void)
{
	return sv_highchars.value ? INFO_CHARSET_ANY : INFO_CHARSET_ASCII;
}

/*
===============
SV_ServerinfoCvarChanged

Cvars flagged as info are mirrored into the serverinfo string.
===============
*/
static void SV_ServerinfoCvarChanged (char *name, char *value)
{
	Info_SetValueForKey (svs.info, name, value, MAX_SERVERINFO_STRING, SV_InfoCharset ());
	SV_SendServerInfoChange (name, value);
}

static void SV_InitLocal (void)
{
	int		i;

	svs.floodprot.messages = 4;
	svs.floodprot.persecond = 4;
	svs.floodprot.secondsdead = 10;
	extern	cvar_t	sv_maxvelocity;
	extern	cvar_t	sv_gravity;
	extern	cvar_t	sv_aim;
	extern	cvar_t	sv_stopspeed;
	extern	cvar_t	sv_spectatormaxspeed;

	Cvar_SetServerinfoHook (SV_ServerinfoCvarChanged);
	Con_AddPrintSink (SV_LogPrint);
	extern	cvar_t	sv_accelerate;
	extern	cvar_t	sv_airaccelerate;
	extern	cvar_t	sv_wateraccelerate;
	extern	cvar_t	sv_friction;
	extern	cvar_t	sv_waterfriction;

	SV_InitOperatorCommands	();
	SV_UserInit ();
	
	Cvar_RegisterVariable (&spectator_password);

	Cvar_RegisterVariable (&sv_mintic);
	Cvar_RegisterVariable (&sv_maxtic);
	Cvar_RegisterVariable (&sv_bigcoords);
	Cvar_RegisterVariable (&sv_replacementdeltas);
	Cvar_RegisterVariable (&sv_websocket);
	Cvar_RegisterVariable (&sv_public);
	Cvar_RegisterVariable (&sv_webrtc_room);
	SV_MVDInit ();
	Cvar_RegisterVariable (&sv_maxrate);
	Cvar_RegisterVariable (&sv_maxdrate);
	Cvar_RegisterVariable (&pm_ktjump);
	Cvar_RegisterVariable (&pm_bunnyspeedcap);
	Cvar_RegisterVariable (&pm_slidefix);
	Cvar_RegisterVariable (&pm_airstep);
	Cvar_RegisterVariable (&pm_pground);
	Cvar_RegisterVariable (&pm_rampjump);
	Cvar_RegisterVariable (&sv_maxpitch);
	Cvar_RegisterVariable (&sv_minpitch);

	Cvar_RegisterVariable (&fraglimit);
	Cvar_RegisterVariable (&timelimit);
	Cvar_RegisterVariable (&teamplay);
	Cvar_RegisterVariable (&samelevel);
	Cvar_RegisterVariable (&maxclients);
	Cvar_RegisterVariable (&maxspectators);
	Cvar_RegisterVariable (&hostname);
	Cvar_RegisterVariable (&deathmatch);
	Cvar_RegisterVariable (&coop);
	Cvar_RegisterVariable (&skill);
	Cvar_RegisterVariable (&spawn);
	Cvar_RegisterVariable (&watervis);


	Cvar_RegisterVariable (&timeout);
	Cvar_RegisterVariable (&zombietime);

	Cvar_RegisterVariable (&sv_maxvelocity);
	Cvar_RegisterVariable (&sv_gravity);
	Cvar_RegisterVariable (&sv_stopspeed);
	Cvar_RegisterVariable (&sv_maxspeed);
	Cvar_RegisterVariable (&sv_spectatormaxspeed);
	Cvar_RegisterVariable (&sv_accelerate);
	Cvar_RegisterVariable (&sv_airaccelerate);
	Cvar_RegisterVariable (&sv_wateraccelerate);
	Cvar_RegisterVariable (&sv_friction);
	Cvar_RegisterVariable (&sv_waterfriction);

	Cvar_RegisterVariable (&sv_aim);

	Cvar_RegisterVariable (&filterban);
	
	Cvar_RegisterVariable (&sv_csqc_progname);
	Cvar_RegisterVariable (&allow_download);
	Cvar_RegisterVariable (&allow_download_skins);
	Cvar_RegisterVariable (&allow_download_models);
	Cvar_RegisterVariable (&allow_download_sounds);
	Cvar_RegisterVariable (&allow_download_maps);

	Cvar_RegisterVariable (&sv_highchars);

	Cvar_RegisterVariable (&sv_phs);

	Cvar_RegisterVariable (&pausable);

	Cmd_AddCommand ("addip", SV_AddIP_f, "Adds an IP filter, which filterban makes a ban or an allowance: "
		"IPv4's octets that are 0 or left out match any value, and an address/bits its first bits "
		"(2001:db8::/32). Usage: addip <ip>[/bits]");
	Cmd_AddCommand ("removeip", SV_RemoveIP_f, "Removes an IP filter, given exactly as it was added. "
		"Usage: removeip <ip>");
	Cmd_AddCommand ("listip", SV_ListIP_f, "Lists the IP filters.");
	Cmd_AddCommand ("writeip", SV_WriteIP_f, "Writes the IP filters to listip.cfg in the game directory, "
		"as addip commands to exec.");

	for (i=0 ; i<MAX_MODELS ; i++)
		snprintf (svs.localmodels[i], sizeof(svs.localmodels[i]), "*%i", i);

	Info_SetValueForStarKey (svs.info, "*version", va("%4.2f", VERSION), MAX_SERVERINFO_STRING, SV_InfoCharset ());
	Info_SetValueForStarKey (svs.info, "*z_ext", va("%i", SV_Z_EXTENSIONS), MAX_SERVERINFO_STRING, SV_InfoCharset ());

	// init fraglog stuff
	svs.logsequence = 1;
	svs.logtime = host.realtime;
	svs.log[0].data = svs.log_buf[0];
	svs.log[0].maxsize = sizeof(svs.log_buf[0]);
	svs.log[0].cursize = 0;
	svs.log[0].allowoverflow = true;
	svs.log[1].data = svs.log_buf[1];
	svs.log[1].maxsize = sizeof(svs.log_buf[1]);
	svs.log[1].cursize = 0;
	svs.log[1].allowoverflow = true;
}


//============================================================================

/*
================
Master_Heartbeat

Send a message to the master every few minutes to
let it know we are alive, and log information
================
*/
#define	HEARTBEAT_SECONDS	300
static void Master_Heartbeat (void)
{
	char		string[2048];
	int			active;
	int			i;

	if (host.realtime - svs.last_heartbeat < HEARTBEAT_SECONDS)
		return;		// not time to send yet

	svs.last_heartbeat = host.realtime;

	//
	// count active users
	//
	active = 0;
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (svs.clients[i].state == cs_connected ||
		svs.clients[i].state == cs_spawned )
			active++;

	svs.heartbeat_sequence++;
	snprintf (string, sizeof(string), "%c\n%i\n%i\n", S2M_HEARTBEAT,
		svs.heartbeat_sequence, active);


	// send to group master
	for (i=0 ; i<MAX_MASTERS ; i++)
		if (svs.master_adr[i].port)
		{
			Con_Printf ("Sending heartbeat to %s\n", NET_AdrToString (svs.master_adr[i]));
			NET_SendPacket (NS_SERVER, (int)strlen(string), string, svs.master_adr[i]);
		}
}

/*
================
SV_WebRTCInfo

What the server tells its WebRTC broker it is, for the broker's list: the
serverinfo, and FTE's keys for the rest. Made every few seconds; the broker
hears it every 30.
================
*/
static void SV_WebRTCInfo (void)
{
	static double	made = -1000;
	char			info[1024];
	const char		*gamedir;
	int				i, clients = 0;

	if (!sv_public.value || host.realtime - made < 5)
		return;
	made = host.realtime;
	for (i = 0 ; i < MAX_CLIENTS ; i++)
		if ((svs.clients[i].state == cs_connected || svs.clients[i].state == cs_spawned) && !svs.clients[i].spectator)
			clients++;
	gamedir = Info_ValueForKey (svs.info, "*gamedir");
	Q_strncpyz (info, svs.info, sizeof(info));
	Info_SetValueForKey (info, "clients", va("%i", clients), sizeof(info), INFO_CHARSET_ANY);
	Info_SetValueForKey (info, "mapname", sv.name, sizeof(info), INFO_CHARSET_ANY);
	Info_SetValueForKey (info, "modname", *gamedir ? gamedir : "qw", sizeof(info), INFO_CHARSET_ANY);
	Info_SetValueForKey (info, "protocol", va("%i", PROTOCOL_VERSION), sizeof(info), INFO_CHARSET_ANY);
	NET_RTCInfo (info);
}

/*
=================
Master_Shutdown

Informs all masters that this server is going down
=================
*/
static void Master_Shutdown (void)
{
	char		string[2048];
	int			i;

	snprintf (string, sizeof(string), "%c\n", S2M_SHUTDOWN);

	// send to group master
	for (i=0 ; i<MAX_MASTERS ; i++)
		if (svs.master_adr[i].port)
		{
			Con_Printf ("Sending heartbeat to %s\n", NET_AdrToString (svs.master_adr[i]));
			NET_SendPacket (NS_SERVER, (int)strlen(string), string, svs.master_adr[i]);
		}
}

/*
==================
SV_BoundRate

A client's rate, at least 500 and at most sv_maxrate
==================
*/
int SV_BoundRate (int rate)
{
	if (sv_maxrate.value > 0 && rate > sv_maxrate.value)
		rate = (int)sv_maxrate.value;
	if (rate < 500)
		rate = 500;
	return rate;
}

/*
==================
SV_SetChannelRate

The client's rate, or while it downloads sv_maxdrate (FTE)
==================
*/
void SV_SetChannelRate (client_t *cl)
{
	if (!cl->download)
		cl->netchan.rate = cl->rate;
	else
		cl->netchan.rate = sv_maxdrate.value > 0 ? 1.0 / sv_maxdrate.value : 0;
}

/*
=================
SV_ExtractFromUserinfo

Pull specific info from a newly changed userinfo string
into a more C freindly form.
=================
*/
void SV_ExtractFromUserinfo (client_t *cl)
{
	char	*val, *p, *q;
	int		i;
	client_t	*client;
	int		dupc = 1;
	char	newname[80];


	// name for C code
	val = Info_ValueForKey (cl->userinfo, "name");

	// trim user name
	strncpy(newname, val, sizeof(newname) - 1);
	newname[sizeof(newname) - 1] = 0;

	for (p = newname; (*p == ' ' || *p == '\r' || *p == '\n') && *p; p++)
		;

	if (p != newname && !*p) {
		//white space only
		Q_strncpyz(newname, "unnamed", sizeof(newname));
		p = newname;
	}

	if (p != newname && *p) {
		for (q = newname; *p; *q++ = *p++)
			;
		*q = 0;
	}
	for (p = newname + strlen(newname) - 1; p != newname && (*p == ' ' || *p == '\r' || *p == '\n') ; p--)
		;
	p[1] = 0;

	if (strcmp(val, newname)) {
		Info_SetValueForKey (cl->userinfo, "name", newname, MAX_INFO_STRING, SV_InfoCharset ());
		val = Info_ValueForKey (cl->userinfo, "name");
	}

	if (!val[0] || !Q_strcasecmp (val, "console")) {
		Info_SetValueForKey (cl->userinfo, "name", "unnamed", MAX_INFO_STRING, SV_InfoCharset ());
		val = Info_ValueForKey (cl->userinfo, "name");
	}

	// check to see if another user by the same name exists
	while (1) {
		for (i=0, client = svs.clients ; i<MAX_CLIENTS ; i++, client++) {
			if (client->state != cs_spawned || client == cl)
				continue;
			if (!Q_strcasecmp (client->name, val))
				break;
		}
		if (i != MAX_CLIENTS) { // dup name
			if (strlen(val) > sizeof(cl->name) - 1)
				val[sizeof(cl->name) - 4] = 0;
			p = val;

			if (val[0] == '(')
			{
				if (val[2] == ')')
					p = val + 3;
				else if (val[3] == ')')
					p = val + 4;
			}

			snprintf(newname, sizeof(newname), "(%d)%-.40s", dupc++, p);
			Info_SetValueForKey (cl->userinfo, "name", newname, MAX_INFO_STRING, SV_InfoCharset ());
			val = Info_ValueForKey (cl->userinfo, "name");
		} else
			break;
	}
	
	if (strncmp(val, cl->name, strlen(cl->name))) {
		if (!sv.paused) {
			if (!cl->lastnametime || host.realtime - cl->lastnametime > 5) {
				cl->lastnamecount = 0;
				cl->lastnametime = (float)host.realtime;
			} else if (cl->lastnamecount++ > 4) {
				SV_BroadcastPrintf (PRINT_HIGH, "%s was kicked for name spam\n", cl->name);
				SV_ClientPrintf (cl, PRINT_HIGH, "You were kicked from the game for name spamming\n");
				SV_DropClient (cl); 
				return;
			}
		}
				
		if (cl->state >= cs_spawned && !cl->spectator)
			SV_BroadcastPrintf (PRINT_HIGH, "%s changed name to %s\n", cl->name, val);
	}


	strncpy (cl->name, val, sizeof(cl->name)-1);	

	// rate command; vanilla's 2500 without one
	val = Info_ValueForKey (cl->userinfo, "rate");
	cl->rate = 1.0 / SV_BoundRate (*val ? atoi (val) : 2500);

	// msg command
	val = Info_ValueForKey (cl->userinfo, "msg");
	if (strlen(val))
	{
		cl->messagelevel = atoi(val);
	}

}


//============================================================================

/*
====================
SV_InitNet
====================
*/
static void SV_InitNet (void)
{
	int	port;
	int	p;

	port = PORT_SERVER;
	p = COM_CheckParm ("-port");
	if (p && p + 1 < com_argc)
	{
		port = atoi(com_argv[p+1]);
		Con_Printf ("Port: %i\n", port);
	}
	svs.port = port;
	svs.net_message.data = svs.net_message_buf;
	svs.net_message.maxsize = sizeof(svs.net_message_buf);

	svs.last_heartbeat = -99999;		// send immediately
}


/*
====================
SV_Init
====================
*/
void SV_Init (void)
{
	PR_Init ();
	SV_InitNet ();
	SV_InitLocal ();
}
