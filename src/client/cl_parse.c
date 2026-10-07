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
// cl_parse.c  -- parse a message received from the server

#include "cl_local.h"
#include "markup.h"

static const char *svc_strings[] =
{
	[0] = "svc_bad",
	[1] = "svc_nop",
	[2] = "svc_disconnect",
	[3] = "svc_updatestat",
	[4] = "svc_version",
	[5] = "svc_setview",
	[6] = "svc_sound",
	[7] = "svc_time",
	[8] = "svc_print",
	[9] = "svc_stufftext",
	[10] = "svc_setangle",
	[11] = "svc_serverdata",
	[12] = "svc_lightstyle",
	[13] = "svc_updatename",
	[14] = "svc_updatefrags",
	[15] = "svc_clientdata",
	[16] = "svc_stopsound",
	[17] = "svc_updatecolors",
	[18] = "svc_particle",
	[19] = "svc_damage",
	[20] = "svc_spawnstatic",
	[21] = "svc_fte_spawnstatic2",
	[22] = "svc_spawnbaseline",
	[23] = "svc_temp_entity",
	[24] = "svc_setpause",
	[25] = "svc_signonnum",
	[26] = "svc_centerprint",
	[27] = "svc_killedmonster",
	[28] = "svc_foundsecret",
	[29] = "svc_spawnstaticsound",
	[30] = "svc_intermission",
	[31] = "svc_finale",
	[32] = "svc_cdtrack",
	[33] = "svc_sellscreen",
	[34] = "svc_smallkick",
	[35] = "svc_bigkick",
	[36] = "svc_updateping",
	[37] = "svc_updateentertime",
	[38] = "svc_updatestatlong",
	[39] = "svc_muzzleflash",
	[40] = "svc_updateuserinfo",
	[41] = "svc_download",
	[42] = "svc_playerinfo",
	[43] = "svc_nails",
	[44] = "svc_chokecount",
	[45] = "svc_modellist",
	[46] = "svc_soundlist",
	[47] = "svc_packetentities",
	[48] = "svc_deltapacketentities",
	[49] = "svc_maxspeed",
	[50] = "svc_entgravity",
	[51] = "svc_setinfo",
	[52] = "svc_serverinfo",
	[53] = "svc_updatepl",
	[54] = "svc_nails2",
	[56] = "svc_fte_soundlistshort",
	[60] = "svc_fte_modellistshort",
	[66] = "svc_fte_spawnbaseline2",
	[76] = "svc_fte_csqcentities",
	[78] = "svc_fte_updatestatstring",
	[79] = "svc_fte_updatestatfloat",
	[83] = "svc_fte_cgamepacket",
	[84] = "svc_fte_voicechat",
	[90] = "svc_fte_cgamepacket_sized",
	[92] = "svc_fte_csqcentities_sized",
};

static const char *CL_SvcName (int cmd)
{
	if (cmd < 0 || cmd >= (int)(sizeof(svc_strings) / sizeof(svc_strings[0])) || !svc_strings[cmd])
		return "unknown";
	return svc_strings[cmd];
}


//=============================================================================


int CL_CalcNet (void)
{
	int		a, i;
	frame_t	*frame;
	int lost;

	for (i=cls.netchan.outgoing_sequence-UPDATE_BACKUP+1
		; i <= cls.netchan.outgoing_sequence
		; i++)
	{
		frame = &cl.frames[i&UPDATE_MASK];
		if (frame->receivedtime == -1)
			cl.packet_latency[i&NET_TIMINGSMASK] = 9999;	// dropped
		else if (frame->receivedtime == -2)
			cl.packet_latency[i&NET_TIMINGSMASK] = 10000;	// choked
		else if (frame->invalid)
			cl.packet_latency[i&NET_TIMINGSMASK] = 9998;	// invalid delta
		else
			cl.packet_latency[i&NET_TIMINGSMASK] = (int)((frame->receivedtime - frame->senttime)*20);
	}

	lost = 0;
	for (a=0 ; a<NET_TIMINGS ; a++)
	{
		i = (cls.netchan.outgoing_sequence-a) & NET_TIMINGSMASK;
		if (cl.packet_latency[i] == 9999)
			lost++;
	}
	return lost * 100 / NET_TIMINGS;
}

//=============================================================================

/*
=================
CL_SetModelChecksum

Servers check the player and eye models for cheats through userinfo
=================
*/
static void CL_SetModelChecksum (const char *modelname, const char *key)
{
	byte	*data;
	int		len;
	char	st[40];

	data = FS_LoadFile (modelname, &len);
	if (!data)
		return;
	snprintf (st, sizeof(st), "%d", (int)CRC_Block (data, len));
	Mem_Free (data);

	Info_SetValueForKey (cls.userinfo, key, st, MAX_INFO_STRING, INFO_CHARSET_USERINFO);

	if (cls.state >= ca_connected)
	{
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, va("setinfo %s %s", key, st));
	}
}

/*
=================
CL_ParseVWepPrecache

"//vwep vwplayer w_axe w_shot ...": the models for visible weapons, the first
the player without one; names without a path are in progs/, without an
extension .mdl (ezQuake)
=================
*/
static void CL_ParseVWepPrecache (const char *s)
{
	const char	*p;
	char		*vw;
	int			i, num;

	Cmd_TokenizeString ((char *)s + 2);
	if (Cmd_Argc () < 2)
	{
		cl.vwep_enabled = false;
		return;
	}
	if (cls.state == ca_active)
		return;		// they can be turned off in the game, not on

	num = Cmd_Argc () - 1;
	if (num > MAX_VWEP_MODELS)
		num = MAX_VWEP_MODELS;
	for (i=0 ; i<num ; i++)
	{
		p = Cmd_Argv (i+1);
		vw = cl.vw_model_name[i];
		if (!strcmp (p, "-"))
		{
			Q_strncpyz (vw, "-", MAX_QPATH);
			continue;
		}
		if (strstr (p, "..") || p[0] == '/' || p[0] == '\\' || strchr (p, ':'))
		{
			Con_Printf ("Ignoring the visible weapon model %s\n", p);
			vw[0] = 0;
			continue;
		}
		snprintf (vw, MAX_QPATH, "%s%s%s", strchr (p, '/') ? "" : "progs/", p, strchr (p, '.') ? "" : ".mdl");
	}
}

/*
=================
CL_Prespawn

Done with the models: the first of the static signon messages
=================
*/
static void CL_Prespawn (void)
{
	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
//	MSG_WriteString (&cls.netchan.message, va("prespawn %i 0 %i", cl.servercount, cl.worldmodel->checksum2));
	MSG_WriteString (&cls.netchan.message, va(prespawn_name, cl.servercount, cl.map_checksum2));
}

