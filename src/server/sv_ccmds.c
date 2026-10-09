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

bool	sv_allow_cheats;

extern cvar_t cl_warncmd;


/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

/*
====================
SV_SetMaster_f

Make a master server current
====================
*/
static void SV_SetMaster_f (void)
{
	char	data[2];
	int		i;

	memset (&svs.master_adr, 0, sizeof(svs.master_adr));

	for (i=1 ; i<Cmd_Argc() ; i++)
	{
		if (!strcmp(Cmd_Argv(i), "none") || !NET_StringToAdr (Cmd_Argv(i), &svs.master_adr[i-1]))
		{
			Con_Printf ("Setting nomaster mode.\n");
			return;
		}
		if (svs.master_adr[i-1].port == 0)
			svs.master_adr[i-1].port = BigShort (27000);

		Con_Printf ("Master server at %s\n", NET_AdrToString (svs.master_adr[i-1]));

		Con_Printf ("Sending a ping.\n");

		data[0] = A2A_PING;
		data[1] = 0;
		NET_SendPacket (NS_SERVER, 2, data, svs.master_adr[i-1]);
	}

	svs.last_heartbeat = -99999;
}


/*
==================
SV_KillServer_f
==================
*/
static void SV_KillServer_f (void)
{
	SV_AttractStop ();
	if (!SV_Active ())
		Con_Printf ("No map is running\n");
	SV_Kill ();
}

/*
==================
SV_HasLocalClient

Whether the client in this process is on the server
==================
*/
static bool SV_HasLocalClient (void)
{
	int		i;

	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (svs.clients[i].state != cs_free && svs.clients[i].netchan.remote_address.type == NA_LOOPBACK)
			return true;
	return false;
}

/*
============
SV_Logfile_f
============
*/
static void SV_Logfile_f (void)
{
	char	name[MAX_OSPATH];

	if (svs.logfile)
	{
		Con_Printf ("File logging off.\n");
		fclose (svs.logfile);
		svs.logfile = NULL;
		return;
	}

	snprintf (name, sizeof(name), "%s/qconsole.log", com_gamedir);
	Con_Printf ("Logging text to %s.\n", name);
	svs.logfile = fopen (name, "w");
	if (!svs.logfile)
		Con_Printf ("failed.\n");
}


/*
============
SV_Fraglogfile_f
============
*/
static void SV_Fraglogfile_f (void)
{
	char	name[MAX_OSPATH];
	int		i;

	if (svs.fraglogfile)
	{
		Con_Printf ("Frag file logging off.\n");
		fclose (svs.fraglogfile);
		svs.fraglogfile = NULL;
		return;
	}

	// find an unused name
	for (i=0 ; i<1000 ; i++)
	{
		snprintf (name, sizeof(name), "%s/frag_%i.log", com_gamedir, i);
		svs.fraglogfile = fopen (name, "r");
		if (!svs.fraglogfile)
		{	// can't read it, so create this one
			svs.fraglogfile = fopen (name, "w");
			if (!svs.fraglogfile)
				i=1000;	// give error
			break;
		}
		fclose (svs.fraglogfile);
	}
	if (i==1000)
	{
		Con_Printf ("Can't open any logfiles.\n");
		svs.fraglogfile = NULL;
		return;
	}

	Con_Printf ("Logging frags to %s.\n", name);
}


/*
==================
SV_SetPlayer

Sets host_client and sv_player to the player with idnum Cmd_Argv(1)
==================
*/
static bool SV_SetPlayer (void)
{
	client_t	*cl;
	int			i;
	int			idnum;

	idnum = atoi(Cmd_Argv(1));

	for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
	{
		if (!cl->state)
			continue;
		if (cl->userid == idnum)
		{
			host_client = cl;
			sv_player = host_client->edict;
			return true;
		}
	}
	Con_Printf ("Userid %i is not on the server\n", idnum);
	return false;
}


