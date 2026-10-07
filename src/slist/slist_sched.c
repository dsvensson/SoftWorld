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
// slist_sched.c -- what a scan asks next, and when (qualia's scheduler)
//
// Pings sweep: every server once, in order of priority, then the whole list
// again, so after the first sweep everything shown has a number, and a
// server's samples are seconds apart rather than milliseconds. A status
// follows a ping answered, so servers gone for months cost six bytes each and
// no more. Each lane is paced by deadline: woken late (Windows' 16 ms for 6),
// it sends what came due meanwhile and stays on time.

#include "slist_local.h"

#include "q_string.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

char	sl_noinfo[1];

#define SL_MAXPIECES	65536	// a status answer put together, at most

void SL_SetInfo (slserver_t *s, const char *text, size_t length)
{
	char	*info;

	if (length > SL_MAXINFO - 1)
		length = SL_MAXINFO - 1;
	if (!length)
		info = sl_noinfo;
	else if ((info = malloc (length + 1)))
	{
		memcpy (info, text, length);
		info[length] = 0;
	}
	else
		return;		// the old one kept
	if (s->info && s->info != sl_noinfo)
		free (s->info);
	s->info = info;
}

void SL_ClearServer (slserver_t *s)
{
	if (s->info && s->info != sl_noinfo)
		free (s->info);
	free (s->roster);
	s->info = sl_noinfo;
	s->roster = NULL;
	s->numroster = 0;
}

void SL_DefaultConfig (slconfig_t *c)
{
	*c = (slconfig_t){
		.pingrate = 150, .pings = 3, .pingtimeout = 1,
		.inforate = 100, .inforetries = 3, .infotimeout = 1,
		.proxyrate = 10, .proxyretries = 3, .proxytimeout = 1,
		.masterretries = 3, .mastertimeout = 1};
}

/*
==============================================================================

QUEUES

==============================================================================
*/

static bool SLQ_Grow (slqueue_t *q)
{
	int		size = q->size ? q->size * 2 : 64, i;
	int		*index = malloc ((size_t)size * sizeof(*index));
	double	*when = malloc ((size_t)size * sizeof(*when));

	if (!index || !when)
	{
		free (index);
		free (when);
		return false;
	}
	for (i = 0 ; i < q->count ; i++)
	{
		index[i] = q->index[(q->head + i) % q->size];
		when[i] = q->when[(q->head + i) % q->size];
	}
	free (q->index);
	free (q->when);
	q->index = index;
	q->when = when;
	q->head = 0;
	q->size = size;
	return true;
}

static void SLQ_PushBack (slqueue_t *q, int index, double when)
{
	int		at;

	if (q->count == q->size && !SLQ_Grow (q))
		return;
	at = (q->head + q->count) % q->size;
	q->index[at] = index;
	q->when[at] = when;
	q->count++;
}

static void SLQ_PushFront (slqueue_t *q, int index)
{
	if (q->count == q->size && !SLQ_Grow (q))
		return;
	q->head = (q->head + q->size - 1) % q->size;
	q->index[q->head] = index;
	q->when[q->head] = 0;
	q->count++;
}

// the front, taken off; false when there is none
static bool SLQ_PopFront (slqueue_t *q, int *index, double *when)
{
	if (!q->count)
		return false;
	*index = q->index[q->head];
	if (when)
		*when = q->when[q->head];
	q->head = (q->head + 1) % q->size;
	q->count--;
	return true;
}

static void SLQ_Free (slqueue_t *q)
{
	free (q->index);
	free (q->when);
	memset (q, 0, sizeof(*q));
}

/*
==============================================================================

HOSTS BY ADDRESS

==============================================================================
*/

static unsigned SL_HashAdr (netadr_t a)
{
	unsigned	h = 2166136261u;
	int			i;

	for (i = 0 ; i < 16 ; i++)
		h = (h ^ a.ip[i]) * 16777619u;
	h = (h ^ (a.port & 0xff)) * 16777619u;
	return (h ^ (a.port >> 8)) * 16777619u;
}

static bool SL_SameAdr (netadr_t a, netadr_t b)
{
	return a.port == b.port && !memcmp (a.ip, b.ip, sizeof(a.ip));
}

