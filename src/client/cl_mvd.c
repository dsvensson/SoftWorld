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
// cl_mvd.c  -- playing multiview demos
//
// An MVD is what a server records: every player's view, as blocks of server
// messages without netchan headers. A block is a time byte (msec since the
// block before), a type byte (the type in its low 3 bits, a player in the
// rest) and the type's payload; the blocks of one server frame share its
// time. dem_read and dem_all go to everybody, dem_single and dem_stats to one
// player, dem_multiple to the players in its mask (none: mvdsv's hidden data,
// skipped).
//
// Played on qualia's clock: a frame is read as soon as playback passes the
// one before it, so the two frames around the moment drawn are always in
// hand, and all blocks of a frame are read together. Each frame is one
// netchan sequence, so the parser's frame ring works as it does live.

#include "cl_local.h"

#define	DEM_CMD			0
#define	DEM_READ		1
#define	DEM_SET			2
#define	DEM_MULTIPLE	3
#define	DEM_SINGLE		4
#define	DEM_STATS		5
#define	DEM_ALL			6

#define	MVD_MAXBLOCK	8192	// the writer's blocks are at most 8092 bytes
#define	MVD_MAXLAG		1.0		// seconds: a recorded stall longer than this is skipped

static struct
{
	byte		*data;			// the whole recording, or what a stream has sent
	size_t		size;
	size_t		pos;			// the next block
	size_t		capacity;		// a stream's buffer
	bool		stream;			// QTV: a short block is one still arriving
	bool		closed;			// the stream's connection closed
	bool		held;			// the stream's cushion was held for this level
	double		hold_until;
	uint32_t	time;			// msec, up to pos
	uint32_t	frame;			// msec: the frame last delivered
	bool		started;		// a block has been delivered

	int			type;			// the block being parsed
	unsigned	to;				// its player, or its mask for dem_multiple

	int			track;			// the player the view follows, -1 none
	bool		picked;			// the user picked it: the server's hints are ignored
	bool		paused;

	// the clock, in demo seconds
	double		logical;		// the moment played
	double		old, next;		// the frames around it
	float		speed;			// 1 is real time
} mvd = {.track = -1};

static cvar_t	demo_speed = {.name = "demo_speed", .string = "1"};	// MVD playback speed, 1 is real time
// follow the player the server's "//at" hints name, until one is picked by hand
static cvar_t	demo_autotrack = {.name = "demo_autotrack", .string = "1", .archive = true};

static uint32_t MVD_Long (size_t at)
{
	return mvd.data[at] | (mvd.data[at+1] << 8) | (mvd.data[at+2] << 16) | ((uint32_t)mvd.data[at+3] << 24);
}

/*
==================
CL_MVDStart

Plays the recording in data, size bytes, which it keeps and frees
==================
*/
void CL_MVDStart (byte *data, size_t size)
{
	mvd.data = data;
	mvd.size = size;
	mvd.capacity = size;
	mvd.stream = false;
	mvd.pos = 0;
	mvd.time = mvd.frame = 0;
	mvd.started = false;
	mvd.type = DEM_ALL;
	mvd.to = 0;
	mvd.logical = mvd.old = mvd.next = 0;
	mvd.track = -1;
	mvd.picked = false;
	mvd.paused = false;
	cls.mvdplayback = true;
}

/*
==================
CL_MVDStartStream / CL_MVDFeed / CL_MVDStreamClosed

A QTV stream: played as it arrives, a block once all of it has
==================
*/
void CL_MVDStartStream (void)
{
	CL_MVDStart (NULL, 0);
	mvd.stream = true;
	mvd.closed = false;
	mvd.held = false;
	mvd.hold_until = 0;
}