/*
==================
SV_God_f

Sets client to godmode
==================
*/
static void SV_God_f (void)
{
	if (!sv_allow_cheats)
	{
		Con_Printf ("You must run the server with -cheats to enable this command.\n");
		return;
	}

	if (!SV_SetPlayer ())
		return;

	sv_player->v.flags = (float)((int)sv_player->v.flags ^ FL_GODMODE);
	if (!((int)sv_player->v.flags & FL_GODMODE) )
		SV_ClientPrintf (host_client, PRINT_HIGH, "godmode OFF\n");
	else
		SV_ClientPrintf (host_client, PRINT_HIGH, "godmode ON\n");
}


static void SV_Noclip_f (void)
{
	if (!sv_allow_cheats)
	{
		Con_Printf ("You must run the server with -cheats to enable this command.\n");
		return;
	}

	if (!SV_SetPlayer ())
		return;

	if (sv_player->v.movetype != MOVETYPE_NOCLIP)
	{
		sv_player->v.movetype = MOVETYPE_NOCLIP;
		SV_ClientPrintf (host_client, PRINT_HIGH, "noclip ON\n");
	}
	else
	{
		sv_player->v.movetype = MOVETYPE_WALK;
		SV_ClientPrintf (host_client, PRINT_HIGH, "noclip OFF\n");
	}
}


/*
==================
SV_Give_f
==================
*/
static void SV_Give_f (void)
{
	char	*t;
	int		v;
	
	if (!sv_allow_cheats)
	{
		Con_Printf ("You must run the server with -cheats to enable this command.\n");
		return;
	}
	
	if (!SV_SetPlayer ())
		return;

	t = Cmd_Argv(2);
	v = atoi (Cmd_Argv(3));
	
	switch (t[0])
	{
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		sv_player->v.items = (float)((int)sv_player->v.items | IT_SHOTGUN<< (t[0] - '2'));
		break;
	
	case 's':
		sv_player->v.ammo_shells = (float)v;
		break;		
	case 'n':
		sv_player->v.ammo_nails = (float)v;
		break;		
	case 'r':
		sv_player->v.ammo_rockets = (float)v;
		break;		
	case 'h':
		sv_player->v.health = (float)v;
		break;		
	case 'c':
		sv_player->v.ammo_cells = (float)v;
		break;		
	}
}


/*
======================
SV_GotoLevel

The server on a level, taking the connected clients along with their spawn
parms as parms says: map's, changelevel's and restart's
======================
*/
void SV_GotoLevel (const char *name, spawnparms_t parms, cmap_t *built)
{
	char	level[MAX_QPATH];
	char	expanded[MAX_QPATH];
	FILE	*f;

	Q_strncpyz (level, name, sizeof(level));		// name may be sv.name, which spawning clears

	// check to make sure the level exists
	snprintf (expanded, sizeof(expanded), "maps/%s.bsp", level);
	COM_FOpenFile (expanded, &f);
	if (!f)
	{
		Con_Printf ("Can't find %s\n", expanded);
		if (built)
			CM_DiscardMap (built);
		return;
	}
	fclose (f);

	SV_BroadcastCommand ("changing\n");
	SV_SendMessagesToAll ();

	SV_SpawnServer (level, parms, built);

	SV_BroadcastCommand ("reconnect\n");

	// the player of a listen server joins the game
	if (!host.dedicated && !SV_HasLocalClient ())
		Cbuf_AddText ("connect local\n");
}

/*
======================
SV_Map_f

map <mapname>: as FTE's, a new game on the level for NetQuake's progs and
when no level runs (SetNewParms, no serverflags); QuakeWorld's keep their
players' parms (SetChangeParms), as QuakeWorld's map always did
======================
*/
static void SV_Map_f (void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf ("map <levelname> : start a game on a level\n");
		return;
	}
	SV_AttractStop ();
	SV_GotoLevel (Cmd_Argv(1), sv.state == ss_dead || pr.nq ? SPAWNPARMS_NEW : SPAWNPARMS_CHANGE, NULL);
}