/*
=================
VWepModel_NextDownload

The visible weapons' models, when the server named them; a missing weapon is
drawn as the plain player, a missing player model turns them off (ezQuake)
=================
*/
static void VWepModel_NextDownload (void)
{
	int		i;

	// an MVD's players show their weapons with the usual models (ezQuake)
	if (cls.mvdplayback && !cl.vw_model_name[0][0] && !cls.downloadnumber)
		CL_ParseVWepPrecache ("//vwep vwplayer w_axe w_shot w_shot2 w_nail w_nail2 w_rock w_rock2 w_light");
	if ((!(cl.z_ext & Z_EXT_VWEP) && !cls.mvdplayback) || !cl.vw_model_name[0][0])
	{
		CL_Prespawn ();
		return;
	}

	cls.downloadtype = dl_vwep_model;
	for ( ; cls.downloadnumber < MAX_VWEP_MODELS ; cls.downloadnumber++)
	{
		if (!cl.vw_model_name[cls.downloadnumber][0] || cl.vw_model_name[cls.downloadnumber][0] == '-')
			continue;
		if (!CL_CheckOrDownloadFile (cl.vw_model_name[cls.downloadnumber]))
			return;		// started a download
	}

	for (i=0 ; i<MAX_VWEP_MODELS ; i++)
		if (cl.vw_model_name[i][0] && strcmp (cl.vw_model_name[i], "-"))
			cl.vw_model_precache[i] = Mod_ForName (cl.vw_model_name[i], false);
	cl.vwep_enabled = !strcmp (cl.vw_model_name[0], "-") || cl.vw_model_precache[0];

	CL_Prespawn ();
}

/*
=================
CL_StartCSQC

The csprogs the serverinfo offers, as FTE starts it before the models load:
any csprogs will do in a demo or with the server's anycsqc
=================
*/
static void CL_StartCSQC (void)
{
	char		*s = Info_ValueForKey (cl.serverinfo, "*csprogs"), *end;
	unsigned	checksum = (unsigned)strtoul (s, &end, 0);
	size_t		size = strtoul (Info_ValueForKey (cl.serverinfo, "*csprogssize"), NULL, 0);
	bool		anycsqc = atoi (Info_ValueForKey (cl.serverinfo, "anycsqc")) || cls.demoplayback;

	if (*end)
	{
		Con_Printf ("The serverinfo's *csprogs is corrupt\n");
		anycsqc = true;
		checksum = 0;
	}
	CSQC_Init (anycsqc, *s ? Info_ValueForKey (cl.serverinfo, "*csprogsname") : NULL, checksum, size);
}

/*
=================
Model_NextDownload
=================
*/
static void Model_NextDownload (void)
{
	char	*s;
	int		i;
	extern	char gamedirfile[];

	if (cls.downloadnumber == 0)
	{
		Con_Printf ("Checking models...\n");
		cls.downloadnumber = 1;
	}

	cls.downloadtype = dl_model;
	for (
		; cls.downloadnumber < MAX_MODELS && cl.model_name[cls.downloadnumber][0]
		; cls.downloadnumber++)
	{
		s = cl.model_name[cls.downloadnumber];
		if (s[0] == '*')
			continue;	// inline brush model
		if (!CL_CheckOrDownloadFile(s))
			return;		// started a download
	}

	CL_StartCSQC ();

	for (i=1 ; i<MAX_MODELS ; i++)
	{
		if (!cl.model_name[i][0])
			break;

		cl.model_precache[i] = Mod_ForName (cl.model_name[i], false);
		if (i == 1)
		{
			if (cl.map)
				CM_FreeMap (cl.map);
			cl.map = CM_LoadMap (cl.model_name[i], NULL, &cl.map_checksum2);
			cl.clipmodels[i] = cl.map ? CM_WorldModel (cl.map) : NULL;
		}
		else if (cl.model_name[i][0] == '*')
			cl.clipmodels[i] = CM_InlineModel (cl.map, cl.model_name[i]);
		if (!strcmp (cl.model_name[i], "progs/player.mdl"))
			CL_SetModelChecksum (cl.model_name[i], pmodel_name);
		else if (!strcmp (cl.model_name[i], "progs/eyes.mdl"))
			CL_SetModelChecksum (cl.model_name[i], emodel_name);

		// a recording plays without what it can't show, but not without its map
		if (!cl.model_precache[i] && i != 1 && cls.demoplayback)
		{
			Con_Printf ("Can't show %s\n", cl.model_name[i]);
			continue;
		}
		if (!cl.model_precache[i] || (i == 1 && !cl.clipmodels[i]))
		{
			Con_Printf ("\nThe required model file '%s' could not be found or downloaded.\n\n"
				, cl.model_name[i]);
			Con_Printf ("You may need to download or purchase a %s client "
				"pack in order to play on this server.\n\n", gamedirfile);
			CL_Disconnect ();
			return;
		}
	}

	// all done
	cl.worldmodel = cl.model_precache[1];
	r_scene.worldmodel = cl.worldmodel;
	R_NewMap ();
	CSQC_WorldLoaded ();
	CSQC_Announce ();

	// done with the model list: the visible weapons', then the static signon
	cls.downloadnumber = 0;
	VWepModel_NextDownload ();
}

/*
=================
Csprogs_NextDownload

Before the sounds, the csprogs the server offers, as FTE downloads it: the
server's csprogs.dat saved as csprogsvers/<checksum>.dat, unless a matching
one is here, CSQC is off, or the client runs the server
=================
*/
static void Sound_NextDownload (void);

static void Csprogs_NextDownload (void)
{
	char		*s = Info_ValueForKey (cl.serverinfo, "*csprogs"), *end;
	unsigned	checksum = (unsigned)strtoul (s, &end, 0);
	size_t		size = strtoul (Info_ValueForKey (cl.serverinfo, "*csprogssize"), NULL, 0);

	cls.downloadtype = dl_csprogs;
	if (cls.downloadnumber == 0 && *s && !*end && !cl_nocsqc.value && !cls.demoplayback && !SV_Active ()
		&& !CSQC_CheckDownload (Info_ValueForKey (cl.serverinfo, "*csprogsname"), checksum, size))
	{
		if (!cl_download_csprogs.value)
			Con_Printf ("Not downloading csprogs.dat: %s is off\n", cl_download_csprogs.name);
		else if (!CL_CheckOrDownloadFileAs ("csprogs.dat", va ("csprogsvers/%x.dat", checksum)))
			return;		// started a download
	}

	cls.downloadnumber = 0;
	Sound_NextDownload ();
}

