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
// slist_engine.c -- the list's threads: the engine, which asks and reads the
// answers, and a reader of the sources a scan
//
// The engine reads its sockets, hands the answers to the scheduler, sends what
// came due and publishes the list when it changed, then waits on its sockets
// for the next. Alone on its thread, the scheduler needs no locks; what is
// shared is little: orders in (a scan, a server to describe), snapshots and
// messages out, under a spin lock held for a pointer's swap.
//
// Names and lists are read on a thread of their own a scan, as a name's lookup
// takes as long as its server likes and a list's download longer: a source is
// posted as it is read, so a slow one delays itself only. The engine and the
// reader share its inbox, freed by the last to let it go: at shutdown the
// reader is left to finish alone.
//
// Neither thread prints, reads cvars or calls Mem_* or Sys_Error: messages
// wait for the main thread (SL_TakeMessage).

#include "slist_local.h"

#include "net_socket.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SL_SOCKETS		16		// source ports: a router's table of flows fills slower
#define SL_PUBLISH		0.1		// seconds between snapshots at most
#define SL_MAXDATAGRAM	16384	// one filled is taken as cut short
#define SL_MAXMESSAGES	16
#define SL_MAXMESSAGE	192
#define SL_MAXDESCRIBES	16
#define SL_MAXLIST		(4 << 20)	// a list's bytes, a file's or a URL's

// a scan asked for
typedef struct
{
	slsource_t	sources[SL_MAXSOURCES];
	int			numsources;
	slconfig_t	config;
} slorder_t;

#define SL_QTVBATCH		-2		// a batch's source: the QTV list's streams (its servers' info)

// a game's stream on QTV, by its server's address
typedef struct
{
	netadr_t	address;
	char		stream[SL_MAXSTREAM];
} slstream_t;

// what a source came to: a master to ask, or servers
typedef struct slbatch_s
{
	struct slbatch_s	*next;
	int			source;			// -1: the sources are done
	bool		ok;
	char		message[SL_MAXMESSAGE];
	netadr_t	master;			// NA_INVALID for none
	int			numservers, maxservers;
	struct
	{
		netadr_t	address;
		char		*info;		// what the list knew, NULL for none
	}			*servers;
} slbatch_t;

// a reader's, shared with the engine: the last to let go of it frees it
typedef struct
{
	atomic_int		refs;
	atomic_bool		abandoned;
	atomic_flag		lock;
	slbatch_t		*first, *last;
	char			dir[1024];
	slsource_t		sources[SL_MAXSOURCES];
	int				numsources;
	char			qtvlist[256];	// the QTV list's URL, "" none
} slinbox_t;

typedef struct
{
	netadr_t	address;
	int			source;
	int			asked;
	double		last;
	bool		answered;
} slmaster_t;

static struct
{
	systhread_t		*thread;
	atomic_bool		stop, paused;
	atomic_flag		lock;			// the next five
	slorder_t		*order;
	netadr_t		describes[SL_MAXDESCRIBES];
	int				numdescribes;
	char			messages[SL_MAXMESSAGES][SL_MAXMESSAGE];
	int				nummessages;
	slsnapshot_t	*published;

	// the engine's, set up by SL_Start before it runs
	udpsocket_t		*sockets[SL_SOCKETS];
	int				numsockets, nextsocket;
	char			dir[1024], cachepath[1024];
	slsource_t		sources[SL_MAXSOURCES];
	int				numsources;
	slserver_t		*seed;
	int				numseed;
} sl = {.lock = ATOMIC_FLAG_INIT};

static void SL_Lock (atomic_flag *lock)
{
	while (atomic_flag_test_and_set_explicit (lock, memory_order_acquire))
		;
}

static void SL_Unlock (atomic_flag *lock)
{
	atomic_flag_clear_explicit (lock, memory_order_release);
}

// for the main thread's console; the oldest kept when there are too many
static void SL_Message (const char *fmt, ...)
{
	va_list	args;
	char	text[SL_MAXMESSAGE];

	va_start (args, fmt);
	vsnprintf (text, sizeof(text), fmt, args);
	va_end (args);
	SL_Lock (&sl.lock);
	if (sl.nummessages < SL_MAXMESSAGES)
		memcpy (sl.messages[sl.nummessages++], text, sizeof(text));
	SL_Unlock (&sl.lock);
}

