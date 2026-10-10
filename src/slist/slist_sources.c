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
// slist_sources.c -- where the servers are named: ezQuake's sources.txt, the
// marks, and the lists a file or URL source is
//
//	/ a comment
//	master "QuakeServers.net" master.quakeservers.net:27000
//	file   "Capture The Flag" ctf.txt
//	url    "QuakeServers URL" http://www.quakeservers.net/lists/servers/global.txt
//	server "My server" play.example.net
//
// read as ezQuake has it, with qualia's server lines besides. The ports a
// master and a server leave out differ (27000, 27500), so it is the column an
// address is in that says which.

#include "slist.h"

#include "q_endian.h"
#include "q_string.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char	*const sl_sourcetypes[] = {"master", "file", "url", "server"};

// the next token of a line, quoted or up to a space, into out; the text after
// it, NULL for none
static const char *SL_Token (const char *line, char *out, size_t size)
{
	const char	*end;
	size_t		n;

	while (*line && isspace ((byte)*line))
		line++;
	if (*line == '"')
	{
		line++;
		end = strchr (line, '"');
		if (!end)
			return NULL;
	}
	else
	{
		for (end = line ; *end && !isspace ((byte)*end) ; end++)
			;
		if (end == line)
			return NULL;
	}
	n = (size_t)(end - line);
	if (n > size - 1)
		n = size - 1;
	memcpy (out, line, n);
	out[n] = 0;
	return *end == '"' ? end + 1 : end;
}

int SL_ParseSources (const char *text, slsource_t *sources, int max)
{
	char		line[1024], kind[16];
	const char	*p, *eol;
	slsource_t	*s;
	int			count = 0, type;
	size_t		n;

	for ( ; *text && count < max ; text = *eol ? eol + 1 : eol)
	{
		eol = strchr (text, '\n');
		if (!eol)
			eol = text + strlen (text);
		n = (size_t)(eol - text);
		if (n > sizeof(line) - 1)
			n = sizeof(line) - 1;
		memcpy (line, text, n);
		line[n] = 0;

		for (p = line ; isspace ((byte)*p) ; p++)
			;
		if (!*p || *p == '/' || *p == '#')
			continue;
		s = &sources[count];
		memset (s, 0, sizeof(*s));
		if (!(p = SL_Token (p, kind, sizeof(kind))) || !(p = SL_Token (p, s->name, sizeof(s->name)))
			|| !SL_Token (p, s->location, sizeof(s->location)))
			continue;
		for (type = 0 ; type < (int)(sizeof(sl_sourcetypes) / sizeof(sl_sourcetypes[0])) ; type++)
			if (!Q_strcasecmp (kind, sl_sourcetypes[type]))
				break;
		if (type == (int)(sizeof(sl_sourcetypes) / sizeof(sl_sourcetypes[0])))
			continue;		// a kind not known isn't guessed at
		s->type = (slsourcetype_t)type;
		if (s->type == SL_MASTER || s->type == SL_SERVER)
		{
			Q_strncpyz (line, s->location, sizeof(line));
			SL_WithPort (line, s->type == SL_MASTER ? SL_MASTERPORT : SL_SERVERPORT, s->location, sizeof(s->location));
		}
		count++;
	}
	return count;
}

size_t SL_FormatSources (const slsource_t *sources, int count, char *buf, size_t size)
{
	size_t	length = 0;
	int		i, n;

	if (size)
		*buf = 0;
	for (i = 0 ; i < count ; i++)
	{
		n = snprintf (length < size ? buf + length : NULL, length < size ? size - length : 0, "%s \"%s\" %s\n",
			sl_sourcetypes[sources[i].type], sources[i].name, sources[i].location);
		if (n > 0)
			length += (size_t)n;
	}
	return length;
}

int SL_DefaultSources (slsource_t *sources, int max)
{
	static const slsource_t	defaults[] =
	{
		{SL_MASTER, "QuakeServers.net", "master.quakeservers.net:27000", true},
		{SL_MASTER, "Asgaard", "qwmaster.fodquake.net:27000", true},
	};
	int		count = (int)(sizeof(defaults) / sizeof(defaults[0]));

	if (count > max)
		count = max;
	memcpy (sources, defaults, (size_t)count * sizeof(*sources));
	return count;
}