/*
======================
SV_Changelevel_f

changelevel <mapname>: on to the level, the players with the parms the progs
gives them for it (SetChangeParms); what QuakeC's changelevel does
======================
*/
static void SV_Changelevel_f (void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf ("changelevel <levelname> : continue the game on a new level\n");
		return;
	}
	SV_AttractStop ();
	SV_GotoLevel (Cmd_Argv(1), sv.state == ss_dead ? SPAWNPARMS_NEW : SPAWNPARMS_CHANGE, NULL);
}

/*
======================
SV_Restart_f

restart: the level again, the players with the parms they began it with, as
NetQuake's; id1's progs restart a single player's game so when the player dies
======================
*/
static void SV_Restart_f (void)
{
	SV_AttractStop ();		// a showcase's level is no game to restart
	if (sv.state == ss_dead)
		return;
	SV_GotoLevel (sv.name, SPAWNPARMS_KEEP, NULL);
}


/*
==================
SV_Kick_f

Kick a user off of the server
==================
*/
static void SV_Kick_f (void)
{
	int			i;
	client_t	*cl;
	int			uid;

	uid = atoi(Cmd_Argv(1));
	
	for (i = 0, cl = svs.clients; i < MAX_CLIENTS; i++, cl++)
	{
		if (!cl->state)
			continue;
		if (cl->userid == uid)
		{
			SV_BroadcastPrintf (PRINT_HIGH, "%s was kicked\n", cl->name);
			// print directly, because the dropped client won't get the
			// SV_BroadcastPrintf message
			SV_ClientPrintf (cl, PRINT_HIGH, "You were kicked from the game\n");
			SV_DropClient (cl); 
			return;
		}
	}

	Con_Printf ("Couldn't find user number %i\n", uid);
}


/*
================
SV_Status_f
================
*/
static void SV_Status_f (void)
{
	int			i, j, l;
	client_t	*cl;
	float		cpu, avg, pak;
	char		*s;


	cpu = (float)(svs.stats.latched_active+svs.stats.latched_idle);
	if (cpu)
		cpu = (float)(100*svs.stats.latched_active/cpu);
	avg = (float)(1000*svs.stats.latched_active / STATFRAMES);
	pak = (float)svs.stats.latched_packets/ STATFRAMES;

	Con_Printf ("net address      : %s\n",NET_AdrToString (NET_SocketAddress (NS_SERVER)));
	Con_Printf ("cpu utilization  : %3i%%\n",(int)cpu);
	Con_Printf ("avg response time: %i ms\n",(int)avg);
	Con_Printf ("packets/frame    : %5.2f\n", pak);
	
// min fps lat drp
	if (svs.redirected != RD_NONE) {
		// most remote clients are 40 columns
		//           0123456789012345678901234567890123456789
		Con_Printf ("name               userid frags\n");
        Con_Printf ("  address          rate ping drop\n");
		Con_Printf ("  ---------------- ---- ---- -----\n");
		for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
		{
			if (!cl->state)
				continue;

			Con_Printf ("%-16.16s  ", cl->name);

			Con_Printf ("%6i %5i", cl->userid, (int)cl->edict->v.frags);
			if (cl->spectator)
				Con_Printf(" (s)\n");
			else			
				Con_Printf("\n");

			s = NET_BaseAdrToString ( cl->netchan.remote_address);
			// an address too long for its column (IPv6's) has a line of its own
			if (strlen (s) > 16)
				Con_Printf ("  %s\n%18s", s, "");
			else
				Con_Printf ("  %-16.16s", s);
			if (cl->state == cs_connected)
			{
				Con_Printf ("CONNECTING\n");
				continue;
			}
			if (cl->state == cs_zombie)
			{
				Con_Printf ("ZOMBIE\n");
				continue;
			}
			Con_Printf ("%4i %4i %5.2f\n"
				, (int)(1000*cl->netchan.frame_rate)
				, (int)SV_CalcPing (cl)
				, 100.0*cl->netchan.drop_count / cl->netchan.incoming_sequence);
		}
	} else {
		Con_Printf ("frags userid address         name            rate ping drop  qport\n");
		Con_Printf ("----- ------ --------------- --------------- ---- ---- ----- -----\n");
		for (i=0,cl=svs.clients ; i<MAX_CLIENTS ; i++,cl++)
		{
			if (!cl->state)
				continue;
			Con_Printf ("%5i %6i ", (int)cl->edict->v.frags,  cl->userid);

			s = NET_BaseAdrToString ( cl->netchan.remote_address);
			Con_Printf ("%s", s);
			l = (int)(16 - strlen(s));
			for (j=0 ; j<l ; j++)
				Con_Printf (" ");
			
			Con_Printf ("%s", cl->name);
			l = (int)(16 - strlen(cl->name));
			for (j=0 ; j<l ; j++)
				Con_Printf (" ");
			if (cl->state == cs_connected)
			{
				Con_Printf ("CONNECTING\n");
				continue;
			}
			if (cl->state == cs_zombie)
			{
				Con_Printf ("ZOMBIE\n");
				continue;
			}
			Con_Printf ("%4i %4i %3.1f %4i"
				, (int)(1000*cl->netchan.frame_rate)
				, (int)SV_CalcPing (cl)
				, 100.0*cl->netchan.drop_count / cl->netchan.incoming_sequence
				, cl->netchan.qport);
			if (cl->spectator)
				Con_Printf(" (s)\n");
			else			
				Con_Printf("\n");

				
		}
	}
	Con_Printf ("\n");
}

