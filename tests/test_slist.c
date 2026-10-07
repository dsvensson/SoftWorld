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
// test_slist.c -- the server list's parts without a network, qualia's tests
// ported: the packets, the sources file and lists, the cache, the scheduler's
// order, gates, retries and lanes (with the time handed in), the routes
// through relays, and the view's masks and sort

#include "host.h"
#include "q_endian.h"
#include "q_string.h"
#include "slist_local.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;

hoststate_t	host;		// the netchan's clock, which sw_net brings

#define CHECK(x)	do { if (!(x)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

static netadr_t Adr (const char *s)
{
	netadr_t	a = {0};

	if (!SL_ParseAddress (s, SL_SERVERPORT, &a))
		printf ("FAIL no address: %s\n", s), failures++;
	return a;
}

static bool SameAdr (netadr_t a, netadr_t b)
{
	return a.port == b.port && !memcmp (a.ip, b.ip, sizeof(a.ip));
}

// ff ff ff ff, a type byte and a body
static int OOB (byte *out, char type, const void *body, int length)
{
	memcpy (out, "\xff\xff\xff\xff", 4);
	out[4] = (byte)type;
	memcpy (out + 5, body, (size_t)length);
	return 5 + length;
}

/*
==============================================================================

PACKETS

==============================================================================
*/

static void CountServer (void *ctx, netadr_t a)
{
	netadr_t	*got = ctx;
	int			i;

	for (i = 0 ; got[i].type ; i++)
		;
	got[i] = a;
}

static void TestMaster (void)
{
	// two servers, a third cut short, after the padding's zero port
	static const byte	reply[] = {0xff, 0xff, 0xff, 0xff, 'd', '\n',
		10, 0, 0, 1, 0x6b, 0x6c, 10, 0, 0, 2, 0x75, 0x30, 0, 0, 0, 0, 0, 0, 10, 0, 0, 3, 0x6b};
	netadr_t	got[8] = {0};

	CHECK (SL_ParseMasterReply (reply, sizeof(reply), CountServer, got) == 2);
	CHECK (SameAdr (got[0], Adr ("10.0.0.1:27500")));
	CHECK (SameAdr (got[1], Adr ("10.0.0.2:30000")));
	CHECK (SL_ParseMasterReply ((const byte *)"\xff\xff\xff\xffn\\x", 7, CountServer, got) == -1);
	CHECK (sizeof(sl_masterquery) == 3 && !memcmp (sl_masterquery, "c\n", 3));
}

// both of a ping's answers count: the bare byte, and out of band
static void TestPingAck (void)
{
	CHECK (SL_IsPingAck ((const byte *)"l", 1));
	CHECK (SL_IsPingAck ((const byte *)"\xff\xff\xff\xffl", 5));
	CHECK (!SL_IsPingAck ((const byte *)"\xff\xff\xff\xffn", 5));
	CHECK (!SL_IsPingAck ((const byte *)"", 0));
}

// a table's port is little-endian, the other way round from a master's
static void TestTable (void)
{
	byte		packet[64];
	byte		records[] = {10, 0, 0, 1, 0x6c, 0x6b, 30, 0, 10, 0, 0, 2, 0x6c, 0x6b, 0xff, 0xff};
	slpeer_t	peers[4];
	int			length = OOB (packet, 'n', records, sizeof(records));

	CHECK (SL_ParseTable (packet, length, peers, 4) == 1);
	CHECK (SameAdr (peers[0].address, Adr ("10.0.0.1:27500")) && peers[0].ms == 30);
	length = OOB (packet, 'n', "\\hostname\\a relay", 17);
	CHECK (SL_ParseTable (packet, length, peers, 4) == -1);
	length = OOB (packet, 'n', records, 7);
	CHECK (SL_ParseTable (packet, length, peers, 4) == -1);
}

static void TestStatus (void)
{
	const char	*text = "\\hostname\\x\\maxclients\\16\n"
		"1 12 5 27 \"player\" \"base\" 4 4 \"red\"\n"
		"2 -9999 3 -40 \"\\s\\watcher\" \"\" 0 0 \"\"\n"
		"3 -9999 3 -40 \"old(s)\" \"\" 0 0\n"
		"4 0 0 20 \"hairdo(s)\" \"\" 0 0 \"\"\n"
		"5 30 5 7 \"ElonMusk\" \"base\" 7 8 \"\" \"b\"\n"
		"6 10 4 4 \"human\" \"base\" 5 9 \"blue\" \"h\"\n"
		"garbage\n";
	char		info[256];
	slplayer_t	roster[8];
	int			count;

	SL_ParseStatus (text, strlen (text) + 1, info, sizeof(info), roster, 8, &count);
	CHECK (!strcmp (info, "\\hostname\\x\\maxclients\\16"));
	CHECK (count == 6);
	// mvdsv's client type, where it gives one: bots told from people
	CHECK (roster[4].bot && !strcmp (roster[4].name, "ElonMusk") && roster[4].frags == 30);
	CHECK (!roster[5].bot && !strcmp (roster[5].team, "blue") && !roster[0].bot);
	CHECK (!strcmp (roster[0].name, "player") && !roster[0].spectator && roster[0].frags == 12);
	CHECK (!strcmp (roster[0].team, "red") && roster[0].ping == 27);
	CHECK (!strcmp (roster[1].name, "watcher") && roster[1].spectator && roster[1].ping == 40);
	CHECK (!strcmp (roster[2].name, "old") && roster[2].spectator && !roster[2].team[0]);
	CHECK (!strcmp (roster[3].name, "hairdo(s)") && !roster[3].spectator);
}