/*
=================
Sound_NextDownload
=================
*/
static void Sound_NextDownload (void)
{
	char	*s;
	int		i;

	if (cls.downloadnumber == 0)
	{
		Con_Printf ("Checking sounds...\n");
		cls.downloadnumber = 1;
	}

	cls.downloadtype = dl_sound;
	for ( 
		; cl.sound_name[cls.downloadnumber][0]
		; cls.downloadnumber++)
	{
		s = cl.sound_name[cls.downloadnumber];
		if (!CL_CheckOrDownloadFile(va("sound/%s",s)))
			return;		// started a download
	}

	for (i=1 ; i<MAX_SOUNDS ; i++)
	{
		if (!cl.sound_name[i][0])
			break;
		cl.sound_precache[i] = S_PrecacheSound (cl.sound_name[i]);
	}

	// done with sounds, request models now
	memset (cl.model_precache, 0, sizeof(cl.model_precache));
	cl.playerindex = -1;
	cl.spikeindex = -1;
	cl.flagindex = -1;
	cl.h_playerindex = cl.gib1index = cl.gib2index = cl.gib3index = -1;
	cl.rocketindex = cl.grenadeindex = -1;
	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
//	MSG_WriteString (&cls.netchan.message, va("modellist %i 0", cl.servercount));
	MSG_WriteString (&cls.netchan.message, va(modellist_name, cl.servercount, 0));
}


/*
======================
CL_RequestNextDownload
======================
*/
void CL_RequestNextDownload (void)
{
	switch (cls.downloadtype)
	{
	case dl_single:
		break;
	case dl_skin:
		Skin_NextDownload ();
		break;
	case dl_model:
		Model_NextDownload ();
		break;
	case dl_sound:
		Sound_NextDownload ();
		break;
	case dl_vwep_model:
		VWepModel_NextDownload ();
		break;
	case dl_csprogs:
		Csprogs_NextDownload ();
		break;
	case dl_none:
	default:
		Con_DPrintf("Unknown download type.\n");
	}
}

static byte *upload_data;
static int upload_pos;
static int upload_size;

void CL_NextUpload(void)
{
	byte	buffer[1024];
	int		r;
	int		percent;
	int		size;

	if (!upload_data)
		return;

	r = upload_size - upload_pos;
	if (r > 768)
		r = 768;
	memcpy(buffer, upload_data + upload_pos, r);
	MSG_WriteByte (&cls.netchan.message, clc_upload);
	MSG_WriteShort (&cls.netchan.message, r);

	upload_pos += r;
	size = upload_size;
	if (!size)
		size = 1;
	percent = upload_pos*100/size;
	MSG_WriteByte (&cls.netchan.message, percent);
	SZ_Write (&cls.netchan.message, buffer, r);

Con_DPrintf ("UPLOAD: %6d: %d written\n", upload_pos - r, r);

	if (upload_pos != upload_size)
		return;

	Con_Printf ("Upload completed\n");

	free(upload_data);
	upload_data = 0;
	upload_pos = upload_size = 0;
}

void CL_StartUpload (byte *data, int size)
{
	if (cls.state < ca_onserver)
		return; // gotta be connected

	// override
	if (upload_data)
		free(upload_data);

Con_DPrintf("Upload starting of %d...\n", size);

	upload_data = malloc(size);
	memcpy(upload_data, data, size);
	upload_size = size;
	upload_pos = 0;

	CL_NextUpload();
} 

bool CL_IsUploading(void)
{
	if (upload_data)
		return true;
	return false;
}

void CL_StopUpload(void)
{
	if (upload_data)
		free(upload_data);
	upload_data = NULL;
}

/*
=====================================================================

  SERVER CONNECTING MESSAGES

=====================================================================
*/

/*
==================
CL_ParseServerData
==================
*/
static void CL_ParseServerData (void)
{
	char	*str;
	FILE	*f;
	char	fn[MAX_OSPATH];
	bool	cflag = false;
	extern	char	gamedirfile[MAX_OSPATH];
	int protover;
	unsigned	fteext2;
	
	Con_DPrintf ("Serverdata packet received.\n");
	// the next map's serverinfo brings it back
	CSQC_Shutdown ();
//
// wipe the client_state_t struct
//
	CL_ClearState ();

// the protocol extensions in use, (magic, mask) pairs, then the protocol
// version number
	cls.fteext = cls.mvdext1 = 0;
	fteext2 = 0;
	for (;;)
	{
		protover = MSG_ReadLong ();
		if (protover == PROTOCOL_VERSION_FTE)
			cls.fteext = (unsigned)MSG_ReadLong ();
		else if (protover == PROTOCOL_VERSION_FTE2)
			fteext2 = (unsigned)MSG_ReadLong ();
		else if (protover == PROTOCOL_VERSION_MVD1)
			cls.mvdext1 = (unsigned)MSG_ReadLong ();
		else
			break;
	}
	// a server only uses what the client asked for, but a demo can have been
	// recorded by a client that knows more
	// a recording's FTE2 bits other than voice chat are ignored, as ezQuake
	// ignores them all: recordings from mvdsv have been seen with 0x1, which
	// no mvdsv defines, and no other bit changes what the messages hold
	if (cls.demoplayback && (fteext2 & ~CL_FTE2_READABLE))
	{
		Con_DPrintf ("Ignoring FTE2 extensions 0x%x in the recording\n", fteext2 & ~CL_FTE2_READABLE);
		fteext2 &= CL_FTE2_READABLE;
	}
	if ((cls.fteext & ~CL_FTE_READABLE) || (fteext2 & ~CL_FTE2_READABLE) || (cls.mvdext1 & ~CL_MVD1_READABLE))
		Host_EndGame ("The server uses protocol extensions this client lacks:\n"
			"FTE 0x%x, FTE2 0x%x, MVD1 0x%x\n", cls.fteext & ~CL_FTE_READABLE, fteext2 & ~CL_FTE2_READABLE,
			cls.mvdext1 & ~CL_MVD1_READABLE);
	// the rest of this message is already in the new encoding
	cls.net_message.floatcoords = (cls.fteext & FTE_PEXT_FLOATCOORDS) != 0;
	cls.netchan.message.floatcoords = cls.net_message.floatcoords;
	Con_DPrintf ("Protocol extensions: FTE 0x%x, MVD1 0x%x\n", cls.fteext, cls.mvdext1);

// allow 2.2 and 2.29 demos to play
	if (protover != PROTOCOL_VERSION && 
		!(cls.demoplayback && (protover == 26 || protover == 27 || protover == 28)))
		Host_EndGame ("Server returned version %i, not %i\nYou probably need to upgrade.\nCheck http://www.quakeworld.net/", protover, PROTOCOL_VERSION);

	cl.servercount = MSG_ReadLong ();

	// game directory
	str = MSG_ReadString ();

	if (Q_strcasecmp (gamedirfile, str)) {
		// save current config
		CL_WriteConfiguration (); 
		cflag = true;
	}

	COM_Gamedir(str);
	FS_FlushGamedir ();		// the last level's models and sounds are done with

	//ZOID--run the autoexec.cfg in the gamedir
	//if it exists
	if (cflag) {
		snprintf(fn, sizeof(fn), "%s/%s", com_gamedir, "config.cfg");
		if ((f = fopen(fn, "r")) != NULL) {
			fclose(f);
			Cbuf_AddText ("cl_warncmd 0\n");
			Cbuf_AddText("exec config.cfg\n");
			Cbuf_AddText("exec frontend.cfg\n");
			Cbuf_AddText ("cl_warncmd 1\n");
		}
	}

	if (cls.mvdplayback)
	{	// an MVD: the server's clock when it began; the watcher is the last slot
		cl.mvd_server_time = MSG_ReadFloat ();
		cl.playernum = MAX_CLIENTS - 1;
		cl.spectator = true;
	}
	else
	{
		// parse player slot, high bit means spectator
		cl.playernum = MSG_ReadByte ();
		if (cl.playernum & 128)
		{
			cl.spectator = true;
			cl.playernum &= ~128;
		}
	}
	cl.viewplayer = cl.playernum;

	// get the full level name
	str = MSG_ReadString ();
	strncpy (cl.levelname, str, sizeof(cl.levelname)-1);

	// get the movevars
	cl.movevars.gravity			= MSG_ReadFloat();
	cl.movevars.stopspeed          = MSG_ReadFloat();
	cl.movevars.maxspeed           = MSG_ReadFloat();
	cl.movevars.spectatormaxspeed  = MSG_ReadFloat();
	cl.movevars.accelerate         = MSG_ReadFloat();
	cl.movevars.airaccelerate      = MSG_ReadFloat();
	cl.movevars.wateraccelerate    = MSG_ReadFloat();
	cl.movevars.friction           = MSG_ReadFloat();
	cl.movevars.waterfriction      = MSG_ReadFloat();
	cl.movevars.entgravity         = MSG_ReadFloat();

	// seperate the printfs so the server message can have a color
	Con_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
	Con_Printf ("%c%s\n", 2, str);

	// ask for the sound list next
	memset(cl.sound_name, 0, sizeof(cl.sound_name));
	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
//	MSG_WriteString (&cls.netchan.message, va("soundlist %i 0", cl.servercount));
	MSG_WriteString (&cls.netchan.message, va(soundlist_name, cl.servercount, 0));

	// now waiting for downloads, etc
	cls.state = ca_onserver;
}

