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
//
// Seeking, as qualia does it: when a level of a file goes active, the rest of
// it is read at once, quietly, and every MVD_SPACING a keyframe is kept: what
// the parser holds between two frames, as little of it as a replay needs. A
// seek goes back to the keyframe before the moment asked for and reads on,
// quietly, to it. A stream can't be read ahead, so it can't seek.

#include "cl_local.h"

#define	MVD_MAXBLOCK	8192	// the writer's blocks are at most 8092 bytes
#define	MVD_MAXLAG		1.0		// seconds: a recorded stall longer than this is skipped
#define	MVD_SPACING		10000	// msec between keyframes

// a keyframe: the frame at `at` is the next one read
typedef struct
{
	uint32_t	at;
	size_t		size;
	byte		data[];		// an mvdkeyhead_t, then what it counts (MVD_Keyframe)
} mvdkey_t;

typedef struct
{
	// the reader
	size_t		pos;
	uint32_t	time, frame;
	bool		started;
	int			sequence;
	int			hint;
	// the client
	int			parsecount, validsequence;
	int			entityframe;			// whose entities the next frame's are deltas from
	bool		paused;
	int			intermission, completed_time;
	vec3_t		simorg, simangles;		// the intermission's view
	movevars_t	movevars;
	int			numentities, numplayers, numcarried, numstyles;
} mvdkeyhead_t;

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
	bool		flying;			// the viewer flies the camera instead (qualia's free look)
	vec3_t		flyorg;			// where it is
	int			hint;			// the userid the server's last hint named, 0 none
	bool		paused;

	// the clock, in demo seconds
	double		logical;		// the moment played
	double		old, next;		// the frames around it

	// seeking: the level's keyframes, from its scan
	mvdkey_t	**keys;
	int			numkeys, maxkeys;
	uint32_t	level_end;		// msec: the level's last frame
	bool		quiet;			// a scan or a seek: read, not shown or heard
	bool		scanning;		// reading the whole level, keeping keyframes
	bool		level_over;		// the scan met the level's end
	uint32_t	until;			// a seek reads the frames up to this
} mvd = {.track = -1};

static cvar_t	demo_speed = {.name = "demo_speed", .string = "1",	// MVD playback speed, 1 is real time
	.description = "How fast an MVD file plays, as a multiple of real time; 0 holds it still. "
		"A QTV stream plays at real time."};
// follow the player the server's "//at" hints name, until one is picked by hand
static cvar_t	demo_autotrack = {.name = "demo_autotrack", .string = "1", .archive = true,
	.description = "In an MVD or QTV stream, follows the player the server's hints name, "
		"until one is picked with jump or track.",
	.values = (const cvar_value_t[]){{"0", "Stays on the player followed"}, {"1", "Follows the server's picks"}, {0}}};

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
	mvd.flying = false;
	mvd.hint = 0;
	mvd.paused = false;
	mvd.quiet = mvd.scanning = false;
	CL_ItemsClear ();
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

static void MVD_FreeKeys (void)
{
	int		i;

	for (i=0 ; i<mvd.numkeys ; i++)
		Mem_Free (mvd.keys[i]);
	mvd.numkeys = 0;
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
	MVD_FreeKeys ();
	mvd.quiet = mvd.scanning = false;
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
	if (CL_Downloading ())
		return;		// the moment played waits for a map from the web
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
		if (!mvd.paused)
			mvd.logical += cls.frametime;
		return;
	}

	// a pause the recording holds is played through: its frames say how long
	if (!mvd.paused)
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

Whether the block stamped ms is read now: before the level's serverdata
everything is, afterwards a frame once playback has passed the one before it.
A scan reads all of the level, keeping a keyframe before a frame when one is
due; a seek reads the frames up to where it goes.
==================
*/
static void MVD_Keyframe (uint32_t at);

