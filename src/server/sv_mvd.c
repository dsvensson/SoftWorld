// sv_mvd.c -- the game as an MVD, for QTV's viewers (the net layer's net_qtv.c)
//
// Written while somebody watches, once for all of them, in mvdsv's format,
// which its relays and ezQuake read: blocks of server messages, [msec][type]
// [length][messages], to everybody (dem_all), to one player's view
// (dem_single, dem_stats) or to some players' (dem_multiple). The game's
// events go in as they happen; each frame, every entity, as a delta from the
// frame before, then every player and their stats, as deltas from what was
// last said of them.Each second a frame also gets a snapshot, the whole state
// after it, for viewers joining; a level starts with its gamestate's fixed
// part (serverdata, lists, statics, baselines, signon) and a snapshot. The
// net layer lets it all out qtv_delay seconds later.
//
// Unlike mvdsv's, a level change writes no svc_disconnect "EndOfDemo": the new
// gamestate follows the old level's frames.

#include "sv_local.h"

#define	MVD_FRAMETIME		(1.0 / 77)	// mvdsv's sv_demofps
#define	MVD_SNAPSHOTTIME	1.0
#define	MVD_PINGTIME		2.0
#define	MVD_MAXBLOCK		8092		// a block's messages, at most (MAX_MVD_SIZE, as relays take them)
// mvdsv's; float coordinates on the maps that need them
#define	MVD_FTE_EXTENSIONS	(FTE_PEXT_256PACKETENTITIES | FTE_PEXT_MODELDBL | FTE_PEXT_ENTITYDBL | \
	FTE_PEXT_ENTITYDBL2 | FTE_PEXT_SPAWNSTATIC2 | FTE_PEXT_TRANS | FTE_PEXT_COLOURMOD)

// blocks written, grown as needed
typedef struct
{
	byte		*data;
	int			length, size;
	int			last;			// the last block, which messages to the same players join; -1 none
	int			lasttype;
	unsigned	lastto;
} mvdbuf_t;

// what was last said of a player
typedef struct
{
	bool	valid;
	vec3_t	origin, angles;
	int		frame, model, skin, effects, weaponframe;
	int		flags;				// DF_DEAD, DF_GIB
} mvdplayer_t;

bool	sv_mvd;

static cvar_t	qtv_delay = {.name = "qtv_delay", .string = "10",
	.description = "Seconds the QTV stream (sv_public) runs behind the game, so that a player can't watch "
		"their opponents through it; 0 for none."};

static struct
{
	mvdbuf_t	frame;			// the events since the last frame, then the frame
	mvdbuf_t	snapshot;
	mvdbuf_t	level;
	double		clock;			// when the last frame's time was
	double		nextframe, nextsnapshot, nextpings;
	client_t	recorder;		// the stream's protocol extensions, for the writers shared with clients
	packet_entities_t	entities, current;	// the last frame's, and this one's
	mvdplayer_t	players[MAX_CLIENTS];
	int			stats[MAX_CLIENTS][MAX_STATS];
	bool		statsvalid[MAX_CLIENTS];
	byte		scratch[MVD_MAXBLOCK];	// a message being written
	byte		scratch2[MVD_MAXBLOCK];
	bool		overflowed;		// said once
} mvd;

/*
===============================================================================

BLOCKS

===============================================================================
*/

static void MVD_PutLong (byte *p, unsigned v)
{
	p[0] = (byte)v;
	p[1] = (byte)(v >> 8);
	p[2] = (byte)(v >> 16);
	p[3] = (byte)(v >> 24);
}

static void MVD_Grow (mvdbuf_t *b, int length)
{
	if (b->length + length <= b->size)
		return;
	b->size = (b->length + length) * 2 < 16384 ? 16384 : (b->length + length) * 2;
	b->data = Mem_Realloc (b->data, (size_t)b->size);
}

static void MVD_Clear (mvdbuf_t *b)
{
	b->length = 0;
	b->last = -1;
}

