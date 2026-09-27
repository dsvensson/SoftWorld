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



static cvar_t	rcon_address = {.name = "rcon_address", .string = ""};

static cvar_t	cl_timeout = {.name = "cl_timeout", .string = "60"};

cvar_t	cl_shownet = {.name = "cl_shownet", .string = "0"};	// can be 0, 1, or 2

cvar_t	cl_sbar		= {.name = "cl_sbar", .string = "0", .archive = true};
cvar_t	cl_hudswap	= {.name = "cl_hudswap", .string = "0", .archive = true};
// frames per second drawn; with independent physics 0 is no cap (vid_vsync still applies)
static cvar_t	cl_maxfps	= {.name = "cl_maxfps", .string = "0", .archive = true};

cvar_t	lookspring = {.name = "lookspring", .string = "0", .archive = true};
cvar_t	lookstrafe = {.name = "lookstrafe", .string = "0", .archive = true};
cvar_t	sensitivity = {.name = "sensitivity", .string = "3", .archive = true};

cvar_t	m_pitch = {.name = "m_pitch", .string = "0.022", .archive = true};
cvar_t	m_yaw = {.name = "m_yaw", .string = "0.022"};
cvar_t	m_forward = {.name = "m_forward", .string = "1"};
cvar_t	m_side = {.name = "m_side", .string = "0.8"};

cvar_t	cl_predict_players = {.name = "cl_predict_players", .string = "1"};
cvar_t	cl_predict_players2 = {.name = "cl_predict_players2", .string = "1"};
cvar_t	cl_solid_players = {.name = "cl_solid_players", .string = "1"};

static cvar_t  localid = {.name = "localid", .string = ""};

static bool allowremotecmd = true;

//
// info mirrors
//
static cvar_t	spectator = {.name = "spectator", .string = "", .userinfo = true};
cvar_t	name = {.name = "name", .string = "unnamed", .archive = true, .userinfo = true};
static cvar_t	team = {.name = "team", .string = "", .archive = true, .userinfo = true};
static cvar_t	skin = {.name = "skin", .string = "", .archive = true, .userinfo = true};
static cvar_t	topcolor = {.name = "topcolor", .string = "0", .archive = true, .userinfo = true};
static cvar_t	bottomcolor = {.name = "bottomcolor", .string = "0", .archive = true, .userinfo = true};
static cvar_t	rate = {.name = "rate", .string = "2500", .archive = true, .userinfo = true};
static cvar_t	noaim = {.name = "noaim", .string = "0", .archive = true, .userinfo = true};
static cvar_t	msg = {.name = "msg", .string = "1", .archive = true, .userinfo = true};


client_static_t	cls;
client_state_t	cl;


static double			connect_time = -1;		// for connection retransmits


static double		oldrealtime;			// last frame run


static cvar_t	host_speeds = {.name = "host_speeds", .string = "0"};			// set for running times
cvar_t	show_fps = {.name = "show_fps", .string = "0"};			// set for running times




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
void CL_Quit_f (void)
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
void CL_Version_f (void)
{
	Con_Printf ("Version %4.2f\n", VERSION);
	Con_Printf ("Exe: "__TIME__" "__DATE__"\n");
}


/*
=======================
CL_SendConnectPacket

called by CL_Connect_f and CL_CheckResend
======================
*/
void CL_SendConnectPacket (void)
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
void CL_CheckForResend (void)
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
void CL_Connect_f (void)
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
void CL_Rcon_f (void)
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

// wipe the entire cl structure
	if (cl.map)
		CM_FreeMap (cl.map);
	memset (&cl, 0, sizeof(cl));

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

	VID_SetCaption ("QuakeWorld: disconnected");

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

	if (cls.download) {
		fclose(cls.download);
		cls.download = NULL;
	}

	CL_StopUpload();

}

void CL_Disconnect_f (void)
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
void CL_User_f (void)
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
void CL_Users_f (void)
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