/*
==================
SV_ConSay_f
==================
*/
static void SV_ConSay_f(void)
{
	client_t *client;
	int		j;
	char	*p;
	char	text[1024];

	if (Cmd_Argc () < 2)
		return;

	Q_strncpyz (text, "console: ", sizeof(text));
	p = Cmd_Args();

	if (*p == '"')
	{
		p++;
		p[Q_strlen(p)-1] = 0;
	}

	Q_strncatz(text, p, sizeof(text));

	for (j = 0, client = svs.clients; j < MAX_CLIENTS; j++, client++)
	{
		if (client->state != cs_spawned)
			continue;
		SV_ClientPrintf(client, PRINT_CHAT, "%s\n", text);
	}
}


/*
==================
SV_Heartbeat_f
==================
*/
static void SV_Heartbeat_f (void)
{
	svs.last_heartbeat = -9999;
}

void SV_SendServerInfoChange(char *key, char *value)
{
	if (!sv.state)
		return;

	MSG_WriteByte (&sv.reliable_datagram, svc_serverinfo);
	MSG_WriteString (&sv.reliable_datagram, key);
	MSG_WriteString (&sv.reliable_datagram, value);
}

/*
===========
SV_Serverinfo_f

  Examine or change the serverinfo string
===========
*/
char *CopyString(char *s);
static void SV_Serverinfo_f (void)
{
	cvar_t	*var;

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("Server info settings:\n");
		Info_Print (svs.info);
		return;
	}

	if (Cmd_Argc() != 3)
	{
		Con_Printf ("usage: serverinfo [ <key> <value> ]\n");
		return;
	}

	if (Cmd_Argv(1)[0] == '*')
	{
		Con_Printf ("Star variables cannot be changed.\n");
		return;
	}
	Info_SetValueForKey (svs.info, Cmd_Argv(1), Cmd_Argv(2), MAX_SERVERINFO_STRING, SV_InfoCharset ());

	// if this is a cvar, change it too	
	var = Cvar_FindVar (Cmd_Argv(1));
	if (var)
	{
		Mem_Free (var->string);	// free the old value string
		var->string = CopyString (Cmd_Argv(2));
		var->value = Q_atof (var->string);
	}

	SV_SendServerInfoChange(Cmd_Argv(1), Cmd_Argv(2));
}