int SLS_Find (const slsched_t *s, netadr_t a)
{
	unsigned	mask, i;

	if (!s->hashsize)
		return -1;
	mask = (unsigned)s->hashsize - 1;
	for (i = SL_HashAdr (a) & mask ; s->hash[i] ; i = (i + 1) & mask)
		if (SL_SameAdr (s->hosts[s->hash[i] - 1].e.address, a))
			return s->hash[i] - 1;
	return -1;
}

static void SLS_HashInsert (slsched_t *s, int index)
{
	unsigned	mask = (unsigned)s->hashsize - 1, i;

	for (i = SL_HashAdr (s->hosts[index].e.address) & mask ; s->hash[i] ; i = (i + 1) & mask)
		;
	s->hash[i] = index + 1;
}

// room for one more host (its hash, its place in the order); false when none
static bool SLS_Reserve (slsched_t *s)
{
	slhost_t	*hosts;
	int			*order, *fresh, *hash, size, i;

	if (s->numhosts == SL_MAXSERVERS)
		return false;
	if (s->numhosts == s->maxhosts)
	{
		size = s->maxhosts ? s->maxhosts * 2 : 256;
		if (size > SL_MAXSERVERS)
			size = SL_MAXSERVERS;
		hosts = realloc (s->hosts, (size_t)size * sizeof(*hosts));
		if (!hosts)
			return false;
		s->hosts = hosts;
		order = realloc (s->order, (size_t)size * sizeof(*order));
		if (!order)
			return false;
		s->order = order;
		fresh = realloc (s->fresh, (size_t)size * sizeof(*fresh));
		if (!fresh)
			return false;
		s->fresh = fresh;
		s->maxhosts = size;
	}
	if ((s->numhosts + 1) * 2 > s->hashsize)
	{
		size = s->hashsize ? s->hashsize * 2 : 512;
		hash = calloc ((size_t)size, sizeof(*hash));
		if (!hash)
			return false;
		free (s->hash);
		s->hash = hash;
		s->hashsize = size;
		for (i = 0 ; i < s->numhosts ; i++)
			SLS_HashInsert (s, i);
	}
	return true;
}

// a host for the server (its contents taken); -1 when there is no room
static int SLS_AddHost (slsched_t *s, const slserver_t *e)
{
	slhost_t	*h;

	if (!SLS_Reserve (s))
		return -1;
	h = &s->hosts[s->numhosts];
	memset (h, 0, sizeof(*h));
	h->e = *e;
	SLS_HashInsert (s, s->numhosts);
	if (h->e.sources & s->scanmask)
	{
		h->swept = true;
		s->fresh[s->numfresh++] = s->numhosts;
	}
	return s->numhosts++;
}

/*
==============================================================================

THE SCAN

==============================================================================
*/

void SL_LaneInit (sllane_t *l, int persecond, double now)
{
	l->interval = 1.0 / (persecond > 0 ? persecond : 1);
	// the first send is due an interval in: at the start nothing is due, and a
	// lane started in debt bursts its cap
	l->next = now + l->interval;
}

// the sends come due, the schedule moved on an interval each; a lane further
// behind than a burst writes the debt off rather than sprint after it
int SL_LaneTake (sllane_t *l, double now, int cap)
{
	int		due = 0;

	while (l->next <= now && due < cap)
	{
		l->next += l->interval;
		due++;
	}
	if (l->next < now)
		l->next = now + l->interval;
	return due;
}

void SLS_Init (slsched_t *s, const slconfig_t *c, uint64_t scanmask, double now)
{
	memset (s, 0, sizeof(*s));
	s->scanmask = scanmask;
	s->rounds = c->pings > 0 ? c->pings : 1;
	SL_LaneInit (&s->ping, c->pingrate, now);
	SL_LaneInit (&s->info, c->inforate, now);
	SL_LaneInit (&s->table, c->proxyrate, now);
	s->pingtimeout = c->pingtimeout;
	s->infotimeout = c->infotimeout;
	s->tabletimeout = c->proxytimeout;
	s->inforetries = c->inforetries > 0 ? c->inforetries : 1;
	s->tableretries = c->proxyretries > 0 ? c->proxyretries : 1;
	// long enough for the last thing asked to have its whole chance
	s->quiet = s->pingtimeout;
	if (s->quiet < s->infotimeout)
		s->quiet = s->infotimeout;
	if (s->quiet < s->tabletimeout)
		s->quiet = s->tabletimeout;
	s->quiet *= 1.2;
	s->lastsend = -1;
	s->routesbuilt = -1;
}

