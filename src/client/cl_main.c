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
// cl_main.c  -- client main loop

#include "cl_local.h"
static void Cmd_ForwardToServer (void);



static cvar_t	rcon_address = {.name = "rcon_address", .string = "",
	.description = "The server that rcon commands go to while not connected to one."};

static cvar_t	cl_timeout = {.name = "cl_timeout", .string = "60",
	.description = "Seconds without a packet from the server before the client gives up on it; "
		"a server in this process never times out."};
// 0 keeps FTE chunked downloads out of the extensions offered (ezQuake's name)
static cvar_t	cl_pext_chunkeddownloads = {.name = "cl_pext_chunkeddownloads", .string = "1", .archive = true,
	.description = "Offers servers FTE chunked downloads (many pieces of a file in flight at once) when connecting.",
	.values = (const cvar_value_t[]){{"0", "One block at a time"},
		{"1", "Chunked where the server has them"}, {0}}};

cvar_t	cl_shownet = {.name = "cl_shownet", .string = "0",	// can be 0, 1, or 2
	.description = "Prints the server's messages as they arrive, for debugging the network.",
	.values = (const cvar_value_t[]){{"0", "Nothing"}, {"1", "Each message's size in bytes"},
		{"2", "Each message's commands, with their offsets"}, {0}}};

cvar_t	cl_sbar		= {.name = "cl_sbar", .string = "0", .archive = true,
	.description = "Where the status bar goes at viewsize 100 and above; smaller views always have it below.",
	.values = (const cvar_value_t[]){{"0", "Over the view, without its backdrop"},
		{"1", "Below the view, with its backdrop"}, {0}}};
cvar_t	cl_hudswap	= {.name = "cl_hudswap", .string = "0", .archive = true,
	.description = "Which side the heads-up status bar (cl_sbar 0) shows the weapons and ammo on.",
	.values = (const cvar_value_t[]){{"0", "Right"}, {"1", "Left"}, {0}}};
// frames per second drawn; with independent physics 0 is no cap (vid_vsync still applies)
static cvar_t	cl_maxfps	= {.name = "cl_maxfps", .string = "0", .archive = true,
	.description = "The most frames a second drawn, at least 30; 0 is no limit but the display's. "
		"Without cl_independentPhysics: 30 to 72, 0 is rate/80."};

cvar_t	lookspring = {.name = "lookspring", .string = "0", .archive = true,
	.description = "Recenters the view's pitch when +mlook is let go, with freelook off.",
	.values = (const cvar_value_t[]){{"0", "The view stays pitched"}, {"1", "The view recenters"}, {0}}};
cvar_t	lookstrafe = {.name = "lookstrafe", .string = "0", .archive = true,
	.description = "Moving the mouse sideways strafes instead of turning while mouse looking.",
	.values = (const cvar_value_t[]){{"0", "Turns"}, {"1", "Strafes"}, {0}}};
cvar_t	sensitivity = {.name = "sensitivity", .string = "3", .archive = true,
	.description = "Mouse speed: a multiplier on the mouse's motion before m_yaw, m_pitch, m_side and m_forward; "
		"the menu sets 1 to 11."};

cvar_t	m_pitch = {.name = "m_pitch", .string = "0.022", .archive = true,
	.description = "Degrees the view pitches per mouse count, times sensitivity; negative inverts the mouse."};
cvar_t	m_yaw = {.name = "m_yaw", .string = "0.022",
	.description = "Degrees the view turns per sideways mouse count, times sensitivity."};
cvar_t	m_forward = {.name = "m_forward", .string = "1",
	.description = "Forward speed per mouse count, times sensitivity, when the mouse moves you: "
		"without mouse look, or with +strafe held."};
cvar_t	m_side = {.name = "m_side", .string = "0.8",
	.description = "Sideways speed per mouse count, times sensitivity, when the mouse strafes: "
		"with +strafe held, or lookstrafe while mouse looking."};

cvar_t	cl_predict_players = {.name = "cl_predict_players", .string = "1",
	.description = "Moves other players on from where the server last had them to now; "
		"off only with cl_predict_players2 off too.",
	.values = (const cvar_value_t[]){{"0", "As the server last had them (if cl_predict_players2 is 0)"},
		{"1", "Moved on to now"}, {0}}};
cvar_t	cl_predict_players2 = {.name = "cl_predict_players2", .string = "1",
	.description = "The same switch as cl_predict_players: other players are moved on to now while either is set.",
	.values = (const cvar_value_t[]){{"0", "As the server last had them (if cl_predict_players is 0)"},
		{"1", "Moved on to now"}, {0}}};
cvar_t	cl_solid_players = {.name = "cl_solid_players", .string = "1",
	.description = "Other players (not the dead) block your predicted movement, as they do on the server.",
	.values = (const cvar_value_t[]){{"0", "Predicted as if they weren't there"}, {"1", "They block"}, {0}}};
// players hold visible weapons where the server says (Z_EXT_VWEP; ezQuake's name)
cvar_t	r_drawvweps = {.name = "r_drawvweps", .string = "1", .archive = true,
	.description = "Draws the weapon in each player's hands, on servers that send the weapon models.",
	.values = (const cvar_value_t[]){{"0", "Players' models alone"}, {"1", "Players hold their weapons"}, {0}}};

static cvar_t  localid = {.name = "localid", .string = "",
	.description = "The key a server browser on this machine sends with its commands, which it sets itself; "
		"needed after its first command or a connect."};

static bool allowremotecmd = true;

//
// info mirrors
//
static cvar_t	spectator = {.name = "spectator", .string = "", .userinfo = true,
	.description = "Joins servers as a spectator when set, to 1 or the server's spectator password; "
		"empty or 0 plays. join and observe set it."};
cvar_t	name = {.name = "name", .string = "unnamed", .archive = true, .userinfo = true,
	.description = "Your player name, as the scoreboard and chat show it to everyone."};
static cvar_t	team = {.name = "team", .string = "", .archive = true, .userinfo = true,
	.description = "Your team's name: say_team reaches those with the same one, "
		"and teamplay games and the scoreboard group players by it."};
static cvar_t	skin = {.name = "skin", .string = "", .archive = true, .userinfo = true,
	.description = "The skin others see you in, skins/<name>.pcx, downloaded from the server by those who lack it; "
		"empty is their baseskin."};
static cvar_t	topcolor = {.name = "topcolor", .string = "0", .archive = true, .userinfo = true,
	.description = "Your shirt's color, as others see it: "
		"0 to 13 the palette's rows, 14 orange, 15 dark red, 16 black."};
static cvar_t	bottomcolor = {.name = "bottomcolor", .string = "0", .archive = true, .userinfo = true,
	.description = "Your pants' color, as others see it: "
		"0 to 13 the palette's rows, 14 orange, 15 dark red, 16 black."};