/*
==================
CL_Model
==================
*/
model_t *CL_Model (int index)
{
	if (index < 0 || index >= MAX_MODELS)
		return NULL;
	return cl.model_precache[index];
}

/*
==================
CL_ParseSoundlist

A part of the sound list: the number before its first name (a short with
FTE's svc_fte_soundlistshort), the names, then the low byte of the number to
ask for the rest from, 0 when there is no more
==================
*/
static void CL_ParseSoundlist (bool shortstart)
{
	int	numsounds;
	char	*str;
	int n;

// precache sounds
//	memset (cl.sound_precache, 0, sizeof(cl.sound_precache));

	numsounds = shortstart ? MSG_ReadShort () & 0xffff : MSG_ReadByte ();

	for (;;) {
		str = MSG_ReadString ();
		if (!str[0])
			break;
		numsounds++;
		if (numsounds >= MAX_SOUNDS)
			Host_EndGame ("Server sent too many sound_precache");
		Q_strncpyz (cl.sound_name[numsounds], str, sizeof(cl.sound_name[numsounds]));
	}

	n = MSG_ReadByte();
	if (n)
		n += numsounds & 0xff00;

	if (n) {
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
//		MSG_WriteString (&cls.netchan.message, va("soundlist %i %i", cl.servercount, n));
		MSG_WriteString (&cls.netchan.message, va(soundlist_name, cl.servercount, n));
		return;
	}

	cls.downloadnumber = 0;
	Csprogs_NextDownload ();
}

/*
==================
CL_ParseModellist

As CL_ParseSoundlist, with svc_fte_modellistshort
==================
*/
static void CL_ParseModellist (bool shortstart)
{
	int	nummodels;
	char	*str;
	int n;

// precache models and note certain default indexes
	nummodels = shortstart ? MSG_ReadShort () & 0xffff : MSG_ReadByte ();

	for (;;)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		nummodels++;
		if (nummodels >= MAX_MODELS)
			Host_EndGame ("Server sent too many model_precache");
		Q_strncpyz (cl.model_name[nummodels], str, sizeof(cl.model_name[nummodels]));

		if (!strcmp(cl.model_name[nummodels],"progs/spike.mdl"))
			cl.spikeindex = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/player.mdl"))
			cl.playerindex = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/flag.mdl"))
			cl.flagindex = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/h_player.mdl"))
			cl.h_playerindex = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/gib1.mdl"))
			cl.gib1index = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/gib2.mdl"))
			cl.gib2index = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/gib3.mdl"))
			cl.gib3index = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/missile.mdl"))
			cl.rocketindex = nummodels;
		if (!strcmp(cl.model_name[nummodels],"progs/grenade.mdl"))
			cl.grenadeindex = nummodels;
	}

	n = MSG_ReadByte();
	if (n)
		n += nummodels & 0xff00;

	if (n) {
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
//		MSG_WriteString (&cls.netchan.message, va("modellist %i %i", cl.servercount, n));
		MSG_WriteString (&cls.netchan.message, va(modellist_name, cl.servercount, n));
		return;
	}

	cls.downloadnumber = 0;
	cls.downloadtype = dl_model;
	Model_NextDownload ();
}

/*
==================
CL_ParseBaseline
==================
*/
static void CL_ParseBaseline (entity_state_t *es)
{
	int			i;

	// what it doesn't send (alpha, colormod) is as a delta from nothing leaves it
	*es = (entity_state_t){0};
	es->modelindex = MSG_ReadByte ();
	es->frame = MSG_ReadByte ();
	es->colormap = MSG_ReadByte();
	es->skinnum = MSG_ReadByte();
	for (i=0 ; i<3 ; i++)
	{
		es->origin[i] = MSG_ReadCoord ();
		es->angles[i] = MSG_ReadAngle ();
	}
}


/*
==================
CL_ParseBaseline2

FTE's svc_fte_spawnbaseline2: an entity delta from nothing
==================
*/
static void CL_ParseBaseline2 (void)
{
	static const entity_state_t	nullstate = {0};
	int		num, bits, ext;

	num = MSG_ReadEntityHeader (MSG_ReadShort () & 0xffff, &bits, &ext, cls.fteext);
	MSG_ReadDeltaEntity (&nullstate, &cl.baselines[num], num, bits, ext, cls.mvdext1);
}