void CL_Color_f (void)
{
	// just for quake compatability...
	int		top, bottom;
	char	num[16];

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("\"color\" is \"%s %s\"\n",
			Info_ValueForKey (cls.userinfo, "topcolor"),
			Info_ValueForKey (cls.userinfo, "bottomcolor") );
		Con_Printf ("color <0-13> [0-13]\n");
		return;
	}

	if (Cmd_Argc() == 2)
		top = bottom = atoi(Cmd_Argv(1));
	else
	{
		top = atoi(Cmd_Argv(1));
		bottom = atoi(Cmd_Argv(2));
	}
	
	top &= 15;
	if (top > 13)
		top = 13;
	bottom &= 15;
	if (bottom > 13)
		bottom = 13;
	
	snprintf (num, sizeof(num), "%i", top);
	Cvar_Set ("topcolor", num);
	snprintf (num, sizeof(num), "%i", bottom);
	Cvar_Set ("bottomcolor", num);
}

/*
==================
CL_FullServerinfo_f

Sent by server when serverinfo changes
==================
*/
void CL_FullServerinfo_f (void)
{
	char *p;
	float v;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("usage: fullserverinfo <complete info string>\n");
		return;
	}

	Q_strncpyz (cl.serverinfo, Cmd_Argv(1), sizeof(cl.serverinfo));

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
void CL_FullInfo_f (void)
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
void CL_SetInfo_f (void)
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
void CL_Packet_f (void)
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
void CL_Changing_f (void)
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
void CL_Reconnect_f (void)
{
	if (cls.download)  // don't change when downloading
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
CL_ConnectionlessPacket

Responses to broadcasts, etc
=================
*/
void CL_ConnectionlessPacket (void)
{
	char	*s;
	int		c, magic;
	unsigned	mask;

    MSG_BeginReading (&cls.net_message);
    MSG_ReadLong ();        // skip the -1

	c = MSG_ReadByte ();
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
				cls.fteext = mask & CL_FTE_EXTENSIONS;
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
void CL_ReadPackets (void)
{
	while (CL_GetMessage())
	{
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
=====================
CL_Download_f
=====================
*/
void CL_Download_f (void)
{
	char *p, *q;

	if (cls.state == ca_disconnected)
	{
		Con_Printf ("Must be connected.\n");
		return;
	}

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage: download <datafile>\n");
		return;
	}

	snprintf (cls.downloadname, sizeof(cls.downloadname), "%s/%s", com_gamedir, Cmd_Argv(1));

	p = cls.downloadname;
	for (;;) {
		if ((q = strchr(p, '/')) != NULL) {
			*q = 0;
			Sys_mkdir(cls.downloadname);
			*q = '/';
			p = q + 1;
		} else
			break;
	}

	Q_strncpyz(cls.downloadtempname, cls.downloadname, sizeof(cls.downloadtempname));
	cls.download = fopen (cls.downloadname, "wb");
	cls.downloadtype = dl_single;

	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
	SZ_Print (&cls.netchan.message, va("download %s\n",Cmd_Argv(1)));
}


/*
===================
Cmd_ForwardToServer

adds the current command line as a clc_stringcmd to the client message.
things like godmode, noclip, etc, are commands directed to the server,
so when they are typed in at the console, they will need to be forwarded.
===================
*/
void Cmd_ForwardToServer (void)
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
void Cmd_ForwardToServer_f (void)
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
		SZ_Print (&cls.netchan.message, va("pext 0x%x 0x%x 0x%x 0x%x", PROTOCOL_VERSION_FTE, CL_FTE_EXTENSIONS,
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

	r_scene.numvisedicts = &cl.numvisedicts;
	r_scene.maxvisedicts = MAX_VISEDICTS;
	r_scene.dlights = cl.dlights;
	r_scene.lightstyles = cl.lightstyles;
	r_scene.viewent = &cl.viewent;

	Info_SetValueForKey (cls.userinfo, "name", "unnamed", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "topcolor", "0", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "bottomcolor", "0", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "rate", "2500", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	Info_SetValueForKey (cls.userinfo, "msg", "1", MAX_INFO_STRING, INFO_CHARSET_USERINFO);
	snprintf (st, sizeof(st), "%4.2f-%04d", VERSION, build_number());
	Info_SetValueForStarKey (cls.userinfo, "*ver", st, MAX_INFO_STRING, INFO_CHARSET_USERINFO);

	CL_InitInput ();
	CL_InitTEnts ();
	CL_InitPrediction ();
	CL_InitCam ();
	
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
	Cvar_RegisterVariable (&lookspring);
	Cvar_RegisterVariable (&lookstrafe);
	Cvar_RegisterVariable (&sensitivity);

	Cvar_RegisterVariable (&m_pitch);
	Cvar_RegisterVariable (&m_yaw);
	Cvar_RegisterVariable (&m_forward);
	Cvar_RegisterVariable (&m_side);

	Cvar_RegisterVariable (&rcon_address);

	Cvar_RegisterVariable (&cl_predict_players2);
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


	Cmd_AddCommand ("version", CL_Version_f);
	Cmd_AddCommand ("cmd", Cmd_ForwardToServer_f);
	Cmd_SetForwardHandler (Cmd_ForwardToServer);

	Cmd_AddCommand ("changing", CL_Changing_f);
	Cmd_AddCommand ("disconnect", CL_Disconnect_f);
	Cmd_AddCommand ("record", CL_Record_f);
	Cmd_AddCommand ("rerecord", CL_ReRecord_f);
	Cmd_AddCommand ("stop", CL_Stop_f);
	Cmd_AddCommand ("playdemo", CL_PlayDemo_f);
	Cmd_AddCommand ("timedemo", CL_TimeDemo_f);
	CL_InitDemo ();

	Cmd_AddCommand ("skins", Skin_Skins_f);
	Cmd_AddCommand ("allskins", Skin_AllSkins_f);

	Cmd_AddCommand ("quit", CL_Quit_f);

	Cmd_AddCommand ("connect", CL_Connect_f);
	Cmd_AddCommand ("reconnect", CL_Reconnect_f);

	Cmd_AddCommand ("rcon", CL_Rcon_f);
	Cmd_AddCommand ("packet", CL_Packet_f);
	Cmd_AddCommand ("user", CL_User_f);
	Cmd_AddCommand ("users", CL_Users_f);

	Cmd_AddCommand ("setinfo", CL_SetInfo_f);
	Cmd_AddCommand ("fullinfo", CL_FullInfo_f);
	Cmd_AddCommand ("fullserverinfo", CL_FullServerinfo_f);

	Cmd_AddCommand ("color", CL_Color_f);
	Cmd_AddCommand ("download", CL_Download_f);

	Cmd_AddCommand ("nextul", CL_NextUpload);
	Cmd_AddCommand ("stopul", CL_StopUpload);

//
// forward to server commands
//
	Cmd_AddCommand ("kill", NULL);
	Cmd_AddCommand ("pause", NULL);
	Cmd_AddCommand ("say", NULL);
	Cmd_AddCommand ("say_team", NULL);
	Cmd_AddCommand ("serverinfo", NULL);

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
	if (CL_IndependentPhysics ())
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

	cls.frametime = host.realtime - oldrealtime;
	oldrealtime = host.realtime;
	if (cls.frametime > 0.2)
		cls.frametime = 0.2;
	CL_DecidePhysFrame ();

	// fetch results from server
	oldincoming = cls.netchan.incoming_sequence;
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

	if (!NET_OpenSocket (NS_CLIENT, PORT_CLIENT) && !NET_OpenSocket (NS_CLIENT, PORT_ANY))
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