static cvar_t	rate = {.name = "rate", .string = "30000", .archive = true, .userinfo = true,	// FTE's; ezQuake's is 25000
	.description = "The most bytes a second the server sends you; the server keeps it from 500 to its sv_maxrate."};
static cvar_t	noaim = {.name = "noaim", .string = "0", .archive = true, .userinfo = true,
	.description = "Turns off the server's aiming of your shots at targets near the crosshair.",
	.values = (const cvar_value_t[]){{"0", "The server may aim your shots"}, {"1", "Shots go where you look"}, {0}}};
static cvar_t	msg = {.name = "msg", .string = "1", .archive = true, .userinfo = true,
	.description = "Which of the server's messages you get: this level and more important ones.",
	.values = (const cvar_value_t[]){{"0", "All, item pickups too"}, {"1", "No item pickups"},
		{"2", "No deaths either, only important ones and chat"}, {"3", "Chat only"}, {0}}};


client_static_t	cls;
client_state_t	cl;


static double			connect_time = -1;		// for connection retransmits


static double		oldrealtime;			// last frame run


static cvar_t	host_speeds = {.name = "host_speeds", .string = "0",			// set for running times
	.description = "Prints each frame's milliseconds: all, before drawing (a local server, the network, waiting), "
		"drawing and sound.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "A line a frame"}, {0}}};
cvar_t	show_fps = {.name = "show_fps", .string = "0",			// set for running times
	.description = "Shows the frames drawn a second, counted each second, at the bottom right above the status bar.",
	.values = (const cvar_value_t[]){{"0", "Hidden"}, {"1", "Shown"}, {0}}};




static float	server_version = 0;	// version of server we connected to

char emodel_name[] = 
	{ 'e' ^ 0xff, 'm' ^ 0xff, 'o' ^ 0xff, 'd' ^ 0xff, 'e' ^ 0xff, 'l' ^ 0xff, 0 };
char pmodel_name[] = 
	{ 'p' ^ 0xff, 'm' ^ 0xff, 'o' ^ 0xff, 'd' ^ 0xff, 'e' ^ 0xff, 'l' ^ 0xff, 0 };
char prespawn_name[] = 
	{ 'p'^0xff, 'r'^0xff, 'e'^0xff, 's'^0xff, 'p'^0xff, 'a'^0xff, 'w'^0xff, 'n'^0xff,
		' '^0xff, '%'^0xff, 'i'^0xff, ' '^0xff, '0'^0xff, ' '^0xff, '%'^0xff, 'i'^0xff, 0 };
char modellist_name[] = 
	{ 'm'^0xff, 'o'^0xff, 'd'^0xff, 'e'^0xff, 'l'^0xff, 'l'^0xff, 'i'^0xff, 's'^0xff, 't'^0xff, 
		' '^0xff, '%'^0xff, 'i'^0xff, ' '^0xff, '%'^0xff, 'i'^0xff, 0 };
char soundlist_name[] = 
	{ 's'^0xff, 'o'^0xff, 'u'^0xff, 'n'^0xff, 'd'^0xff, 'l'^0xff, 'i'^0xff, 's'^0xff, 't'^0xff, 
		' '^0xff, '%'^0xff, 'i'^0xff, ' '^0xff, '%'^0xff, 'i'^0xff, 0 };

/*
==================
CL_Quit_f
==================
*/
static void CL_Quit_f (void)
{
	if (1 /* key_dest != key_console */ /* && cls.state != ca_dedicated */)
	{
		M_Menu_Quit_f ();
		return;
	}
	CL_Disconnect ();
	Sys_Quit ();
}

/*
=======================
CL_Version_f
======================
*/
static void CL_Version_f (void)
{
	Con_Printf ("SoftWorld %4.2f\n", VERSION);
	Con_Printf ("Exe: "__TIME__" "__DATE__"\n");
}


/*
=======================
CL_SendConnectPacket

called by CL_Connect_f and CL_CheckResend
======================
*/
static void CL_SendConnectPacket (void)
{
	netadr_t	adr;
	char	data[2048];
	double t1, t2;
// JACK: Fixed bug where DNS lookups would cause two connects real fast
//       Now, adds lookup time to the connect time.
//		 Should I add it to realtime instead?!?!

	if (cls.state != ca_disconnected)
		return;

	t1 = Sys_DoubleTime ();

	if (!NET_StringToAdr (cls.servername, &adr))
	{
		Con_Printf ("Bad server address\n");
		connect_time = -1;
		return;
	}


	if (adr.port == 0)
		adr.port = BigShort (27500);
	t2 = Sys_DoubleTime ();

	connect_time = host.realtime+t2-t1;	// for retransmit requests

	cls.qport = (int)Cvar_VariableValue("qport");

	Info_SetValueForStarKey (cls.userinfo, "*ip", NET_AdrToString(adr), MAX_INFO_STRING, INFO_CHARSET_USERINFO);

//	Con_Printf ("Connecting to %s...\n", cls.servername);
	snprintf (data, sizeof(data), "%c%c%c%cconnect %i %i %i \"%s\"\n",
		255, 255, 255, 255,	PROTOCOL_VERSION, cls.qport, cls.challenge, cls.userinfo);
	// the protocol extensions to use, those both ends know; a family with
	// none is left out
	if (cls.fteext)
		Q_strncatz (data, va("0x%x 0x%x\n", PROTOCOL_VERSION_FTE, cls.fteext), sizeof(data));
	if (cls.mvdext1)
		Q_strncatz (data, va("0x%x 0x%x\n", PROTOCOL_VERSION_MVD1, cls.mvdext1), sizeof(data));
	NET_SendPacket (NS_CLIENT, (int)strlen(data), data, adr);
}

/*
=================
CL_CheckForResend

Resend a connect message if the last one has timed out

=================
*/
static void CL_CheckForResend (void)
{
	netadr_t	adr;
	char	data[2048];
	double t1, t2;

	if (connect_time == -1)
		return;
	if (cls.state != ca_disconnected)
		return;
	if (connect_time && host.realtime - connect_time < 5.0)
		return;

	t1 = Sys_DoubleTime ();
	if (!NET_StringToAdr (cls.servername, &adr))
	{
		Con_Printf ("Bad server address\n");
		connect_time = -1;
		return;
	}

	if (adr.port == 0)
		adr.port = BigShort (27500);
	t2 = Sys_DoubleTime ();

	connect_time = host.realtime+t2-t1;	// for retransmit requests

	Con_Printf ("Connecting to %s...\n", cls.servername);
	snprintf (data, sizeof(data), "%c%c%c%cgetchallenge\n", 255, 255, 255, 255);
	NET_SendPacket (NS_CLIENT, (int)strlen(data), data, adr);
}

void CL_BeginServerConnect(void)
{
	connect_time = 0;
	CL_CheckForResend();
}

/*
================
CL_Connect_f

================
*/
static void CL_Connect_f (void)
{
	char	*server;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("usage: connect <server>\n");
		return;	
	}
	
	server = Cmd_Argv (1);

	CL_Disconnect ();
	if (strcmp (server, "local"))
		SV_Kill ();			// playing elsewhere ends the local game

	strncpy (cls.servername, server, sizeof(cls.servername)-1);
	CL_BeginServerConnect();
}