void SLS_Free (slsched_t *s)
{
	int		i;

	for (i = 0 ; i < s->numhosts ; i++)
	{
		SL_ClearServer (&s->hosts[i].e);
		free (s->hosts[i].pieces);
		free (s->hosts[i].peers);
	}
	free (s->hosts);
	free (s->hash);
	free (s->order);
	free (s->fresh);
	SLQ_Free (&s->inforeadyq);
	SLQ_Free (&s->infowaitq);
	SLQ_Free (&s->tablereadyq);
	SLQ_Free (&s->tablewaitq);
	memset (s, 0, sizeof(*s));
}

void SLS_Seed (slsched_t *s, slserver_t *servers, int count)
{
	int		i;

	for (i = 0 ; i < count ; i++)
	{
		servers[i].samples = 0;
		servers[i].state = SL_CACHED;
		if (SLS_Find (s, servers[i].address) >= 0 || SLS_AddHost (s, &servers[i]) < 0)
			SL_ClearServer (&servers[i]);
	}
	free (servers);
}

bool SLS_Add (slsched_t *s, netadr_t a, const char *info, size_t infolen, uint64_t bit)
{
	slserver_t	e;
	int			i = SLS_Find (s, a);

	if (i >= 0)
	{
		s->hosts[i].e.sources |= bit;
		if (info && infolen && !*s->hosts[i].e.info)
			SL_SetInfo (&s->hosts[i].e, info, infolen);
		// named now by a source that is scanned
		if (!s->hosts[i].swept && (s->hosts[i].e.sources & s->scanmask))
		{
			s->hosts[i].swept = true;
			s->fresh[s->numfresh++] = i;
		}
		return false;
	}
	memset (&e, 0, sizeof(e));
	e.address = a;
	e.info = sl_noinfo;
	e.ping = e.routeping = -1;
	e.sources = bit;
	if (info && infolen)
		SL_SetInfo (&e, info, infolen);
	if (SLS_AddHost (s, &e) < 0)
	{
		SL_ClearServer (&e);
		return false;
	}
	return true;
}

/*
==============================================================================

ORDER

Who is asked first: servers that had players before those that merely
answered, before those not heard of yet, before those already gone; within
each, the near before the far

==============================================================================
*/

typedef struct
{
	int		band, ping, index;
} slpriority_t;

static int SL_ComparePriority (const void *a, const void *b)
{
	const slpriority_t	*x = a, *y = b;

	if (x->band != y->band)
		return x->band < y->band ? -1 : 1;
	if (x->ping != y->ping)
		return x->ping < y->ping ? -1 : 1;
	return x->index < y->index ? -1 : x->index > y->index;
}

static void SLS_SortFrom (slsched_t *s, int from)
{
	slpriority_t	*p;
	slserver_t		*e;
	int				n = s->numorder - from, i;

	if (n < 2 || !(p = malloc ((size_t)n * sizeof(*p))))
		return;
	for (i = 0 ; i < n ; i++)
	{
		e = &s->hosts[s->order[from + i]].e;
		p[i].index = s->order[from + i];
		p[i].ping = e->ping >= 0 ? e->ping : INT_MAX;
		if (!e->seen)
			p[i].band = 2;
		else if (e->players > 0)
			p[i].band = 0;
		else if (e->ping >= 0)
			p[i].band = 1;
		else
			p[i].band = 3;
	}
	qsort (p, (size_t)n, sizeof(*p), SL_ComparePriority);
	for (i = 0 ; i < n ; i++)
		s->order[from + i] = p[i].index;
	free (p);
}

// the servers found since filed into the part of the sweep still to come; the
// part gone is left as it was, or reordering it would ping some twice and
// others never
static void SLS_Arrange (slsched_t *s)
{
	int		from;

	if (!s->numfresh)
		return;
	from = s->cursor < s->numorder ? s->cursor : s->numorder;
	memcpy (s->order + s->numorder, s->fresh, (size_t)s->numfresh * sizeof(*s->fresh));
	s->numorder += s->numfresh;
	s->numfresh = 0;
	SLS_SortFrom (s, from);
}