void CL_MVDFeed (const byte *bytes, int len)
{
	// what was read goes, now and then; never while a block is parsed, since
	// this runs before they are read
	if (mvd.pos > 64 * 1024)
	{
		memmove (mvd.data, mvd.data + mvd.pos, mvd.size - mvd.pos);
		mvd.size -= mvd.pos;
		mvd.pos = 0;
	}
	if (mvd.size + len > mvd.capacity)
	{
		mvd.capacity = (mvd.size + len) * 2;
		mvd.data = Mem_Realloc (mvd.data, mvd.capacity);
	}
	memcpy (mvd.data + mvd.size, bytes, len);
	mvd.size += len;
}

void CL_MVDStreamClosed (void)
{
	mvd.closed = true;
}

/*
==================
CL_MVDStop
==================
*/
void CL_MVDStop (void)
{
	Mem_Free (mvd.data);
	mvd.data = NULL;
	cls.mvdplayback = false;

	// the parser reads blocks where they are; give it its buffer back
	cls.net_message.data = cls.net_message_buf;
	cls.net_message.maxsize = sizeof(cls.net_message_buf);
}

/*
==================
CL_MVDTime

The moment drawn, in demo seconds: never outside the two frames read
==================
*/
double CL_MVDTime (void)
{
	return mvd.logical < mvd.old ? mvd.old : mvd.logical > mvd.next ? mvd.next : mvd.logical;
}

/*
==================
CL_MVDAdvance

Moves the moment played on, once a client frame, before the blocks are read.
A timedemo takes one frame each time.
==================
*/
void CL_MVDAdvance (void)
{
	if (cls.timedemo)
	{
		mvd.logical = mvd.next;
		if (!cls.td_starttime && cls.state == ca_active)
		{
			cls.td_starttime = (float)Sys_DoubleTime ();
			cls.td_startframe = cls.framecount;
		}
		return;
	}
	// a stream holds half a second after each gamestate, a cushion against
	// the network; at real time only
	if (mvd.stream)
	{
		if (cls.state < ca_onserver)
			mvd.held = false;
		else if (!mvd.held)
		{
			mvd.held = true;
			mvd.hold_until = host.realtime + 0.5;
		}
		if (host.realtime < mvd.hold_until)
			return;
		if (!cl.paused && !mvd.paused)
			mvd.logical += cls.frametime;
		return;
	}

	if (!cl.paused && !mvd.paused)
		mvd.logical += cls.frametime * (demo_speed.value > 0 ? demo_speed.value : 0);
}

/*
==================
CL_MVDFrameTime

The frame last read, in demo seconds: when what it says is stamped
==================
*/
double CL_MVDFrameTime (void)
{
	return mvd.frame * 0.001;
}

/*
==================
CL_MVDTogglePause
==================
*/
void CL_MVDTogglePause (void)
{
	mvd.paused = !mvd.paused;
	Con_Printf (mvd.paused ? "Paused\n" : "Playing\n");
}

/*
==================
MVD_Gate

Whether the block stamped t is read now: before the level's serverdata
everything is, afterwards a frame once playback has passed the one before it
==================
*/
static bool MVD_Gate (double t)
{
	if (cls.state < ca_onserver)
	{
		mvd.logical = mvd.old = mvd.next = t;
		return true;
	}
	if (t > mvd.logical + MVD_MAXLAG)
		mvd.logical = t;		// a stall in the recording: skip it, don't replay it
	else if (t > mvd.next && mvd.logical < mvd.next)
		return false;			// the frames around the moment are both read
	if (t != mvd.next)
	{
		mvd.old = mvd.next;
		mvd.next = t;
	}
	return true;
}

static void MVD_End (const char *why)
{
	if (why)
		Con_Printf ("%s\n", why);
	CL_StopPlayback ();
}

/*
==================
MVD_Short

A block that isn't whole: a stream's is still arriving; a file ends there,
or a stream that closed
==================
*/
static bool MVD_Short (void)
{
	if (mvd.stream && !mvd.closed)
		return false;
	MVD_End (NULL);
	return false;
}