void SL_ParseMarks (const char *text, slsource_t *sources, int count)
{
	// ezQuake's (MarkDefaultSources), by name
	static const char	*const defaults[] = {"id limbo", "Global", "QuakeServers.net", "Asgaard"};
	char		name[64];
	const char	*p, *eol;
	bool		any = false;
	int			i, j;
	size_t		n;

	for (i = 0 ; i < count ; i++)
		sources[i].marked = false;
	if (!text)
	{
		for (i = 0 ; i < count ; i++)
			for (j = 0 ; j < (int)(sizeof(defaults) / sizeof(defaults[0])) ; j++)
				if (!strcmp (sources[i].name, defaults[j]))
					any = sources[i].marked = true;
		if (!any)
			for (i = 0 ; i < count ; i++)
				sources[i].marked = true;
		return;
	}

	for ( ; *text ; text = *eol ? eol + 1 : eol)
	{
		char	line[256];

		eol = strchr (text, '\n');
		if (!eol)
			eol = text + strlen (text);
		n = (size_t)(eol - text);
		if (n > sizeof(line) - 1)
			n = sizeof(line) - 1;
		memcpy (line, text, n);
		line[n] = 0;
		for (p = line ; isspace ((byte)*p) ; p++)
			;
		if (!*p || *p == '/' || *p == '#' || !SL_Token (p, name, sizeof(name)))
			continue;
		for (i = 0 ; i < count ; i++)
			if (!strcmp (sources[i].name, name))
				sources[i].marked = true;
	}
}

size_t SL_FormatMarks (const slsource_t *sources, int count, char *buf, size_t size)
{
	size_t	length = 0;
	int		i, n;

	if (size)
		*buf = 0;
	for (i = 0 ; i < count ; i++)
	{
		if (!sources[i].marked)
			continue;
		n = snprintf (length < size ? buf + length : NULL, length < size ? size - length : 0, "\"%s\"\n",
			sources[i].name);
		if (n > 0)
			length += (size_t)n;
	}
	return length;
}

void SL_ParseListing (const char *data, size_t length,
	void (*entry) (void *ctx, const char *address, const char *info, size_t infolen), void *ctx)
{
	const char	*end = data + length, *line, *eol, *last, *word, *info;
	char		address[128];
	size_t		n;

	for (line = data ; line < end ; line = eol + 1)
	{
		eol = memchr (line, '\n', (size_t)(end - line));
		if (!eol)
			eol = end;
		for (last = eol ; last > line && isspace ((byte)last[-1]) ; last--)
			;
		while (line < last && isspace ((byte)*line))
			line++;
		if (line == last || *line == '#' || *line == '/')
			continue;
		for (word = line ; word < last && !isspace ((byte)*word) ; word++)
			;
		n = (size_t)(word - line);
		if (n > sizeof(address) - 1)
			n = sizeof(address) - 1;
		memcpy (address, line, n);
		address[n] = 0;
		info = memchr (word, '\\', (size_t)(last - word));
		entry (ctx, address, info, info ? (size_t)(last - info) : 0);
	}
}

/*
==============================================================================

THE QTV LIST

JSON as qtvapi.quakeworld.nu gives it: {"Servers": [{"GameStates": [{
"Hostname", "IpAddress", "Port", "Link", "Players": [...]}, ...]}], ...}. Read
as JSON, each object for the IpAddress, Port and Link it has, wherever it
is; it comes from the network, so its nesting is bounded.

==============================================================================
*/

#define SL_JSONDEPTH	32

typedef struct
{
	const char	*p, *end;
	int			depth, count;
	void		(*stream) (void *ctx, netadr_t server, const char *stream);
	void		*ctx;
} sljson_t;