// messages to the players `to` (a player for dem_single and dem_stats, a mask
// for dem_multiple), joining the block before when it goes to them too
static void MVD_Write (mvdbuf_t *b, int type, unsigned to, const void *data, int length)
{
	byte	*p;
	int		header, blocklength;

	if (length <= 0)
		return;
	header = type == DEM_MULTIPLE ? 10 : 6;
	if (b->last >= 0 && b->lasttype == type && b->lastto == to)
	{
		p = b->data + b->last + header - 4;
		blocklength = p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
		if (blocklength + length <= MVD_MAXBLOCK)
		{
			MVD_Grow (b, length);
			MVD_PutLong (b->data + b->last + header - 4, (unsigned)(blocklength + length));
			memcpy (b->data + b->length, data, (size_t)length);
			b->length += length;
			return;
		}
	}
	MVD_Grow (b, header + length);
	p = b->data + b->length;
	p[0] = 0;		// the frame's first block's time is set as the frame is written
	p[1] = (byte)(type | (type == DEM_SINGLE || type == DEM_STATS ? to << 3 : 0));
	if (type == DEM_MULTIPLE)
		MVD_PutLong (p + 2, to);
	MVD_PutLong (p + header - 4, (unsigned)length);
	memcpy (p + header, data, (size_t)length);
	b->last = b->length;
	b->lasttype = type;
	b->lastto = to;
	b->length += header + length;
}

static void MVD_InitMessage (sizebuf_t *msg, byte *data)
{
	memset (msg, 0, sizeof(*msg));
	msg->data = data;
	msg->maxsize = MVD_MAXBLOCK;
	msg->allowoverflow = true;
	msg->floatcoords = sv.bigcoords;
}

// a message to everybody, written out first if what is to come mightn't fit
static void MVD_Room (mvdbuf_t *b, sizebuf_t *msg, int length)
{
	if (msg->cursize + length <= msg->maxsize)
		return;
	MVD_Write (b, DEM_ALL, 0, msg->data, msg->cursize);
	SZ_Clear (msg);
}

/*
===============================================================================

THE GAME'S EVENTS

===============================================================================
*/

sizebuf_t *SV_MVDMessage (void)
{
	static sizebuf_t	msg;

	MVD_InitMessage (&msg, mvd.scratch);
	return &msg;
}

void SV_MVDAll (const void *data, int length)
{
	if (sv_mvd)
		MVD_Write (&mvd.frame, DEM_ALL, 0, data, length);
}

void SV_MVDSingle (const client_t *cl, const void *data, int length)
{
	if (sv_mvd && cl->state == cs_spawned && !cl->spectator)
		MVD_Write (&mvd.frame, DEM_SINGLE, (unsigned)(cl - svs.clients), data, length);
}

void SV_MVDMultiple (unsigned mask, const void *data, int length)
{
	if (sv_mvd && mask)
		MVD_Write (&mvd.frame, DEM_MULTIPLE, mask, data, length);
}

void SV_MVDPrint (const client_t *cl, int level, const char *text)
{
	sizebuf_t	*msg;

	if (!sv_mvd)
		return;
	msg = SV_MVDMessage ();
	MSG_WriteByte (msg, svc_print);
	MSG_WriteByte (msg, level);
	MSG_WriteString (msg, text);
	if (cl)
		SV_MVDSingle (cl, msg->data, msg->cursize);
	else
		SV_MVDAll (msg->data, msg->cursize);
}

/*
===============================================================================

FRAMES

===============================================================================
*/

// every entity with a model, seen from nowhere in particular
static void MVD_Entities (packet_entities_t *pack)
{
	edict_t	*ent;
	int		e;

	pack->num_entities = 0;
	for (e=MAX_CLIENTS+1, ent=EDICT_NUM(e) ; e<sv.num_edicts && pack->num_entities < MAX_MVD_PACKET_ENTITIES ;
		e++, ent = NEXT_EDICT(ent))
	{
		if (!ent->v.modelindex || !*PR_GetString(ent->v.model))
			continue;
		if (!SV_EntityFits (&mvd.recorder, e, (int)ent->v.modelindex))
			continue;
		SV_EntityState (ent, e, &pack->entities[pack->num_entities++]);
	}
}