/*
=====================
CL_ParseStatic

Static entities are non-interactive world objects
like torches; FTE's svc_fte_spawnstatic2 sends them as deltas from nothing
=====================
*/
static void CL_ParseStatic (bool delta)
{
	static const entity_state_t	nullstate = {0};
	entity_t *ent;
	int		i, num, bits, ext;
	entity_state_t	es;
	model_t	*model;

	if (delta)
	{
		num = MSG_ReadEntityHeader (MSG_ReadShort () & 0xffff, &bits, &ext, cls.fteext);
		MSG_ReadDeltaEntity (&nullstate, &es, num, bits, ext, cls.mvdext1);
	}
	else
		CL_ParseBaseline (&es);
	model = CL_Model (es.modelindex);
	if (!model)
		return;
		
	i = cl.num_statics;
	if (!(i % STATIC_BLOCK))
	{
		cl.static_blocks = Mem_Realloc (cl.static_blocks, (size_t)(i / STATIC_BLOCK + 1) * sizeof(*cl.static_blocks));
		cl.static_blocks[i / STATIC_BLOCK] = Mem_Calloc (STATIC_BLOCK, sizeof(entity_t));
	}
	ent = CL_StaticEntity (i);
	cl.num_statics++;

// copy it to the current state
	ent->model = model;
	ent->alpha = es.alpha;
	ent->frame = es.frame;
	ent->translate = NULL;
	ent->palette = NULL;
	ent->skinnum = es.skinnum;

	VectorCopy (es.origin, ent->origin);
	VectorCopy (es.angles, ent->angles);
	
	R_AddEfrags (ent);
}

/*
===================
CL_ParseStaticSound
===================
*/
static void CL_ParseStaticSound (void)
{
	vec3_t		org;
	int			sound_num, vol, atten;
	int			i;
	
	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord ();
	sound_num = MSG_ReadByte ();
	vol = MSG_ReadByte ();
	atten = MSG_ReadByte ();
	
	S_StaticSound (cl.sound_precache[sound_num], org, (float)vol, (float)atten);
}


/*
=====================================================================

ACTION MESSAGES

=====================================================================
*/

/*
==================
CL_Unseen

A message that shows or sounds something the watcher doesn't get: an MVD's
for another player than the one followed, or any a scan or a seek reads
==================
*/
static bool CL_Unseen (void)
{
	return CL_MVDSkipMessage () || CL_MVDQuiet ();
}

/*
==================
CL_ScoreClock

The clock the scoreboard's times are on: an MVD's own, else real time
==================
*/
double CL_ScoreClock (void)
{
	return cls.mvdplayback ? cl.time : host.realtime;
}

/*
==================
CL_ParseStartSoundPacket
==================
*/
static void CL_ParseStartSoundPacket(void)
{
    vec3_t  pos;
    int 	channel, ent;
    int 	sound_num;
    int 	packetvolume;
    float 	attenuation;
 	int		i;

    channel = MSG_ReadShort();

    if (channel & SND_VOLUME)
		packetvolume = MSG_ReadByte ();
	else
		packetvolume = DEFAULT_SOUND_PACKET_VOLUME;
	
    if (channel & SND_ATTENUATION)
		attenuation = MSG_ReadByte () / 64.0f;
	else
		attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;
	
	sound_num = MSG_ReadByte ();

	for (i=0 ; i<3 ; i++)
		pos[i] = MSG_ReadCoord ();
 
	ent = (channel>>3)&1023;
	channel &= 7;

	if (ent > MAX_EDICTS)
		Host_EndGame ("CL_ParseStartSoundPacket: ent = %i", ent);
	if (CL_Unseen ())
		return;
	// CSQC may take it, as its own sound played already
	if (CSQC_EventSound (ent, channel, cl.sound_name[sound_num], packetvolume/255.0f, attenuation, pos))
		return;

    S_StartSound (ent, channel, cl.sound_precache[sound_num], pos, packetvolume/255.0f, attenuation);
}       


/*
==================
CL_ParseClientdata

Server information pertaining to this client only, sent every frame
==================
*/
static void CL_ParseClientdata (void)
{
	int				i;
	float		latency;
	frame_t		*frame;

// calculate simulated time of message
	i = cls.netchan.incoming_acknowledged;
	cl.parsecount = i;
	i &= UPDATE_MASK;
	cl.parsecountmod = i;
	frame = &cl.frames[i];
	cl.parsecounttime = cl.frames[i].senttime;

	frame->receivedtime = host.realtime;

// calculate latency
	latency = (float)(frame->receivedtime - frame->senttime);

	if (latency < 0 || latency > 1.0)
	{
//		Con_Printf ("Odd latency: %5.2f\n", latency);
	}
	else
	{
	// drift the average latency towards the observed latency
		if (latency < cls.latency)
			cls.latency = latency;
		else
			cls.latency += 0.001f;	// drift up, so correction are needed
	}	
}

/*
=====================
CL_NewTranslation
=====================
*/
static void CL_NewTranslation (int slot)
{
	player_info_t	*player;
	char s[512];

	if (slot > MAX_CLIENTS)
		Sys_Error ("CL_NewTranslation: slot > MAX_CLIENTS");

	player = &cl.players[slot];

	Q_strncpyz(s, Info_ValueForKey(player->userinfo, "skin"), sizeof(s));
	COM_StripExtension(s, s);
	if (player->skin && !Q_strcasecmp (s, player->skin->name))
		player->skin = NULL;

	if (player->_topcolor != player->topcolor ||
		player->_bottomcolor != player->bottomcolor || !player->skin) {
		player->_topcolor = player->topcolor;
		player->_bottomcolor = player->bottomcolor;
		Skin_Colors (player);
	}
}

/*
==============
CL_UpdateUserinfo
==============
*/
void CL_ProcessUserInfo (int slot, player_info_t *player)
{
	strncpy (player->name, Info_ValueForKey (player->userinfo, "name"), sizeof(player->name)-1);
	player->topcolor = atoi(Info_ValueForKey (player->userinfo, "topcolor"));
	player->bottomcolor = atoi(Info_ValueForKey (player->userinfo, "bottomcolor"));
	if (Info_ValueForKey (player->userinfo, "*spectator")[0])
		player->spectator = true;
	else
		player->spectator = false;

	if (cls.state == ca_active)
		Skin_Find (player);

	CL_NewTranslation (slot);
}

/*
==============
CL_UpdateUserinfo
==============
*/
static void CL_UpdateUserinfo (void)
{
	int		slot;
	player_info_t	*player;

	slot = MSG_ReadByte ();
	if (slot >= MAX_CLIENTS)
		Host_EndGame ("CL_ParseServerMessage: svc_updateuserinfo > MAX_SCOREBOARD");

	player = &cl.players[slot];
	player->userid = MSG_ReadLong ();
	strncpy (player->userinfo, MSG_ReadString(), sizeof(player->userinfo)-1);

	CL_ProcessUserInfo (slot, player);
}