static void SL_JsonSpace (sljson_t *j)
{
	while (j->p < j->end && isspace ((byte)*j->p))
		j->p++;
}

// a string into out, cut to its size; escapes as the characters they are
// (\uXXXX a ?); false if it isn't one
static bool SL_JsonString (sljson_t *j, char *out, size_t size)
{
	size_t	n = 0;
	char	c;

	if (j->p == j->end || *j->p != '"')
		return false;
	for (j->p++ ; j->p < j->end && *j->p != '"' ; j->p++)
	{
		c = *j->p;
		if (c == '\\')
		{
			if (++j->p == j->end)
				return false;
			switch (*j->p)
			{
			case 'n':	c = '\n'; break;
			case 't':	c = '\t'; break;
			case 'r':	c = '\r'; break;
			case 'b':	c = '\b'; break;
			case 'f':	c = '\f'; break;
			case 'u':
				c = '?';
				j->p += j->end - j->p > 4 ? 4 : j->end - j->p - 1;
				break;
			default:	c = *j->p; break;
			}
		}
		if (n + 1 < size)
			out[n++] = c;
	}
	if (size)
		out[n] = 0;
	if (j->p == j->end)
		return false;
	j->p++;
	return true;
}

static bool SL_JsonValue (sljson_t *j, char *text, size_t size, double *number);

// an object: the stream of the one with IpAddress, Port and Link
static bool SL_JsonObject (sljson_t *j)
{
	char	key[32], text[256], ip[64] = "", link[256] = "", stream[SL_MAXSTREAM], host[192], *slash, *sid;
	double	number = 0, port = -1;
	netadr_t	a;

	j->p++;
	SL_JsonSpace (j);
	if (j->p < j->end && *j->p == '}')
	{
		j->p++;
		return true;
	}
	for (;;)
	{
		SL_JsonSpace (j);
		if (!SL_JsonString (j, key, sizeof(key)))
			return false;
		SL_JsonSpace (j);
		if (j->p == j->end || *j->p++ != ':')
			return false;
		text[0] = 0;
		if (!SL_JsonValue (j, text, sizeof(text), &number))
			return false;
		if (!strcmp (key, "IpAddress"))
			Q_strncpyz (ip, text, sizeof(ip));
		else if (!strcmp (key, "Port"))
			port = number;
		else if (!strcmp (key, "Link"))
			Q_strncpyz (link, text, sizeof(link));
		SL_JsonSpace (j);
		if (j->p < j->end && *j->p == ',')
		{
			j->p++;
			continue;
		}
		if (j->p == j->end || *j->p++ != '}')
			return false;
		break;
	}

	// a relay's http://host:port/watch.qtv?sid=N, as N@host:port
	if (!*ip || port <= 0 || port > 65535 || !*link || !SL_ParseAddress (ip, (int)port, &a))
		return true;
	if (!Q_strncasecmp (link, "http://", 7) || !Q_strncasecmp (link, "https://", 8))
	{
		Q_strncpyz (host, strstr (link, "://") + 3, sizeof(host));
		if ((slash = strchr (host, '/')))
			*slash = 0;
		if (!(sid = strstr (link, "sid=")) || !isdigit ((byte)sid[4]))
			return true;
		if (!Q_snprintfz (stream, sizeof(stream), "%d@%s", atoi (sid + 4), host))
			return true;	// no relay is named that
	}
	else if (strchr (link, '@'))
		Q_strncpyz (stream, link, sizeof(stream));
	else
		return true;
	j->stream (j->ctx, a, stream);
	j->count++;
	return true;
}

static bool SL_JsonArray (sljson_t *j)
{
	char	text[8];
	double	number;

	j->p++;
	SL_JsonSpace (j);
	if (j->p < j->end && *j->p == ']')
	{
		j->p++;
		return true;
	}
	for (;;)
	{
		if (!SL_JsonValue (j, text, sizeof(text), &number))
			return false;
		SL_JsonSpace (j);
		if (j->p < j->end && *j->p == ',')
		{
			j->p++;
			continue;
		}
		return j->p < j->end && *j->p++ == ']';
	}
}

