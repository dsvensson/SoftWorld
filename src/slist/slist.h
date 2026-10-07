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
// slist.h -- the server list: QuakeWorld's servers found through masters, lists
// and files, pinged and asked what they are, as qualia's indexer finds them.
// The menu's browser reads it through the client's hostcache builtins
// (cl_slist.c).
//
// Cheap questions gate dear ones: a master names a thousand servers in a
// datagram, a ping is six bytes out and one back, and a status (kilobytes, in
// pieces) is asked only of a server that answered a ping. Last time's list
// (the cache) is shown at once and checked in place: the first sweep of pings
// measures what was remembered, later sweeps keep the best sample, and the
// statuses fill in who plays.

#include "net.h"
#include "q_types.h"

#include <stddef.h>
#include <stdint.h>

#define SL_MAXSOURCES	64		// a server's sources are a mask of them
#define SL_MAXPLAYERS	64		// of a roster, kept
#define SL_MAXHOPS		4		// relays on a route
#define SL_MAXSERVERS	4096	// more than QuakeWorld has had at once, by far
#define SL_MAXINFO		2048	// a serverinfo, kept

#define SL_MASTERPORT	27000	// what an address without a port is: a master's,
#define SL_SERVERPORT	27500	// or a server's

/*
==============================================================================

SOURCES (slist_sources.c)

==============================================================================
*/

typedef enum { SL_MASTER, SL_FILE, SL_URL, SL_SERVER } slsourcetype_t;

typedef struct
{
	slsourcetype_t	type;
	char	name[64];
	char	location[256];	// a master's or server's host:port (the port filled in), a file's
							// path (from the sources file's directory), a URL
	bool	marked;			// scanned, and its servers listed
} slsource_t;

// ezQuake's sources.txt: lines of master, file, url (and qualia's server),
// "name" and location; comments begin with / or #, and a line that can't be
// read costs only itself. The sources read, up to max, unmarked.
int		SL_ParseSources (const char *text, slsource_t *sources, int max);
// the same, as ezQuake writes it; the length it needs (snprintf's)
size_t	SL_FormatSources (const slsource_t *sources, int count, char *buf, size_t size);
// where there is no sources.txt: the masters everyone asks
int		SL_DefaultSources (slsource_t *sources, int max);

// the marks: the marked sources' names, one quoted per line. Without a file
// (text NULL), ezQuake's default ones, else every one.
void	SL_ParseMarks (const char *text, slsource_t *sources, int count);
size_t	SL_FormatMarks (const slsource_t *sources, int count, char *buf, size_t size);

// a list's lines (a file's or a URL's): an address, a name or numeric, and
// what the list knows of the server, a serverinfo after it (infolen 0 for none)
void	SL_ParseListing (const char *data, size_t length,
	void (*entry) (void *ctx, const char *address, const char *info, size_t infolen), void *ctx);

// an address with the port it left out, IPv6's put in [ ]
void	SL_WithPort (const char *address, int port, char *out, size_t size);
// host and port apart (the port defaultport where it has none); false if it
// can't be read
bool	SL_SplitAddress (const char *address, int defaultport, char *host, size_t size, int *port);
// a numeric address, with its port or defaultport; false for a name
bool	SL_ParseAddress (const char *address, int defaultport, netadr_t *a);
// an IP address's text as NET_AdrToString has it, in buf (64 bytes at least),
// from any thread
char	*SL_AdrToBuf (netadr_t a, char *buf, size_t size);

/*
==============================================================================

SERVERS

==============================================================================
*/

typedef enum { SL_CACHED, SL_ALIVE, SL_DEAD } slstate_t;	// last time's, answered, never did
typedef enum { SL_NOPROXY, SL_QIZMO, SL_QWFWD } slproxy_t;	// a relay that forwards connections

typedef struct
{
	int		userid, frags;
	int		time;			// minutes on it
	int		ping;			// ms, never negative (a spectator's comes so)
	int		topcolor, bottomcolor;
	bool	spectator;
	char	name[32], skin[32], team[16];	// Quake's characters
} slplayer_t;