/*
=====================
CL_Rcon_f

  Send the rest of the command line over as
  an unconnected command.
=====================
*/
static void CL_Rcon_f (void)
{
	char	message[1024];
	int		i;
	netadr_t	to;

	if (!rcon_password.string)
	{
		Con_Printf ("You must set 'rcon_password' before\n"
					"issuing an rcon command.\n");
		return;
	}

	message[0] = '\xff';
	message[1] = '\xff';
	message[2] = '\xff';
	message[3] = '\xff';
	message[4] = 0;

	Q_strncatz (message, "rcon ", sizeof(message));

	Q_strncatz (message, rcon_password.string, sizeof(message));
	Q_strncatz (message, " ", sizeof(message));

	for (i=1 ; i<Cmd_Argc() ; i++)
	{
		Q_strncatz (message, Cmd_Argv(i), sizeof(message));
		Q_strncatz (message, " ", sizeof(message));
	}

	if (cls.state >= ca_connected)
		to = cls.netchan.remote_address;
	else
	{
		if (!strlen(rcon_address.string))
		{
			Con_Printf ("You must either be connected,\n"
						"or set the 'rcon_address' cvar\n"
						"to issue rcon commands\n");

			return;
		}
		NET_StringToAdr (rcon_address.string, &to);
	}
	
	NET_SendPacket (NS_CLIENT, (int)strlen(message)+1, message
		, to);
}


/*
=====================
CL_ClearState

=====================
*/
void CL_ClearState (void)
{
	entity_t	**static_blocks = cl.static_blocks;
	int			i, num_static_blocks = (cl.num_statics + STATIC_BLOCK - 1) / STATIC_BLOCK;

	S_StopAllSounds (true);

	Con_DPrintf ("Clearing memory\n");
	D_FlushCaches ();
	Mod_ClearAll ();

	CL_ClearTEnts ();
	CL_ResetSmoothing ();

// wipe the entire cl structure
	if (cl.map)
		CM_FreeMap (cl.map);
	memset (&cl, 0, sizeof(cl));
	CL_ProcessServerInfo ();

	SZ_Clear (&cls.netchan.message);

	R_ClearEfrags ();
	for (i = 0 ; i < num_static_blocks ; i++)
		Mem_Free (static_blocks[i]);
	Mem_Free (static_blocks);
	CL_DisableLerpMove ();
	r_scene.time = 0;
	r_scene.worldmodel = NULL;
}

/*
=====================
CL_Disconnect

Sends a disconnect message to the server
This is also called after errors, so it shouldn't cause any
=====================
*/
void CL_Disconnect (void)
{
	byte	final[10];

	connect_time = -1;

	VID_SetCaption ("SoftWorld: disconnected");

// stop sounds (especially looping!)
	S_StopAllSounds (true);
	
// if running a local server, shut it down
	if (cls.demoplayback)
		CL_StopPlayback ();
	else if (cls.state != ca_disconnected)
	{
		if (cls.demorecording)
			CL_Stop_f ();

		final[0] = clc_stringcmd;
		Q_strncpyz ((char *)final+1, "drop", sizeof(final)-1);
		Netchan_Transmit (&cls.netchan, 6, final);
		Netchan_Transmit (&cls.netchan, 6, final);
		Netchan_Transmit (&cls.netchan, 6, final);

		cls.state = ca_disconnected;

		cls.demoplayback = cls.demorecording = cls.timedemo = false;
	}
	Cam_Reset();

	// the next server negotiates its own
	cls.fteext = cls.mvdext1 = 0;
	cls.net_message.floatcoords = false;

	CL_StopDownload ();
	CL_StopUpload();
	CL_QTVStop ();
	CSQC_Shutdown ();

}

static void CL_Disconnect_f (void)
{
	CL_Disconnect ();
	SV_Kill ();
}

/*
====================
CL_User_f

user <name or userid>

Dump userdata / masterdata for a user
====================
*/
static void CL_User_f (void)
{
	int		uid;
	int		i;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage: user <username / userid>\n");
		return;
	}

	uid = atoi(Cmd_Argv(1));

	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		if (!cl.players[i].name[0])
			continue;
		if (cl.players[i].userid == uid
		|| !strcmp(cl.players[i].name, Cmd_Argv(1)) )
		{
			Info_Print (cl.players[i].userinfo);
			return;
		}
	}
	Con_Printf ("User not in server.\n");
}

/*
====================
CL_Users_f

Dump userids for all current players
====================
*/
static void CL_Users_f (void)
{
	int		i;
	int		c;

	c = 0;
	Con_Printf ("userid frags name\n");
	Con_Printf ("------ ----- ----\n");
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		if (cl.players[i].name[0])
		{
			Con_Printf ("%6i %4i %s\n", cl.players[i].userid, cl.players[i].frags, cl.players[i].name);
			c++;
		}
	}

	Con_Printf ("%i total users\n", c);
}

static void CL_Color_f (void)
{
	// just for quake compatability...
	int		top, bottom;
	char	num[16];

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("\"color\" is \"%s %s\"\n",
			Info_ValueForKey (cls.userinfo, "topcolor"),
			Info_ValueForKey (cls.userinfo, "bottomcolor") );
		Con_Printf ("color <0-16> [0-16]\n");
		return;
	}

	if (Cmd_Argc() == 2)
		top = bottom = atoi(Cmd_Argv(1));
	else
	{
		top = atoi(Cmd_Argv(1));
		bottom = atoi(Cmd_Argv(2));
	}

	// 14 orange, 15 dark red, 16 black (skin.c)
	top = top < 0 ? 0 : top > 16 ? 16 : top;
	bottom = bottom < 0 ? 0 : bottom > 16 ? 16 : bottom;
	
	snprintf (num, sizeof(num), "%i", top);
	Cvar_Set ("topcolor", num);
	snprintf (num, sizeof(num), "%i", bottom);
	Cvar_Set ("bottomcolor", num);
}

