/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the included (GNU.txt) GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "cl_local.h"
#include "png.h"

void CL_FinishTimeDemo (void);

// every nth timedemo frame is written to <basedir>/frames, for comparing renderers
static cvar_t	timedemo_dump = {.name = "timedemo_dump", .string = "0"};

/*
==============
CL_InitDemo
==============
*/
void CL_InitDemo (void)
{
	Cvar_RegisterVariable (&timedemo_dump);
}

/*
==============
CL_DumpTimedemoFrame

The frame as the renderer drew it, before blends and gamma, SDR white clipped
==============
*/
void CL_DumpTimedemoFrame (void)
{
	char		path[MAX_OSPATH];
	byte		*rgb;
	unsigned	x, y;
	int			frame;

	if (!cls.timedemo || timedemo_dump.value < 1 || cls.state != ca_active)
		return;
	frame = cls.framecount - cls.td_startframe;
	if (frame % (int)timedemo_dump.value)
		return;

	rgb = Mem_Alloc ((size_t)vid.width * vid.height * 3);
	for (y = 0 ; y < vid.height ; y++)
		for (x = 0 ; x < vid.width ; x++)
		{
			pixel_t	p = vid.buffer[y * vid.rowpixels + x];
			byte	*out = rgb + (y * vid.width + x) * 3;

			out[0] = (byte)(RGB30_R (p) > 255 ? 255 : RGB30_R (p));
			out[1] = (byte)(RGB30_G (p) > 255 ? 255 : RGB30_G (p));
			out[2] = (byte)(RGB30_B (p) > 255 ? 255 : RGB30_B (p));
		}
	snprintf (path, sizeof(path), "%s/frames/%06d.png", FS_BaseDir (), frame);
	COM_CreatePath (path);
	if (!PNG_WriteRGB (path, (int)vid.width, (int)vid.height, rgb, (int)vid.width * 3))
		Con_Printf ("Couldn't write %s\n", path);
	Mem_Free (rgb);
}

/*
==============================================================================

DEMO CODE

When a demo is playing back, all NET_SendMessages are skipped, and
NET_GetMessages are read from the demo file.

Whenever cl.time gets past the last received message, another message is
read from the demo file.
==============================================================================
*/

/*
==============
CL_StopPlayback

Called when a demo file runs out, or the user starts a game
==============
*/
void CL_StopPlayback (void)
{
	if (!cls.demoplayback)
		return;

	if (cls.demofile)
		fclose (cls.demofile);
	cls.demofile = NULL;
	if (cls.mvdplayback)
		CL_MVDStop ();
	CL_QTVStop ();
	cls.state = ca_disconnected;
	cls.demoplayback = 0;

	if (cls.timedemo)
		CL_FinishTimeDemo ();
}

#define dem_cmd		0
#define dem_read	1
#define dem_set		2

