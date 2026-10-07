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
// slist_view.c -- what the browser shows of a list: the hostcache's keys,
// masks and sort as FTE has them (net_master.c), over the servers of the
// marked sources
//
// FTE's masks are tested in the order they were set, each ANDed into the
// answer, or ORed for the ones set as OR (which, the answer starting true,
// pass everything after them: FTE's way, kept). Unlike FTE's, servers that
// haven't answered aren't hidden but by a mask (SLK_STATE): ezQuake shows them
// unless sb_hidedead. Strings compare without case, Quake's coloured
// characters as their white ones.

#include "slist.h"

#include "q_endian.h"
#include "q_string.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_QWPROTOCOL	28		// what a QuakeWorld server's protocol is, to FTE's key

static const char	*const sl_keynames[] =
{
	"ping", "map", "name", "address", "numplayers", "maxplayers", "gamedir",
	"freeplayers", "basegame", "flags", "timelimit", "fraglimit",
	"mod", "protocol", "numbots", "numspectators", "numhumans", "qcstatus", "category",
	"isfavorite", "islocal", "isproxy", "serverinfo",
	"state", "seen", "directping", "hops", "samples"
};

static const struct
{
	const char	*name;
	int			key;
} sl_keyaliases[] =
{
	{"hostname", SLK_NAME}, {"cname", SLK_ADDRESS}, {"freeslots", SLK_FREEPLAYERS},
	{"game", SLK_GAMEDIR}, {"*gamedir", SLK_GAMEDIR}
};

void SL_FreeView (slview_t *v)
{
	free (v->shown);
	v->shown = NULL;
	v->numshown = v->maxshown = 0;
}

int SL_KeyForName (slview_t *v, const char *name)
{
	int		i;

	for (i = 0 ; i < (int)(sizeof(sl_keynames) / sizeof(sl_keynames[0])) ; i++)
		if (!strcmp (name, sl_keynames[i]))
			return i;
	for (i = 0 ; i < (int)(sizeof(sl_keyaliases) / sizeof(sl_keyaliases[0])) ; i++)
		if (!strcmp (name, sl_keyaliases[i].name))
			return sl_keyaliases[i].key;
	if (!strncmp (name, "player", 6) && isdigit ((byte)name[6]))
	{
		i = atoi (name + 6);
		return i < SL_MAXPLAYERS ? SLK_PLAYER0 + i : SLK_TOOMANY;
	}
	for (i = 0 ; i < v->numcustom ; i++)
		if (!strcmp (name, v->custom[i]))
			return SLK_CUSTOM + i;
	if (v->numcustom == SL_MAXCUSTOM)
		return SLK_TOOMANY;
	Q_strncpyz (v->custom[v->numcustom], name, sizeof(v->custom[0]));
	return SLK_CUSTOM + v->numcustom++;
}

void SL_ClearRules (slview_t *v)
{
	v->numrules = 0;
}

void SL_AddRule (slview_t *v, bool or, int key, sltest_t test, const char *string, double number)
{
	slrule_t	*r;

	if (v->numrules == SL_MAXRULES)
		return;
	r = &v->rules[v->numrules++];
	r->key = key;
	r->test = test;
	r->or = or;
	Q_strncpyz (r->string, string ? string : "", sizeof(r->string));
	r->number = string ? atof (string) : number;
}

void SL_SetSort (slview_t *v, int key, int flags)
{
	if (key < 0)
	{
		v->numsort = 0;
		return;
	}
	if (!(flags & SL_SORT_APPEND))
		v->numsort = 0;
	if (v->numsort == SL_MAXSORT)
		return;
	v->sort[v->numsort].key = key;
	v->sort[v->numsort].descending = (flags & SL_SORT_DESCENDING) != 0;
	v->numsort++;
}

/*
==============================================================================

KEYS

==============================================================================
*/

// whether a key compares as a number (FTE's integer keys, and SoftWorld's)
static bool SL_NumericKey (int key)
{
	switch (key)
	{
	case SLK_PING: case SLK_NUMPLAYERS: case SLK_MAXPLAYERS: case SLK_FREEPLAYERS: case SLK_BASEGAME:
	case SLK_FLAGS: case SLK_TIMELIMIT: case SLK_FRAGLIMIT: case SLK_PROTOCOL: case SLK_NUMBOTS:
	case SLK_NUMSPECTATORS: case SLK_NUMHUMANS: case SLK_CATEGORY: case SLK_ISFAVORITE: case SLK_ISLOCAL:
	case SLK_ISPROXY: case SLK_STATE: case SLK_SEEN: case SLK_DIRECTPING: case SLK_HOPS: case SLK_SAMPLES:
		return true;
	default:
		return false;
	}
}

// a private address: the loopback, a LAN's, a link's
static bool SL_IsLocal (netadr_t a)
{
	const byte	*ip = a.ip;

	if (NET_IsIPv4 (a))
		return ip[12] == 10 || ip[12] == 127 || (ip[12] == 172 && (ip[13] & 0xf0) == 16)
			|| (ip[12] == 192 && ip[13] == 168) || (ip[12] == 169 && ip[13] == 254);
	if ((ip[0] & 0xfe) == 0xfc || (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80))
		return true;
	return !memcmp (ip, (const byte[16]){[15] = 1}, 16);
}