/*
==================
CL_GetMVDMessage

The next block to parse now, in cls.net_message; false when there is none
yet, or the recording has ended
==================
*/
bool CL_GetMVDMessage (void)
{
	size_t		payload;
	int			type, len;
	unsigned	to, mask;
	uint32_t	t;

	for (;;)
	{
		if (mvd.pos + 2 > mvd.size)
		{	// a recording cut off without its disconnect ends here too
			return MVD_Short ();
		}

		type = mvd.data[mvd.pos+1] & 7;
		to = mvd.data[mvd.pos+1] >> 3;
		mask = 0;
		switch (type)
		{
		case DEM_SET:
			payload = mvd.pos + 2;
			len = 8;
			break;
		case DEM_MULTIPLE:
			if (mvd.pos + 10 > mvd.size)
			{
				return MVD_Short ();
			}
			mask = MVD_Long (mvd.pos + 2);
			len = (int)MVD_Long (mvd.pos + 6);
			payload = mvd.pos + 10;
			break;
		case DEM_READ:
		case DEM_SINGLE:
		case DEM_STATS:
		case DEM_ALL:
			if (mvd.pos + 6 > mvd.size)
			{
				return MVD_Short ();
			}
			len = (int)MVD_Long (mvd.pos + 2);
			payload = mvd.pos + 6;
			break;
		default:	// dem_cmd is a QWD's; either is a misread recording
			MVD_End (va("Corrupt MVD: block type %i at %zu", type, mvd.pos));
			return false;
		}
		if (len < 0 || len > MVD_MAXBLOCK)
		{
			MVD_End (va("Corrupt MVD: block of %i bytes at %zu", len, mvd.pos));
			return false;
		}
		if (payload + len > mvd.size)
		{	// the last block, cut off
			return MVD_Short ();
		}

		t = mvd.time + mvd.data[mvd.pos];
		if (!MVD_Gate (t * 0.001))
			return false;
		mvd.time = t;
		mvd.pos = payload + len;

		if (type == DEM_SET)
		{
			cls.netchan.outgoing_sequence = (int)MVD_Long (payload);
			cls.netchan.incoming_sequence = (int)MVD_Long (payload + 4);
			cls.netchan.incoming_acknowledged = cls.netchan.incoming_sequence;
			continue;
		}
		if (type == DEM_MULTIPLE && !mask)
			continue;		// mvdsv's hidden data

		// a new frame: the next sequence
		if (!mvd.started || t != mvd.frame)
		{
			cls.netchan.incoming_sequence++;
			cls.netchan.incoming_acknowledged = cls.netchan.incoming_sequence;
			cls.netchan.outgoing_sequence = cls.netchan.incoming_sequence + 1;
			mvd.frame = t;
			mvd.started = true;
		}
		cls.netchan.last_received = (float)host.realtime;

		mvd.type = type == DEM_READ ? DEM_ALL : type;
		mvd.to = type == DEM_MULTIPLE ? mask : type == DEM_ALL ? 0 : to;

		cls.net_message.data = mvd.data + payload;
		cls.net_message.cursize = len;
		cls.net_message.maxsize = len;
		return true;
	}
}

/*
==================
CL_MVDSkipMessage

A message for other players than the one the view follows: a print, a
centerprint, a kick, a sound... is read but not acted on (ezQuake)
==================
*/
bool CL_MVDSkipMessage (void)
{
	if (!cls.mvdplayback)
		return false;
	if (mvd.type == DEM_MULTIPLE)
		return mvd.track < 0 || !(mvd.to & (1u << mvd.track));
	if (mvd.type == DEM_SINGLE)
		return mvd.track < 0 || (int)mvd.to != mvd.track;
	return false;
}

/*
==================
CL_MVDStatTarget

Whose stats a stat message sets: the player of a dem_stats or dem_single
block, else -1
==================
*/
int CL_MVDStatTarget (void)
{
	if ((mvd.type == DEM_STATS || mvd.type == DEM_SINGLE) && mvd.to < MAX_CLIENTS)
		return (int)mvd.to;
	return -1;
}

/*
==================
CL_MVDTracking

The player the view follows, -1 none
==================
*/
int CL_MVDTracking (void)
{
	return mvd.track;
}