typedef struct
{
	netadr_t	address;
	char		*info;		// its serverinfo as it gave it, \key\value...; "" until it has
	slplayer_t	*roster;	// who is on it, as it listed them
	int			numroster;
	int			players, spectators;	// counted off the roster (the cache's without one)
	int			ping;		// ms, the best of this scan's samples (the cache's before one); -1 none
	int			samples;	// this scan's
	int			routeping;	// ms through relays, where that is shorter; -1 none
	int			numhops;
	netadr_t	hops[SL_MAXHOPS];	// those relays, nearest first
	slproxy_t	proxy;
	slstate_t	state;
	int64_t		seen;		// unix seconds when it last answered; 0 never
	uint64_t	sources;	// the sources that named it, by their index
} slserver_t;

// a serverinfo's value of key, in value; "" when it has none
const char	*SL_InfoValue (const char *info, const char *key, char *value, size_t size);
int		SL_InfoInt (const char *info, const char *key);
// the ping to show and sort by: through relays where that is shorter; -1 none
int		SL_Ping (const slserver_t *s);
bool	SL_Relayed (const slserver_t *s);		// whether SL_Ping is the relays'
// how many can play on it, 0 where it doesn't say
int		SL_MaxClients (const slserver_t *s);

// how a scan asks: ezQuake's sb_* (SL_DefaultConfig their defaults)
typedef struct
{
	int		pingrate;		// a second
	int		pings;			// sweeps of the list
	double	pingtimeout;	// seconds a ping is waited for
	int		inforate, inforetries;
	double	infotimeout;
	int		proxyrate, proxyretries;
	double	proxytimeout;
	int		masterretries;
	double	mastertimeout;
} slconfig_t;

void	SL_DefaultConfig (slconfig_t *c);

/*
==============================================================================

PACKETS (slist_proto.c)

==============================================================================
*/

extern const byte	sl_masterquery[3];		// c \n \0: a master's question, its NUL too
extern const byte	sl_pingquery[6];		// ff ff ff ff k \n
extern const byte	sl_statusquery[14];		// ff ff ff ff status 23 \n: info, players, spectators, teams
extern const byte	sl_tablequery[14];		// ff ff ff ff pingstatus: a relay's pings

// a master's reply: each IPv4 server it names to server, until a port 0 or the
// end; the servers it named, -1 when it isn't one
int		SL_ParseMasterReply (const byte *data, int length, void (*server) (void *ctx, netadr_t a), void *ctx);
// a server answering a ping, the type byte alone or out of band
bool	SL_IsPingAck (const byte *data, int length);
// an out-of-band print's text (a status's, or a relay's table): NULL when it
// isn't one
const byte	*SL_PrintPayload (const byte *data, int length, int *payloadlength);

typedef struct
{
	netadr_t	address;
	int			ms;
} slpeer_t;

// a relay's table: what it reaches and how far, 8-byte records (little-endian
// port and ms, unreachable ones left out); the peers, -1 when it isn't one
int		SL_ParseTable (const byte *data, int length, slpeer_t *peers, int max);

// a status's text, its pieces put together: the serverinfo (its first line)
// into info, and up to max players
void	SL_ParseStatus (const char *text, size_t length, char *info, size_t infosize,
	slplayer_t *roster, int max, int *count);

/*
==============================================================================

THE CACHE (slist_cache.c)

==============================================================================
*/

// last time's servers, as qualia keeps them ("qualia-slist 1"): shown before a
// packet is sent, marked SL_CACHED. Their sources are matched by name to
// sources'. A malloc'd array (SL_FreeServers), NULL with 0 where there is none.
int		SL_LoadCache (const char *path, const slsource_t *sources, int numsources, slserver_t **servers);
// written beside the old one and moved over it; false if it couldn't be
bool	SL_SaveCache (const char *path, const slserver_t *servers, int count,
	const slsource_t *sources, int numsources);
void	SL_FreeServers (slserver_t *servers, int count);