static uint64_t SL_Marked (const slsource_t *sources, int count)
{
	uint64_t	mask = 0;
	int			i;

	for (i = 0 ; i < count && i < SL_MAXSOURCES ; i++)
		if (sources[i].marked)
			mask |= 1ull << i;
	return mask;
}

/*
==============================================================================

SNAPSHOTS

One block a snapshot: its servers, their rosters and their serverinfos

==============================================================================
*/

static slsnapshot_t *SL_BuildSnapshot (const byte *servers, size_t stride, int count)
{
	const slserver_t	*s;
	slsnapshot_t		*snap;
	slplayer_t			*roster;
	char				*text;
	size_t				players = 0, infos = 0, length;
	int					i;

	for (i = 0 ; i < count ; i++)
	{
		s = (const slserver_t *)(servers + (size_t)i * stride);
		players += (size_t)s->numroster;
		infos += strlen (s->info) + 1;
	}
	snap = malloc (sizeof(*snap) + (size_t)count * sizeof(slserver_t) + players * sizeof(slplayer_t) + infos);
	if (!snap)
		return NULL;
	memset (snap, 0, sizeof(*snap));
	snap->servers = (slserver_t *)(snap + 1);
	snap->numservers = count;
	roster = (slplayer_t *)(snap->servers + count);
	text = (char *)(roster + players);
	for (i = 0 ; i < count ; i++)
	{
		s = (const slserver_t *)(servers + (size_t)i * stride);
		snap->servers[i] = *s;
		snap->servers[i].roster = roster;
		if (s->numroster)
			memcpy (roster, s->roster, (size_t)s->numroster * sizeof(*roster));
		roster += s->numroster;
		length = strlen (s->info) + 1;
		memcpy (text, s->info, length);
		snap->servers[i].info = text;
		text += length;
	}
	return snap;
}

// the snapshot's counts, of its servers and of the sources that named them
static void SL_CountSnapshot (slsnapshot_t *snap, const slsourcestatus_t *status, int numsources)
{
	const slserver_t	*s;
	int					i, b;

	memcpy (snap->sources, status, sizeof(snap->sources));
	for (b = 0 ; b < numsources ; b++)
		snap->sources[b].servers = 0;
	for (i = 0, s = snap->servers ; i < snap->numservers ; i++, s++)
	{
		snap->alive += s->state == SL_ALIVE;
		snap->dead += s->state == SL_DEAD;
		snap->described += *s->info != 0;
		snap->relays += s->proxy != SL_NOPROXY;
		snap->routed += SL_Relayed (s);
		for (b = 0 ; b < numsources ; b++)
			if (s->sources & (1ull << b))
				snap->sources[b].servers++;
	}
}

static void SL_Publish (slsnapshot_t *snap)
{
	slsnapshot_t	*old;

	if (!snap)
		return;
	SL_Lock (&sl.lock);
	old = sl.published;
	sl.published = snap;
	SL_Unlock (&sl.lock);
	free (old);		// never taken: the main thread missed it
}

slsnapshot_t *SL_TakeSnapshot (void)
{
	slsnapshot_t	*snap;

	SL_Lock (&sl.lock);
	snap = sl.published;
	sl.published = NULL;
	SL_Unlock (&sl.lock);
	return snap;
}

void SL_FreeSnapshot (slsnapshot_t *s)
{
	free (s);
}

bool SL_TakeMessage (char *buf, size_t size)
{
	bool	got = false;

	SL_Lock (&sl.lock);
	if (sl.nummessages)
	{
		Q_strncpyz (buf, sl.messages[0], size);
		memmove (sl.messages, sl.messages + 1, sizeof(sl.messages[0]) * (size_t)--sl.nummessages);
		got = true;
	}
	SL_Unlock (&sl.lock);
	return got;
}

/*
==============================================================================

THE READER OF SOURCES

==============================================================================
*/

static void SL_ReleaseInbox (slinbox_t *inbox)
{
	slbatch_t	*b, *next;
	int			i;

	if (atomic_fetch_sub (&inbox->refs, 1) != 1)
		return;
	for (b = inbox->first ; b ; b = next)
	{
		next = b->next;
		for (i = 0 ; i < b->numservers ; i++)
			free (b->servers[i].info);
		free (b->servers);
		free (b);
	}
	free (inbox);
}