/*
==================
CL_ProcessServerInfo
==================
*/
void CL_ProcessServerInfo (void)
{
	char	*s;

	cl.z_ext = atoi (Info_ValueForKey (cl.serverinfo, "*z_ext")) & CL_Z_EXTENSIONS;

	cl.maxpitch = 80;
	cl.minpitch = -70;
	if (cl.z_ext & Z_EXT_PITCHLIMITS)
	{
		s = Info_ValueForKey (cl.serverinfo, "maxpitch");
		if (*s)
			cl.maxpitch = (float)atof (s);
		s = Info_ValueForKey (cl.serverinfo, "minpitch");
		if (*s)
			cl.minpitch = (float)atof (s);
		cl.maxpitch = cl.maxpitch < 0 ? 0 : cl.maxpitch > 89.9f ? 89.9f : cl.maxpitch;
		cl.minpitch = cl.minpitch > 0 ? 0 : cl.minpitch < -89.9f ? -89.9f : cl.minpitch;
	}

	// the server's player movement, as ezQuake reads it: pm_ktjump is on
	// unless said otherwise, except for Team Fortress; pm_pground needs the
	// ground from the server
	cl.movevars.bunnyspeedcap = (float)atof (Info_ValueForKey (cl.serverinfo, "pm_bunnyspeedcap"));
	cl.movevars.slidefix = atof (Info_ValueForKey (cl.serverinfo, "pm_slidefix")) != 0;
	cl.movevars.airstep = atof (Info_ValueForKey (cl.serverinfo, "pm_airstep")) != 0;
	cl.movevars.pground = atof (Info_ValueForKey (cl.serverinfo, "pm_pground")) != 0
		&& (cl.z_ext & Z_EXT_PF_ONGROUND);
	cl.movevars.rampjump = atof (Info_ValueForKey (cl.serverinfo, "pm_rampjump")) != 0;
	s = Info_ValueForKey (cl.serverinfo, "pm_ktjump");
	if (*s)
		cl.movevars.ktjump = (float)atof (s);
	else
		cl.movevars.ktjump = Q_strcasecmp (Info_ValueForKey (cl.serverinfo, "*gamedir"), "fortress") ? 1.0f : 0.0f;
}

/*
==================
CL_ViewHeight
==================
*/
float CL_ViewHeight (void)
{
	if ((cl.z_ext & Z_EXT_VIEWHEIGHT) && cl.stats[STAT_VIEWHEIGHT])
		return (float)cl.stats[STAT_VIEWHEIGHT];
	return 22;
}

/*
==================
CL_Join_f / CL_Observe_f

Play or watch: on a server with Z_EXT_JOIN_OBSERVE without reconnecting,
elsewhere by reconnecting with the spectator userinfo changed
==================
*/
static void CL_JoinObserve (bool observe)
{
	Cvar_Set ("spectator", observe ? "1" : "");
	if (cls.state < ca_connected || cls.demoplayback)
		return;
	if (cl.z_ext & Z_EXT_JOIN_OBSERVE)
	{
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message, observe ? "observe" : "join");
		return;
	}
	Cbuf_AddText ("reconnect\n");
}

static void CL_Join_f (void)
{
	CL_JoinObserve (false);
}

static void CL_Observe_f (void)
{
	CL_JoinObserve (true);
}

/*
==================
CL_FullServerinfo_f

Sent by server when serverinfo changes
==================
*/
static void CL_FullServerinfo_f (void)
{
	char *p;
	float v;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("usage: fullserverinfo <complete info string>\n");
		return;
	}

	Q_strncpyz (cl.serverinfo, Cmd_Argv(1), sizeof(cl.serverinfo));
	CL_ProcessServerInfo ();

	if ((p = Info_ValueForKey(cl.serverinfo, "*vesion")) && *p) {
		v = Q_atof(p);
		if (v) {
			if (!server_version)
				Con_Printf("Version %1.2f Server\n", v);
			server_version = v;
		}
	}
}

/*
==================
CL_FullInfo_f

Allow clients to change userinfo
==================
Casey was here :)
*/
static void CL_FullInfo_f (void)
{
	char	key[512];
	char	value[512];
	char	*o;
	char	*s;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("fullinfo <complete info string>\n");
		return;
	}

	s = Cmd_Argv(1);
	if (*s == '\\')
		s++;
	while (*s)
	{
		o = key;
		while (*s && *s != '\\')
			*o++ = *s++;
		*o = 0;

		if (!*s)
		{
			Con_Printf ("MISSING VALUE\n");
			return;
		}

		o = value;
		s++;
		while (*s && *s != '\\')
			*o++ = *s++;
		*o = 0;

		if (*s)
			s++;

		if (!Q_strcasecmp (key, pmodel_name) || !Q_strcasecmp (key, emodel_name))
			continue;

		Info_SetValueForKey (cls.userinfo, key, value, MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	}
}