/*
==============================================================================

THE VIEW (slist_view.c)

What the browser shows of a list: masks and sort keys as FTE's hostcache has
them, over the servers of the marked sources

==============================================================================
*/

// the keys: FTE's, then SoftWorld's, then the players' and the serverinfo's
enum
{
	SLK_PING, SLK_MAP, SLK_NAME, SLK_ADDRESS, SLK_NUMPLAYERS, SLK_MAXPLAYERS, SLK_GAMEDIR,
	SLK_FREEPLAYERS, SLK_BASEGAME, SLK_FLAGS, SLK_TIMELIMIT, SLK_FRAGLIMIT,
	SLK_MOD, SLK_PROTOCOL, SLK_NUMBOTS, SLK_NUMSPECTATORS, SLK_NUMHUMANS, SLK_QCSTATUS, SLK_CATEGORY,
	SLK_ISFAVORITE, SLK_ISLOCAL, SLK_ISPROXY, SLK_SERVERINFO,
	SLK_STATE,			// SoftWorld's: slstate_t
	SLK_SEEN,			// when it last answered, unix seconds
	SLK_DIRECTPING,		// the ping not through relays
	SLK_HOPS,			// the relays on the way
	SLK_SAMPLES,		// this scan's pings
	SLK_TOOMANY,		// a custom key past the last there is room for
	SLK_PLAYER0 = 32,
	SLK_CUSTOM = SLK_PLAYER0 + SL_MAXPLAYERS
};

// FTE's comparisons
typedef enum
{
	SLT_CONTAINS, SLT_NOTCONTAIN, SLT_LESSEQUAL, SLT_LESS, SLT_EQUAL, SLT_GREATER, SLT_GREATEREQUAL,
	SLT_NOTEQUAL, SLT_STARTSWITH, SLT_NOTSTARTSWITH
} sltest_t;

// FTE's sort flags, and SoftWorld's
#define SL_SORT_DESCENDING	1
#define SL_SORT_FAVOURITES	2	// no favourites here
#define SL_SORT_CATEGORIES	4	// nor categories
#define SL_SORT_APPEND		8	// a key after the ones before, rather than in their place

#define SL_MAXRULES		32
#define SL_MAXSORT		8
#define SL_MAXCUSTOM	32

typedef struct
{
	int			key;
	sltest_t	test;
	bool		or;
	char		string[64];
	double		number;
} slrule_t;

typedef struct
{
	slrule_t	rules[SL_MAXRULES];
	int			numrules;
	struct
	{
		int		key;
		bool	descending;
	} sort[SL_MAXSORT];
	int			numsort;
	char		custom[SL_MAXCUSTOM][64];	// the serverinfo keys asked for by name
	int			numcustom;
	int			*shown;		// SL_Arrange's: indexes of the servers, in order
	int			numshown, maxshown;
} slview_t;

void	SL_FreeView (slview_t *v);
// a key's number by FTE's names (and SoftWorld's); any other name is a
// serverinfo key's, from SLK_CUSTOM (SLK_TOOMANY past the room)
int		SL_KeyForName (slview_t *v, const char *name);
void	SL_ClearRules (slview_t *v);
// a mask, as FTE's sethostcachemask*: a string's number is atof's, a
// number's string empty
void	SL_AddRule (slview_t *v, bool or, int key, sltest_t test, const char *string, double number);
void	SL_SetSort (slview_t *v, int key, int flags);
// what a key is for a server: as a number (a ping's none 0xffff, a string's
// atof), and as text in buf
double	SL_KeyNumber (const slview_t *v, const slserver_t *s, int key);
const char	*SL_KeyString (const slview_t *v, const slserver_t *s, int key, char *buf, size_t size);
// the servers named by one of the sources (a mask of them by index) that pass
// the masks, as FTE has them (each in order: AND, or OR), sorted by the keys
// and then by address, into shown; their number
int		SL_Arrange (slview_t *v, const slserver_t *servers, int count, uint64_t sources);