static void SL_Post (slinbox_t *inbox, slbatch_t *b)
{
	SL_Lock (&inbox->lock);
	if (inbox->last)
		inbox->last->next = b;
	else
		inbox->first = b;
	inbox->last = b;
	SL_Unlock (&inbox->lock);
}

// a host's address, its port defaultport where it has none: numeric, or a
// name looked up (IPv4's where it has both, as a master's reply is IPv4's)
static bool SL_Lookup (const char *address, int defaultport, netadr_t *a)
{
	char	host[256];
	int		port;

	if (SL_ParseAddress (address, defaultport, a))
		return true;
	if (!SL_SplitAddress (address, defaultport, host, sizeof(host), &port) || !UDP_Resolve (host, false, a))
		return false;
	a->type = NA_IP;
	a->port = (unsigned short)BigShort ((short)port);
	return true;
}

typedef struct
{
	slinbox_t	*inbox;
	slbatch_t	*batch;
} sllisted_t;

static void SL_Listed (void *ctx, const char *address, const char *info, size_t infolen)
{
	sllisted_t	*l = ctx;
	slbatch_t	*b = l->batch;
	netadr_t	a;
	void		*grown;
	int			size;

	if (atomic_load (&l->inbox->abandoned) || b->numservers == SL_MAXSERVERS || !SL_Lookup (address, SL_SERVERPORT, &a))
		return;
	if (b->numservers == b->maxservers)
	{
		size = b->maxservers ? b->maxservers * 2 : 64;
		if (!(grown = realloc (b->servers, (size_t)size * sizeof(*b->servers))))
			return;
		b->servers = grown;
		b->maxservers = size;
	}
	b->servers[b->numservers].address = a;
	b->servers[b->numservers].info = NULL;
	if (info && infolen && (b->servers[b->numservers].info = malloc (infolen + 1)))
	{
		memcpy (b->servers[b->numservers].info, info, infolen);
		b->servers[b->numservers].info[infolen] = 0;
	}
	b->numservers++;
}

// a file of the sources' directory (or one named whole), up to SL_MAXLIST
static char *SL_ReadList (const char *dir, const char *name, size_t *length)
{
	char	path[2048];
	FILE	*f;
	char	*data = NULL;
	long	size;

	if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':'))
		Q_strncpyz (path, name, sizeof(path));
	else
		snprintf (path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen (path, "rb")))
		return NULL;
	if (!fseek (f, 0, SEEK_END) && (size = ftell (f)) >= 0 && size <= SL_MAXLIST && !fseek (f, 0, SEEK_SET)
		&& (data = malloc ((size_t)size + 1)))
	{
		if (fread (data, 1, (size_t)size, f) == (size_t)size)
		{
			data[size] = 0;
			*length = (size_t)size;
		}
		else
		{
			free (data);
			data = NULL;
		}
	}
	fclose (f);
	return data;
}

static void SL_ReadSource (slinbox_t *inbox, const slsource_t *s, slbatch_t *b)
{
	sllisted_t	listed = {inbox, b};
	char		error[128], *data;
	size_t		length;

	switch (s->type)
	{
	case SL_MASTER:
		if ((b->ok = SL_Lookup (s->location, SL_MASTERPORT, &b->master)))
			return;
		b->master.type = NA_INVALID;
		snprintf (b->message, sizeof(b->message), "Server browser: \"%s\": %s isn't found\n", s->name, s->location);
		return;
	case SL_SERVER:
		SL_Listed (&listed, s->location, NULL, 0);
		if (!(b->ok = b->numservers > 0))
			snprintf (b->message, sizeof(b->message), "Server browser: \"%s\": %s isn't found\n", s->name, s->location);
		return;
	case SL_FILE:
		if (!(data = SL_ReadList (inbox->dir, s->location, &length)))
		{
			snprintf (b->message, sizeof(b->message), "Server browser: \"%s\": %s can't be read\n", s->name, s->location);
			return;
		}
		break;
	case SL_URL:
	default:
		if (!(data = SL_HttpGet (s->location, SL_MAXLIST, &length, error, sizeof(error))))
		{
			snprintf (b->message, sizeof(b->message), "Server browser: \"%s\": %s\n", s->name, error);
			return;
		}
		break;
	}
	SL_ParseListing (data, length, SL_Listed, &listed);
	free (data);
	b->ok = true;
}