static bool SLS_Sweeping (const slsched_t *s)
{
	// servers found and not yet filed are still to be pinged
	if (s->numfresh)
		return true;
	return s->numorder && (s->cursor < s->numorder || s->round + 1 < s->rounds);
}

static int SLS_NextPing (slsched_t *s)
{
	if (!s->numorder)
		return -1;
	if (s->cursor >= s->numorder)
	{
		if (s->round + 1 >= s->rounds)
			return -1;
		s->round++;
		s->cursor = 0;
		// all measured now: the next sweep goes by what was found, not remembered
		SLS_SortFrom (s, 0);
	}
	return s->order[s->cursor++];
}

/*
==============================================================================

ROUTES

==============================================================================
*/

static void SLS_Routes (slsched_t *s)
{
	slrelay_t	*relays;
	int			*direct, *cost, *via, *peers, *ms, chain[SL_MAXHOPS + 1];
	int			numrelays = 0, numpeers = 0, i, j, k, n, step, p;
	slhost_t	*h;

	for (i = 0 ; i < s->numhosts ; i++)
		if (s->hosts[i].numpeers)
		{
			numrelays++;
			numpeers += s->hosts[i].numpeers;
		}
	if (!numrelays)
		return;
	relays = malloc ((size_t)numrelays * sizeof(*relays));
	direct = malloc ((size_t)s->numhosts * 3 * sizeof(*direct));
	peers = malloc ((size_t)numpeers * 2 * sizeof(*peers));
	if (!relays || !direct || !peers)
	{
		free (relays);
		free (direct);
		free (peers);
		return;
	}
	cost = direct + s->numhosts;
	via = cost + s->numhosts;
	ms = peers + numpeers;

	// the relays' peers by index: those not on the list lead nowhere we look
	for (i = 0, n = 0, k = 0 ; i < s->numhosts ; i++)
	{
		h = &s->hosts[i];
		direct[i] = h->e.ping;
		if (!h->numpeers)
			continue;
		relays[n] = (slrelay_t){.host = i, .peers = peers + k, .ms = ms + k};
		for (j = 0 ; j < h->numpeers ; j++)
			if ((p = SLS_Find (s, h->peers[j].address)) >= 0)
			{
				peers[k] = p;
				ms[k++] = h->peers[j].ms;
				relays[n].numpeers++;
			}
		n++;
	}
	SL_Routes (direct, s->numhosts, relays, numrelays, cost, via);

	for (i = 0 ; i < s->numhosts ; i++)
	{
		h = &s->hosts[i];
		h->e.routeping = -1;
		h->e.numhops = 0;
		if (via[i] < 0)
			continue;
		// the relays back from the server; a pair naming each other would walk on
		for (n = 0, step = via[i] ; step >= 0 && n <= SL_MAXHOPS && n <= numrelays ; step = via[step])
			chain[n++] = step;
		if (step >= 0 || n > SL_MAXHOPS)
			continue;
		h->e.routeping = cost[i];
		h->e.numhops = n;
		for (j = 0 ; j < n ; j++)
			h->e.hops[j] = s->hosts[chain[n - 1 - j]].e.address;
	}
	free (relays);
	free (direct);
	free (peers);
}

/*
==============================================================================

ASKING AND ANSWERS

==============================================================================
*/

