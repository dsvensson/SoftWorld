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

#pragma once
// slist_local.h -- the server list's own: the scan's scheduler, and the routes
// through relays. What is here runs on the list's thread (malloc's memory,
// never Mem_*), with the time handed in, so the tests can run it without one.

#include "slist.h"

// the serverinfo of a server that hasn't given one; never freed
extern char	sl_noinfo[1];

// a server's serverinfo set to text (length bytes), its old one freed
void	SL_SetInfo (slserver_t *s, const char *text, size_t length);
// what a server holds freed (its serverinfo and roster), the server left as none
void	SL_ClearServer (slserver_t *s);

/*
==============================================================================

ROUTES (slist_routes.c)

Going the long way round to arrive sooner: the relays (qizmo, qwfwd) forward a
whole connection, and say how far they are from what they reach. We are one
node, our pings the edges out of us, each relay's table the edges out of it;
the shortest paths are the relaxation of every edge until none improves.

==============================================================================
*/

typedef struct
{
	int			host;		// the relay's index
	const int	*peers;		// what it reaches, by index
	const int	*ms;		// and how far
	int			numpeers;
} slrelay_t;

// each host's cost (the direct ping, -1 for none, else through relays where
// shorter) and the relay it is reached by last (-1 directly)
void	SL_Routes (const int *direct, int numhosts, const slrelay_t *relays, int numrelays, int *cost, int *via);

/*
==============================================================================

THE SCHEDULER (slist_sched.c)

What to ask next and when: pings sweep the list (all of it, then all again),
statuses follow the pings answered, a relay's table follows its status; each
lane paced by deadline, so a late wake sends what came due while it slept.

==============================================================================
*/

typedef enum { SLQ_PING, SLQ_STATUS, SLQ_TABLE } slquery_t;

typedef struct
{
	int			index;
	slquery_t	query;
} sldue_t;

#define SL_BURST		32		// a lane's sends a wake at most: a stall a burst, not a flood
#define SL_MAXDUE		(SL_BURST * 3)
#define SL_OUTSTANDING	16		// pings waited for, a host's
#define SL_ROUTEINTERVAL	0.5	// seconds between the routes worked out again

typedef struct
{
	slserver_t	e;
	double		sent[SL_OUTSTANDING];	// when the pings still waited for went, oldest first
	int			numsent;
	char		*pieces;		// the status answer being put together
	int			piecelength, piecemax;
	int			infoattempts, tableattempts;
	bool		infoanswered, infoqueued;
	bool		tableanswered, tablequeued;
	bool		swept;			// in the sweep's order (its sources are scanned)
	slpeer_t	*peers;			// what a relay says it reaches
	int			numpeers;
} slhost_t;

typedef struct
{
	double	interval, next;
} sllane_t;

// a lane of sends held to a rate: the first due an interval in
void	SL_LaneInit (sllane_t *l, int persecond, double now);
// the sends come due by now, up to cap; a lane further behind than that
// writes the debt off rather than sprint after it
int		SL_LaneTake (sllane_t *l, double now, int cap);

// a queue of host indexes, and of the times they are due
typedef struct
{
	int		*index;
	double	*when;
	int		head, count, size;
} slqueue_t;

typedef struct
{
	slhost_t	*hosts;
	int			numhosts, maxhosts;
	int			*hash;			// host index + 1 by address, open addressing
	int			hashsize;
	int			*order;			// the hosts in the order the sweep visits them
	int			numorder;
	int			*fresh;			// found since the order was arranged
	int			numfresh;
	uint64_t	scanmask;		// the sources whose servers are swept (the marked)
	int			cursor, round, rounds;
	sllane_t	ping, info, table;
	slqueue_t	inforeadyq, infowaitq, tablereadyq, tablewaitq;
	double		pingtimeout, infotimeout, tabletimeout, quiet;
	int			inforetries, tableretries;
	double		lastsend;		// -1 none
	bool		routesstale;
	double		routesbuilt;	// -1 never
	int			pingssent;
} slsched_t;

// a scan sweeping the servers of the sources in scanmask: the others are
// listed, never asked
void	SLS_Init (slsched_t *s, const slconfig_t *c, uint64_t scanmask, double now);
void	SLS_Free (slsched_t *s);
// starts from last time's servers: their pings are what's shown until
// measured, and the order they are asked in; servers' contents are taken, the
// array freed
void	SLS_Seed (slsched_t *s, slserver_t *servers, int count);
// a server named by a source (bit its mask; info what a list knew, NULL for
// none): true if it was new. A list's serverinfo fills in a server that has
// none, never overrules one that answered.
bool	SLS_Add (slsched_t *s, netadr_t a, const char *info, size_t infolen, uint64_t bit);
int		SLS_Find (const slsched_t *s, netadr_t a);	// -1 none
// what has come due, recorded as sent; their number
int		SLS_CollectDue (slsched_t *s, double now, sldue_t *due);
// a server answering a ping (at now; epoch the unix time): false for one not
// asked, a second copy of an answer too
bool	SLS_OnPingAck (slsched_t *s, netadr_t a, double now, int64_t epoch);
// an out-of-band print: a relay's table where one was asked, else a status's piece
bool	SLS_OnPrint (slsched_t *s, netadr_t a, const byte *packet, int length, int64_t epoch);
bool	SLS_OnStatus (slsched_t *s, netadr_t a, const byte *payload, int length, int64_t epoch);
// one server asked what it is ahead of the rest (a row the browser shows)
void	SLS_Prioritise (slsched_t *s, netadr_t a);
// nothing left to ask, and the last answer's time past
bool	SLS_Finished (const slsched_t *s, double now);
// what never answered is gone; the routes worked out a last time
void	SLS_Finalize (slsched_t *s);
int		SLS_Round (const slsched_t *s);		// the sweep, from 1