static void TestInfo (void)
{
	const char	*info = "\\hostname\\Fool's DM6\\map\\dm6\\mapx\\no\\maxclients\\8";
	char		value[64];

	CHECK (!strcmp (SL_InfoValue (info, "hostname", value, sizeof(value)), "Fool's DM6"));
	CHECK (!strcmp (SL_InfoValue (info, "map", value, sizeof(value)), "dm6"));
	CHECK (!strcmp (SL_InfoValue (info, "maxclients", value, sizeof(value)), "8"));
	CHECK (!*SL_InfoValue (info, "ma", value, sizeof(value)));
	CHECK (!*SL_InfoValue ("", "map", value, sizeof(value)));
	CHECK (SL_InfoInt (info, "maxclients") == 8);
}

/*
==============================================================================

SOURCES

==============================================================================
*/

static void TestSources (void)
{
	slsource_t	s[8], back[8];
	char		text[1024];
	int			n;

	n = SL_ParseSources ("/ a comment\n\nmaster \"QuakeServers.net\" master.quakeservers.net:27000\n"
		"file \"Capture The Flag\" ctf.txt\nurl \"Global\" http://www.quakeservers.net/lists/servers/global.txt\n",
		s, 8);
	CHECK (n == 3);
	CHECK (s[0].type == SL_MASTER && !strcmp (s[0].name, "QuakeServers.net"));
	CHECK (!strcmp (s[0].location, "master.quakeservers.net:27000"));
	CHECK (s[1].type == SL_FILE && !strcmp (s[1].location, "ctf.txt"));
	CHECK (s[2].type == SL_URL && !strcmp (s[2].name, "Global"));

	// the columns' ports differ
	n = SL_ParseSources ("master \"M\" qwmaster.example.net\nserver \"S\" play.example.net\n", s, 8);
	CHECK (n == 2 && !strcmp (s[0].location, "qwmaster.example.net:27000"));
	CHECK (!strcmp (s[1].location, "play.example.net:27500"));
	n = SL_ParseSources ("master \"M\" 1.2.3.4:30000\nserver \"S\" [2a02:1234::7]:27502\nserver \"V6\" 2a02:1234::7\n",
		s, 8);
	CHECK (n == 3 && !strcmp (s[0].location, "1.2.3.4:30000") && !strcmp (s[1].location, "[2a02:1234::7]:27502"));
	CHECK (!strcmp (s[2].location, "[2a02:1234::7]:27500"));

	// a broken line costs itself; a kind not known isn't guessed at
	n = SL_ParseSources ("master Fodquake qwmaster.fodquake.net\nmaster \"no location\"\nrss \"x\" y\nFILE \"F\" a.txt\n",
		s, 8);
	CHECK (n == 2 && !strcmp (s[0].name, "Fodquake") && s[1].type == SL_FILE);

	// written back as read
	n = SL_ParseSources ("master \"A b\" a.example:27000\nurl \"U\" https://x.example/l.txt\nserver \"S\" 1.2.3.4:27501\n",
		s, 8);
	CHECK (SL_FormatSources (s, n, text, sizeof(text)) < sizeof(text));
	CHECK (SL_ParseSources (text, back, 8) == 3 && !memcmp (s, back, sizeof(s[0]) * 3));
	CHECK (SL_FormatSources (s, n, text, 4) == strlen ("master \"A b\" a.example:27000\nurl \"U\" "
		"https://x.example/l.txt\nserver \"S\" 1.2.3.4:27501\n"));
}

static void TestMarks (void)
{
	slsource_t	s[4];
	char		text[256];
	int			n = SL_ParseSources ("master \"QuakeServers.net\" a\nmaster \"Other\" b\nfile \"Global\" g.txt\n", s, 4);

	// no marks file: ezQuake's default ones
	SL_ParseMarks (NULL, s, n);
	CHECK (s[0].marked && !s[1].marked && s[2].marked);
	// none of those: every one
	n = SL_ParseSources ("master \"X\" a\nmaster \"Y\" b\n", s, 4);
	SL_ParseMarks (NULL, s, n);
	CHECK (s[0].marked && s[1].marked);
	s[0].marked = false;
	SL_FormatMarks (s, n, text, sizeof(text));
	CHECK (!strcmp (text, "\"Y\"\n"));
	SL_ParseMarks ("\"X\"\n", s, n);
	CHECK (s[0].marked && !s[1].marked);
}

typedef struct
{
	int		count;
	char	address[4][128];
	char	info[4][64];
} listing_t;

static void ListEntry (void *ctx, const char *address, const char *info, size_t infolen)
{
	listing_t	*l = ctx;

	if (l->count == 4)
		return;
	Q_strncpyz (l->address[l->count], address, sizeof(l->address[0]));
	l->info[l->count][0] = 0;
	if (info)
		Q_strncpyz (l->info[l->count], info, infolen + 1 < sizeof(l->info[0]) ? infolen + 1 : sizeof(l->info[0]));
	l->count++;
}