// a stream the QTV list has, for the server at its address
static void SL_Streamed (void *ctx, netadr_t server, const char *stream)
{
	slbatch_t	*b = ctx;
	void		*grown;
	int			size;

	if (b->numservers == SL_MAXSERVERS)
		return;
	if (b->numservers == b->maxservers)
	{
		size = b->maxservers ? b->maxservers * 2 : 64;
		if (!(grown = realloc (b->servers, (size_t)size * sizeof(*b->servers))))
			return;
		b->servers = grown;
		b->maxservers = size;
	}
	b->servers[b->numservers].address = server;
	if ((b->servers[b->numservers].info = malloc (strlen (stream) + 1)))
	{
		strcpy (b->servers[b->numservers].info, stream);
		b->numservers++;
	}
}

// the QTV list's streams (ezQuake's qtv_api_url), in a batch of their own
static slbatch_t *SL_ReadQTVList (const char *url)
{
	slbatch_t	*b = calloc (1, sizeof(*b));
	char		error[128], *data;
	size_t		length;

	if (!b)
		return NULL;
	b->source = SL_QTVBATCH;
	if (!(data = SL_HttpGet (url, SL_MAXLIST, &length, error, sizeof(error))))
		snprintf (b->message, sizeof(b->message), "Server browser: the QTV list: %s\n", error);
	else if (!(b->ok = SL_ParseQTVList (data, length, SL_Streamed, b) >= 0))
		snprintf (b->message, sizeof(b->message), "Server browser: the QTV list (%s) isn't one\n", url);
	free (data);
	return b;
}

// the marked sources in order, each posted as it is read, then the end; then
// the QTV list, which a scan doesn't wait for
static void SL_ReadSources (void *arg)
{
	slinbox_t	*inbox = arg;
	slbatch_t	*b;
	int			i;

	for (i = 0 ; i < inbox->numsources && !atomic_load (&inbox->abandoned) ; i++)
	{
		if (!inbox->sources[i].marked || !(b = calloc (1, sizeof(*b))))
			continue;
		b->source = i;
		SL_ReadSource (inbox, &inbox->sources[i], b);
		SL_Post (inbox, b);
	}
	if ((b = calloc (1, sizeof(*b))))
	{
		b->source = -1;
		SL_Post (inbox, b);
	}
	if (*inbox->qtvlist && !atomic_load (&inbox->abandoned) && (b = SL_ReadQTVList (inbox->qtvlist)))
		SL_Post (inbox, b);
	SL_ReleaseInbox (inbox);
}

// the reader of a scan's sources, started; NULL if it couldn't be
static slinbox_t *SL_StartReading (const slsource_t *sources, int numsources, const char *qtvlist)
{
	slinbox_t	*inbox = calloc (1, sizeof(*inbox));
	systhread_t	*t;

	if (!inbox)
		return NULL;
	atomic_init (&inbox->refs, 2);
	atomic_init (&inbox->abandoned, false);
	atomic_flag_clear (&inbox->lock);
	Q_strncpyz (inbox->dir, sl.dir, sizeof(inbox->dir));
	memcpy (inbox->sources, sources, (size_t)numsources * sizeof(*sources));
	inbox->numsources = numsources;
	Q_strncpyz (inbox->qtvlist, qtvlist, sizeof(inbox->qtvlist));
	if (!(t = Sys_StartThread ("slist-sources", SL_ReadSources, inbox)))
	{
		free (inbox);
		return NULL;
	}
	Sys_DetachThread (t);
	return inbox;
}

/*
==============================================================================

THE ENGINE

==============================================================================
*/

typedef struct
{
	slsched_t			sched;
	slconfig_t			config;
	slsource_t			sources[SL_MAXSOURCES];
	int					numsources;
	slsourcestatus_t	status[SL_MAXSOURCES];
	slmaster_t			masters[SL_MAXSOURCES];
	int					nummasters;
	slinbox_t			*inbox;
	bool				scanning, sourcesdone;
	unsigned			generation;
	slstream_t			*streams;		// the QTV list's, by address
	int					numstreams;
	bool				streamsread;	// this run's list read: servers not in it have none
} slengine_t;