static void MVD_PlayerState (const client_t *cl, mvdplayer_t *p)
{
	const edict_t	*ent = cl->edict;

	p->valid = true;
	VectorCopy (ent->v.origin, p->origin);
	p->angles[0] = -3 * ent->v.angles[0];		// a model's pitch is a third of the view's, and turned
	p->angles[1] = ent->v.angles[1];
	p->angles[2] = 0;
	if (ent->v.health <= 0)
		p->angles[0] = 0;		// don't show the corpse looking around
	p->frame = (int)ent->v.frame;
	p->model = (int)ent->v.modelindex;
	p->skin = (int)ent->v.skin;
	p->effects = (int)ent->v.effects;
	p->weaponframe = (int)ent->v.weaponframe;
	p->flags = (ent->v.health <= 0 ? DF_DEAD : 0) | (ent->v.mins[2] != -24 ? DF_GIB : 0);
}

// an MVD's playerinfo: what differs from what was last said, all of it without
static void MVD_WritePlayer (sizebuf_t *msg, int num, const mvdplayer_t *p, const mvdplayer_t *from)
{
	int		flags, i;

	flags = p->flags;
	for (i=0 ; i<3 ; i++)
	{
		if (!from || p->origin[i] != from->origin[i])
			flags |= DF_ORIGIN << i;
		if (!from || p->angles[i] != from->angles[i])
			flags |= DF_ANGLES << i;
	}
	if (!from || p->model != from->model)
		flags |= DF_MODEL;
	if (!from || p->skin != from->skin || ((flags & DF_MODEL) && p->model > 255))
		flags |= DF_SKINNUM;		// a model past 255 borrows the skin's top bit
	if (!from || p->effects != from->effects)
		flags |= DF_EFFECTS;
	if (!from || p->weaponframe != from->weaponframe)
		flags |= DF_WEAPONFRAME;

	MSG_WriteByte (msg, svc_playerinfo);
	MSG_WriteByte (msg, num);
	MSG_WriteShort (msg, flags);
	MSG_WriteByte (msg, p->frame);
	for (i=0 ; i<3 ; i++)
		if (flags & (DF_ORIGIN << i))
			MSG_WriteCoord (msg, p->origin[i]);
	for (i=0 ; i<3 ; i++)
		if (flags & (DF_ANGLES << i))
			MSG_WriteAngle16 (msg, p->angles[i]);
	if (flags & DF_MODEL)
		MSG_WriteByte (msg, p->model & 255);
	if (flags & DF_SKINNUM)
		MSG_WriteByte (msg, p->skin | ((flags & DF_MODEL) && p->model > 255 ? 128 : 0));
	if (flags & DF_EFFECTS)
		MSG_WriteByte (msg, p->effects);
	if (flags & DF_WEAPONFRAME)
		MSG_WriteByte (msg, p->weaponframe);
}

// a player's stats, with the level's totals the status bar shows
static void MVD_Stats (const client_t *cl, int *stats)
{
	SV_ClientStats (cl, stats);
	stats[STAT_TOTALSECRETS] = (int)pr.global_struct->total_secrets;
	stats[STAT_TOTALMONSTERS] = (int)pr.global_struct->total_monsters;
	stats[STAT_SECRETS] = (int)pr.global_struct->found_secrets;
	stats[STAT_MONSTERS] = (int)pr.global_struct->killed_monsters;
}

// a player's stats that differ from what was last said, all of them without
static void MVD_WriteStats (mvdbuf_t *b, int num, const int *stats, const int *from)
{
	sizebuf_t	msg;
	int			i;

	MVD_InitMessage (&msg, mvd.scratch2);
	for (i=0 ; i<MAX_STATS ; i++)
	{
		if (from && stats[i] == from[i])
			continue;
		if (stats[i] >= 0 && stats[i] <= 255)
		{
			MSG_WriteByte (&msg, svc_updatestat);
			MSG_WriteByte (&msg, i);
			MSG_WriteByte (&msg, stats[i]);
		}
		else
		{
			MSG_WriteByte (&msg, svc_updatestatlong);
			MSG_WriteByte (&msg, i);
			MSG_WriteLong (&msg, stats[i]);
		}
	}
	MVD_Write (b, DEM_STATS, (unsigned)num, msg.data, msg.cursize);
}

// the entities, a delta from the last frame's or whole, as one message
static void MVD_WriteEntities (mvdbuf_t *b, const packet_entities_t *from)
{
	sizebuf_t	msg;

	MVD_InitMessage (&msg, mvd.scratch2);
	SV_EmitPacketEntities (&mvd.recorder, from, &mvd.current, &msg);
	if (msg.overflowed)
	{
		if (!mvd.overflowed)
			Con_Printf ("QTV: this level's entities don't fit in an MVD block\n");
		mvd.overflowed = true;
		return;
	}
	MVD_Write (b, DEM_ALL, 0, msg.data, msg.cursize);
}