/*
===========
SV_Serverinfo_f

  Examine or change the serverinfo string
===========
*/
char *CopyString(char *s);
static void SV_Localinfo_f (void)
{
	if (Cmd_Argc() == 1)
	{
		Con_Printf ("Local info settings:\n");
		Info_Print (svs.localinfo);
		return;
	}

	if (Cmd_Argc() != 3)
	{
		Con_Printf ("usage: localinfo [ <key> <value> ]\n");
		return;
	}

	if (Cmd_Argv(1)[0] == '*')
	{
		Con_Printf ("Star variables cannot be changed.\n");
		return;
	}
	Info_SetValueForKey (svs.localinfo, Cmd_Argv(1), Cmd_Argv(2), MAX_LOCALINFO_STRING, SV_InfoCharset ());
}


/*
===========
SV_User_f

Examine a users info strings
===========
*/
static void SV_User_f (void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage: info <userid>\n");
		return;
	}

	if (!SV_SetPlayer ())
		return;

	Info_Print (host_client->userinfo);
}

/*
================
SV_Gamedir

Sets the fake *gamedir to a different directory.
================
*/
static void SV_Gamedir (void)
{
	char			*dir;

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("Current *gamedir: %s\n", Info_ValueForKey (svs.info, "*gamedir"));
		return;
	}

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage: sv_gamedir <newgamedir>\n");
		return;
	}

	dir = Cmd_Argv(1);

	if (strstr(dir, "..") || strstr(dir, "/")
		|| strstr(dir, "\\") || strstr(dir, ":") )
	{
		Con_Printf ("*Gamedir should be a single filename, not a path\n");
		return;
	}

	Info_SetValueForStarKey (svs.info, "*gamedir", dir, MAX_SERVERINFO_STRING, SV_InfoCharset ());
}

/*
================
SV_Floodport_f

Sets the gamedir and path to a different directory.
================
*/

static void SV_Floodprot_f (void)
{
	int arg1, arg2, arg3;
	
	if (Cmd_Argc() == 1)
	{
		if (svs.floodprot.messages) {
			Con_Printf ("Current floodprot settings: \nAfter %d msgs per %d seconds, silence for %d seconds\n", 
				svs.floodprot.messages, svs.floodprot.persecond, svs.floodprot.secondsdead);
			return;
		} else
			Con_Printf ("No floodprots enabled.\n");
	}

	if (Cmd_Argc() != 4)
	{
		Con_Printf ("Usage: floodprot <# of messages> <per # of seconds> <seconds to silence>\n");
		Con_Printf ("Use floodprotmsg to set a custom message to say to the flooder.\n");
		return;
	}

	arg1 = atoi(Cmd_Argv(1));
	arg2 = atoi(Cmd_Argv(2));
	arg3 = atoi(Cmd_Argv(3));

	if (arg1<=0 || arg2 <= 0 || arg3<=0) {
		Con_Printf ("All values must be positive numbers\n");
		return;
	}
	
	if (arg1 > 10) {
		Con_Printf ("Can only track up to 10 messages.\n");
		return;
	}

	svs.floodprot.messages	= arg1;
	svs.floodprot.persecond = arg2;
	svs.floodprot.secondsdead = arg3;
}

static void SV_Floodprotmsg_f (void)
{
	if (Cmd_Argc() == 1) {
		Con_Printf("Current msg: %s\n", svs.floodprot.msg);
		return;
	} else if (Cmd_Argc() != 2) {
		Con_Printf("Usage: floodprotmsg \"<message>\"\n");
		return;
	}
	snprintf(svs.floodprot.msg, sizeof(svs.floodprot.msg), "%s", Cmd_Argv(1));
}
  