static void TestListing (void)
{
	const char	*list = "144.24.188.96:28501\n89.66.241.225:30000\r\n\n# a comment\n[2a02::7]:27500\n"
		"1.2.3.4:27500 \\hostname\\Somewhere\\map\\dm6\r\n";
	listing_t	l = {0};

	SL_ParseListing (list, strlen (list), ListEntry, &l);
	CHECK (l.count == 4);
	CHECK (!strcmp (l.address[0], "144.24.188.96:28501") && !l.info[0][0]);
	CHECK (!strcmp (l.address[1], "89.66.241.225:30000"));
	CHECK (!strcmp (l.address[2], "[2a02::7]:27500"));
	CHECK (!strcmp (l.info[3], "\\hostname\\Somewhere\\map\\dm6"));
}

typedef struct
{
	int			count;
	netadr_t	server[4];
	char		stream[4][SL_MAXSTREAM];
} streams_t;

static void StreamEntry (void *ctx, netadr_t server, const char *stream)
{
	streams_t	*s = ctx;

	if (s->count == 4)
		return;
	s->server[s->count] = server;
	Q_strncpyz (s->stream[s->count], stream, sizeof(s->stream[0]));
	s->count++;
}

// the QTV list as qtvapi.quakeworld.nu has it: each game's stream, as
// qtvplay takes it, by its server's address, the players' objects within
// passed by; a link without a stream, and what isn't JSON, given nothing
static void TestQTVList (void)
{
	const char	*json = "{\"Servers\":[{\"GameStates\":[{\"Hostname\":\"100.36.32.147\",\"IpAddress\":\"100.36.32.147\","
		"\"Port\":27500,\"Link\":\"http://nicotinelounge.com:28000/watch.qtv?sid=1\",\"Players\":[{\"Name\":\"a\\\"b\","
		"\"Port\":1}]},{\"IpAddress\":\"10.0.0.2\",\"Port\":27501,\"Link\":\"http://qtv.example/\"},"
		"{\"IpAddress\":\"10.0.0.3\",\"Port\":28501,\"Link\":\"7@qtv.example:28000\"}]}],\"ServerCount\":3}";
	char		deep[200];
	streams_t	s = {0};
	int			i;

	CHECK (SL_ParseQTVList (json, strlen (json), StreamEntry, &s) == 2);
	CHECK (s.count == 2 && SameAdr (s.server[0], Adr ("100.36.32.147:27500")));
	CHECK (!strcmp (s.stream[0], "1@nicotinelounge.com:28000"));
	CHECK (SameAdr (s.server[1], Adr ("10.0.0.3:28501")) && !strcmp (s.stream[1], "7@qtv.example:28000"));
	CHECK (SL_ParseQTVList ("<html>", 6, StreamEntry, &s) == -1);
	CHECK (SL_ParseQTVList ("{\"a\":[1,2", 9, StreamEntry, &s) == -1);
	// nested past reason: refused, not followed down
	for (i = 0 ; i < (int)sizeof(deep) - 1 ; i++)
		deep[i] = i < 100 ? '[' : ']';
	deep[0] = '{';
	deep[sizeof(deep) - 1] = 0;
	CHECK (SL_ParseQTVList (deep, strlen (deep), StreamEntry, &s) == -1);
}

static void TestAddresses (void)
{
	char		name[64], out[64];
	int			port;
	netadr_t	a;

	CHECK (SL_SplitAddress ("example.net", 27000, name, sizeof(name), &port) && !strcmp (name, "example.net")
		&& port == 27000);
	CHECK (SL_SplitAddress ("[::1]:30000", 27000, name, sizeof(name), &port) && !strcmp (name, "::1") && port == 30000);
	CHECK (SL_SplitAddress ("2a02::7", 27500, name, sizeof(name), &port) && !strcmp (name, "2a02::7") && port == 27500);
	CHECK (!SL_SplitAddress ("x:99999", 27500, name, sizeof(name), &port));
	CHECK (SL_ParseAddress ("10.0.0.1", 27500, &a) && BigShort ((short)a.port) == 27500 && NET_IsIPv4 (a));
	CHECK (!SL_ParseAddress ("example.net:27500", 27500, &a));
	SL_WithPort ("example.net", 27000, out, sizeof(out));
	CHECK (!strcmp (out, "example.net:27000"));
}

/*
==============================================================================

THE CACHE

==============================================================================
*/

#define CACHEPATH	"test_slist.cache"

static slserver_t Server (const char *address, int ping, const char *info)
{
	slserver_t	s = {.address = Adr (address), .info = sl_noinfo, .ping = ping, .routeping = -1, .seen = 1754450123};

	if (info)
		SL_SetInfo (&s, info, strlen (info));
	return s;
}