/*
==============
CL_SetInfo
==============
*/
static void CL_SetInfo (void)
{
	int		slot;
	player_info_t	*player;
	char key[MAX_MSGLEN];
	char value[MAX_MSGLEN];

	slot = MSG_ReadByte ();
	if (slot >= MAX_CLIENTS)
		Host_EndGame ("CL_ParseServerMessage: svc_setinfo > MAX_SCOREBOARD");

	player = &cl.players[slot];

	strncpy (key, MSG_ReadString(), sizeof(key) - 1);
	key[sizeof(key) - 1] = 0;
	strncpy (value, MSG_ReadString(), sizeof(value) - 1);
	value[sizeof(value) - 1] = 0;

	Con_DPrintf("SETINFO %s: %s=%s\n", player->name, key, value);

	// the server sets * keys too (mvdsv's *auth, *flag, a bot's *skill)
	Info_SetValueForStarKey (player->userinfo, key, value, MAX_INFO_STRING, INFO_CHARSET_USERINFO);

	CL_ProcessUserInfo (slot, player);
}

/*
==============
CL_ServerInfo
==============
*/
static void CL_ServerInfo (void)
{
	char key[MAX_MSGLEN];
	char value[MAX_MSGLEN];

	strncpy (key, MSG_ReadString(), sizeof(key) - 1);
	key[sizeof(key) - 1] = 0;
	strncpy (value, MSG_ReadString(), sizeof(value) - 1);
	value[sizeof(value) - 1] = 0;

	Con_DPrintf("SERVERINFO: %s=%s\n", key, value);

	Info_SetValueForStarKey (cl.serverinfo, key, value, MAX_SERVERINFO_STRING, INFO_CHARSET_USERINFO);
	CL_ProcessServerInfo ();
}

/*
=====================
CL_SetStat

A stat as an integer and a float, as FTE keeps them: svc_updatestat and
svc_updatestatlong set both from the integer, svc_fte_updatestatfloat both
from the float
=====================
*/
static void CL_SetStat (int stat, int value, float fvalue)
{
	int	j, target;

	if (stat < 0 || stat >= MAX_CL_STATS)
		return;

	// an MVD's stats are each player's own, shown for the one followed
	if (cls.mvdplayback)
	{
		target = CL_MVDStatTarget ();
		if (target < 0)
			return;
		cl.players[target].stats[stat] = value;
		cl.players[target].statsf[stat] = fvalue;
		if (target != CL_MVDTracking ())
			return;
	}


	if (stat == STAT_ITEMS)
	{	// set flash times
		for (j=0 ; j<32 ; j++)
			if ( (value & (1u<<j)) && !(cl.stats[stat] & (1u<<j)))
				cl.item_gettime[j] = (float)cl.time;
	}

	cl.stats[stat] = value;
	cl.statsf[stat] = fvalue;
}

// FTE's string stat: the player's own (an MVD's aren't kept)
static void CL_SetStatString (int stat, const char *value)
{
	if (stat < 0 || stat >= MAX_CL_STATS || cls.mvdplayback)
		return;
	if (cl.statsstr[stat])
		Mem_Free (cl.statsstr[stat]);
	cl.statsstr[stat] = Mem_Alloc (strlen (value) + 1);
	strcpy (cl.statsstr[stat], value);
}

void CL_FreeStatStrings (void)
{
	int		i;

	for (i = 0 ; i < MAX_CL_STATS ; i++)
	{
		if (cl.statsstr[i])
			Mem_Free (cl.statsstr[i]);
		cl.statsstr[i] = NULL;
	}
}

/*
==============
CL_MuzzleFlash
==============
*/
static void CL_MuzzleFlash (void)
{
	vec3_t		fv, rv, uv;
	dlight_t	*dl;
	int			i;
	player_state_t	*pl;

	i = MSG_ReadShort ();

	if ((unsigned)(i-1) >= MAX_CLIENTS || CL_MVDQuiet ())
		return;


	pl = &cl.frames[cl.parsecountmod].playerstate[i-1];

	dl = CL_AllocDlight (i);
	VectorCopy (pl->origin,  dl->origin);
	AngleVectors (pl->viewangles, fv, rv, uv);
		
	VectorMA (dl->origin, 18, fv, dl->origin);
	dl->radius = (float)(200 + (rand()&31));
	dl->minlight = 32;
	dl->die = (float)(cl.time + 0.1f);
	dl->color[0] = 0.2f;
	dl->color[1] = 0.1f;
	dl->color[2] = 0.05f;
	dl->color[3] = 0.7f;
}


/*
==================
CL_TurnPendingMoves

The server turns the moves it gets that the client sent before it saw a
teleport's new view angles (MVD1 high-lag teleport); the client turns its
copies it predicts from the same way, and keeps the view turning the mouse
did since. After a respawn the server gives them the new yaw instead.
==================
*/
static void CL_TurnPendingMoves (bool teleport)
{
	frame_t	*f;
	float	newyaw, delta;
	int		i;

	newyaw = cl.viewangles[YAW];
	delta = newyaw - cl.frames[cl.parsecountmod].cmd.angles[YAW];
	for (i=2 ; i<UPDATE_BACKUP-1 && cl.validsequence + i < cls.netchan.outgoing_sequence ; i++)
	{
		f = &cl.frames[(cl.validsequence + i) & UPDATE_MASK];
		if (teleport)
		{
			PM_RotateMove (&f->cmd, delta);
			cl.viewangles[YAW] = f->cmd.angles[YAW] + delta;
		}
		else
			f->cmd.angles[YAW] = newyaw;
	}
}

#define SHOWNET(x) if(cl_shownet.value==2)Con_Printf ("%3i:%s\n", msg_readcount-1, x);
/*
=====================
CL_ParseServerMessage
=====================
*/
/*
==================
CL_ChatNameLength

How much of a chat line is its sender's name as the server puts it: "name: ",
"(name): " in a team message, "[SPEC] name: " (ezQuake); 0 for none
==================
*/
static int CL_ChatNameLength (const char *s)
{
	const char	*sender;
	int			i, len, msglen = (int)strlen (s);

	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		if (!cl.players[i].name[0])
			continue;
		sender = Info_ValueForKey (cl.players[i].userinfo, "name");
		len = (int)strlen (sender);
		if (len + 2 <= msglen && s[len] == ':' && s[len+1] == ' ' && !strncmp (sender, s, len))
			return len + 2;
		if (s[0] == '(' && len + 4 <= msglen && !strncmp (s + len + 1, "): ", 3) && !strncmp (sender, s + 1, len))
			return len + 4;
		if (!strncmp (s, "[SPEC] ", 7) && len + 9 <= msglen && s[len+7] == ':' && s[len+8] == ' '
		 && !strncmp (sender, s + 7, len))
			return len + 9;
	}
	return 0;
}