static bool MVD_IsPlayer (int slot)
{
	return cl.players[slot].name[0] && !cl.players[slot].spectator;
}

/*
==================
MVD_Track

Follows a player: their stats are the ones shown
==================
*/
static void MVD_Track (int slot)
{
	mvd.track = slot;
	cl.viewplayer = slot;
	memcpy (cl.stats, cl.players[slot].stats, sizeof(cl.stats));
}

/*
==================
MVD_NextPlayer

The first player after slot, wrapping; -1 none
==================
*/
static int MVD_NextPlayer (int slot)
{
	int		i, j;

	for (i=1 ; i<=MAX_CLIENTS ; i++)
	{
		j = (slot + i + MAX_CLIENTS) % MAX_CLIENTS;
		if (MVD_IsPlayer (j))
			return j;
	}
	return -1;
}

/*
==================
CL_MVDView

The view is the tracked player's: picked when there is none, or the one
followed left or went to watch (qualia)
==================
*/
void CL_MVDView (void)
{
	const player_state_t	*state;
	int		next;

	CL_LerpMVDPlayers ();

	if (mvd.track < 0 || !MVD_IsPlayer (mvd.track))
	{
		next = MVD_NextPlayer (mvd.track);
		if (next < 0)
		{
			mvd.track = -1;
			return;
		}
		MVD_Track (next);
	}
	else if (cl.viewplayer != mvd.track)
		MVD_Track (mvd.track);		// a new level cleared the view

	// where the recording has the player at the moment played
	state = &cl.mvd_prev[mvd.track];
	if (!CL_PlayerPlace (mvd.track, cl.simorg, cl.simangles))
	{
		VectorCopy (state->origin, cl.simorg);
		VectorCopy (state->viewangles, cl.simangles);
	}
	VectorCopy (vec3_origin, cl.simvel);
	cl.onground = false;
	cl.crouch = 0;
}

/*
==================
CL_MVDHint

"//at <userid>": the server's pick of who to watch (mvdsv)
==================
*/
void CL_MVDHint (const char *s)
{
	int		i, userid;

	if (strncmp (s, "//at ", 5) || !demo_autotrack.value || mvd.picked)
		return;
	userid = atoi (s + 5);
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (MVD_IsPlayer (i) && cl.players[i].userid == userid)
		{
			if (i != mvd.track)
				MVD_Track (i);
			return;
		}
}

/*
==================
CL_MVDButtons

The buttons pressed since the last frame: jump for the next player, attack
for the one before; either stops following the server's hints
==================
*/
void CL_MVDButtons (bool attack, bool jump)
{
	int		i, slot;

	if (!attack && !jump)
		return;
	slot = mvd.track;
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		slot = (slot + (jump ? 1 : -1) + MAX_CLIENTS) % MAX_CLIENTS;
		if (MVD_IsPlayer (slot))
		{
			MVD_Track (slot);
			mvd.picked = true;
			return;
		}
	}
}

/*
==================
CL_MVDTrack_f

track [name|userid]: follow that player, or the next one
==================
*/
static void CL_MVDTrack_f (void)
{
	const char	*arg;
	int			i, userid;

	if (!cls.mvdplayback)
	{
		Con_Printf ("Not playing an MVD\n");
		return;
	}
	mvd.picked = true;
	if (Cmd_Argc () < 2)
	{
		i = MVD_NextPlayer (mvd.track);
		if (i >= 0)
			MVD_Track (i);
		return;
	}

	arg = Cmd_Argv (1);
	userid = atoi (arg);
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		if (!MVD_IsPlayer (i))
			continue;
		if (!Q_strcasecmp (cl.players[i].name, arg) || (userid && cl.players[i].userid == userid))
		{
			MVD_Track (i);
			return;
		}
	}
	Con_Printf ("No player %s\n", arg);
}

void CL_InitMVD (void)
{
	Cvar_RegisterVariable (&demo_speed);
	Cvar_RegisterVariable (&demo_autotrack);
	Cmd_AddCommand ("track", CL_MVDTrack_f);
}