// everything a viewer joining needs besides the level's fixed part: the
// server's info, the clients, the lightstyles, the players and the entities
// as the frame left them, then "skins", which relays go live at
static void MVD_WriteSnapshot (mvdbuf_t *b)
{
	sizebuf_t	msg;
	client_t	*cl;
	int			i;

	MVD_Clear (b);
	MVD_InitMessage (&msg, mvd.scratch2);
	MSG_WriteByte (&msg, svc_stufftext);
	MSG_WriteString (&msg, va("fullserverinfo \"%s\"\n", svs.info));
	for (i=0, cl=svs.clients ; i<MAX_CLIENTS ; i++, cl++)
	{
		if (cl->state < cs_connected)
			continue;
		MVD_Room (b, &msg, 32 + MAX_INFO_STRING);
		SV_FullClientUpdate (cl, &msg);
	}
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
	{
		MVD_Room (b, &msg, 3 + (sv.lightstyles[i] ? (int)strlen (sv.lightstyles[i]) : 0));
		MSG_WriteByte (&msg, svc_lightstyle);
		MSG_WriteByte (&msg, i);
		MSG_WriteString (&msg, sv.lightstyles[i] ? sv.lightstyles[i] : "");
	}
	if (sv.paused)
	{
		MSG_WriteByte (&msg, svc_setpause);
		MSG_WriteByte (&msg, 1);
	}
	MVD_Write (b, DEM_ALL, 0, msg.data, msg.cursize);

	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (mvd.statsvalid[i])
			MVD_WriteStats (b, i, mvd.stats[i], NULL);

	MVD_WriteEntities (b, NULL);
	MVD_InitMessage (&msg, mvd.scratch2);
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (mvd.players[i].valid)
			MVD_WritePlayer (&msg, i, &mvd.players[i], NULL);
	MSG_WriteByte (&msg, svc_stufftext);
	MSG_WriteString (&msg, "skins\n");
	MVD_Write (b, DEM_ALL, 0, msg.data, msg.cursize);
}

// a list of names, svc_soundlist's or svc_modellist's, in pieces the size of
// a client's (SV_Modellist_f), each saying where the next starts: a piece only
// ends where that byte isn't 0; models past 255 go on with FTE's short start
static void MVD_WriteList (sizebuf_t *msg, mvdbuf_t *b, int svc, char **names, int max)
{
	int		n, i;

	n = 0;
	do
	{
		if (n > 255)
		{
			MSG_WriteByte (msg, svc_fte_modellistshort);
			MSG_WriteShort (msg, n);
		}
		else
		{
			MSG_WriteByte (msg, svc);
			MSG_WriteByte (msg, n);
		}
		for (i = n + 1 ; i < max && names[i] && (!((i - 1) & 255) || msg->cursize < MAX_MSGLEN / 2) ; i++)
			MSG_WriteString (msg, names[i]);
		MSG_WriteByte (msg, 0);
		n = i < max && names[i] ? i - 1 : 0;
		MSG_WriteByte (msg, n & 255);
		MVD_Write (b, DEM_ALL, 0, msg->data, msg->cursize);
		SZ_Clear (msg);
	} while (n);
}