/*
====================
CL_WriteDemoCmd

Writes the current user cmd
====================
*/
void CL_WriteDemoCmd (usercmd_t *pcmd)
{
	int		i;
	float	fl;
	byte	c;
	usercmd_t cmd;

//Con_Printf("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	fl = LittleFloat((float)host.realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_cmd;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	// correct for byte order, bytes don't matter
	cmd = *pcmd;

	for (i = 0; i < 3; i++)
		cmd.angles[i] = LittleFloat(cmd.angles[i]);
	cmd.forwardmove = LittleShort(cmd.forwardmove);
	cmd.sidemove    = LittleShort(cmd.sidemove);
	cmd.upmove      = LittleShort(cmd.upmove);

	fwrite(&cmd, sizeof(cmd), 1, cls.demofile);

	for (i=0 ; i<3 ; i++)
	{
		fl = LittleFloat (cl.viewangles[i]);
		fwrite (&fl, 4, 1, cls.demofile);
	}

	fflush (cls.demofile);
}

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length and view angles
====================
*/
void CL_WriteDemoMessage (sizebuf_t *msg)
{
	int		len;
	float	fl;
	byte	c;

//Con_Printf("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	if (!cls.demorecording)
		return;

	fl = LittleFloat((float)host.realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_read;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	len = LittleLong (msg->cursize);
	fwrite (&len, 4, 1, cls.demofile);
	fwrite (msg->data, msg->cursize, 1, cls.demofile);

	fflush (cls.demofile);
}

/*
====================
CL_GetDemoMessage

  FIXME...
====================
*/
bool CL_GetDemoMessage (void)
{
	int		r, i, j;
	float	f;
	float	demotime;
	byte	c;
	usercmd_t *pcmd;

	// read the time from the packet
	fread(&demotime, sizeof(demotime), 1, cls.demofile);
	demotime = LittleFloat(demotime);

// decide if it is time to grab the next message		
	if (cls.timedemo) {
		if (cls.td_lastframe < 0)
			cls.td_lastframe = demotime;
		else if (demotime > cls.td_lastframe) {
			cls.td_lastframe = demotime;
			// rewind back to time
			fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
					SEEK_SET);
			return 0;		// allready read this frame's message
		}
		if (!cls.td_starttime && cls.state == ca_active) {
			cls.td_starttime = (float)Sys_DoubleTime();
			cls.td_startframe = cls.framecount;
		}
		host.realtime = demotime; // warp
	} else if (!cl.paused && cls.state >= ca_onserver) {	// allways grab until fully connected
		if (host.realtime + 1.0 < demotime) {
			// too far back
			host.realtime = demotime - 1.0;
			// rewind back to time
			fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
					SEEK_SET);
			return 0;
		} else if (host.realtime < demotime) {
			// rewind back to time
			fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
					SEEK_SET);
			return 0;		// don't need another message yet
		}
	} else
		host.realtime = demotime; // we're warping

	if (cls.state < ca_demostart)
		Host_Error ("CL_GetDemoMessage: cls.state != ca_active");
	
	// get the msg type
	fread (&c, sizeof(c), 1, cls.demofile);
	
	switch (c) {
	case dem_cmd :
		// user sent input
		i = cls.netchan.outgoing_sequence & UPDATE_MASK;
		pcmd = &cl.frames[i].cmd;
		r = (int)fread (pcmd, sizeof(*pcmd), 1, cls.demofile);
		if (r != 1)
		{
			CL_StopPlayback ();
			return 0;
		}
		// byte order stuff
		for (j = 0; j < 3; j++)
			pcmd->angles[j] = LittleFloat(pcmd->angles[j]);
		pcmd->forwardmove = LittleShort(pcmd->forwardmove);
		pcmd->sidemove    = LittleShort(pcmd->sidemove);
		pcmd->upmove      = LittleShort(pcmd->upmove);
		cl.frames[i].senttime = demotime;
		cl.frames[i].receivedtime = -1;		// we haven't gotten a reply yet
		cls.netchan.outgoing_sequence++;
		for (i=0 ; i<3 ; i++)
		{
			r = (int)fread (&f, 4, 1, cls.demofile);
			cl.viewangles[i] = LittleFloat (f);
		}
		break;

	case dem_read:
		// get the next message
		fread (&cls.net_message.cursize, 4, 1, cls.demofile);
		cls.net_message.cursize = LittleLong (cls.net_message.cursize);
	//Con_Printf("read: %ld bytes\n", net_message.cursize);
		if (cls.net_message.cursize > MAX_MSGLEN)
			Sys_Error ("Demo message > MAX_MSGLEN");
		r = (int)fread (cls.net_message.data, cls.net_message.cursize, 1, cls.demofile);
		if (r != 1)
		{
			CL_StopPlayback ();
			return 0;
		}
		break;

	case dem_set :
		fread (&i, 4, 1, cls.demofile);
		cls.netchan.outgoing_sequence = LittleLong(i);
		fread (&i, 4, 1, cls.demofile);
		cls.netchan.incoming_sequence = LittleLong(i);
		break;

	default :
		Con_Printf("Corrupted demo.\n");
		CL_StopPlayback ();
		return 0;
	}

	return 1;
}

/*
====================
CL_GetMessage

Handles recording and playback of demos, on top of NET_ code
====================
*/
bool CL_GetMessage (void)
{
	if	(cls.demoplayback)
		return cls.mvdplayback ? CL_GetMVDMessage () : CL_GetDemoMessage ();

	if (!NET_GetPacket (NS_CLIENT, &cls.net_from, &cls.net_message))
		return false;

	CL_WriteDemoMessage (&cls.net_message);
	
	return true;
}