double SL_KeyNumber (const slview_t *v, const slserver_t *s, int key)
{
	char	buf[256];
	int		n;

	switch (key)
	{
	case SLK_PING:
		return (n = SL_Ping (s)) < 0 ? 0xffff : n;
	case SLK_DIRECTPING:
		return s->ping < 0 ? 0xffff : s->ping;
	case SLK_NUMPLAYERS:
	case SLK_NUMHUMANS:
		return s->players;
	case SLK_NUMSPECTATORS:
		return s->spectators;
	case SLK_MAXPLAYERS:
		return SL_MaxClients (s);
	case SLK_FREEPLAYERS:
		return SL_MaxClients (s) - s->players;
	case SLK_TIMELIMIT:
		return SL_InfoInt (s->info, "timelimit");
	case SLK_FRAGLIMIT:
		return SL_InfoInt (s->info, "fraglimit");
	case SLK_PROTOCOL:
		return SL_QWPROTOCOL;
	case SLK_BASEGAME: case SLK_FLAGS: case SLK_NUMBOTS: case SLK_CATEGORY: case SLK_ISFAVORITE:
		return 0;
	case SLK_ISLOCAL:
		return SL_IsLocal (s->address);
	case SLK_ISPROXY:
		return s->proxy != SL_NOPROXY;		// ezQuake's proxies: qizmo, qwfwd
	case SLK_STATE:
		return s->state;
	case SLK_SEEN:
		return (double)s->seen;
	case SLK_HOPS:
		return SL_Relayed (s) ? s->numhops : 0;
	case SLK_SAMPLES:
		return s->samples;
	default:
		return atof (SL_KeyString (v, s, key, buf, sizeof(buf)));
	}
}

const char *SL_KeyString (const slview_t *v, const slserver_t *s, int key, char *buf, size_t size)
{
	const slplayer_t	*p;

	*buf = 0;
	if (key >= SLK_CUSTOM)
		return key - SLK_CUSTOM < v->numcustom ? SL_InfoValue (s->info, v->custom[key - SLK_CUSTOM], buf, size) : buf;
	if (key >= SLK_PLAYER0)
	{
		// FTE's player<N>, its colours as they come and its team after
		if (key - SLK_PLAYER0 < s->numroster)
		{
			p = &s->roster[key - SLK_PLAYER0];
			snprintf (buf, size, "%i %i %i %i \"%s\" \"%s\" %i %i \"%s\"", p->userid, p->frags, p->time, p->ping,
				p->name, p->skin, p->topcolor, p->bottomcolor, p->team);
		}
		return buf;
	}
	switch (key)
	{
	case SLK_MAP:
		return SL_InfoValue (s->info, "map", buf, size);
	case SLK_NAME:
		return SL_InfoValue (s->info, "hostname", buf, size);
	case SLK_ADDRESS:
		return SL_AdrToBuf (s->address, buf, size);
	case SLK_GAMEDIR:
		return SL_InfoValue (s->info, "*gamedir", buf, size);
	case SLK_MOD:
		return SL_InfoValue (s->info, "*progs", buf, size);
	case SLK_QCSTATUS:
		return SL_InfoValue (s->info, "qcstatus", buf, size);
	case SLK_SERVERINFO:
		Q_strncpyz (buf, s->info, size);
		return buf;
	default:
		if (key >= 0 && key < SLK_TOOMANY)
			snprintf (buf, size, "%g", SL_KeyNumber (v, s, key));
		return buf;
	}
}

/*
==============================================================================

MASKS

==============================================================================
*/

static int SL_Fold (int c)
{
	return tolower (c & 127);
}

static int SL_FoldCompare (const char *a, const char *b)
{
	for ( ; *a && SL_Fold ((byte)*a) == SL_Fold ((byte)*b) ; a++, b++)
		;
	return SL_Fold ((byte)*a) - SL_Fold ((byte)*b);
}

static bool SL_FoldStarts (const char *s, const char *prefix)
{
	for ( ; *prefix ; s++, prefix++)
		if (SL_Fold ((byte)*s) != SL_Fold ((byte)*prefix))
			return false;
	return true;
}

static bool SL_FoldContains (const char *s, const char *needle)
{
	for ( ; ; s++)
	{
		if (SL_FoldStarts (s, needle))
			return true;
		if (!*s)
			return false;
	}
}

static bool SL_TestNumber (int a, int b, sltest_t test)
{
	switch (test)
	{
	case SLT_CONTAINS:		return (a & b) != 0;
	case SLT_NOTCONTAIN:	return !(a & b);
	case SLT_LESSEQUAL:		return a <= b;
	case SLT_LESS:			return a < b;
	case SLT_STARTSWITH:
	case SLT_EQUAL:			return a == b;
	case SLT_GREATER:		return a > b;
	case SLT_GREATEREQUAL:	return a >= b;
	case SLT_NOTSTARTSWITH:
	case SLT_NOTEQUAL:		return a != b;
	}
	return false;
}