static void TestCache (void)
{
	slsource_t	sources[2] = {{SL_MASTER, "One", "a:27000", true}, {SL_MASTER, "Two", "b:27000", true}};
	slsource_t	renamed[2] = {{SL_MASTER, "Two", "b:27000", true}, {SL_MASTER, "New", "c:27000", true}};
	slserver_t	servers[3], *back;
	char		brown[] = "\\hostname\\\xce\xe9\xee";
	FILE		*f;
	int			n;

	servers[0] = Server ("81.66.1.102:27500", 34, "\\hostname\\Fool's DM6\\map\\dm6");
	servers[0].players = 1;
	servers[0].spectators = 1;
	servers[0].sources = 3;
	Q_strncpyz (servers[0].qtv, "3@qtv.example:28000", sizeof(servers[0].qtv));
	servers[1] = Server ("[2a02:1234::7]:27502", 51, brown);
	servers[1].sources = 2;
	servers[2] = (slserver_t){.address = Adr ("10.0.0.9:27500"), .info = sl_noinfo, .ping = -1, .routeping = -1};

	CHECK (SL_SaveCache (CACHEPATH, servers, 3, sources, 2));
	n = SL_LoadCache (CACHEPATH, sources, 2, &back);
	CHECK (n == 3);
	if (n == 3)
	{
		CHECK (SameAdr (back[0].address, servers[0].address) && back[0].ping == 34 && back[0].seen == 1754450123);
		CHECK (back[0].players == 1 && back[0].spectators == 1 && back[0].sources == 3);
		CHECK (!strcmp (back[0].info, servers[0].info));
		CHECK (back[0].state == SL_CACHED && !back[0].samples && !back[0].numroster);
		CHECK (!strcmp (back[0].qtv, "3@qtv.example:28000") && !back[1].qtv[0]);
		CHECK (SameAdr (back[1].address, servers[1].address) && !strcmp (back[1].info, brown));
		CHECK (back[2].ping == -1 && !back[2].seen && !*back[2].info);
		SL_FreeServers (back, n);
	}

	// sources matched by name: "One" is gone, "Two" moved to 0
	n = SL_LoadCache (CACHEPATH, renamed, 2, &back);
	CHECK (n == 3 && back[0].sources == 1 && back[1].sources == 1);
	SL_FreeServers (back, n);

	// rewritten whole, nothing left beside it
	CHECK (SL_SaveCache (CACHEPATH, servers + 2, 1, sources, 2));
	n = SL_LoadCache (CACHEPATH, sources, 2, &back);
	CHECK (n == 1);
	SL_FreeServers (back, n);
	CHECK (!(f = fopen (CACHEPATH ".tmp", "rb")));
	if (f)
		fclose (f);

	// another version thrown away; a broken line costs a server, a field from
	// the future nothing
	if ((f = fopen (CACHEPATH, "wb")))
	{
		fputs ("qualia-slist 99\n10.0.0.1:27500 ping=20 \\hostname\\x\n", f);
		fclose (f);
	}
	CHECK (SL_LoadCache (CACHEPATH, sources, 2, &back) == 0 && !back);
	if ((f = fopen (CACHEPATH, "wb")))
	{
		fputs ("qualia-slist 1\nnot-an-address ping=5\n10.0.0.2:27500 ping=41 mood=cheerful \\hostname\\second\n\n", f);
		fclose (f);
	}
	n = SL_LoadCache (CACHEPATH, sources, 2, &back);
	CHECK (n == 1 && back[0].ping == 41 && !strcmp (back[0].info, "\\hostname\\second"));
	SL_FreeServers (back, n);
	CHECK (SL_LoadCache ("no such cache", sources, 2, &back) == 0);
	remove (CACHEPATH);

	for (n = 0 ; n < 3 ; n++)
		SL_ClearServer (&servers[n]);
}

/*
==============================================================================

THE SCHEDULER

==============================================================================
*/

static slconfig_t Config (void)
{
	slconfig_t	c;

	SL_DefaultConfig (&c);
	c.pingrate = c.inforate = 1000;
	return c;
}

static slserver_t *Cached (int count, ...)
{
	slserver_t	*s = calloc ((size_t)count, sizeof(*s));
	va_list		args;
	int			i;

	va_start (args, count);
	for (i = 0 ; i < count ; i++)
	{
		s[i].address = Adr (va_arg (args, const char *));
		s[i].ping = va_arg (args, int);
		s[i].players = va_arg (args, int);
		s[i].info = sl_noinfo;
		s[i].routeping = -1;
		s[i].seen = 1;
		s[i].sources = 1;
	}
	va_end (args);
	return s;
}

#define MS(n)	((n) / 1000.0)

// the lanes run until count questions came due, so a test can speak of what
// is asked rather than of milliseconds
static int Drain (slsched_t *s, double start, int count, sldue_t *out, int max)
{
	sldue_t	due[SL_MAXDUE];
	int		n = 0, step, got, i;

	for (step = 1 ; step < 2000 && n < count ; step++)
	{
		got = SLS_CollectDue (s, start + MS (step), due);
		for (i = 0 ; i < got && n < max ; i++)
			out[n++] = due[i];
	}
	return n;
}

// the pings' hosts among the questions
static int Pings (const sldue_t *due, int n, int *out)
{
	int		i, count = 0;

	for (i = 0 ; i < n ; i++)
		if (due[i].query == SLQ_PING)
			out[count++] = due[i].index;
	return count;
}

static int Count (slsched_t *s, double from, double to, slquery_t query)
{
	sldue_t	due[SL_MAXDUE];
	double	t;
	int		count = 0, n, i;

	for (t = from ; t < to ; t += MS (1))
		for (n = SLS_CollectDue (s, t, due), i = 0 ; i < n ; i++)
			count += due[i].query == query;
	return count;
}