// a value: a string's text, a number, the literals; objects and arrays walked
static bool SL_JsonValue (sljson_t *j, char *text, size_t size, double *number)
{
	char	digits[64], *stop;
	size_t	n = 0;
	bool	ok;

	SL_JsonSpace (j);
	if (j->p == j->end)
		return false;
	switch (*j->p)
	{
	case '"':
		return SL_JsonString (j, text, size);
	case '{':
	case '[':
		if (j->depth == SL_JSONDEPTH)
			return false;
		j->depth++;
		ok = *j->p == '{' ? SL_JsonObject (j) : SL_JsonArray (j);
		j->depth--;
		return ok;
	default:
		// a number, true, false or null
		while (j->p < j->end && (isalnum ((byte)*j->p) || *j->p == '-' || *j->p == '+' || *j->p == '.'))
		{
			if (n < sizeof(digits) - 1)
				digits[n++] = *j->p;
			j->p++;
		}
		digits[n] = 0;
		*number = strtod (digits, &stop);
		return n > 0;
	}
}

int SL_ParseQTVList (const char *json, size_t length,
	void (*stream) (void *ctx, netadr_t server, const char *stream), void *ctx)
{
	sljson_t	j = {json, json + length, 0, 0, stream, ctx};
	char		text[8];
	double		number;

	SL_JsonSpace (&j);
	if (j.p == j.end || *j.p != '{' || !SL_JsonValue (&j, text, sizeof(text), &number))
		return -1;
	return j.count;
}

/*
==============================================================================

ADDRESSES

==============================================================================
*/

void SL_WithPort (const char *address, int port, char *out, size_t size)
{
	const char	*close = strchr (address, ']'), *colon = strchr (address, ':');
	bool		hasport;

	if (close)
		hasport = strchr (close, ':') != NULL;
	else
		hasport = colon && colon == strrchr (address, ':');
	if (hasport)
		snprintf (out, size, "%s", address);
	else if (colon && address[0] != '[')
		snprintf (out, size, "[%s]:%i", address, port);	// a bare IPv6 address's colons aren't a port's
	else
		snprintf (out, size, "%s:%i", address, port);
}

bool SL_SplitAddress (const char *address, int defaultport, char *host, size_t size, int *port)
{
	const char	*close, *colon = NULL, *start = address;
	size_t		n;
	char		*end;
	long		p;

	if (*address == '[')
	{
		close = strchr (address, ']');
		if (!close || (close[1] && close[1] != ':'))
			return false;
		start = address + 1;
		n = (size_t)(close - start);
		if (close[1])
			colon = close + 1;
	}
	else
	{
		colon = strchr (address, ':');
		if (colon && colon != strrchr (address, ':'))
			colon = NULL;		// an IPv6 address alone
		n = colon ? (size_t)(colon - address) : strlen (address);
	}
	if (!n || n > size - 1)
		return false;
	memcpy (host, start, n);
	host[n] = 0;
	*port = defaultport;
	if (colon)
	{
		p = strtol (colon + 1, &end, 10);
		if (*end || p <= 0 || p > 65535)
			return false;
		*port = (int)p;
	}
	return true;
}

bool SL_ParseAddress (const char *address, int defaultport, netadr_t *a)
{
	char	host[128];
	int		port;

	if (!SL_SplitAddress (address, defaultport, host, sizeof(host), &port))
		return false;
	memset (a, 0, sizeof(*a));
	if (!NET_ParseIP (host, a->ip))
		return false;
	a->type = NA_IP;
	a->port = (unsigned short)BigShort ((short)port);
	return true;
}

char *SL_AdrToBuf (netadr_t a, char *buf, size_t size)
{
	char	ip[48];

	// IPv6's in [ ], as its colons aren't the port's
	snprintf (buf, size, NET_IsIPv4 (a) ? "%s:%i" : "[%s]:%i", NET_IPToBuf (a.ip, ip, sizeof(ip)),
		(unsigned short)BigShort ((short)a.port));
	return buf;
}