static bool MVD_Gate (uint32_t ms)
{
	double	t = ms * 0.001;

	if (mvd.scanning)
	{
		if (ms != mvd.frame && ms - mvd.keys[mvd.numkeys-1]->at >= MVD_SPACING)
			MVD_Keyframe (ms);
		return true;
	}
	if (mvd.quiet)
		return ms <= mvd.until;

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

// the recording ends here; for a scan, the level does, and playback will
// find out for itself
static void MVD_End (const char *why)
{
	if (mvd.scanning)
	{
		mvd.level_over = true;
		return;
	}
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
		if (!MVD_Gate (t))
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
			if (mvd.quiet)
				cl.time = t * 0.001;	// what the parser stamps things with
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
	memcpy (cl.statsf, cl.players[slot].statsf, sizeof(cl.statsf));
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

bool CL_MVDFlying (void)
{
	return cls.mvdplayback && mvd.flying;
}

/*
==================
MVD_Fly

The camera the viewer flies: the mouse turns it, the movement keys move it
along the view, up and down straight up and down, in real time
==================
*/
static void MVD_Fly (void)
{
	vec3_t	forward, right, up;
	float	move[3];
	int		i;

	CL_FlyMove (move);
	AngleVectors (cl.viewangles, forward, right, up);
	for (i=0 ; i<3 ; i++)
		mvd.flyorg[i] += (forward[i] * move[0] + right[i] * move[1]) * (float)cls.frametime;
	mvd.flyorg[2] += move[2] * (float)cls.frametime;

	VectorCopy (mvd.flyorg, cl.simorg);
	VectorCopy (cl.viewangles, cl.simangles);
	VectorCopy (vec3_origin, cl.simvel);
	cl.onground = false;
	cl.crouch = 0;
}

/*
==================
CL_MVDView

The view is the tracked player's: picked when there is none, or the one
followed left or went to watch (qualia); or the camera the viewer flies
==================
*/
void CL_MVDView (void)
{
	const player_state_t	*state;
	int		next;

	CL_LerpMVDPlayers ();

	if (mvd.flying)
	{
		MVD_Fly ();
		return;
	}

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
MVD_FollowHint

Follows the player the server's last hint named, unless one was picked by
hand
==================
*/
static void MVD_FollowHint (void)
{
	int		i;

	if (!demo_autotrack.value || mvd.picked || !mvd.hint)
		return;
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (MVD_IsPlayer (i) && cl.players[i].userid == mvd.hint)
		{
			if (i != mvd.track)
				MVD_Track (i);
			return;
		}
}

/*
==================
CL_MVDHint

"//at <userid>": the server's pick of who to watch (mvdsv)
==================
*/
void CL_MVDHint (const char *s)
{
	if (strncmp (s, "//at ", 5))
		return;
	mvd.hint = atoi (s + 5);
	if (!mvd.quiet)		// a seek follows the last one before where it goes
		MVD_FollowHint ();
}

/*
==================
CL_MVDButtons

The buttons pressed since the last frame, as qualia has them: jump rides
with the next player, stopping the server's hints; attack takes the camera
from the player followed to fly it, from where the view is, or gives it back
==================
*/
void CL_MVDButtons (bool attack, bool jump)
{
	int		next;

	if (cls.state != ca_active)
		return;
	if (jump)
	{
		mvd.flying = false;
		next = MVD_NextPlayer (mvd.track);
		if (next >= 0)
		{
			MVD_Track (next);
			mvd.picked = true;
		}
	}
	else if (attack && mvd.flying)
		mvd.flying = false;		// back to the player followed, or the next if they left
	else if (attack)
	{
		mvd.flying = true;
		VectorCopy (cl.simorg, mvd.flyorg);
		VectorCopy (cl.simangles, cl.viewangles);
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

/*
===============================================================================

KEYFRAMES AND SEEKING

===============================================================================
*/

// what a keyframe keeps of a player: all but what follows from it
#define	PLAYER_KEPT		offsetof(player_info_t, translate)

static byte			*key_buf;		// the keyframe being written
static size_t		key_len, key_max;
static const byte	*key_read;		// the one being read

static void Key_Put (const void *data, size_t len)
{
	if (key_len + len > key_max)
	{
		key_max = (key_len + len) * 2;
		key_buf = Mem_Realloc (key_buf, key_max);
	}
	memcpy (key_buf + key_len, data, len);
	key_len += len;
}

static void Key_PutByte (int c)
{
	byte	b = (byte)c;

	Key_Put (&b, 1);
}

static void Key_Get (void *data, size_t len)
{
	memcpy (data, key_read, len);
	key_read += len;
}

static bool MVD_KeepPlayer (int slot)
{
	return cl.players[slot].userid || cl.players[slot].userinfo[0];
}

// a carried state, even one from the gamestate, before the first frame
static bool MVD_KeepCarried (int slot)
{
	static const player_state_t	none = {0};

	return memcmp (&cl.mvd_prev[slot], &none, sizeof(none)) != 0;
}

/*
==================
MVD_Keyframe

Keeps what the parser holds between two frames, as the keyframe before the
one stamped at: the reader, the latest entities, the players and their
carried states, the lightstyles, the serverinfo. Only what is in use.
==================
*/
static void MVD_Keyframe (uint32_t at)
{
	mvdkeyhead_t			head = {0};
	const cl_entities_t	*pack;
	mvdkey_t				*key;
	int						i;

	head.pos = mvd.pos;
	head.time = mvd.time;
	head.frame = mvd.frame;
	head.started = mvd.started;
	head.sequence = cls.netchan.incoming_sequence;
	head.hint = mvd.hint;
	head.parsecount = cl.parsecount;
	head.validsequence = cl.validsequence;
	head.paused = cl.paused;
	head.intermission = cl.intermission;
	head.completed_time = cl.completed_time;
	VectorCopy (cl.simorg, head.simorg);
	VectorCopy (cl.simangles, head.simangles);
	head.movevars = cl.movevars;

	// the next frame's entities are deltas from the last that had them, or
	// with none yet, from the frame before (CL_ParsePacketEntities)
	head.entityframe = cl.validsequence ? cl.validsequence : cls.netchan.incoming_sequence;
	pack = &cl.frames[head.entityframe & UPDATE_MASK].packet_entities;
	head.numentities = pack->num_entities;
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		head.numplayers += MVD_KeepPlayer (i);
		head.numcarried += MVD_KeepCarried (i);
	}
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
		head.numstyles += cl.lightstyles[i].length != 0;

	key_len = 0;
	Key_Put (&head, sizeof(head));
	Key_Put (cl.serverinfo, strlen (cl.serverinfo) + 1);
	Key_Put (pack->entities, head.numentities * sizeof(entity_state_t));
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (MVD_KeepPlayer (i))
		{
			Key_PutByte (i);
			Key_Put (&cl.players[i], PLAYER_KEPT);
		}
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (MVD_KeepCarried (i))
		{
			Key_PutByte (i);
			Key_Put (&cl.mvd_prev[i], sizeof(player_state_t));
		}
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
		if (cl.lightstyles[i].length)
		{
			Key_PutByte (i);
			Key_PutByte (cl.lightstyles[i].length);
			Key_Put (cl.lightstyles[i].map, cl.lightstyles[i].length);
		}

	key = Mem_Alloc (sizeof(*key) + key_len);
	key->at = at;
	key->size = key_len;
	memcpy (key->data, key_buf, key_len);
	if (mvd.numkeys == mvd.maxkeys)
	{
		mvd.maxkeys = mvd.maxkeys ? mvd.maxkeys * 2 : 64;
		mvd.keys = Mem_Realloc (mvd.keys, (size_t)mvd.maxkeys * sizeof(*mvd.keys));
	}
	mvd.keys[mvd.numkeys++] = key;
}

/*
==================
MVD_Restore

The parser as it was at the keyframe. Players, carried states and
lightstyles it doesn't have were nobody's then; what follows from the rest
(skins, colors, what the serverinfo tells, the stats shown) is worked out
again.
==================
*/
static void MVD_Restore (const mvdkey_t *key)
{
	mvdkeyhead_t		head;
	cl_entities_t	*pack;
	lightstyle_t		*style;
	int					i, slot;

	key_read = key->data;
	Key_Get (&head, sizeof(head));

	mvd.pos = head.pos;
	mvd.time = head.time;
	mvd.frame = head.frame;
	mvd.started = head.started;
	mvd.hint = head.hint;
	cls.netchan.incoming_sequence = cls.netchan.incoming_acknowledged = head.sequence;
	cls.netchan.outgoing_sequence = head.sequence + 1;

	cl.parsecount = head.parsecount;
	cl.parsecountmod = head.parsecount & UPDATE_MASK;
	cl.validsequence = head.validsequence;
	cl.paused = head.paused;
	if (cl.intermission != head.intermission)
		vid.recalc_refdef = true;
	cl.intermission = head.intermission;
	cl.completed_time = head.completed_time;
	VectorCopy (head.simorg, cl.simorg);
	VectorCopy (head.simangles, cl.simangles);
	cl.movevars = head.movevars;

	Key_Get (cl.serverinfo, strlen ((const char *)key_read) + 1);
	CL_ProcessServerInfo ();

	pack = &cl.frames[head.entityframe & UPDATE_MASK].packet_entities;
	pack->num_entities = head.numentities;
	if (head.numentities)
		CL_FrameEntity (pack, head.numentities - 1);
	Key_Get (pack->entities, head.numentities * sizeof(entity_state_t));
	cl.frames[head.entityframe & UPDATE_MASK].invalid = false;

	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		memset (&cl.players[i], 0, PLAYER_KEPT);
		cl.players[i].skin = NULL;
	}
	for (i=0 ; i<head.numplayers ; i++)
	{
		slot = *key_read++;
		Key_Get (&cl.players[slot], PLAYER_KEPT);
		CL_ProcessUserInfo (slot, &cl.players[slot]);
	}

	memset (cl.mvd_prev, 0, sizeof(cl.mvd_prev));
	for (i=0 ; i<head.numcarried ; i++)
	{
		slot = *key_read++;
		Key_Get (&cl.mvd_prev[slot], sizeof(player_state_t));
	}
	memcpy (cl.frames[cl.parsecountmod].playerstate, cl.mvd_prev, sizeof(cl.mvd_prev));

	memset (cl.lightstyles, 0, sizeof(cl.lightstyles));
	for (i=0 ; i<head.numstyles ; i++)
	{
		style = &cl.lightstyles[*key_read++];
		style->length = *key_read++;
		Key_Get (style->map, style->length);
	}

	memset (cl.item_gettime, 0, sizeof(cl.item_gettime));
	if (mvd.track >= 0)
		MVD_Track (mvd.track);
}

/*
==================
MVD_ClearTransients

What was under way when the moment played jumps: effects, sounds, and the
places things were drawn moving from
==================
*/
static void MVD_ClearTransients (void)
{
	CL_ResetSmoothing ();
	CL_ClearTEnts ();
	CL_ClearProjectiles ();
	memset (cl.dlights, 0, sizeof(cl.dlights));
	R_ClearParticles ();
	S_StopDynamicSounds ();
	SCR_CenterPrint ("");
	cl.punchangle = 0;
	cl.faceanimtime = 0;
	cl.cshifts[CSHIFT_DAMAGE].percent = 0;
	cl.cshifts[CSHIFT_BONUS].percent = 0;
}

// the stamp of the next block, or of the last when there are no more
static uint32_t MVD_NextStamp (void)
{
	return mvd.pos < mvd.size ? mvd.time + mvd.data[mvd.pos] : mvd.time;
}

/*
==================
MVD_Scan

A level of a file just went active: the rest of it is read now, quietly,
keeping a keyframe at its start and every MVD_SPACING, and play goes on
from the start
==================
*/
static void MVD_Scan (void)
{
	double	logical = mvd.logical, old = mvd.old, next = mvd.next;
	double	started = Sys_DoubleTime ();
	size_t	bytes;
	int		i;

	MVD_FreeKeys ();
	MVD_Keyframe (MVD_NextStamp ());
	mvd.quiet = mvd.scanning = true;
	mvd.level_over = false;
	mvd.level_end = mvd.frame;
	while (!mvd.level_over && CL_GetMVDMessage ())
	{
		MSG_BeginReading (&cls.net_message);
		CL_ParseServerMessage ();
		if (!mvd.level_over)
			mvd.level_end = mvd.frame;		// not the next level's first
	}
	mvd.quiet = mvd.scanning = false;

	MVD_Restore (mvd.keys[0]);
	MVD_ClearTransients ();
	mvd.logical = logical;
	mvd.old = old;
	mvd.next = next;

	for (bytes = 0, i=0 ; i<mvd.numkeys ; i++)
		bytes += mvd.keys[i]->size;
	Con_DPrintf ("MVD: level read in %.0f ms, %i keyframes of %zu KB\n",
		(Sys_DoubleTime () - started) * 1000, mvd.numkeys, bytes / 1024);
}

/*
==================
MVD_Seek

Plays on from ms, within the level: reading on from where the reader is when
that is on the way, else from the keyframe before it
==================
*/
static void MVD_Seek (uint32_t ms)
{
	int		i;

	if (ms < mvd.keys[0]->at)
		ms = mvd.keys[0]->at;
	if (ms > mvd.level_end)
		ms = mvd.level_end;
	for (i=mvd.numkeys-1 ; i>0 && mvd.keys[i]->at > ms ; i--)
		;
	if (mvd.time > ms || mvd.keys[i]->at > mvd.time)
		MVD_Restore (mvd.keys[i]);

	// what the frames on the way leave behind is where things now move from
	MVD_ClearTransients ();
	mvd.quiet = true;
	mvd.until = ms;
	while (CL_GetMVDMessage ())
	{
		MSG_BeginReading (&cls.net_message);
		CL_ParseServerMessage ();
	}
	mvd.quiet = false;
	if (!cls.mvdplayback)
		return;		// that was the end of the recording

	mvd.logical = mvd.old = mvd.next = ms * 0.001;
	MVD_FollowHint ();
}

/*
==================
CL_MVDActive

A level went active; a file's is scanned for seeking, but not a timedemo's
==================
*/
void CL_MVDActive (void)
{
	if (!mvd.stream && !cls.timedemo)
		MVD_Scan ();
}

/*
==================
CL_MVDNewLevel

A serverdata: a scan ends at the next level, which it doesn't read (true);
in play, the last level's keyframes go
==================
*/
bool CL_MVDNewLevel (void)
{
	if (!cls.mvdplayback)
		return false;
	if (mvd.scanning)
	{
		mvd.level_over = true;
		return true;
	}
	MVD_FreeKeys ();
	CL_ItemsClear ();
	return false;
}

/*
==================
CL_MVDAnnouncements

A stufftext: KTX's lines about items. A file's are all collected by the scan
of the level, a stream's (and a timedemo's) as they come.
==================
*/
void CL_MVDAnnouncements (const char *s)
{
	if (mvd.scanning || (!mvd.quiet && !mvd.numkeys))
		CL_ItemsMarker (s, CL_MVDFrameTime ());
}

/*
==================
CL_MVDQuiet

A scan or a seek is reading: the messages change what is kept, but nothing is
shown or heard
==================
*/
bool CL_MVDQuiet (void)
{
	return cls.mvdplayback && mvd.quiet;
}

static const char *MVD_Clock (double seconds, char *buf, size_t size)
{
	int		s = (int)seconds;

	snprintf (buf, size, "%i:%02i", s / 60, s % 60);
	return buf;
}

/*
==================
CL_MVDJump_f

demo_jump [+|-][m:]s: to that moment of the recording, or that far on or
back; without one, where playback is (ezQuake's command)
==================
*/
static void CL_MVDJump_f (void)
{
	const char	*arg, *colon;
	char		now_s[16], end_s[16];
	double		now, to;
	int			sign;

	if (!cls.mvdplayback || cls.state != ca_active)
	{
		Con_Printf ("Not playing an MVD\n");
		return;
	}
	now = CL_MVDTime ();
	if (Cmd_Argc () != 2)
	{
		Con_Printf ("demo_jump [+|-][m:]s\n");
		if (mvd.numkeys)
			Con_Printf ("At %s of %s\n", MVD_Clock (now, now_s, sizeof(now_s)),
				MVD_Clock (mvd.level_end * 0.001, end_s, sizeof(end_s)));
		return;
	}
	if (!mvd.numkeys)
	{
		Con_Printf ("Can't seek in a stream or a timedemo\n");
		return;
	}

	arg = Cmd_Argv (1);
	sign = *arg == '+' ? 1 : *arg == '-' ? -1 : 0;
	if (sign)
		arg++;
	colon = strchr (arg, ':');
	to = colon ? atoi (arg) * 60 + atof (colon + 1) : atof (arg);
	if (sign)
		to = now + sign * to;
	MVD_Seek (to > 0 ? (uint32_t)(to * 1000 + 0.5) : 0);
}

void CL_InitMVD (void)
{
	Cvar_RegisterVariable (&demo_speed);
	Cvar_RegisterVariable (&demo_autotrack);
	Cmd_AddCommand ("track", CL_MVDTrack_f,
		"Follows a player in the MVD or QTV stream watched, by name or userid; without one, the next player. "
		"Usage: track [name|userid]");
	Cmd_AddCommand ("demo_jump", CL_MVDJump_f,
		"Seeks an MVD file to a time, m:s or seconds, or that far on or back with + or -; without one, prints where "
		"playback is. Usage: demo_jump [[+|-]time]");
}