static void TestOrder (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	sldue_t		due[64];
	int			pings[64], n;

	// busy first and the nearest of those, gone last
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (4, "10.0.0.1:27500", 20, 0, "10.0.0.2:27500", 90, 4, "10.0.0.3:27500", -1, 0,
		"10.0.0.4:27500", 8, 2), 4);
	n = Pings (due, Drain (&s, 0, 4, due, 64), pings);
	CHECK (n >= 4 && pings[0] == 3 && pings[1] == 1 && pings[2] == 0 && pings[3] == 2);
	SLS_Free (&s);

	// one not heard of before one already gone
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	SLS_Add (&s, Adr ("10.0.0.9:27500"), NULL, 0, 1);
	n = Pings (due, Drain (&s, 0, 2, due, 64), pings);
	CHECK (n >= 2 && pings[0] == 1 && pings[1] == 0);
	SLS_Free (&s);

	// the whole list, then the whole list again: not one host three times
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (2, "10.0.0.1:27500", 10, 0, "10.0.0.2:27500", 20, 0), 2);
	n = Pings (due, Drain (&s, 0, 6, due, 64), pings);
	CHECK (n == 6 && pings[0] == 0 && pings[1] == 1 && pings[2] == 0 && pings[3] == 1 && pings[4] == 0 && pings[5] == 1);
	CHECK (!Drain (&s, 5, 1, due, 64));
	SLS_Free (&s);

	// one found mid-sweep joins the part still to come, by priority
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (2, "10.0.0.1:27500", 10, 0, "10.0.0.9:27500", 99, 0), 2);
	n = SLS_CollectDue (&s, MS (1), due);
	CHECK (n == 1 && due[0].index == 0);
	SLS_Add (&s, Adr ("10.0.0.5:27500"), NULL, 0, 1);
	s.hosts[2].e.players = 6;
	s.hosts[2].e.ping = 30;
	s.hosts[2].e.seen = 1;
	n = Pings (due, Drain (&s, MS (2), 2, due, 64), pings);
	CHECK (n >= 2 && pings[0] == 2 && pings[1] == 1);
	SLS_Free (&s);
}

static void TestAcks (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	sldue_t		due[64];
	netadr_t	a = Adr ("10.0.0.1:27500");
	int			n, i;
	bool		others;

	// timed against its own ping, and once
	c.pings = 1;
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	SLS_CollectDue (&s, MS (1), due);
	CHECK (SLS_OnPingAck (&s, a, MS (41), 0));
	CHECK (s.hosts[0].e.ping == 40);
	CHECK (!SLS_OnPingAck (&s, a, MS (900), 0));
	CHECK (s.hosts[0].e.ping == 40);
	CHECK (!SLS_OnPingAck (&s, Adr ("10.9.9.9:27500"), 0, 0));
	SLS_Free (&s);

	// a ping past its timeout isn't timed: it would invent the gap's latency
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	SLS_CollectDue (&s, MS (1), due);
	CHECK (!SLS_OnPingAck (&s, a, MS (1500), 0));
	CHECK (s.hosts[0].e.ping == -1 && s.hosts[0].e.state == SL_CACHED);
	SLS_Free (&s);

	// a remembered ping is replaced by the first measured, and then the best wins
	c = Config ();
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", 3, 0), 1);
	SLS_CollectDue (&s, MS (1), due);
	SLS_CollectDue (&s, MS (2), due);
	CHECK (SLS_OnPingAck (&s, a, MS (28), 0) && s.hosts[0].e.ping == 27);
	CHECK (SLS_OnPingAck (&s, a, MS (33), 0) && s.hosts[0].e.ping == 27);
	CHECK (s.hosts[0].e.samples == 2 && s.hosts[0].e.state == SL_ALIVE);
	SLS_Free (&s);

	// only one that answered a ping is asked what it is
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (2, "10.0.0.1:27500", -1, 0, "10.0.0.2:27500", -1, 0), 2);
	n = Drain (&s, 0, 2, due, 64);
	for (i = 0 ; i < n ; i++)
		CHECK (due[i].query == SLQ_PING);
	SLS_OnPingAck (&s, Adr ("10.0.0.2:27500"), MS (5), 0);
	n = Drain (&s, MS (6), 8, due, 64);
	for (i = 0, others = false ; i < n ; i++)
		if (due[i].query == SLQ_STATUS && due[i].index != 1)
			others = true;
	CHECK (Count (&s, MS (2006), MS (2008), SLQ_PING) == 0);
	CHECK (!others);
	SLS_Free (&s);
}

static void TestRetries (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	sldue_t		due[64];
	netadr_t	a = Adr ("10.0.0.1:27500");

	// asked again, then given up on
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	SLS_CollectDue (&s, MS (1), due);
	SLS_OnPingAck (&s, a, MS (2), 0);
	CHECK (Count (&s, MS (3), 12, SLQ_STATUS) == 3);
	SLS_Free (&s);

	// an answer stops them
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	SLS_CollectDue (&s, MS (1), due);
	SLS_OnPingAck (&s, a, MS (2), 0);
	SLS_CollectDue (&s, MS (3), due);
	CHECK (SLS_OnStatus (&s, a, (const byte *)"\\hostname\\answered\\map\\dm6", 26, 0));
	CHECK (Count (&s, MS (4), 12, SLQ_STATUS) == 0);
	CHECK (!strcmp (s.hosts[0].e.info, "\\hostname\\answered\\map\\dm6"));
	SLS_Free (&s);
}