// the level's gamestate that doesn't change: serverdata, its lists of sounds
// and models, statics, baselines and signon
static void MVD_WriteLevel (mvdbuf_t *b)
{
	sizebuf_t	msg;
	char		*gamedir;
	int			i;

	MVD_Clear (b);
	MVD_InitMessage (&msg, mvd.scratch2);
	gamedir = Info_ValueForKey (svs.info, "*gamedir");
	MSG_WriteByte (&msg, svc_serverdata);
	MSG_WriteLong (&msg, PROTOCOL_VERSION_FTE);
	MSG_WriteLong (&msg, (int)mvd.recorder.fteext);
	MSG_WriteLong (&msg, PROTOCOL_VERSION);
	MSG_WriteLong (&msg, svs.spawncount);
	MSG_WriteString (&msg, gamedir[0] ? gamedir : "qw");
	MSG_WriteFloat (&msg, (float)sv.time);		// an MVD's, for the player number
	MSG_WriteString (&msg, PR_GetString(sv.edicts->v.message));
	MSG_WriteFloat (&msg, sv.movevars.gravity);
	MSG_WriteFloat (&msg, sv.movevars.stopspeed);
	MSG_WriteFloat (&msg, sv.movevars.maxspeed);
	MSG_WriteFloat (&msg, sv.movevars.spectatormaxspeed);
	MSG_WriteFloat (&msg, sv.movevars.accelerate);
	MSG_WriteFloat (&msg, sv.movevars.airaccelerate);
	MSG_WriteFloat (&msg, sv.movevars.wateraccelerate);
	MSG_WriteFloat (&msg, sv.movevars.friction);
	MSG_WriteFloat (&msg, sv.movevars.waterfriction);
	MSG_WriteFloat (&msg, sv.movevars.entgravity);
	MSG_WriteByte (&msg, svc_cdtrack);
	MSG_WriteByte (&msg, (int)sv.edicts->v.sounds);
	MVD_Write (b, DEM_ALL, 0, msg.data, msg.cursize);
	SZ_Clear (&msg);

	MVD_WriteList (&msg, b, svc_soundlist, sv.sound_precache, MAX_SOUNDS);
	MVD_WriteList (&msg, b, svc_modellist, sv.model_precache, MAX_MODELS);

	for (i=0 ; i<sv.num_static_entities ; i++)
	{
		MVD_Room (b, &msg, 64);
		SV_WriteStatic (&mvd.recorder, &msg, &sv.static_entities[i]);
	}
	for (i=0 ; i<sv.num_baselines ; i++)
	{
		MVD_Room (b, &msg, 64);
		SV_WriteBaseline (&mvd.recorder, &msg, i);
	}
	for (i=0 ; i<sv.num_signon_buffers ; i++)
	{
		MVD_Room (b, &msg, sv.signon_buffer_size[i]);
		SZ_Write (&msg, sv.signon_buffers[i], sv.signon_buffer_size[i]);
	}
	MVD_Write (b, DEM_ALL, 0, msg.data, msg.cursize);
}

// the frame's time: msec since the last, at most a byte's. The clock is the
// world's, a pause's too: a frame of no time would be read as the one before
// it, its entities a delta from themselves.
static int MVD_FrameTime (void)
{
	int		msec;

	msec = (int)((host.realtime - mvd.clock) * 1000);
	if (msec > 255)
	{
		mvd.clock = host.realtime;
		return 255;
	}
	mvd.clock += msec * 0.001;
	return msec;
}