// the address a stream is the game of, in order
static int SL_CompareStreams (const void *a, const void *b)
{
	const slstream_t	*x = a, *y = b;
	int					c = memcmp (x->address.ip, y->address.ip, sizeof(x->address.ip));

	return c ? c : (int)x->address.port - (int)y->address.port;
}

// the servers' streams as the QTV list has them, where one was read
static void SL_ApplyStreams (slengine_t *e)
{
	slstream_t	key, *found;
	slserver_t	*s;
	int			i;

	if (!e->streamsread)
		return;
	for (i = 0 ; i < e->sched.numhosts ; i++)
	{
		s = &e->sched.hosts[i].e;
		key.address = s->address;
		found = e->numstreams ? bsearch (&key, e->streams, (size_t)e->numstreams, sizeof(*e->streams),
			SL_CompareStreams) : NULL;
		Q_strncpyz (s->qtv, found ? found->stream : "", sizeof(s->qtv));
	}
}

static void SL_Send (const void *data, int length, netadr_t to)
{
	UDP_Send (sl.sockets[sl.nextsocket], data, length, &to);
	sl.nextsocket = (sl.nextsocket + 1) % sl.numsockets;
}

static void SL_SaveTable (slengine_t *e)
{
	slserver_t	*servers;
	int			i;

	if (!(servers = malloc ((size_t)(e->sched.numhosts ? e->sched.numhosts : 1) * sizeof(*servers))))
		return;
	for (i = 0 ; i < e->sched.numhosts ; i++)
		servers[i] = e->sched.hosts[i].e;
	if (!SL_SaveCache (sl.cachepath, servers, e->sched.numhosts, e->sources, e->numsources))
		SL_Message ("Server browser: %s can't be written\n", sl.cachepath);
	free (servers);
}

static slsnapshot_t *SL_Snapshot (const slengine_t *e)
{
	slsnapshot_t	*snap = SL_BuildSnapshot ((const byte *)e->sched.hosts + offsetof (slhost_t, e), sizeof(slhost_t),
		e->sched.numhosts);

	if (!snap)
		return NULL;
	snap->generation = e->generation;
	snap->scanning = e->scanning;
	snap->paused = atomic_load (&sl.paused);
	snap->round = SLS_Round (&e->sched);
	snap->rounds = e->sched.rounds;
	snap->pingssent = e->sched.pingssent;
	snap->pingstotal = e->sched.numorder * e->sched.rounds;
	SL_CountSnapshot (snap, e->status, e->numsources);
	return snap;
}

// a new scan: what was found carried over, unconfirmed again, under the new
// sources (matched by name); the servers of none of them dropped
static void SL_Restart (slengine_t *e, const slorder_t *order, double now)
{
	slserver_t			*carried;
	slsourcestatus_t	status[SL_MAXSOURCES];
	int					map[SL_MAXSOURCES], count = 0, i, j;
	uint64_t			sources;

	for (i = 0 ; i < SL_MAXSOURCES ; i++)
		map[i] = -1;
	memset (status, 0, sizeof(status));
	for (i = 0 ; i < order->numsources ; i++)
	{
		for (j = 0 ; j < e->numsources ; j++)
			if (!strcmp (order->sources[i].name, e->sources[j].name))
			{
				map[j] = i;
				status[i].updated = e->status[j].updated;
			}
		status[i].state = order->sources[i].marked ? SLSRC_ASKING : SLSRC_IDLE;
	}

	carried = malloc ((size_t)(e->sched.numhosts ? e->sched.numhosts : 1) * sizeof(*carried));
	for (i = 0 ; carried && i < e->sched.numhosts ; i++)
	{
		slserver_t	*s = &e->sched.hosts[i].e;

		for (j = 0, sources = 0 ; j < SL_MAXSOURCES ; j++)
			if ((s->sources & (1ull << j)) && map[j] >= 0)
				sources |= 1ull << map[j];
		if (!sources)
			continue;
		carried[count] = *s;
		carried[count].sources = sources;
		carried[count].routeping = -1;
		carried[count].numhops = 0;
		free (carried[count].roster);		// yesterday's roster is worse than none
		carried[count].roster = NULL;
		carried[count].numroster = 0;
		count++;
		s->info = sl_noinfo;		// moved
		s->roster = NULL;
	}
	SLS_Free (&e->sched);
	memcpy (e->sources, order->sources, sizeof(e->sources));
	e->numsources = order->numsources;
	memcpy (e->status, status, sizeof(e->status));
	e->config = order->config;
	SLS_Init (&e->sched, &e->config, SL_Marked (e->sources, e->numsources), now);
	if (carried)
		SLS_Seed (&e->sched, carried, count);

	// the sources read again
	if (e->inbox)
	{
		atomic_store (&e->inbox->abandoned, true);
		SL_ReleaseInbox (e->inbox);
	}
	e->nummasters = 0;
	e->inbox = SL_StartReading (e->sources, e->numsources, e->config.qtvlist);
	e->sourcesdone = !e->inbox;
	if (!e->inbox)
		SL_Message ("Server browser: the sources can't be read\n");
	e->scanning = true;
}