static void TestPieces (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	netadr_t	a = Adr ("10.0.0.1:27500");
	const char	*one = "\\hostname\\busy\\maxclients\\8\n1 0 0 10 \"a\" \"\" 0 0 \"\"\n";
	const char	*two = "2 0 0 20 \"b\" \"\" 0 0 \"\"\n3 0 0 30 \"c\" \"\" 0 0 \"\"\n";
	const char	*claims = "\\maxclients\\16\\players\\16\n1 0 0 1 \"a\" \"\" 0 0\n2 -9999 0 -1 \"b\" \"\" 0 0\n";

	// a long answer's pieces make one roster, usable as they come
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	SLS_OnStatus (&s, a, (const byte *)one, (int)strlen (one), 0);
	CHECK (s.hosts[0].e.players == 1);
	SLS_OnStatus (&s, a, (const byte *)two, (int)strlen (two), 0);
	CHECK (s.hosts[0].e.players == 3 && s.hosts[0].e.numroster == 3);
	CHECK (!strcmp (s.hosts[0].e.info, "\\hostname\\busy\\maxclients\\8"));

	// a new answer starts over rather than piling up
	SLS_OnStatus (&s, a, (const byte *)one, (int)strlen (one), 0);
	CHECK (s.hosts[0].e.players == 1);

	// the counts are the roster's, not what the server claims
	SLS_OnStatus (&s, a, (const byte *)claims, (int)strlen (claims), 0);
	CHECK (s.hosts[0].e.players == 1 && s.hosts[0].e.spectators == 1);
	SLS_Free (&s);
}

static void TestRelays (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	sldue_t		due[64];
	netadr_t	server = Adr ("10.0.0.1:27500"), relay = Adr ("10.0.0.9:30000");
	byte		packet[128];
	byte		table[] = {10, 0, 0, 1, 0x6c, 0x6b, 30, 0};
	int			n, i, length;
	bool		asked;

	// a far server, a near relay, and a table that says the relay is near it
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (2, "10.0.0.1:27500", -1, 0, "10.0.0.9:30000", -1, 0), 2);
	SLS_CollectDue (&s, MS (1), due);
	SLS_CollectDue (&s, MS (2), due);
	SLS_OnPingAck (&s, server, MS (201), 0);
	SLS_OnPingAck (&s, relay, MS (22), 0);
	length = OOB (packet, 'n', "\\hostname\\a relay\\*version\\qwfwd 1.3", 36);
	CHECK (SLS_OnPrint (&s, relay, packet, length, 0));
	CHECK (s.hosts[1].e.proxy == SL_QWFWD);
	n = Drain (&s, MS (3), 20, due, 64);
	for (i = 0, asked = false ; i < n ; i++)
		asked |= due[i].query == SLQ_TABLE && due[i].index == 1;
	CHECK (asked);
	length = OOB (packet, 'n', table, sizeof(table));
	CHECK (SLS_OnPrint (&s, relay, packet, length, 0));
	SLS_CollectDue (&s, MS (600), due);		// worked out on the loop, a turn later
	CHECK (s.hosts[0].e.routeping == 50 && s.hosts[0].e.numhops == 1);
	CHECK (SameAdr (s.hosts[0].e.hops[0], relay));
	CHECK (SL_Ping (&s.hosts[0].e) == 50 && SL_Relayed (&s.hosts[0].e));
	SLS_Free (&s);

	// a relay's status still read as one while its table is waited for
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.9:30000", -1, 0), 1);
	length = OOB (packet, 'n', "\\hostname\\a relay\\*version\\qwfwd 1.3", 36);
	SLS_OnPrint (&s, relay, packet, length, 0);
	Drain (&s, MS (1), 20, due, 64);
	CHECK (s.hosts[0].tableattempts > 0);
	length = OOB (packet, 'n', "\\hostname\\renamed\\*version\\qwfwd 1.3\\map\\dm4", 44);
	SLS_OnPrint (&s, relay, packet, length, 0);
	CHECK (!strcmp (s.hosts[0].e.info, "\\hostname\\renamed\\*version\\qwfwd 1.3\\map\\dm4"));
	SLS_Free (&s);

	// an ordinary server is never asked what it reaches
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	length = OOB (packet, 'n', "\\hostname\\x\\*version\\MVDSV 0.36", 31);
	SLS_OnPrint (&s, server, packet, length, 0);
	n = Drain (&s, MS (1), 20, due, 64);
	for (i = 0, asked = false ; i < n ; i++)
		asked |= due[i].query == SLQ_TABLE;
	CHECK (!asked && s.hosts[0].e.proxy == SL_NOPROXY);
	SLS_Free (&s);
}

static void TestFinish (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	sldue_t		due[64];
	int			step;

	// nothing in it: finished at once
	SLS_Init (&s, &c, ~0ull, 0);
	CHECK (SLS_Finished (&s, 0));
	SLS_Free (&s);

	// not while anything is owed an answer
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (1, "10.0.0.1:27500", -1, 0), 1);
	CHECK (!SLS_Finished (&s, 0));
	for (step = 1 ; step < 2000 ; step++)
		SLS_CollectDue (&s, MS (step), due);
	CHECK (SLS_Finished (&s, 30));
	SLS_Free (&s);

	// what never answered ends up gone, what it was still shown
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Seed (&s, Cached (2, "10.0.0.1:27500", 20, 0, "10.0.0.2:27500", 30, 0), 2);
	SLS_CollectDue (&s, MS (1), due);
	SLS_OnPingAck (&s, Adr ("10.0.0.1:27500"), MS (5), 7);
	SLS_Finalize (&s);
	CHECK (s.hosts[0].e.state == SL_ALIVE && s.hosts[0].e.seen == 7);
	CHECK (s.hosts[1].e.state == SL_DEAD && s.hosts[1].e.ping == 30);
	SLS_Free (&s);
}