/*
==================
CL_ChatText

A chat line as ezQuake shows it (its cl_parseWhiteText): in the other
charset, but what the sender put in braces, after the name, in the usual one
and without the braces. Color codes are kept as they are.
==================
*/
static void CL_ChatText (const char *in, char *out, size_t size)
{
	const char	*p, *close;
	size_t		len, n;
	int			sender = CL_ChatNameLength (in);

	for (p = in, len = 0 ; *p && len + 1 < size ; )
	{
		if (p - in >= sender && *p == '{' && (close = strchr (p + 1, '}')))
		{
			n = (size_t)(close - p - 1);
			if (n > size - 1 - len)
				n = size - 1 - len;
			memcpy (out + len, p + 1, n);
			len += n;
			p = close + 1;
			continue;
		}
		n = (size_t)Markup_CodeLength (p);
		if (n && n <= size - 1 - len)
		{
			memcpy (out + len, p, n);
			len += n;
			p += n;
			continue;
		}
		out[len++] = (char)(*p == '\n' || *p == '\r' ? *p : *p | 128);
		p++;
	}
	out[len] = 0;
}

static int	projectiles_frame;
void CL_ParseServerMessage (void)
{
	int			cmd;
	char		*s;
	char		chat[2048];
	int			i, j;
	float		f;

	cl.last_servermessage = host.realtime;
	// an MVD's frame is several messages: its nails are cleared once
	if (!cls.mvdplayback || cls.netchan.incoming_acknowledged != projectiles_frame)
		CL_ClearProjectiles ();
	projectiles_frame = cls.netchan.incoming_acknowledged;

//
// if recording demos, copy the message out
//
	if (cl_shownet.value == 1)
		Con_Printf ("%i ",cls.net_message.cursize);
	else if (cl_shownet.value == 2)
		Con_Printf ("------------------\n");


	CL_ParseClientdata ();

//
// parse the message
//
	while (1)
	{
		if (msg_badread)
		{
			Host_EndGame ("CL_ParseServerMessage: Bad server message");
			break;
		}

		cmd = MSG_ReadByte ();

		if (cmd == -1)
		{
			msg_readcount++;	// so the EOM showner has the right value
			SHOWNET("END OF MESSAGE");
			break;
		}

		SHOWNET(CL_SvcName (cmd));
	
	// other commands
		switch (cmd)
		{
		default:
			Host_EndGame ("CL_ParseServerMessage: Illegible server message %i", cmd);
			break;
			
		case svc_nop:
//			Con_Printf ("svc_nop\n");
			break;
			
		case svc_disconnect:
			if (cls.mvdplayback)
			{	// "EndOfDemo", or a level change: the end of the recording ends it
				if (msg_readcount < cls.net_message.cursize)
					MSG_ReadString ();
				break;
			}
			if (cls.state == ca_connected)
				Host_EndGame ("Server disconnected\n"
					"Server version may not be compatible");
			else
				Host_EndGame ("Server disconnected");
			break;

		case svc_print:
			i = MSG_ReadByte ();
			s = MSG_ReadString ();
			if (CL_Unseen ())
				break;
			if (i == PRINT_CHAT)
			{
				CL_FCheckRequest (s);
				S_LocalSound ("misc/talk.wav");
				CL_ChatText (s, chat, sizeof(chat));
				s = chat;
			}
			Con_Printf ("%s", s);
			break;

		case svc_centerprint:
			s = MSG_ReadString ();
			if (!CL_Unseen ())
				SCR_CenterPrint (s);
			break;
			
		case svc_stufftext:
			s = MSG_ReadString ();
			Con_DPrintf ("stufftext: %s\n", s);
			if (cls.mvdplayback)
				CL_MVDAnnouncements (s);
			if (!strncmp (s, "//vwep ", 7))
				CL_ParseVWepPrecache (s);
			else if (cls.mvdplayback && !strncmp (s, "//at ", 5))
				CL_MVDHint (s);
			else if (cls.state < ca_active || !CL_Unseen ())
				Cbuf_AddText (s);
			break;

		case svc_damage:
			if (!CL_Unseen ())
				V_ParseDamage ();
			else
			{
				MSG_ReadByte ();
				MSG_ReadByte ();
				for (i=0 ; i<3 ; i++)
					MSG_ReadCoord ();
			}
			break;
			
		case svc_serverdata:
			if (CL_MVDNewLevel ())
				return;				// a scan ends at the next level
			Cbuf_Execute ();		// make sure any stuffed commands are done
			CL_ParseServerData ();
			vid.recalc_refdef = true;	// leave full screen intermission
			break;
			
		case svc_setangle:
			if (cls.mvdplayback)
			{	// an MVD's names the player; the view turns with the player's
				// state, and doesn't turn to this one
				CL_MVDFixAngle (MSG_ReadByte ());
				for (i=0 ; i<3 ; i++)
					MSG_ReadAngle ();
				break;
			}
			// with MVD1 high-lag teleport first why: 1 a teleport, 2 a
			// respawn, 0 unknown
			j = (cls.mvdext1 & MVD_PEXT1_HIGHLAGTELEPORT) ? MSG_ReadByte () : 0;
			for (i=0 ; i<3 ; i++)
				cl.viewangles[i] = MSG_ReadAngle ();
			CL_DisableLerpMove ();
			if (j == 1 || j == 2)
			{
				Con_DPrintf ("View angles set by a %s: yaw %.1f\n", j == 1 ? "teleport" : "respawn", cl.viewangles[YAW]);
				CL_TurnPendingMoves (j == 1);
			}
//			cl.viewangles[PITCH] = cl.viewangles[ROLL] = 0;
			break;
			
		case svc_lightstyle:
			i = MSG_ReadByte ();
			if (i >= MAX_LIGHTSTYLES)
				Sys_Error ("svc_lightstyle > MAX_LIGHTSTYLES");
			Q_strncpyz (cl.lightstyles[i].map, MSG_ReadString(), sizeof(cl.lightstyles[i].map));
			cl.lightstyles[i].length = Q_strlen(cl.lightstyles[i].map);
			break;

		case svc_sound:
			CL_ParseStartSoundPacket();
			break;

		case svc_stopsound:
			i = MSG_ReadShort();
			if (!CL_Unseen ())
				S_StopSound(i>>3, i&7);
			break;
		
		case svc_updatefrags:
			i = MSG_ReadByte ();
			if (i >= MAX_CLIENTS)
				Host_EndGame ("CL_ParseServerMessage: svc_updatefrags > MAX_SCOREBOARD");
			cl.players[i].frags = MSG_ReadShort ();
			break;			

		case svc_updateping:
			i = MSG_ReadByte ();
			if (i >= MAX_CLIENTS)
				Host_EndGame ("CL_ParseServerMessage: svc_updateping > MAX_SCOREBOARD");
			cl.players[i].ping = MSG_ReadShort ();
			break;
			
		case svc_updatepl:
			i = MSG_ReadByte ();
			if (i >= MAX_CLIENTS)
				Host_EndGame ("CL_ParseServerMessage: svc_updatepl > MAX_SCOREBOARD");
			cl.players[i].pl = (byte)MSG_ReadByte ();
			break;
			
		case svc_updateentertime:
		// time is sent over as seconds ago
			i = MSG_ReadByte ();
			if (i >= MAX_CLIENTS)
				Host_EndGame ("CL_ParseServerMessage: svc_updateentertime > MAX_SCOREBOARD");
			cl.players[i].entertime = (float)(CL_ScoreClock () - MSG_ReadFloat ());
			break;
			
		case svc_spawnbaseline:
			i = MSG_ReadShort ();
			if (i < 0 || i >= MAX_EDICTS)
				Host_EndGame ("CL_ParseServerMessage: svc_spawnbaseline %i", i);
			CL_ParseBaseline (&cl.baselines[i]);
			break;
		case svc_fte_spawnbaseline2:
			CL_ParseBaseline2 ();
			break;
		case svc_spawnstatic:
			CL_ParseStatic (false);
			break;
		case svc_fte_spawnstatic2:
			CL_ParseStatic (true);
			break;
		case svc_temp_entity:
			if (!CSQC_ParseTempEntity ())
				CL_ParseTEnt ();
			break;

		case svc_killedmonster:
			cl.statsf[STAT_MONSTERS] = (float)++cl.stats[STAT_MONSTERS];
			break;

		case svc_foundsecret:
			cl.statsf[STAT_SECRETS] = (float)++cl.stats[STAT_SECRETS];
			break;

		case svc_updatestat:
			i = MSG_ReadByte ();
			j = MSG_ReadByte ();
			CL_SetStat (i, j, (float)j);
			break;
		case svc_updatestatlong:
			i = MSG_ReadByte ();
			j = MSG_ReadLong ();
			CL_SetStat (i, j, (float)j);
			break;
		case svc_fte_updatestatfloat:
			i = MSG_ReadByte ();
			f = MSG_ReadFloat ();
			CL_SetStat (i, (int)f, f);
			break;
		case svc_fte_updatestatstring:
			i = MSG_ReadByte ();
			CL_SetStatString (i, MSG_ReadString ());
			break;

		case svc_fte_csqcentities:
		case svc_fte_csqcentities_sized:
			CSQC_ParseEntities (cmd == svc_fte_csqcentities_sized);
			break;
		case svc_fte_cgamepacket:
		case svc_fte_cgamepacket_sized:
			CSQC_ParseEvent (cmd == svc_fte_cgamepacket_sized);
			break;
			
		case svc_spawnstaticsound:
			CL_ParseStaticSound ();
			break;

		case svc_cdtrack:
			cl.cdtrack = MSG_ReadByte ();
			break;

		case svc_intermission:
			cl.intermission = 1;
			cl.completed_time = (int)CL_ScoreClock ();
			vid.recalc_refdef = true;	// go to full screen
			for (i=0 ; i<3 ; i++)
				cl.simorg[i] = MSG_ReadCoord ();			
			for (i=0 ; i<3 ; i++)
				cl.simangles[i] = MSG_ReadAngle ();
			VectorCopy (vec3_origin, cl.simvel);
			break;

		case svc_finale:
			cl.intermission = 2;
			cl.completed_time = (int)CL_ScoreClock ();
			vid.recalc_refdef = true;	// go to full screen
			s = MSG_ReadString ();
			if (!CL_MVDQuiet ())
				SCR_CenterPrint (s);
			break;
			
		case svc_sellscreen:
			Cmd_ExecuteString ("help");
			break;

		case svc_smallkick:
			if (!CL_Unseen ())
				cl.punchangle = -2;
			break;
		case svc_bigkick:
			if (!CL_Unseen ())
				cl.punchangle = -4;
			break;

		case svc_muzzleflash:
			CL_MuzzleFlash ();
			break;

		case svc_updateuserinfo:
			if (CL_MVDSkipMessage ())
			{	// old recordings send blank userinfo to one watcher, then resend it
				MSG_ReadByte ();
				MSG_ReadLong ();
				MSG_ReadString ();
				break;
			}
			CL_UpdateUserinfo ();
			break;

		case svc_setinfo:
			CL_SetInfo ();
			break;

		case svc_serverinfo:
			CL_ServerInfo ();
			break;

		case svc_download:
			CL_ParseDownload ();
			break;

		case svc_playerinfo:
			CL_ParsePlayerinfo ();
			break;

		case svc_nails:
			CL_ParseProjectiles (false);
			break;
		case svc_nails2:
			CL_ParseProjectiles (true);
			break;

		case svc_fte_voicechat:		// in recordings; not asked for, skipped
			MSG_ReadByte ();
			MSG_ReadByte ();
			MSG_ReadByte ();
			j = MSG_ReadShort () & 0xffff;
			for (i=0 ; i<j && !msg_badread ; i++)
				MSG_ReadByte ();
			break;

		case svc_chokecount:		// some preceding packets were choked
			i = MSG_ReadByte ();
			for (j=0 ; j<i ; j++)
				cl.frames[ (cls.netchan.incoming_acknowledged-1-j)&UPDATE_MASK ].receivedtime = -2;
			break;

		case svc_modellist:
			CL_ParseModellist (false);
			break;
		case svc_fte_modellistshort:
			CL_ParseModellist (true);
			break;

		case svc_soundlist:
			CL_ParseSoundlist (false);
			break;
		case svc_fte_soundlistshort:
			CL_ParseSoundlist (true);
			break;

		case svc_packetentities:
			CL_ParsePacketEntities (false);
			break;

		case svc_deltapacketentities:
			CL_ParsePacketEntities (true);
			break;

		case svc_maxspeed :
			f = MSG_ReadFloat();
			if (!CL_MVDSkipMessage ())
				cl.movevars.maxspeed = f;
			break;

		case svc_entgravity :
			f = MSG_ReadFloat();
			if (!CL_MVDSkipMessage ())
				cl.movevars.entgravity = f;
			break;

		case svc_setpause:
			cl.paused = MSG_ReadByte ();
			break;

		}
	}

	CL_SetSolidEntities ();
}