// the QTV list read, in place of the one before
static void SL_TakeStreams (slengine_t *e, const slbatch_t *b)
{
	slstream_t	*streams = b->numservers ? malloc ((size_t)b->numservers * sizeof(*streams)) : NULL;
	int			i;

	if (b->numservers && !streams)
		return;
	for (i = 0 ; i < b->numservers ; i++)
	{
		streams[i].address = b->servers[i].address;
		Q_strncpyz (streams[i].stream, b->servers[i].info, sizeof(streams[i].stream));
	}
	if (b->numservers)
		qsort (streams, (size_t)b->numservers, sizeof(*streams), SL_CompareStreams);
	free (e->streams);
	e->streams = streams;
	e->numstreams = b->numservers;
	e->streamsread = true;
}

// what the reader posted: masters to ask, servers listed; true if any came
static bool SL_TakeBatches (slengine_t *e, int64_t epoch)
{
	slbatch_t	*b, *next;
	bool		any = false;
	int			i;

	SL_Lock (&e->inbox->lock);
	b = e->inbox->first;
	e->inbox->first = e->inbox->last = NULL;
	SL_Unlock (&e->inbox->lock);

	for ( ; b ; b = next)
	{
		next = b->next;
		any = true;
		if (b->source == SL_QTVBATCH)
		{
			if (b->message[0])
				SL_Message ("%s", b->message);
			if (b->ok)
				SL_TakeStreams (e, b);
		}
		else if (b->source < 0)
			e->sourcesdone = true;
		else if (b->source < e->numsources)
		{
			if (b->message[0])
				SL_Message ("%s", b->message);
			if (!b->ok)
				e->status[b->source].state = SLSRC_FAILED;
			else if (b->master.type != NA_INVALID)
			{
				if (e->nummasters < SL_MAXSOURCES)
					e->masters[e->nummasters++] = (slmaster_t){b->master, b->source, 0, -1e9, false};
			}
			else
			{
				for (i = 0 ; i < b->numservers ; i++)
					SLS_Add (&e->sched, b->servers[i].address, b->servers[i].info,
						b->servers[i].info ? strlen (b->servers[i].info) : 0, 1ull << b->source);
				e->status[b->source].state = SLSRC_ANSWERED;
				e->status[b->source].updated = epoch;
			}
		}
		for (i = 0 ; i < b->numservers ; i++)
			free (b->servers[i].info);
		free (b->servers);
		free (b);
	}
	return any;
}

typedef struct
{
	slsched_t	*sched;
	uint64_t	bit;
} slnamed_t;

static void SL_Named (void *ctx, netadr_t a)
{
	slnamed_t	*n = ctx;

	SLS_Add (n->sched, a, NULL, 0, n->bit);
}