static void TestAdd (void)
{
	slconfig_t	c = Config ();
	slsched_t	s;
	sldue_t		due[64];
	netadr_t	a = Adr ("10.0.0.1:27500");

	// one server from two masters, both its sources
	SLS_Init (&s, &c, ~0ull, 0);
	CHECK (SLS_Add (&s, a, NULL, 0, 1));
	CHECK (!SLS_Add (&s, a, NULL, 0, 4));
	CHECK (s.numhosts == 1 && s.hosts[0].e.sources == 5);
	SLS_Free (&s);

	// a list fills in a server, never overrules one
	SLS_Init (&s, &c, ~0ull, 0);
	SLS_Add (&s, a, "\\hostname\\from the list", 23, 1);
	CHECK (!strcmp (s.hosts[0].e.info, "\\hostname\\from the list"));
	SLS_OnStatus (&s, a, (const byte *)"\\hostname\\from the server", 25, 0);
	SLS_Add (&s, a, "\\hostname\\from the list", 23, 1);
	CHECK (!strcmp (s.hosts[0].e.info, "\\hostname\\from the server"));
	SLS_Free (&s);

	// a source not scanned: its servers listed and never asked, until a
	// scanned one names them too
	SLS_Init (&s, &c, 1, 0);
	SLS_Add (&s, a, NULL, 0, 2);
	CHECK (!Drain (&s, 0, 1, due, 64) && s.numhosts == 1);
	SLS_Add (&s, a, NULL, 0, 1);
	CHECK (Drain (&s, 2, 1, due, 64) >= 1 && due[0].query == SLQ_PING && !due[0].index);
	SLS_Free (&s);
}

// paced by deadline: a wake late by 16 ms for 6 sends what came due
static void TestLanes (void)
{
	sllane_t	l;

	SL_LaneInit (&l, 150, 0);
	CHECK (SL_LaneTake (&l, MS (7), SL_BURST) == 1);
	CHECK (SL_LaneTake (&l, MS (22), SL_BURST) == 2);
	// hopelessly behind: capped, and the debt written off
	SL_LaneInit (&l, 150, 0);
	CHECK (SL_LaneTake (&l, 10, SL_BURST) == SL_BURST);
	CHECK (SL_LaneTake (&l, 10, SL_BURST) == 0);
}

/*
==============================================================================

ROUTES

==============================================================================
*/

static void TestRoutes (void)
{
	int		cost[3], via[3];

	// 200 direct, or 20 to the relay and 30 on
	{
		int			direct[] = {200, 20}, peers[] = {0}, ms[] = {30};
		slrelay_t	r = {1, peers, ms, 1};

		SL_Routes (direct, 2, &r, 1, cost, via);
		CHECK (cost[0] == 50 && via[0] == 1 && via[1] == -1);
	}
	// a relay that is no help makes no route
	{
		int			direct[] = {20, 90}, peers[] = {0}, ms[] = {15};
		slrelay_t	r = {1, peers, ms, 1};

		SL_Routes (direct, 2, &r, 1, cost, via);
		CHECK (cost[0] == 20 && via[0] == -1);
	}
	// two relays in series found unasked: 10 to A, 10 to B, 10 on, against 500
	{
		int			direct[] = {500, 10, -1}, apeers[] = {2}, ams[] = {10}, bpeers[] = {0}, bms[] = {10};
		slrelay_t	r[] = {{1, apeers, ams, 1}, {2, bpeers, bms, 1}};

		SL_Routes (direct, 3, r, 2, cost, via);
		CHECK (cost[0] == 30 && via[0] == 2 && via[2] == 1 && via[1] == -1);
	}
	// a relay we don't reach leads nowhere
	{
		int			direct[] = {200, -1}, peers[] = {0}, ms[] = {5};
		slrelay_t	r = {1, peers, ms, 1};

		SL_Routes (direct, 2, &r, 1, cost, via);
		CHECK (cost[0] == 200 && via[0] == -1);
	}
	// relays naming each other don't walk in circles
	{
		int			direct[] = {300, 10, 12}, apeers[] = {2, 0}, ams[] = {1, 40}, bpeers[] = {1, 0}, bms[] = {1, 30};
		slrelay_t	r[] = {{1, apeers, ams, 2}, {2, bpeers, bms, 2}};

		SL_Routes (direct, 3, r, 2, cost, via);
		CHECK (cost[0] == 41 && via[0] == 2 && via[2] == 1);
	}
	// no relays, nothing to say
	{
		int		direct[] = {20};

		SL_Routes (direct, 1, NULL, 0, cost, via);
		CHECK (cost[0] == 20 && via[0] == -1);
	}
}

/*
==============================================================================

THE VIEW

==============================================================================
*/

static void TestKeys (void)
{
	slview_t	v = {0};

	CHECK (SL_KeyForName (&v, "ping") == SLK_PING && SL_KeyForName (&v, "hostname") == SLK_NAME);
	CHECK (SL_KeyForName (&v, "cname") == SLK_ADDRESS && SL_KeyForName (&v, "*gamedir") == SLK_GAMEDIR);
	CHECK (SL_KeyForName (&v, "state") == SLK_STATE);
	CHECK (SL_KeyForName (&v, "player3") == SLK_PLAYER0 + 3);
	CHECK (SL_KeyForName (&v, "teamplay") == SLK_CUSTOM);
	CHECK (SL_KeyForName (&v, "players") == SLK_CUSTOM + 1);	// not player0, as FTE has it
	CHECK (SL_KeyForName (&v, "teamplay") == SLK_CUSTOM);
}