/*
================
SV_Gamedir_f

Sets the gamedir and path to a different directory.
================
*/
extern char	gamedirfile[MAX_OSPATH];
static void SV_Gamedir_f (void)
{
	char			*dir;

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("Current gamedir: %s\n", com_gamedir);
		return;
	}

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage: gamedir <newdir>\n");
		return;
	}

	dir = Cmd_Argv(1);

	if (strstr(dir, "..") || strstr(dir, "/")
		|| strstr(dir, "\\") || strstr(dir, ":") )
	{
		Con_Printf ("Gamedir should be a single filename, not a path\n");
		return;
	}

	COM_Gamedir (dir);
	Info_SetValueForStarKey (svs.info, "*gamedir", dir, MAX_SERVERINFO_STRING, SV_InfoCharset ());
}

/*
================
SV_Snap
================
*/
static void SV_Snap (int uid)
{
	client_t *cl;
	char		pcxname[80]; 
	char		checkname[MAX_OSPATH];
	int			i;

	for (i = 0, cl = svs.clients; i < MAX_CLIENTS; i++, cl++)
	{
		if (!cl->state)
			continue;
		if (cl->userid == uid)
			break;
	}
	if (i >= MAX_CLIENTS) {
		Con_Printf ("userid not found\n");
		return;
	}

	snprintf(pcxname, sizeof(pcxname), "%d-00.pcx", uid);

	snprintf(checkname, sizeof(checkname), "%s/snap", gamedirfile);
	Sys_mkdir(gamedirfile);
	Sys_mkdir(checkname);

	for (i=0 ; i<=99 ; i++)
	{
		pcxname[strlen(pcxname) - 6] = (char)(i/10 + '0');
		pcxname[strlen(pcxname) - 5] = (char)(i%10 + '0');
		snprintf (checkname, sizeof(checkname), "%s/snap/%s", gamedirfile, pcxname);
		if (Sys_FileTime(checkname) == -1)
			break;	// file doesn't exist
	} 
	if (i==100) 
	{
		Con_Printf ("Snap: Couldn't create a file, clean some out.\n"); 
		return;
	}
	Q_strncpyz(cl->uploadfn, checkname, sizeof(cl->uploadfn));

	memcpy(&cl->snap_from, &svs.net_from, sizeof(svs.net_from));
	if (svs.redirected != RD_NONE)
		cl->remote_snap = true;
	else
		cl->remote_snap = false;

	ClientReliableWrite_Begin (cl, svc_stufftext, 24);
	ClientReliableWrite_String (cl, "cmd snap");
	Con_Printf ("Requesting snap from user %d...\n", uid);
}

/*
================
SV_Snap_f
================
*/
static void SV_Snap_f (void)
{
	int			uid;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage:  snap <userid>\n");
		return;
	}

	uid = atoi(Cmd_Argv(1));

	SV_Snap(uid);
}

/*
================
SV_Snap
================
*/
static void SV_SnapAll_f (void)
{
	client_t *cl;
	int			i;

	for (i = 0, cl = svs.clients; i < MAX_CLIENTS; i++, cl++)
	{
		if (cl->state < cs_connected || cl->spectator)
			continue;
		SV_Snap(cl->userid);
	}
}