int SLS_CollectDue (slsched_t *s, double now, sldue_t *due)
{
	slhost_t	*h;
	double		deadline;
	int			n = 0, count, i;

	SLS_Arrange (s);

	// what wasn't answered back on the ready queues, to the retries: the queues
	// are in send order, the deadlines as long, so the front is the oldest
	while (s->infowaitq.count && s->infowaitq.when[s->infowaitq.head] <= now)
	{
		SLQ_PopFront (&s->infowaitq, &i, &deadline);
		if (!s->hosts[i].infoanswered && s->hosts[i].infoattempts < s->inforetries)
			SLQ_PushBack (&s->inforeadyq, i, 0);
	}
	while (s->tablewaitq.count && s->tablewaitq.when[s->tablewaitq.head] <= now)
	{
		SLQ_PopFront (&s->tablewaitq, &i, &deadline);
		if (!s->hosts[i].tableanswered && s->hosts[i].tableattempts < s->tableretries)
			SLQ_PushBack (&s->tablereadyq, i, 0);
	}

	// the routes again, not as each table comes (they cost the reading of
	// answers the time to work them out)
	if (s->routesstale && (s->routesbuilt < 0 || now - s->routesbuilt >= SL_ROUTEINTERVAL))
	{
		s->routesstale = false;
		s->routesbuilt = now;
		SLS_Routes (s);
	}

	for (count = SL_LaneTake (&s->ping, now, SL_BURST) ; count > 0 ; count--)
	{
		if ((i = SLS_NextPing (s)) < 0)
			break;
		h = &s->hosts[i];
		if (h->numsent == SL_OUTSTANDING)
			memmove (h->sent, h->sent + 1, --h->numsent * sizeof(h->sent[0]));
		h->sent[h->numsent++] = now;
		s->pingssent++;
		s->lastsend = now;
		due[n++] = (sldue_t){i, SLQ_PING};
	}
	for (count = SL_LaneTake (&s->info, now, SL_BURST) ; count > 0 ; count--)
	{
		if (!SLQ_PopFront (&s->inforeadyq, &i, NULL))
			break;
		h = &s->hosts[i];
		h->infoattempts++;
		h->piecelength = 0;		// what was half put together answered the question replaced
		SLQ_PushBack (&s->infowaitq, i, now + s->infotimeout);
		s->lastsend = now;
		due[n++] = (sldue_t){i, SLQ_STATUS};
	}
	for (count = SL_LaneTake (&s->table, now, SL_BURST) ; count > 0 ; count--)
	{
		if (!SLQ_PopFront (&s->tablereadyq, &i, NULL))
			break;
		h = &s->hosts[i];
		h->tableattempts++;
		SLQ_PushBack (&s->tablewaitq, i, now + s->tabletimeout);
		s->lastsend = now;
		due[n++] = (sldue_t){i, SLQ_TABLE};
	}
	return n;
}

bool SLS_OnPingAck (slsched_t *s, netadr_t a, double now, int64_t epoch)
{
	slhost_t	*h;
	int			i = SLS_Find (s, a), ms;

	if (i < 0)
		return false;
	h = &s->hosts[i];

	// the pings older than the timeout aren't waited for: an answer timed
	// against one would invent a latency out of the gap
	while (h->numsent && now - h->sent[0] > s->pingtimeout)
		memmove (h->sent, h->sent + 1, --h->numsent * sizeof(h->sent[0]));
	// each answer takes one ping: a second copy, or one not asked for, has none
	if (!h->numsent)
		return false;
	ms = (int)((now - h->sent[0]) * 1000 + 0.5);
	memmove (h->sent, h->sent + 1, --h->numsent * sizeof(h->sent[0]));
	if (ms < 0)
		ms = 0;

	// a delay only adds to a round trip, so this scan's best sample is the
	// truest; a remembered one is replaced, not competed with
	h->e.ping = h->e.samples > 0 && h->e.ping >= 0 && h->e.ping < ms ? h->e.ping : ms;
	if (h->e.samples < 255)
		h->e.samples++;
	h->e.state = SL_ALIVE;
	h->e.seen = epoch;
	// a route was worked out against the ping before
	h->e.routeping = -1;
	h->e.numhops = 0;

	// there, so worth the dear question
	if (!h->infoqueued)
	{
		h->infoqueued = true;
		SLQ_PushBack (&s->inforeadyq, i, 0);
	}
	return true;
}

bool SLS_OnPrint (slsched_t *s, netadr_t a, const byte *packet, int length, int64_t epoch)
{
	slhost_t	*h;
	slpeer_t	*peers;
	const byte	*payload;
	int			i = SLS_Find (s, a), n, payloadlength;

	if (i < 0)
		return false;
	h = &s->hosts[i];

	// a table and a status share the type byte: only a relay asked for its
	// table (sent, not queued: a status's later pieces needn't look like text)
	// has its print read as one, and it has to be whole records
	if (h->tableattempts && !h->tableanswered && (peers = malloc ((size_t)(length / 8 + 1) * sizeof(*peers))))
	{
		if ((n = SL_ParseTable (packet, length, peers, length / 8 + 1)) >= 0)
		{
			free (h->peers);
			h->peers = peers;
			h->numpeers = n;
			h->tableanswered = true;
			s->routesstale = true;	// worked out on the loop, not in the reading
			return true;
		}
		free (peers);
	}

	if (!(payload = SL_PrintPayload (packet, length, &payloadlength)))
		return false;
	return SLS_OnStatus (s, a, payload, payloadlength, epoch);
}