// a datagram, to the question it answers: whether it told anything
static bool SL_Dispatch (slengine_t *e, netadr_t from, const byte *data, int length, double at, int64_t epoch)
{
	slmaster_t	*m;
	slnamed_t	named;
	int			i;

	// a master's list is believed of a master asked only (its port aside: some
	// answer from another), or anyone could post one
	if (length >= 6 && !memcmp (data, "\xff\xff\xff\xff" "d\n", 6))
	{
		for (i = 0, m = e->masters ; i < e->nummasters ; i++, m++)
			if (!memcmp (m->address.ip, from.ip, sizeof(from.ip)))
				break;
		if (i == e->nummasters)
			return false;
		m->answered = true;
		e->status[m->source].state = SLSRC_ANSWERED;
		e->status[m->source].updated = epoch;
		named = (slnamed_t){&e->sched, 1ull << m->source};
		return SL_ParseMasterReply (data, length, SL_Named, &named) > 0;
	}
	if (SL_IsPingAck (data, length))
		return SLS_OnPingAck (&e->sched, from, at, epoch);
	return SLS_OnPrint (&e->sched, from, data, length, epoch);
}

static void SL_Engine (void *arg)
{
	slengine_t	*e = arg;
	slorder_t	*order;
	netadr_t	describes[SL_MAXDESCRIBES], from;
	sldue_t		due[SL_MAXDUE];
	byte		*buf = malloc (SL_MAXDATAGRAM);
	double		now, published = -1;
	int64_t		epoch;
	bool		changed = false, settled;
	int			numdescribes, length, n, i;

	while (buf && !atomic_load (&sl.stop))
	{
		now = Sys_DoubleTime ();
		epoch = (int64_t)time (NULL);

		SL_Lock (&sl.lock);
		order = sl.order;
		sl.order = NULL;
		numdescribes = sl.numdescribes;
		memcpy (describes, sl.describes, (size_t)numdescribes * sizeof(describes[0]));
		sl.numdescribes = 0;
		SL_Unlock (&sl.lock);
		if (order)
		{
			SL_Restart (e, order, now);
			free (order);
			changed = true;
		}
		for (i = 0 ; i < numdescribes ; i++)
			SLS_Prioritise (&e->sched, describes[i]);

		if (e->inbox && SL_TakeBatches (e, epoch))
			changed = true;

		// each answer timed as it is read, not as the reading began
		for (i = 0 ; i < sl.numsockets ; i++)
			while ((length = UDP_Recv (sl.sockets[i], buf, SL_MAXDATAGRAM, &from)) > 0)
				if (SL_Dispatch (e, from, buf, length, Sys_DoubleTime (), epoch))
					changed = true;

		if (!atomic_load (&sl.paused))
		{
			// a master asked again until it answers, as a reply may be lost
			for (i = 0 ; i < e->nummasters ; i++)
				if (!e->masters[i].answered && e->masters[i].asked < e->config.masterretries
					&& now - e->masters[i].last >= e->config.mastertimeout)
				{
					SL_Send (sl_masterquery, sizeof(sl_masterquery), e->masters[i].address);
					e->masters[i].asked++;
					e->masters[i].last = now;
				}
			// stamped as they go, not as the loop began
			n = SLS_CollectDue (&e->sched, Sys_DoubleTime (), due);
			for (i = 0 ; i < n ; i++)
				if (due[i].query == SLQ_PING)
					SL_Send (sl_pingquery, sizeof(sl_pingquery), e->sched.hosts[due[i].index].e.address);
				else if (due[i].query == SLQ_STATUS)
					SL_Send (sl_statusquery, sizeof(sl_statusquery), e->sched.hosts[due[i].index].e.address);
				else
					SL_Send (sl_tablequery, sizeof(sl_tablequery), e->sched.hosts[due[i].index].e.address);
		}

		if (e->scanning && e->sourcesdone)
		{
			for (i = 0, settled = true ; i < e->nummasters ; i++)
				if (!e->masters[i].answered && (e->masters[i].asked < e->config.masterretries
					|| now - e->masters[i].last < e->config.mastertimeout))
					settled = false;
			if (settled && SLS_Finished (&e->sched, now))
			{
				SLS_Finalize (&e->sched);
				for (i = 0 ; i < e->nummasters ; i++)
					if (!e->masters[i].answered)
					{
						e->status[e->masters[i].source].state = SLSRC_FAILED;
						SL_Message ("Server browser: \"%s\" didn't answer\n", e->sources[e->masters[i].source].name);
					}
				e->scanning = false;
				changed = true;
				SL_ApplyStreams (e);
				SL_SaveTable (e);
			}
		}

		if (changed && now - published >= SL_PUBLISH)
		{
			SL_ApplyStreams (e);
			e->generation++;
			SL_Publish (SL_Snapshot (e));
			changed = false;
			published = now;
		}
		UDP_Wait (sl.sockets, sl.numsockets, e->scanning ? 0.002 : 0.02);
	}

	// what was learned kept
	SL_ApplyStreams (e);
	SL_SaveTable (e);
	if (e->inbox)
	{
		atomic_store (&e->inbox->abandoned, true);
		SL_ReleaseInbox (e->inbox);
	}
	SLS_Free (&e->sched);
	free (e->streams);
	free (buf);
	free (e);
}