/*
==================
CL_SetInfo_f

Allow clients to change userinfo
==================
*/
static void CL_SetInfo_f (void)
{
	if (Cmd_Argc() == 1)
	{
		Info_Print (cls.userinfo);
		return;
	}
	if (Cmd_Argc() != 3)
	{
		Con_Printf ("usage: setinfo [ <key> <value> ]\n");
		return;
	}
	if (!Q_strcasecmp (Cmd_Argv(1), pmodel_name) || !strcmp(Cmd_Argv(1), emodel_name))
		return;

	Info_SetValueForKey (cls.userinfo, Cmd_Argv(1), Cmd_Argv(2), MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	if (cls.state >= ca_connected)
		Cmd_ForwardToServer ();
}

/*
====================
CL_Packet_f

packet <destination> <contents>

Contents allows \n escape character
====================
*/
static void CL_Packet_f (void)
{
	char	send[2048];
	int		i, l;
	char	*in, *out;
	netadr_t	adr;

	if (Cmd_Argc() != 3)
	{
		Con_Printf ("packet <destination> <contents>\n");
		return;
	}

	if (!NET_StringToAdr (Cmd_Argv(1), &adr))
	{
		Con_Printf ("Bad address\n");
		return;
	}

	in = Cmd_Argv(2);
	out = send+4;
	send[0] = send[1] = send[2] = send[3] = 0xff;

	l = (int)strlen (in);
	for (i=0 ; i<l ; i++)
	{
		if (in[i] == '\\' && in[i+1] == 'n')
		{
			*out++ = '\n';
			i++;
		}
		else
			*out++ = in[i];
	}
	*out = 0;

	NET_SendPacket (NS_CLIENT, (int)(out-send), send, adr);
}


/*
=====================
CL_NextDemo

Called to play the next demo in the demo loop
=====================
*/
void CL_NextDemo (void)
{
	char	str[1024];

	if (cls.demonum == -1)
		return;		// don't play demos

	if (!cls.demos[cls.demonum][0] || cls.demonum == MAX_DEMOS)
	{
		cls.demonum = 0;
		if (!cls.demos[cls.demonum][0])
		{
//			Con_Printf ("No demos listed with startdemos\n");
			cls.demonum = -1;
			return;
		}
	}

	snprintf (str, sizeof(str), "playdemo %s\n", cls.demos[cls.demonum]);
	Cbuf_InsertText (str);
	cls.demonum++;
}


/*
=================
CL_Changing_f

Just sent as a hint to the client that they should
drop to full console
=================
*/
static void CL_Changing_f (void)
{
	if (cls.download)  // don't change when downloading
		return;

	S_StopAllSounds (true);
	cl.intermission = 0;
	cls.state = ca_connected;	// not active anymore, but not disconnected
	Con_Printf ("\nChanging map...\n");
}


/*
=================
CL_Reconnect_f

The server is changing levels
=================
*/
static void CL_Reconnect_f (void)
{
	if (cls.download)  // don't change when downloading
		return;
	if (cls.demoplayback)	// a recording goes on to its next level itself
		return;

	S_StopAllSounds (true);

	if (cls.state == ca_connected) {
		Con_Printf ("reconnecting...\n");
		MSG_WriteChar (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message, "new");
		return;
	}

	if (!*cls.servername) {
		Con_Printf("No server to reconnect to...\n");
		return;
	}

	CL_Disconnect();
	CL_BeginServerConnect();
}

/*
=================
CL_FTEExtensions

The FTE extensions the client offers
=================
*/
static unsigned CL_FTEExtensions (void)
{
	return CL_FTE_EXTENSIONS & (cl_pext_chunkeddownloads.value ? ~0u : ~(unsigned)FTE_PEXT_CHUNKEDDOWNLOADS);
}

/*
=================
CL_ConnectionlessPacket

Responses to broadcasts, etc
=================
*/
static void CL_ConnectionlessPacket (void)
{
	char	*s;
	int		c, magic;
	unsigned	mask;

    MSG_BeginReading (&cls.net_message);
    MSG_ReadLong ();        // skip the -1

	c = MSG_ReadByte ();
	if (c == A2C_PRINT && CL_ParseChunkPacket ())
		return;		// a download's chunk, dressed as a print
	if (!cls.demoplayback)
		Con_Printf ("%s: ", NET_AdrToString (cls.net_from));
//	Con_DPrintf ("%s", net_message.data + 5);
	if (c == S2C_CONNECTION)
	{
		Con_Printf ("connection\n");
		if (cls.state >= ca_connected)
		{
			if (!cls.demoplayback)
				Con_Printf ("Dup connect received.  Ignored.\n");
			return;
		}
		Netchan_Setup (&cls.netchan, cls.net_from, cls.qport, NS_CLIENT);
		MSG_WriteChar (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message, "new");	
		cls.state = ca_connected;
		Con_Printf ("Connected.\n");
		allowremotecmd = false; // localid required now for remote cmds
		return;
	}
	// remote command from gui front end
	if (c == A2C_CLIENT_COMMAND)
	{
		char	cmdtext[2048];

		Con_Printf ("client command\n");

		if (!NET_IsLocalAddress (cls.net_from))
		{
			Con_Printf ("Command packet from remote host.  Ignored.\n");
			return;
		}
		VID_BringToFront ();
		s = MSG_ReadString ();

		strncpy(cmdtext, s, sizeof(cmdtext) - 1);
		cmdtext[sizeof(cmdtext) - 1] = 0;

		s = MSG_ReadString ();

		while (*s && isspace(*s))
			s++;
		while (*s && isspace(s[strlen(s) - 1]))
			s[strlen(s) - 1] = 0;

		if (!allowremotecmd && (!*localid.string || strcmp(localid.string, s))) {
			if (!*localid.string) {
				Con_Printf("===========================\n");
				Con_Printf("Command packet received from local host, but no "
					"localid has been set.  You may need to upgrade your server "
					"browser.\n");
				Con_Printf("===========================\n");
				return;
			}
			Con_Printf("===========================\n");
			Con_Printf("Invalid localid on command packet received from local host. "
				"\n|%s| != |%s|\n"
				"You may need to reload your server browser and QuakeWorld.\n",
				s, localid.string);
			Con_Printf("===========================\n");
			Cvar_Set("localid", "");
			return;
		}

		Cbuf_AddText (cmdtext);
		allowremotecmd = false;
		return;
	}
	// print command from somewhere
	if (c == A2C_PRINT)
	{
		Con_Printf ("print\n");

		s = MSG_ReadString ();
		Con_Print (s);
		return;
	}

	// ping from somewhere
	if (c == A2A_PING)
	{
		char	data[6];

		Con_Printf ("ping\n");

		data[0] = 0xff;
		data[1] = 0xff;
		data[2] = 0xff;
		data[3] = 0xff;
		data[4] = A2A_ACK;
		data[5] = 0;
		
		NET_SendPacket (NS_CLIENT, 6, &data, cls.net_from);
		return;
	}

	if (c == S2C_CHALLENGE) {
		Con_Printf ("challenge\n");

		s = MSG_ReadString ();
		cls.challenge = atoi(s);

		// then the server's protocol extensions, (magic, mask) pairs to the
		// end; unknown families are skipped
		cls.fteext = cls.mvdext1 = 0;
		for (;;)
		{
			magic = MSG_ReadLong ();
			mask = (unsigned)MSG_ReadLong ();
			if (msg_badread)
				break;
			Con_DPrintf ("The server offers protocol extensions 0x%x 0x%x\n", magic, mask);
			if (magic == PROTOCOL_VERSION_FTE)
				cls.fteext = mask & CL_FTEExtensions ();
			else if (magic == PROTOCOL_VERSION_MVD1)
				cls.mvdext1 = mask & CL_MVD1_EXTENSIONS;
		}

		CL_SendConnectPacket ();
		return;
	}


	Con_Printf ("unknown:  %c\n", c);
}


/*
=================
CL_ReadPackets
=================
*/
static void CL_ReadPackets (void)
{
	while (CL_GetMessage())
	{
		// an MVD's block: a server message without a netchan header
		if (cls.mvdplayback)
		{
			MSG_BeginReading (&cls.net_message);
			CL_ParseServerMessage ();
			continue;
		}

		//
		// remote command packet
		//
		if (*(int *)cls.net_message.data == -1)
		{
			CL_ConnectionlessPacket ();
			continue;
		}

		if (cls.net_message.cursize < 8)
		{
			Con_Printf ("%s: Runt packet\n",NET_AdrToString(cls.net_from));
			continue;
		}

		//
		// packet from server
		//
		if (cls.demoplayback)
			cls.net_from = cls.netchan.remote_address;	// demo packets come from the recorded server
		if (!NET_CompareAdr (cls.net_from, cls.netchan.remote_address))
		{
			Con_DPrintf ("%s:sequenced packet without connection\n"
				,NET_AdrToString(cls.net_from));
			continue;
		}
		if (!Netchan_Process (&cls.netchan, cls.net_from, &cls.net_message))
			continue;		// wasn't accepted for some reason
		CL_ParseServerMessage ();
	}

	//
	// check timeout; a server in this process doesn't time out
	//
	if (cls.state >= ca_connected
	 && cls.netchan.remote_address.type != NA_LOOPBACK
	 && host.realtime - cls.netchan.last_received > cl_timeout.value)
	{
		Con_Printf ("\nServer connection timed out.\n");
		CL_Disconnect ();
		return;
	}
	
}

//=============================================================================

/*
===================
CL_Pause_f

Pauses an MVD played, else asks the server
===================
*/
static void CL_Pause_f (void)
{
	if (cls.mvdplayback)
		CL_MVDTogglePause ();
	else
		Cmd_ForwardToServer ();
}

/*
===================
CL_ViewPos_f

The player's origin and view angles, as setpos takes them
===================
*/
static void CL_ViewPos_f (void)
{
	if (cls.state != ca_active)
	{
		Con_Printf ("Not in a game\n");
		return;
	}
	Con_Printf ("setpos %.1f %.1f %.1f %.1f %.1f %.1f\n", cl.simorg[0], cl.simorg[1], cl.simorg[2],
		cl.viewangles[0], cl.viewangles[1], cl.viewangles[2]);
}

/*
===================
Cmd_ForwardToServer

adds the current command line as a clc_stringcmd to the client message.
things like godmode, noclip, etc, are commands directed to the server,
so when they are typed in at the console, they will need to be forwarded.
===================
*/
static void Cmd_ForwardToServer (void)
{
	if (cls.state == ca_disconnected)
	{
		Con_Printf ("Can't \"%s\", not connected\n", Cmd_Argv(0));
		return;
	}
	
	if (cls.demoplayback)
		return;		// not really connected

	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
	SZ_Print (&cls.netchan.message, Cmd_Argv(0));
	if (Cmd_Argc() > 1)
	{
		SZ_Print (&cls.netchan.message, " ");
		SZ_Print (&cls.netchan.message, Cmd_Args());
	}
}

// don't forward the first argument
static void Cmd_ForwardToServer_f (void)
{
	if (cls.state == ca_disconnected)
	{
		Con_Printf ("Can't \"%s\", not connected\n", Cmd_Argv(0));
		return;
	}

	if (Q_strcasecmp(Cmd_Argv(1), "snap") == 0) {
		Cbuf_InsertText ("snap\n");
		return;
	}
	
	if (cls.demoplayback)
		return;		// not really connected

	// FTE and mvdsv servers ask which protocol extensions the client knows
	if (Cmd_Argc() == 2 && !Q_strcasecmp (Cmd_Argv(1), "pext"))
	{
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, va("pext 0x%x 0x%x 0x%x 0x%x", PROTOCOL_VERSION_FTE, CL_FTEExtensions (),
			PROTOCOL_VERSION_MVD1, CL_MVD1_EXTENSIONS));
		return;
	}

	if (Cmd_Argc() > 1)
	{
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, Cmd_Args());
	}
}