/*
====================
CL_Stop_f

stop recording a demo
====================
*/
void CL_Stop_f (void)
{
	if (!cls.demorecording)
	{
		Con_Printf ("Not recording a demo.\n");
		return;
	}

// write a disconnect message to the demo file
	SZ_Clear (&cls.net_message);
	MSG_WriteLong (&cls.net_message, -1);	// -1 sequence means out of band
	MSG_WriteByte (&cls.net_message, svc_disconnect);
	MSG_WriteString (&cls.net_message, "EndOfDemo");
	CL_WriteDemoMessage (&cls.net_message);

// finish up
	fclose (cls.demofile);
	cls.demofile = NULL;
	cls.demorecording = false;
	Con_Printf ("Completed demo\n");
}


/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length and view angles
====================
*/
void CL_WriteRecordDemoMessage (sizebuf_t *msg, int seq)
{
	int		len;
	int		i;
	float	fl;
	byte	c;

//Con_Printf("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	if (!cls.demorecording)
		return;

	fl = LittleFloat((float)host.realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_read;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	len = LittleLong (msg->cursize + 8);
	fwrite (&len, 4, 1, cls.demofile);

	i = LittleLong(seq);
	fwrite (&i, 4, 1, cls.demofile);
	fwrite (&i, 4, 1, cls.demofile);

	fwrite (msg->data, msg->cursize, 1, cls.demofile);

	fflush (cls.demofile);
}


void CL_WriteSetDemoMessage (void)
{
	int		len;
	float	fl;
	byte	c;

//Con_Printf("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	if (!cls.demorecording)
		return;

	fl = LittleFloat((float)host.realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_set;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	len = LittleLong(cls.netchan.outgoing_sequence);
	fwrite (&len, 4, 1, cls.demofile);
	len = LittleLong(cls.netchan.incoming_sequence);
	fwrite (&len, 4, 1, cls.demofile);

	fflush (cls.demofile);
}




/*
====================
CL_RecordNameList

The sound or model list as a server sends it: each message has the number
before its first name, a short past 255 with FTE's short form, and ends with
the low byte of the number to go on from, 0 when done. A message ends only
where that byte isn't 0.
====================
*/
static void CL_RecordNameList (sizebuf_t *buf, int *seq, int svc, int shortsvc, char (*names)[MAX_QPATH], int count)
{
	int		i, start;

	start = 0;
	for (i = 1 ; ; i++)
	{
		if (i == start + 1)
		{
			if (start > 255)
			{
				MSG_WriteByte (buf, shortsvc);
				MSG_WriteShort (buf, start);
			}
			else
			{
				MSG_WriteByte (buf, svc);
				MSG_WriteByte (buf, start);
			}
		}
		if (i == count || !names[i][0])
			break;
		MSG_WriteString (buf, names[i]);
		if (buf->cursize > MAX_MSGLEN/2 && (i & 255) && i + 1 < count && names[i+1][0])
		{
			MSG_WriteByte (buf, 0);
			MSG_WriteByte (buf, i & 255);
			CL_WriteRecordDemoMessage (buf, (*seq)++);
			SZ_Clear (buf);
			start = i;
		}
	}
	MSG_WriteByte (buf, 0);
	MSG_WriteByte (buf, 0);
	CL_WriteRecordDemoMessage (buf, (*seq)++);
	SZ_Clear (buf);
}

/*
====================
CL_RecordEntity

A static entity or a baseline, in the form the recording's protocol
extensions allow: FTE's deltas from nothing with SPAWNSTATIC2, else the
original form, which has no room for model numbers past 255
====================
*/
static void CL_RecordEntity (sizebuf_t *buf, bool isstatic, int number, const entity_state_t *es)
{
	static const entity_state_t	nullstate = {0};
	entity_state_t	s;
	int		i;

	if (cls.fteext & FTE_PEXT_SPAWNSTATIC2)
	{
		s = *es;
		s.number = number;
		MSG_WriteByte (buf, isstatic ? svc_fte_spawnstatic2 : svc_fte_spawnbaseline2);
		MSG_WriteDeltaEntity (buf, &nullstate, &s, true, cls.fteext, cls.mvdext1);
		return;
	}
	if (es->modelindex > 255)
		return;
	if (isstatic)
		MSG_WriteByte (buf, svc_spawnstatic);
	else
	{
		MSG_WriteByte (buf, svc_spawnbaseline);
		MSG_WriteShort (buf, number);
	}
	MSG_WriteByte (buf, es->modelindex);
	MSG_WriteByte (buf, es->frame);
	MSG_WriteByte (buf, es->colormap);
	MSG_WriteByte (buf, es->skinnum);
	for (i=0 ; i<3 ; i++)
	{
		MSG_WriteCoord (buf, es->origin[i]);
		MSG_WriteAngle (buf, es->angles[i]);
	}
}

/*
====================
CL_Record_f

record <demoname> <server>
====================
*/
void CL_Record_f (void)
{
	int		c;
	char	demopath[MAX_OSPATH];
	sizebuf_t	buf;
	byte	buf_data[MAX_MSGLEN];
	int i, j;
	entity_t *ent;
	entity_state_t *es, blankes, state;
	player_info_t *player;
	extern	char gamedirfile[];
	int seq = 1;

	c = Cmd_Argc();
	if (c != 2)
	{
		Con_Printf ("record <demoname>\n");
		return;
	}

	if (cls.state != ca_active) {
		Con_Printf ("You must be connected to record.\n");
		return;
	}
	// an MVD's messages aren't a client's, and seeking reads them twice
	if (cls.mvdplayback) {
		Con_Printf ("Can't record while playing an MVD.\n");
		return;
	}

	if (cls.demorecording)
		CL_Stop_f();

	snprintf (demopath, sizeof(demopath), "%s/%s", com_gamedir, Cmd_Argv(1));

//
// open the demo file
//
	COM_DefaultExtension (demopath, ".qwd");

	cls.demofile = fopen (demopath, "wb");
	if (!cls.demofile)
	{
		Con_Printf ("ERROR: couldn't open.\n");
		return;
	}

	Con_Printf ("recording to %s.\n", demopath);
	cls.demorecording = true;

/*-------------------------------------------------*/

// serverdata
	// send the info about the new client to all connected clients
	memset(&buf, 0, sizeof(buf));
	buf.data = buf_data;
	buf.maxsize = sizeof(buf_data);
	buf.floatcoords = (cls.fteext & FTE_PEXT_FLOATCOORDS) != 0;

// send the serverdata, with the protocol extensions in use
	MSG_WriteByte (&buf, svc_serverdata);
	if (cls.fteext)
	{
		MSG_WriteLong (&buf, PROTOCOL_VERSION_FTE);
		MSG_WriteLong (&buf, (int)cls.fteext);
	}
	if (cls.mvdext1)
	{
		MSG_WriteLong (&buf, PROTOCOL_VERSION_MVD1);
		MSG_WriteLong (&buf, (int)cls.mvdext1);
	}
	MSG_WriteLong (&buf, PROTOCOL_VERSION);
	MSG_WriteLong (&buf, cl.servercount);
	MSG_WriteString (&buf, gamedirfile);

	if (cl.spectator)
		MSG_WriteByte (&buf, cl.playernum | 128);
	else
		MSG_WriteByte (&buf, cl.playernum);

	// send full levelname
	MSG_WriteString (&buf, cl.levelname);

	// send the movevars
	MSG_WriteFloat(&buf, cl.movevars.gravity);
	MSG_WriteFloat(&buf, cl.movevars.stopspeed);
	MSG_WriteFloat(&buf, cl.movevars.maxspeed);
	MSG_WriteFloat(&buf, cl.movevars.spectatormaxspeed);
	MSG_WriteFloat(&buf, cl.movevars.accelerate);
	MSG_WriteFloat(&buf, cl.movevars.airaccelerate);
	MSG_WriteFloat(&buf, cl.movevars.wateraccelerate);
	MSG_WriteFloat(&buf, cl.movevars.friction);
	MSG_WriteFloat(&buf, cl.movevars.waterfriction);
	MSG_WriteFloat(&buf, cl.movevars.entgravity);

	// send music
	MSG_WriteByte (&buf, svc_cdtrack);
	MSG_WriteByte (&buf, 0); // none in demos

	// send server info string
	MSG_WriteByte (&buf, svc_stufftext);
	MSG_WriteString (&buf, va("fullserverinfo \"%s\"\n", cl.serverinfo) );

	// flush packet
	CL_WriteRecordDemoMessage (&buf, seq++);
	SZ_Clear (&buf); 

// soundlist and modellist
	CL_RecordNameList (&buf, &seq, svc_soundlist, svc_fte_soundlistshort, cl.sound_name, MAX_SOUNDS);
	CL_RecordNameList (&buf, &seq, svc_modellist, svc_fte_modellistshort, cl.model_name, MAX_MODELS);

// spawnstatic

	for (i = 0; i < cl.num_statics; i++) {
		ent = CL_StaticEntity (i);

		for (j = 1; j < MAX_MODELS; j++)
			if (ent->model == cl.model_precache[j])
				break;
		memset (&state, 0, sizeof(state));
		state.modelindex = j == MAX_MODELS ? 0 : j;
		state.frame = ent->frame;
		state.skinnum = ent->skinnum;
		state.alpha = ent->alpha;
		VectorCopy (ent->origin, state.origin);
		VectorCopy (ent->angles, state.angles);
		CL_RecordEntity (&buf, true, 1, &state);

		if (buf.cursize > MAX_MSGLEN/2) {
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf);
		}
	}

// spawnstaticsound
	// static sounds are skipped in demos, life is hard

// baselines; the world's is never used

	memset(&blankes, 0, sizeof(blankes));
	for (i = 1; i < MAX_EDICTS; i++) {
		es = cl.baselines + i;

		if (memcmp(es, &blankes, sizeof(blankes))) {
			CL_RecordEntity (&buf, false, i, es);

			if (buf.cursize > MAX_MSGLEN/2) {
				CL_WriteRecordDemoMessage (&buf, seq++);
				SZ_Clear (&buf);
			}
		}
	}

	MSG_WriteByte (&buf, svc_stufftext);
	MSG_WriteString (&buf, va("cmd spawn %i 0\n", cl.servercount) );

	if (buf.cursize) {
		CL_WriteRecordDemoMessage (&buf, seq++);
		SZ_Clear (&buf); 
	}

// send current status of all other players

	for (i = 0; i < MAX_CLIENTS; i++) {
		player = cl.players + i;

		MSG_WriteByte (&buf, svc_updatefrags);
		MSG_WriteByte (&buf, i);
		MSG_WriteShort (&buf, player->frags);
		
		MSG_WriteByte (&buf, svc_updateping);
		MSG_WriteByte (&buf, i);
		MSG_WriteShort (&buf, player->ping);
		
		MSG_WriteByte (&buf, svc_updatepl);
		MSG_WriteByte (&buf, i);
		MSG_WriteByte (&buf, player->pl);
		
		MSG_WriteByte (&buf, svc_updateentertime);
		MSG_WriteByte (&buf, i);
		MSG_WriteFloat (&buf, player->entertime);

		MSG_WriteByte (&buf, svc_updateuserinfo);
		MSG_WriteByte (&buf, i);
		MSG_WriteLong (&buf, player->userid);
		MSG_WriteString (&buf, player->userinfo);

		if (buf.cursize > MAX_MSGLEN/2) {
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
		}
	}
	
// send all current light styles
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
	{
		MSG_WriteByte (&buf, svc_lightstyle);
		MSG_WriteByte (&buf, (char)i);
		MSG_WriteString (&buf, cl.lightstyles[i].map);
	}

	for (i = 0; i < MAX_CL_STATS; i++) {
		MSG_WriteByte (&buf, svc_updatestatlong);
		MSG_WriteByte (&buf, i);
		MSG_WriteLong (&buf, cl.stats[i]);
		if (buf.cursize > MAX_MSGLEN/2) {
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
		}
	}


	// get the client to check and download skins
	// when that is completed, a begin command will be issued
	MSG_WriteByte (&buf, svc_stufftext);
	MSG_WriteString (&buf, va("skins\n") );

	CL_WriteRecordDemoMessage (&buf, seq++);

	CL_WriteSetDemoMessage();

	// done
}

/*
====================
CL_ReRecord_f

record <demoname>
====================
*/
void CL_ReRecord_f (void)
{
	int		c;
	char	demopath[MAX_OSPATH];

	c = Cmd_Argc();
	if (c != 2)
	{
		Con_Printf ("rerecord <demoname>\n");
		return;
	}

	if (!*cls.servername) {
		Con_Printf("No server to reconnect to...\n");
		return;
	}

	if (cls.demorecording)
		CL_Stop_f();

	snprintf (demopath, sizeof(demopath), "%s/%s", com_gamedir, Cmd_Argv(1));

//
// open the demo file
//
	COM_DefaultExtension (demopath, ".qwd");

	cls.demofile = fopen (demopath, "wb");
	if (!cls.demofile)
	{
		Con_Printf ("ERROR: couldn't open.\n");
		return;
	}

	Con_Printf ("recording to %s.\n", demopath);
	cls.demorecording = true;

	CL_Disconnect();
	CL_BeginServerConnect();
}


/*
====================
CL_Extension

A file name's extension, with its dot; "" for none
====================
*/
static const char *CL_Extension (const char *file)
{
	const char	*dot = strrchr (file, '.');

	if (!dot || strchr (dot, '/') || strchr (dot, '\\'))
		return "";
	return dot;
}

/*
====================
CL_PlayDemo_f

play [demoname]
====================
*/
void CL_PlayDemo_f (void)
{
	char	demoname[256];
	byte	*data;
	int		size;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("play <demoname> : plays a demo\n");
		return;
	}

//
// disconnect from server
//
	CL_Disconnect ();

//
// open the demo file
//
	// a name without an extension is a .qwd, else an .mvd
	Q_strncpyz (demoname, Cmd_Argv(1), sizeof(demoname));
	COM_DefaultExtension (demoname, ".qwd");
	size = COM_FOpenFile (demoname, &cls.demofile);
	if (!cls.demofile && !CL_Extension (Cmd_Argv(1))[0])
	{
		Q_strncpyz (demoname, Cmd_Argv(1), sizeof(demoname));
		COM_DefaultExtension (demoname, ".mvd");
		size = COM_FOpenFile (demoname, &cls.demofile);
	}

	Con_Printf ("Playing demo from %s.\n", demoname);
	if (!cls.demofile)
	{
		Con_Printf ("ERROR: couldn't open.\n");
		cls.demonum = -1;		// stop demo loop
		return;
	}

	// an MVD is played from memory
	if (!Q_strcasecmp (CL_Extension (demoname), ".mvd"))
	{
		data = Mem_Alloc (size > 0 ? size : 1);
		if ((int)fread (data, 1, size, cls.demofile) != size)
		{
			Con_Printf ("ERROR: couldn't read.\n");
			Mem_Free (data);
			fclose (cls.demofile);
			cls.demofile = NULL;
			return;
		}
		fclose (cls.demofile);
		cls.demofile = NULL;
		CL_MVDStart (data, (size_t)size);
	}

	cls.demoplayback = true;
	cls.state = ca_demostart;
	Netchan_Setup (&cls.netchan, cls.net_from, 0, NS_CLIENT);
	host.realtime = 0;
}

/*
====================
CL_FinishTimeDemo

====================
*/
void CL_FinishTimeDemo (void)
{
	int		frames;
	float	time;
	
	cls.timedemo = false;
	
// the first frame didn't count
	frames = (cls.framecount - cls.td_startframe) - 1;
	time = (float)(Sys_DoubleTime() - cls.td_starttime);
	if (!time)
		time = 1;
	Con_Printf ("%i frames %5.1f seconds %5.1f fps\n", frames, time, frames/time);
}

/*
====================
CL_TimeDemo_f

timedemo [demoname]
====================
*/
void CL_TimeDemo_f (void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf ("timedemo <demoname> : gets demo speeds\n");
		return;
	}

	CL_PlayDemo_f ();
	
	if (cls.state != ca_demostart)
		return;

// cls.td_starttime will be grabbed at the second frame of the demo, so
// all the loading time doesn't get counted
	
	cls.timedemo = true;
	cls.td_starttime = 0;
	cls.td_startframe = cls.framecount;
	cls.td_lastframe = -1;		// get a new message this frame
}