/*
==================
SV_InitOperatorCommands
==================
*/
void SV_InitOperatorCommands (void)
{
	if (COM_CheckParm ("-cheats"))
	{
		sv_allow_cheats = true;
		Info_SetValueForStarKey (svs.info, "*cheats", "ON", MAX_SERVERINFO_STRING, SV_InfoCharset ());
	}

	Cmd_AddCommand ("logfile", SV_Logfile_f, "Starts or stops logging the console to qconsole.log "
		"in the game directory.");
	Cmd_AddCommand ("fraglogfile", SV_Fraglogfile_f, "Starts or stops logging frags to a new frag_<n>.log "
		"in the game directory.");

	// a listen server's client owns these names; the server's are sv_*
	Cmd_AddCommand (host.dedicated ? "snap" : "sv_snap", SV_Snap_f, host.dedicated
		? "Has a player's client upload a screenshot to snap/ in the game directory. Usage: snap <userid>"
		: "Has a player's client upload a screenshot to snap/ in the game directory. Usage: sv_snap <userid>");
	Cmd_AddCommand (host.dedicated ? "say" : "sv_say", SV_ConSay_f, host.dedicated
		? "Sends a chat message from the server console to every player. Usage: say <message>"
		: "Sends a chat message from the server console to every player. Usage: sv_say <message>");
	Cmd_AddCommand (host.dedicated ? "serverinfo" : "sv_serverinfo", SV_Serverinfo_f, host.dedicated
		? "Shows the serverinfo, or sets a key in it (and the cvar of that name) for the clients. "
			"Usage: serverinfo [<key> <value>]"
		: "Shows the serverinfo, or sets a key in it (and the cvar of that name) for the clients. "
			"Usage: sv_serverinfo [<key> <value>]");
	Cmd_AddCommand (host.dedicated ? "user" : "sv_user", SV_User_f, host.dedicated
		? "Shows a player's userinfo. Usage: user <userid>"
		: "Shows a player's userinfo. Usage: sv_user <userid>");

	Cmd_AddCommand ("snapall", SV_SnapAll_f, "Has every player's client (not spectators') upload a screenshot "
		"to snap/ in the game directory.");
	Cmd_AddCommand ("kick", SV_Kick_f, "Drops a player from the server. Usage: kick <userid>");
	Cmd_AddCommand ("status", SV_Status_f, "Shows the server's load and each client's frags, userid, address, "
		"name, rate, ping and packet loss.");

	Cmd_AddCommand ("map", SV_Map_f, "Starts the server on a map, taking the connected clients with it; for "
		"NetQuake's progs a new game. Usage: map <mapname>");
	Cmd_AddCommand ("changelevel", SV_Changelevel_f, "Goes on to a map, the players keeping what the game "
		"gives them for it, as QuakeC's changelevel does. Usage: changelevel <mapname>");
	Cmd_AddCommand ("restart", SV_Restart_f, "Starts the map over, the players with what they began it with.");
	Cmd_AddCommand ("killserver", SV_KillServer_f, "Ends the game, dropping every client.");
	Cmd_AddCommand ("setmaster", SV_SetMaster_f, "Sets the master servers heartbeats go to, at port 27000 "
		"unless given; none for no master. Usage: setmaster <address|none> [address ...]");

	Cmd_AddCommand ("heartbeat", SV_Heartbeat_f, "Sends the master servers a heartbeat now.");
	Cmd_AddCommand ("god", SV_God_f, "Toggles god mode for a player; needs the server started with -cheats. "
		"Usage: god <userid>");
	Cmd_AddCommand ("give", SV_Give_f, "Gives a player a weapon by its number, or sets their shells, nails, "
		"rockets, cells or health (s n r c h); needs -cheats. Usage: give <userid> <item> [amount]");
	Cmd_AddCommand ("noclip", SV_Noclip_f, "Toggles flying through walls for a player; needs the server started "
		"with -cheats. Usage: noclip <userid>");
	Cmd_AddCommand ("localinfo", SV_Localinfo_f, "Shows the local info, or sets a key in it: settings for the "
		"game code that the clients don't see. Usage: localinfo [<key> <value>]");
	Cmd_AddCommand ("gamedir", SV_Gamedir_f, "Shows or changes the game directory, which the clients are told too. "
		"Usage: gamedir [<dir>]");
	Cmd_AddCommand ("sv_gamedir", SV_Gamedir, "Shows or changes the game directory the clients are told, apart "
		"from the one the server reads. Usage: sv_gamedir [<dir>]");
	Cmd_AddCommand ("floodprot", SV_Floodprot_f, "Shows or sets chat flood protection: a player who sends that "
		"many messages within that many seconds is silenced. "
		"Usage: floodprot [<messages> <seconds> <silence seconds>]");
	Cmd_AddCommand ("floodprotmsg", SV_Floodprotmsg_f, "Shows or sets what a player silenced by floodprot is told. "
		"Usage: floodprotmsg [<message>]");

	cl_warncmd.value = 1;
}