static void TestView (void)
{
	slview_t	v = {0};
	slserver_t	s[4];
	char		buf[64], brown[] = "\\hostname\\x\xcf\xe4\xe5\xf3\xf3\xe1 QW\\map\\dm3";
	int			n, i;

	s[0] = Server ("203.0.113.9:27500", 40, brown);		// Odessa, in brown
	s[1] = Server ("10.0.0.2:27500", 12, "\\hostname\\near\\map\\aerowalk");
	s[2] = Server ("10.0.0.1:27500", -1, "\\hostname\\quiet\\map\\dm6");
	s[3] = Server ("10.0.0.3:27500", 12, "\\hostname\\other\\map\\dm6");
	for (i = 0 ; i < 4 ; i++)
		s[i].sources = 1;
	s[0].players = 2;
	s[3].players = 5;
	s[3].sources = 2;
	s[2].state = SL_DEAD;

	CHECK (!strcmp (SL_KeyString (&v, &s[0], SLK_MAP, buf, sizeof(buf)), "dm3"));
	CHECK (SL_KeyNumber (&v, &s[2], SLK_PING) == 0xffff);
	CHECK (!strcmp (SL_KeyString (&v, &s[1], SLK_ADDRESS, buf, sizeof(buf)), "10.0.0.2:27500"));
	CHECK (SL_KeyNumber (&v, &s[1], SLK_ISLOCAL) == 1 && SL_KeyNumber (&v, &s[0], SLK_ISLOCAL) == 0);

	// the sources' servers, in the table's order without keys; the dead too
	n = SL_Arrange (&v, s, 4, 1);
	CHECK (n == 3 && v.shown[0] == 0 && v.shown[1] == 1 && v.shown[2] == 2);
	CHECK (SL_Arrange (&v, s, 4, 3) == 4);

	// by ping, the silent last; reversed, first
	SL_SetSort (&v, SLK_PING, 0);
	n = SL_Arrange (&v, s, 4, 1);
	CHECK (n == 3 && v.shown[0] == 1 && v.shown[1] == 0 && v.shown[2] == 2);
	SL_SetSort (&v, SLK_PING, SL_SORT_DESCENDING);
	n = SL_Arrange (&v, s, 4, 1);
	CHECK (n == 3 && v.shown[0] == 2 && v.shown[2] == 1);

	// ties by address: 1 and 3 ping the same
	SL_SetSort (&v, SLK_PING, 0);
	n = SL_Arrange (&v, s, 4, 3);
	CHECK (n == 4 && v.shown[0] == 1 && v.shown[1] == 3);
	// keys appended: players down, then ping
	SL_SetSort (&v, SLK_NUMPLAYERS, SL_SORT_DESCENDING);
	SL_SetSort (&v, SLK_PING, SL_SORT_APPEND);
	n = SL_Arrange (&v, s, 4, 3);
	CHECK (n == 4 && v.shown[0] == 3 && v.shown[1] == 0 && v.shown[2] == 1 && v.shown[3] == 2);
	SL_SetSort (&v, -1, 0);

	// masks ANDed: a coloured name found by its white letters
	SL_AddRule (&v, false, SLK_NAME, SLT_CONTAINS, "ODESSA", 0);
	n = SL_Arrange (&v, s, 4, 3);
	CHECK (n == 1 && v.shown[0] == 0);
	SL_AddRule (&v, false, SLK_PING, SLT_LESS, NULL, 20);
	CHECK (SL_Arrange (&v, s, 4, 3) == 0);
	// and an OR after them takes what passes it
	SL_AddRule (&v, true, SLK_MAP, SLT_EQUAL, "DM6", 0);
	n = SL_Arrange (&v, s, 4, 3);
	CHECK (n == 2 && v.shown[0] == 2 && v.shown[1] == 3);
	// the dead hidden by a mask, as sb_hidedead
	SL_ClearRules (&v);
	SL_AddRule (&v, false, SLK_STATE, SLT_NOTEQUAL, NULL, SL_DEAD);
	n = SL_Arrange (&v, s, 4, 3);
	CHECK (n == 3);
	for (i = 0 ; i < n ; i++)
		CHECK (v.shown[i] != 2);
	// a string mask on a number reads the string's number
	SL_ClearRules (&v);
	SL_AddRule (&v, false, SLK_NUMPLAYERS, SLT_GREATEREQUAL, "3", 0);
	CHECK (SL_Arrange (&v, s, 4, 3) == 1);

	SL_FreeView (&v);
	for (i = 0 ; i < 4 ; i++)
		SL_ClearServer (&s[i]);
}

int main (void)
{
	TestMaster ();
	TestPingAck ();
	TestTable ();
	TestStatus ();
	TestInfo ();
	TestSources ();
	TestMarks ();
	TestListing ();
	TestQTVList ();
	TestAddresses ();
	TestCache ();
	TestOrder ();
	TestAcks ();
	TestRetries ();
	TestPieces ();
	TestRelays ();
	TestFinish ();
	TestAdd ();
	TestLanes ();
	TestRoutes ();
	TestKeys ();
	TestView ();
	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("slist: packets, sources, the cache, the scheduler, routes and the view\n");
	return 0;
}