static bool SL_TestString (const char *a, const char *b, sltest_t test)
{
	switch (test)
	{
	case SLT_STARTSWITH:	return SL_FoldStarts (a, b);
	case SLT_NOTSTARTSWITH:	return !SL_FoldStarts (a, b);
	case SLT_CONTAINS:		return SL_FoldContains (a, b);
	case SLT_NOTCONTAIN:	return !SL_FoldContains (a, b);
	case SLT_LESSEQUAL:		return SL_FoldCompare (a, b) <= 0;
	case SLT_LESS:			return SL_FoldCompare (a, b) < 0;
	case SLT_EQUAL:			return !SL_FoldCompare (a, b);
	case SLT_GREATER:		return SL_FoldCompare (a, b) > 0;
	case SLT_GREATEREQUAL:	return SL_FoldCompare (a, b) >= 0;
	case SLT_NOTEQUAL:		return SL_FoldCompare (a, b) != 0;
	}
	return false;
}

static bool SL_Passes (const slview_t *v, const slserver_t *s)
{
	char			buf[SL_MAXINFO];
	const slrule_t	*r;
	bool			pass = true, result;
	int				i;

	for (i = 0, r = v->rules ; i < v->numrules ; i++, r++)
	{
		if (SL_NumericKey (r->key))
			result = SL_TestNumber ((int)SL_KeyNumber (v, s, r->key), (int)r->number, r->test);
		else
			result = SL_TestString (SL_KeyString (v, s, r->key, buf, sizeof(buf)), r->string, r->test);
		if (r->or)
			pass |= result;
		else
			pass &= result;
	}
	return pass;
}

/*
==============================================================================

SORTING

The keys are read once a server, before the sort: a sort's comparisons are
many, a serverinfo's reading not cheap.

==============================================================================
*/

typedef struct
{
	double	number;
	char	text[64];
} slsortvalue_t;

typedef struct
{
	const slview_t	*view;
	netadr_t		address;
	int				index;
	slsortvalue_t	value[];	// the view's sort keys'
} slsortitem_t;

static int SL_CompareItems (const void *pa, const void *pb)
{
	const slsortitem_t	*a = pa, *b = pb;
	const slview_t		*v = a->view;
	int					i, c;

	for (i = 0 ; i < v->numsort ; i++)
	{
		if (SL_NumericKey (v->sort[i].key))
			c = a->value[i].number < b->value[i].number ? -1 : a->value[i].number > b->value[i].number;
		else
			c = SL_FoldCompare (a->value[i].text, b->value[i].text);
		if (c)
			return v->sort[i].descending ? -c : c;
	}
	// every key has ties, and rows that swap between scans can't be picked:
	// the address is the one thing no two servers share
	if ((c = memcmp (a->address.ip, b->address.ip, sizeof(a->address.ip))))
		return c;
	if (a->address.port != b->address.port)
		return BigShort ((short)a->address.port) < BigShort ((short)b->address.port) ? -1 : 1;
	return a->index < b->index ? -1 : a->index > b->index;
}

int SL_Arrange (slview_t *v, const slserver_t *servers, int count, uint64_t sources)
{
	slsortitem_t	*item;
	byte			*items;
	size_t			itemsize;
	int				*shown, n = 0, i, k;

	if (count > v->maxshown)
	{
		if (!(shown = realloc (v->shown, (size_t)count * sizeof(*shown))))
			return v->numshown = 0;
		v->shown = shown;
		v->maxshown = count;
	}
	for (i = 0 ; i < count ; i++)
		if ((servers[i].sources & sources) && SL_Passes (v, &servers[i]))
			v->shown[n++] = i;
	v->numshown = n;

	// in the table's order without keys, else sorted by them (their values
	// taken once) and by address
	itemsize = sizeof(slsortitem_t) + (size_t)v->numsort * sizeof(slsortvalue_t);
	if (!n || !v->numsort || !(items = malloc ((size_t)n * itemsize)))
		return n;
	for (i = 0 ; i < n ; i++)
	{
		item = (slsortitem_t *)(items + (size_t)i * itemsize);
		item->view = v;
		item->index = v->shown[i];
		item->address = servers[item->index].address;
		for (k = 0 ; k < v->numsort ; k++)
			if (SL_NumericKey (v->sort[k].key))
				item->value[k].number = SL_KeyNumber (v, &servers[item->index], v->sort[k].key);
			else
				SL_KeyString (v, &servers[item->index], v->sort[k].key, item->value[k].text,
					sizeof(item->value[k].text));
	}
	qsort (items, (size_t)n, itemsize, SL_CompareItems);
	for (i = 0 ; i < n ; i++)
		v->shown[i] = ((slsortitem_t *)(items + (size_t)i * itemsize))->index;
	free (items);
	return n;
}