/*
==============================================================================

THE MAIN THREAD'S

==============================================================================
*/

bool SL_Start (const char *dir, const char *cachepath, const slsource_t *sources, int numsources)
{
	slengine_t		*e;
	slsnapshot_t	*snap;
	int				i;

	if (sl.thread)
		return true;
	for (sl.numsockets = 0 ; sl.numsockets < SL_SOCKETS ; sl.numsockets++)
		if (!(sl.sockets[sl.numsockets] = UDP_OpenQuiet ()))
			break;
	if (!sl.numsockets || !(e = calloc (1, sizeof(*e))))
	{
		for (i = 0 ; i < sl.numsockets ; i++)
			UDP_Close (sl.sockets[i]);
		sl.numsockets = 0;
		return false;
	}
	if (numsources > SL_MAXSOURCES)
		numsources = SL_MAXSOURCES;
	Q_strncpyz (sl.dir, dir, sizeof(sl.dir));
	Q_strncpyz (sl.cachepath, cachepath, sizeof(sl.cachepath));

	// last time's servers, shown before the thread has run
	memcpy (e->sources, sources, (size_t)numsources * sizeof(*sources));
	e->numsources = numsources;
	SL_DefaultConfig (&e->config);
	SLS_Init (&e->sched, &e->config, SL_Marked (sources, numsources), Sys_DoubleTime ());
	i = SL_LoadCache (cachepath, sources, numsources, &sl.seed);
	SLS_Seed (&e->sched, sl.seed, i);
	sl.seed = NULL;
	e->sourcesdone = true;
	e->generation = 1;
	if ((snap = SL_Snapshot (e)))
		SL_Publish (snap);

	atomic_store (&sl.stop, false);
	atomic_store (&sl.paused, false);
	if (!(sl.thread = Sys_StartThread ("slist", SL_Engine, e)))
	{
		SLS_Free (&e->sched);
		free (e);
		for (i = 0 ; i < sl.numsockets ; i++)
			UDP_Close (sl.sockets[i]);
		sl.numsockets = 0;
		return false;
	}
	return true;
}

bool SL_Running (void)
{
	return sl.thread != NULL;
}

void SL_Refresh (const slsource_t *sources, int numsources, const slconfig_t *c)
{
	slorder_t	*order = malloc (sizeof(*order)), *old;

	if (!order || !sl.thread)
	{
		free (order);
		return;
	}
	if (numsources > SL_MAXSOURCES)
		numsources = SL_MAXSOURCES;
	memcpy (order->sources, sources, (size_t)numsources * sizeof(*sources));
	order->numsources = numsources;
	order->config = *c;
	SL_Lock (&sl.lock);
	old = sl.order;
	sl.order = order;
	SL_Unlock (&sl.lock);
	free (old);
}

void SL_Describe (netadr_t a)
{
	SL_Lock (&sl.lock);
	if (sl.numdescribes < SL_MAXDESCRIBES)
		sl.describes[sl.numdescribes++] = a;
	SL_Unlock (&sl.lock);
}

void SL_Pause (bool paused)
{
	atomic_store (&sl.paused, paused);
}

void SL_Shutdown (void)
{
	int		i;

	if (!sl.thread)
		return;
	atomic_store (&sl.stop, true);
	Sys_JoinThread (sl.thread);
	sl.thread = NULL;
	for (i = 0 ; i < sl.numsockets ; i++)
		UDP_Close (sl.sockets[i]);
	sl.numsockets = sl.nextsocket = 0;
	free (sl.order);
	sl.order = NULL;
	free (sl.published);
	sl.published = NULL;
	sl.numdescribes = sl.nummessages = 0;
}