/*
==================
MVD_WriteFrame

The frame: the events since the last, then the entities, the players and
their stats as deltas, and a snapshot when one is due. A level's start is its
fixed gamestate and a snapshot alone.
==================
*/
static void MVD_WriteFrame (bool start)
{
	sizebuf_t	msg;
	client_t	*cl;
	mvdplayer_t	player;
	int			stats[MAX_STATS];
	bool		snapshot;
	int			i;

	if (start)
	{
		mvd.recorder.fteext = MVD_FTE_EXTENSIONS | (sv.bigcoords ? FTE_PEXT_FLOATCOORDS : 0);
		mvd.recorder.mvdext1 = 0;
		MVD_Clear (&mvd.frame);
		memset (mvd.players, 0, sizeof(mvd.players));
		memset (mvd.statsvalid, 0, sizeof(mvd.statsvalid));
		mvd.entities.num_entities = 0;
		mvd.overflowed = false;
		mvd.clock = host.realtime;
		mvd.nextpings = host.realtime + MVD_PINGTIME;
		MVD_WriteLevel (&mvd.level);
		NET_QTVLevel (mvd.level.data, mvd.level.length);
	}
	else if (host.realtime >= mvd.nextpings)
	{	// the pings and losses, every so often
		mvd.nextpings = host.realtime + MVD_PINGTIME;
		MVD_InitMessage (&msg, mvd.scratch2);
		for (i=0, cl=svs.clients ; i<MAX_CLIENTS ; i++, cl++)
		{
			if (cl->state != cs_spawned)
				continue;
			MSG_WriteByte (&msg, svc_updateping);
			MSG_WriteByte (&msg, i);
			MSG_WriteShort (&msg, SV_CalcPing (cl));
			MSG_WriteByte (&msg, svc_updatepl);
			MSG_WriteByte (&msg, i);
			MSG_WriteByte (&msg, cl->lossage);
		}
		MVD_Write (&mvd.frame, DEM_ALL, 0, msg.data, msg.cursize);
	}

	// the entities, a delta from the last frame's
	MVD_Entities (&mvd.current);
	if (!start)
		MVD_WriteEntities (&mvd.frame, &mvd.entities);
	mvd.entities = mvd.current;

	// the players, as deltas from what was last said; one gone is forgotten,
	// and said whole when back
	MVD_InitMessage (&msg, mvd.scratch2);
	for (i=0, cl=svs.clients ; i<MAX_CLIENTS ; i++, cl++)
	{
		if (cl->state != cs_spawned || cl->spectator)
		{
			mvd.players[i].valid = false;
			continue;
		}
		MVD_PlayerState (cl, &player);
		if (!start)
			MVD_WritePlayer (&msg, i, &player, mvd.players[i].valid ? &mvd.players[i] : NULL);
		mvd.players[i] = player;
	}
	MVD_Write (&mvd.frame, DEM_ALL, 0, msg.data, msg.cursize);

	// their stats, likewise
	for (i=0, cl=svs.clients ; i<MAX_CLIENTS ; i++, cl++)
	{
		if (!mvd.players[i].valid)
		{
			mvd.statsvalid[i] = false;
			continue;
		}
		MVD_Stats (cl, stats);
		if (!start)
			MVD_WriteStats (&mvd.frame, i, stats, mvd.statsvalid[i] ? mvd.stats[i] : NULL);
		memcpy (mvd.stats[i], stats, sizeof(stats));
		mvd.statsvalid[i] = true;
	}

	snapshot = start || host.realtime >= mvd.nextsnapshot;
	if (snapshot)
	{
		mvd.nextsnapshot = host.realtime + MVD_SNAPSHOTTIME;
		MVD_WriteSnapshot (&mvd.snapshot);
	}
	if (start)
		MVD_Clear (&mvd.frame);
	else if (mvd.frame.length)
		mvd.frame.data[0] = (byte)MVD_FrameTime ();
	NET_QTVFrame (mvd.frame.data, mvd.frame.length, mvd.snapshot.data, snapshot ? mvd.snapshot.length : 0);
	MVD_Clear (&mvd.frame);
}

/*
===============================================================================

THE SERVER'S

===============================================================================
*/

/*
==================
SV_MVDFrame

After the clients' messages: the writing started or stopped with the
viewers, the frame written when one is due, and what is out sent
==================
*/
void SV_MVDFrame (void)
{
	if (sv.state != ss_active || !NET_QTVViewers ())
	{
		if (sv_mvd)
		{
			sv_mvd = false;
			NET_QTVReset ();
		}
	}
	else if (!sv_mvd)
	{
		sv_mvd = true;
		mvd.nextframe = host.realtime + MVD_FRAMETIME;
		MVD_WriteFrame (true);
	}
	else if (host.realtime >= mvd.nextframe)
	{
		mvd.nextframe += MVD_FRAMETIME;
		if (mvd.nextframe < host.realtime)
			mvd.nextframe = host.realtime + MVD_FRAMETIME;
		MVD_WriteFrame (false);
	}
	NET_QTVSend (qtv_delay.value > 0 ? qtv_delay.value : 0);
}

// the old level's last events, before a new one spawns: a frame of its own
void SV_MVDEndLevel (void)
{
	if (!sv_mvd || !mvd.frame.length)
		return;
	mvd.frame.data[0] = (byte)MVD_FrameTime ();
	NET_QTVFrame (mvd.frame.data, mvd.frame.length, NULL, 0);
	MVD_Clear (&mvd.frame);
}

// a new level: its gamestate, which the viewers go on with
void SV_MVDNewLevel (void)
{
	if (!sv_mvd)
		return;
	mvd.nextframe = host.realtime + MVD_FRAMETIME;
	MVD_WriteFrame (true);
}

void SV_MVDInit (void)
{
	Cvar_RegisterVariable (&qtv_delay);
	MVD_Clear (&mvd.frame);
	MVD_Clear (&mvd.snapshot);
	MVD_Clear (&mvd.level);
}