bool SLS_OnStatus (slsched_t *s, netadr_t a, const byte *payload, int length, int64_t epoch)
{
	slhost_t	*h;
	slplayer_t	roster[SL_MAXPLAYERS], *copy;
	char		info[SL_MAXINFO], version[16], *pieces;
	int			i = SLS_Find (s, a), count, size;

	if (i < 0)
		return false;
	h = &s->hosts[i];

	// a serverinfo begins an answer, anything else goes on with one; read
	// again whole as each piece comes, as nothing says how many there are
	if (length > 0 && payload[0] == '\\')
		h->piecelength = 0;
	if (h->piecelength + length > h->piecemax && h->piecelength + length <= SL_MAXPIECES)
	{
		size = h->piecelength + length > 2048 ? (h->piecelength + length) * 2 : 2048;
		if ((pieces = realloc (h->pieces, (size_t)size)))
		{
			h->pieces = pieces;
			h->piecemax = size;
		}
	}
	if (h->piecelength + length <= h->piecemax)
	{
		memcpy (h->pieces + h->piecelength, payload, (size_t)length);
		h->piecelength += length;
	}
	h->infoanswered = true;

	SL_ParseStatus (h->pieces, (size_t)h->piecelength, info, sizeof(info), roster, SL_MAXPLAYERS, &count);
	SL_SetInfo (&h->e, info, strlen (info));
	copy = count ? malloc ((size_t)count * sizeof(*copy)) : NULL;
	if (copy || !count)
	{
		if (count)
			memcpy (copy, roster, (size_t)count * sizeof(*copy));
		free (h->e.roster);
		h->e.roster = copy;
		h->e.numroster = count;
	}
	// counted off the roster, which can't be inflated as a serverinfo's can
	h->e.players = h->e.spectators = 0;
	for (i = 0 ; i < h->e.numroster ; i++)
		if (h->e.roster[i].spectator)
			h->e.spectators++;
		else
			h->e.players++;
	SL_InfoValue (h->e.info, "*version", version, sizeof(version));
	if (!Q_strncasecmp (version, "qwfwd", 5))
		h->e.proxy = SL_QWFWD;
	else if (!Q_strncasecmp (version, "qizmo", 5))
		h->e.proxy = SL_QIZMO;
	else
		h->e.proxy = SL_NOPROXY;
	h->e.state = SL_ALIVE;
	h->e.seen = epoch;

	// a relay is worth one question more: what it reaches, and how far
	if (h->e.proxy != SL_NOPROXY && !h->tablequeued)
	{
		h->tablequeued = true;
		SLQ_PushBack (&s->tablereadyq, (int)(h - s->hosts), 0);
	}
	return true;
}

void SLS_Prioritise (slsched_t *s, netadr_t a)
{
	int		i = SLS_Find (s, a);

	if (i < 0)
		return;
	s->hosts[i].infoattempts = 0;
	s->hosts[i].infoanswered = false;
	s->hosts[i].infoqueued = true;
	SLQ_PushFront (&s->inforeadyq, i);
}

bool SLS_Finished (const slsched_t *s, double now)
{
	if (SLS_Sweeping (s) || s->inforeadyq.count || s->infowaitq.count || s->tablereadyq.count || s->tablewaitq.count)
		return false;
	return s->lastsend < 0 || now - s->lastsend > s->quiet;
}

void SLS_Finalize (slsched_t *s)
{
	int		i;

	for (i = 0 ; i < s->numhosts ; i++)
		if (s->hosts[i].e.state != SL_ALIVE)
			s->hosts[i].e.state = SL_DEAD;
	// the last pings may have moved what a route is weighed against; worked out
	// now, as nothing waits on the loop any more
	s->routesstale = false;
	SLS_Routes (s);
}

int SLS_Round (const slsched_t *s)
{
	return s->round + 1;
}