/*
=================
CL_Init
=================
*/
/*
=================
CL_UserinfoCvarChanged

Cvars flagged as info are mirrored into the userinfo string.
=================
*/
static void CL_UserinfoCvarChanged (char *key, char *value)
{
	Info_SetValueForKey (cls.userinfo, key, value, MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	if (cls.state >= ca_connected)
	{
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, va("setinfo \"%s\" \"%s\"\n", key, value));
	}
}

static void CL_InitLocal (void)
{
	extern	cvar_t		baseskin;
	extern	cvar_t		noskins;
	char st[80];

	cls.state = ca_disconnected;
	Cvar_SetUserinfoHook (CL_UserinfoCvarChanged);
	Info_SetValueForStarKey (cls.userinfo, "*z_ext", va("%i", CL_Z_EXTENSIONS), MAX_INFO_STRING, INFO_CHARSET_USERINFO);

	r_scene.numvisedicts = &cl.numvisedicts;
	r_scene.maxvisedicts = MAX_VISEDICTS;
	r_scene.dlights = cl.dlights;
	r_scene.lightstyles = cl.lightstyles;
	r_scene.viewent = &cl.viewent;

	Info_SetValueForKey (cls.userinfo, "name", "unnamed", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "topcolor", "0", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "bottomcolor", "0", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "rate", rate.string, MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "msg", "1", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	snprintf (st, sizeof(st), "%4.2f-%04d", VERSION, build_number());
	Info_SetValueForStarKey (cls.userinfo, "*ver", st, MAX_INFO_STRING, INFO_CHARSET_USERINFO);

	CL_InitInput ();
	CL_InitTEnts ();
	CL_InitPrediction ();
	CL_InitCam ();
	CL_InitFChecks ();
	
//
// register our commands
//
	Cvar_RegisterVariable (&show_fps);
	Cvar_RegisterVariable (&host_speeds);

	Cvar_RegisterVariable (&cl_warncmd);
	Cvar_RegisterVariable (&cl_upspeed);
	Cvar_RegisterVariable (&cl_forwardspeed);
	Cvar_RegisterVariable (&cl_backspeed);
	Cvar_RegisterVariable (&cl_sidespeed);
	Cvar_RegisterVariable (&cl_movespeedkey);
	Cvar_RegisterVariable (&cl_yawspeed);
	Cvar_RegisterVariable (&cl_pitchspeed);
	Cvar_RegisterVariable (&cl_anglespeedkey);
	Cvar_RegisterVariable (&cl_shownet);
	Cvar_RegisterVariable (&cl_sbar);
	Cvar_RegisterVariable (&cl_hudswap);
	Cvar_RegisterVariable (&cl_maxfps);
	Cvar_RegisterVariable (&cl_timeout);
	Cvar_RegisterVariable (&cl_pext_chunkeddownloads);
	CSQC_RegisterVariables ();
	Cvar_RegisterVariable (&lookspring);
	Cvar_RegisterVariable (&lookstrafe);
	Cvar_RegisterVariable (&sensitivity);

	Cvar_RegisterVariable (&m_pitch);
	Cvar_RegisterVariable (&m_yaw);
	Cvar_RegisterVariable (&m_forward);
	Cvar_RegisterVariable (&m_side);

	Cvar_RegisterVariable (&rcon_address);

	Cvar_RegisterVariable (&cl_predict_players2);
	Cvar_RegisterVariable (&r_drawvweps);
	Cvar_RegisterVariable (&cl_predict_players);
	Cvar_RegisterVariable (&cl_solid_players);

	Cvar_RegisterVariable (&localid);

	Cvar_RegisterVariable (&baseskin);
	Cvar_RegisterVariable (&noskins);

	//
	// info mirrors
	//
	Cvar_RegisterVariable (&name);
	Cvar_RegisterVariable (&spectator);
	Cvar_RegisterVariable (&skin);
	Cvar_RegisterVariable (&team);
	Cvar_RegisterVariable (&topcolor);
	Cvar_RegisterVariable (&bottomcolor);
	Cvar_RegisterVariable (&rate);
	Cvar_RegisterVariable (&msg);
	Cvar_RegisterVariable (&noaim);


	Cmd_AddCommand ("version", CL_Version_f, "Prints the client's version and build time.");
	Cmd_AddCommand ("cmd", Cmd_ForwardToServer_f,
		"Sends the rest of the line to the server as a command. Usage: cmd <command> [arguments]");
	Cmd_SetForwardHandler (Cmd_ForwardToServer);

	Cmd_AddCommand ("changing", CL_Changing_f,
		"Stops sounds and waits for the server's next map (the server sends it on a map change).");
	Cmd_AddCommand ("disconnect", CL_Disconnect_f,
		"Leaves the server or stops the demo playing, and ends a game running in this process.");
	Cmd_AddCommand ("record", CL_Record_f,
		"Records a demo of the game from now on, to <name>.qwd in the game directory. Usage: record <name>");
	Cmd_AddCommand ("rerecord", CL_ReRecord_f,
		"Reconnects to the server and records a demo from the start, to <name>.qwd. Usage: rerecord <name>");
	Cmd_AddCommand ("stop", CL_Stop_f, "Stops recording a demo.");
	Cmd_AddCommand ("playdemo", CL_PlayDemo_f,
		"Plays a .qwd or .mvd demo; without an extension, .qwd is tried first. Usage: playdemo <name>");
	Cmd_AddCommand ("timedemo", CL_TimeDemo_f,
		"Plays a demo as fast as it draws, then prints the frames a second. Usage: timedemo <name>");
	Cmd_SetCompletion ("playdemo", CL_CompleteDemo);
	Cmd_SetCompletion ("timedemo", CL_CompleteDemo);
	CL_InitDemo ();
	CL_InitMVD ();
	CL_InitItems ();
	CL_InitQTV ();

	Cmd_AddCommand ("skins", Skin_Skins_f, "Reloads the players' skins, downloading those missing.");
	Cmd_AddCommand ("allskins", Skin_AllSkins_f,
		"Shows every player in one skin, or each in their own when none is given. Usage: allskins [skin]");

	Cmd_AddCommand ("quit", CL_Quit_f, "Asks whether to quit the game.");

	Cmd_AddCommand ("connect", CL_Connect_f,
		"Connects to a server, on port 27500 when none is given; local is the game in this process. "
		"Usage: connect <address>");
	Cmd_AddCommand ("reconnect", CL_Reconnect_f,
		"Asks the server for its new map (the server sends it on a map change), "
		"or connects to the last server again.");

	Cmd_AddCommand ("rcon", CL_Rcon_f,
		"Sends a command to the server's console with rcon_password; to rcon_address when not connected. "
		"Usage: rcon <command>");
	Cmd_AddCommand ("packet", CL_Packet_f,
		"Sends a connectionless packet, \\n in it a newline. Usage: packet <address> <contents>");
	Cmd_AddCommand ("user", CL_User_f, "Lists a player's userinfo. Usage: user <name or userid>");
	Cmd_AddCommand ("users", CL_Users_f, "Lists the players' user ids, frags and names.");

	Cmd_AddCommand ("cfg_reset", Cvar_ResetAll_f,
		"Sets every variable back to its default but the server's serverinfo ones (deathmatch, timelimit...); "
		"binds stay, which exec default.cfg resets.");

	Cmd_AddCommand ("setinfo", CL_SetInfo_f,
		"Sets a userinfo key the server and other players see, or lists your userinfo when given none. "
		"Usage: setinfo [<key> <value>]");
	Cmd_AddCommand ("fullinfo", CL_FullInfo_f,
		"Sets userinfo keys from a string of \\key\\value pairs, without telling the server. "
		"Usage: fullinfo <info string>");
	Cmd_AddCommand ("fullserverinfo", CL_FullServerinfo_f,
		"Replaces the server's info with the string given (the server sends it when it changes). "
		"Usage: fullserverinfo <info string>");
	Cmd_AddCommand ("join", CL_Join_f,
		"Plays instead of spectating, without reconnecting where the server allows it.");
	Cmd_AddCommand ("observe", CL_Observe_f,
		"Spectates instead of playing, without reconnecting where the server allows it.");

	Cmd_AddCommand ("color", CL_Color_f,
		"Sets your shirt and pants colors, 0 to 16 (one number for both), or shows them. "
		"Usage: color [top] [bottom]");
	Cmd_AddCommand ("download", CL_Download_f, "Downloads a file from the server. Usage: download <file>");

	Cmd_AddCommand ("nextul", CL_NextUpload,
		"Sends the server the next block of the file being uploaded (the server asks for it).");
	Cmd_AddCommand ("stopul", CL_StopUpload,
		"Stops uploading a file to the server (the server sends it when it refuses the file).");

//
// forward to server commands
//
	Cmd_AddCommand ("kill", NULL, "Kills your player, a suicide (sent to the server).");
	Cmd_AddCommand ("viewpos", CL_ViewPos_f,
		"Prints your origin and view angles as a setpos command, to come back to the place.");
	Cmd_AddCommand ("setpos", NULL, "Moves you to an origin, looking a way when given; needs the server "
		"started with -cheats (sent to the server). Usage: setpos <x> <y> <z> [<pitch> <yaw> <roll>]");
	Cmd_AddCommand ("pause", CL_Pause_f,
		"Pauses or unpauses an MVD playing, else asks the server to (it must be pausable).");
	Cmd_AddCommand ("say", NULL, "Says something to everyone on the server. Usage: say <message>");
	Cmd_AddCommand ("say_team", NULL,
		"Says something to your team, or as a spectator to the spectators. Usage: say_team <message>");
	Cmd_AddCommand ("serverinfo", NULL, "Asks the server to list its serverinfo.");

}


/*
===============
CL_WriteConfiguration

Writes key bindings and archived cvars to config.cfg
===============
*/
void CL_WriteConfiguration (void)
{
	FILE	*f;

	if (host.initialized)
	{
		f = fopen (va("%s/config.cfg",com_gamedir), "w");
		if (!f)
		{
			Con_Printf ("Couldn't write config.cfg.\n");
			return;
		}
		
		Key_WriteBindings (f);
		Cvar_WriteVariables (f);

		fclose (f);
	}
}


//============================================================================


/*
==================
CL_UpdateSound

Hands the mixer the view and the ambient sound levels where it is
==================
*/
static void CL_UpdateSound (void)
{
	snd_listener_t	listener = {0};

	listener.viewentity = cl.playernum + 1;
	listener.frametime = (float)cls.frametime;
	if (cls.state == ca_active)
	{
		VectorCopy (r_refdef.vieworg, listener.origin);
		AngleVectors (r_refdef.viewangles, listener.forward, listener.right, listener.up);
		if (cl.map)
			listener.ambient_levels = CM_LeafAmbientLevels (CM_PointInLeaf (cl.map, listener.origin));
	}
	S_Update (&listener);
}

/*
==================
CL_MaxFPS
==================
*/
static float CL_MaxFPS (void)
{
	// an MVD has no commands to pace: drawn as a live game is
	if (CL_IndependentPhysics () || cls.mvdplayback)
		return cl_maxfps.value > 0 ? fmaxf (cl_maxfps.value, 30.0f) : 0;	// 0: no cap but the display's
	if (cl_maxfps.value)
		return fmaxf(30.0f, fminf(cl_maxfps.value, 72.0f));
	return fmaxf(30.0f, fminf(rate.value/80.0f, 72.0f));
}

/*
==================
CL_PhysFrameTime

Seconds between commands: cl_physfps, at most the server's maxfps
==================
*/
static double CL_PhysFrameTime (void)
{
	float	fps, servermax;

	fps = cl_physfps.value > 0 ? cl_physfps.value : 77;
	servermax = (float)atof (Info_ValueForKey (cl.serverinfo, "maxfps"));
	if (servermax > 0)
		fps = fminf (fps, servermax);
	return 1.0 / fmaxf (fps, 10);
}

/*
==================
CL_DecidePhysFrame

Whether this frame makes and sends a command
==================
*/
static void CL_DecidePhysFrame (void)
{
	double	minframetime;

	if (!CL_IndependentPhysics ())
	{
		cls.physframe = true;
		cls.physframetime = cls.frametime;
		cls.physaccum = 0;
		return;
	}

	minframetime = CL_PhysFrameTime ();
	cls.physaccum += cls.frametime;
	if (cls.physaccum < minframetime)
	{
		cls.physframe = false;
		return;
	}
	cls.physframe = true;
	// when frames are slower than commands, one command covers the whole time
	cls.physframetime = cls.physaccum > minframetime * 2 ? cls.physaccum : minframetime;
	cls.physaccum -= cls.physframetime;
}

/*
==================
CL_FrameWait

Seconds until CL_Frame will draw the next frame
==================
*/
double CL_FrameWait (void)
{
	double	wait;
	float	fps;

	fps = CL_MaxFPS ();
	if (cls.timedemo)
		return 0;

// yield the CPU when nobody watches: a little while not the focus,
// more when minimized or paused
	if ((VID_IsMinimized () || (cl.paused && !VID_IsActive ())) && (!fps || fps > 20))
		fps = 20;
	else if (!VID_IsActive () && (!fps || fps > 50))
		fps = 50;
	if (!fps)
		return 0;

	wait = oldrealtime + 1.0 / fps - host.realtime;
	return wait > 0 ? wait : 0;
}

/*
==================
CL_Frame

Reads the server's packets, sends a command when one is due, and draws
==================
*/
void CL_Frame (void)
{
	static double		time1 = 0;
	static double		time2 = 0;
	static double		time3 = 0;
	int			pass1, pass2, pass3;
	float fps;
	int			oldincoming;
	bool		repredict;

	if (oldrealtime > host.realtime)
		oldrealtime = 0;

	fps = CL_MaxFPS ();

	if (!cls.timedemo && fps && host.realtime - oldrealtime < 1.0/fps)
		return;			// framerate is too high
	// a timedemo draws as fast as it can: no cl_maxfps (above), no vsync
	VID_SetUnpaced (cls.timedemo);

	cls.frametime = host.realtime - oldrealtime;
	oldrealtime = host.realtime;
	if (cls.frametime > 0.2)
		cls.frametime = 0.2;
	CL_DecidePhysFrame ();

	// fetch results from server
	oldincoming = cls.netchan.incoming_sequence;
	CL_QTVFrame ();
	if (cls.mvdplayback)
		CL_MVDAdvance ();
	CL_ReadPackets ();

	// send intentions now
	// resend a connection request if necessary
	if (cls.state == ca_disconnected)
		CL_CheckForResend ();
	else if (cls.physframe)
		CL_SendCmd ();
	else
	{	// the mouse turns the view between commands too
		usercmd_t	dummy = {0};

		IN_Move (&dummy);
	}

	// predict again when a command was made or the server said something new
	repredict = cls.physframe || cls.netchan.incoming_sequence != oldincoming;
	if (repredict)
		CL_SetUpPlayerPrediction(false);	// other players, without prediction
	CL_PredictMove (repredict);
	if (repredict)
		CL_SetUpPlayerPrediction(true);		// other players, predicted

	// build a refresh entity list
	CL_EmitEntities ();

	// update video
	if (host_speeds.value)
		time1 = Sys_DoubleTime ();

	SCR_UpdateScreen ();
	CL_DumpTimedemoFrame ();

	if (host_speeds.value)
		time2 = Sys_DoubleTime ();
		
	// update audio
	CL_UpdateSound ();
	if (cls.state == ca_active)
		CL_DecayLights ();


	if (host_speeds.value)
	{
		pass1 = (int)((time1 - time3)*1000);
		time3 = Sys_DoubleTime ();
		pass2 = (int)((time2 - time1)*1000);
		pass3 = (int)((time3 - time2)*1000);
		Con_Printf ("%3i tot %3i server %3i gfx %3i snd\n",
					pass1+pass2+pass3, pass1, pass2, pass3);
	}

	cls.framecount++;
	cls.fps_count++;
}

static void simple_crypt(char *buf, int len)
{
	while (len--)
		*buf++ ^= 0xff;
}

static void CL_FixupModelNames (void)
{
	simple_crypt(emodel_name, sizeof(emodel_name) - 1);
	simple_crypt(pmodel_name, sizeof(pmodel_name) - 1);
	simple_crypt(prespawn_name,  sizeof(prespawn_name)  - 1);
	simple_crypt(modellist_name, sizeof(modellist_name) - 1);
	simple_crypt(soundlist_name, sizeof(soundlist_name) - 1);
}

//============================================================================

/*
====================
CL_Init
====================
*/
void CL_Init (void)
{
	Sys_mkdir ("qw");

	V_Init ();
	CL_FixupModelNames ();

	cls.net_message.data = cls.net_message_buf;
	cls.net_message.maxsize = sizeof(cls.net_message_buf);
	W_LoadWadFile ("gfx.wad");
	Key_Init ();
	Con_Init ();

	// a port the system picks, so that clients on one machine never meet
	if (!NET_OpenSocket (NS_CLIENT, PORT_ANY))
		Con_Printf ("No UDP socket, only local games\n");
	M_Init ();
	Mod_Init ();

	R_InitTextures ();

	cls.basepal = FS_LoadFile ("gfx/palette.lmp", NULL);
	if (!cls.basepal)
		Sys_Error ("Couldn't load gfx/palette.lmp");
	cls.colormap = FS_LoadFile ("gfx/colormap.lmp", NULL);
	if (!cls.colormap)
		Sys_Error ("Couldn't load gfx/colormap.lmp");
	R_InitPalette (cls.basepal, cls.colormap);
	VID_Init ();
	Draw_Init ();
	SCR_Init ();
	R_Init ();
	S_Init ();

	cls.state = ca_disconnected;
	Sbar_Init ();
	CL_InitLocal ();
	IN_Init ();
}

/*
===============
CL_Shutdown
===============
*/
void CL_Shutdown (void)
{
	CSQC_Shutdown ();
	CL_WriteConfiguration ();
	S_Shutdown ();
	IN_Shutdown ();
	if (cls.basepal)
		VID_Shutdown ();
}

/*
===============
CL_Drop

Leaves the game after an error; the demo loop stops too
===============
*/
void CL_Drop (void)
{
	CL_Disconnect ();
	cls.demonum = -1;
}

