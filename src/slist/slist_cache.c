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
// slist_cache.c -- last time's servers, kept so the next start shows them
// before a packet is sent, unconfirmed, while the scan checks them in place
//
//	qualia-slist 1
//	source 0 "QuakeServers.net"
//	81.66.1.102:27500 ping=34 seen=1754450123 players=5 specs=2 from=0 \hostname\Fool's DM6\map\dm6
//
// qualia's format: a server a line, written as bytes (a hostname is Quake's
// characters), its scalars before the first backslash (fields not known are
// skipped, so a later version can add some), its serverinfo from there as it
// came. Rosters aren't kept: a stale one is worse than none. The source lines
// and from= are SoftWorld's, which qualia skips: the sources that named a
// server, by name, so it is listed under them before they answer. A line
// that doesn't read costs only itself, and another header the whole file.

#include "slist_local.h"

#include "q_string.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_CACHEMAGIC	"qualia-slist 1"
#define SL_MAXCACHE		(16 << 20)

// the file whole, NUL-terminated; NULL where it doesn't read
static char *SL_ReadFile (const char *path, size_t *length)
{
	FILE	*f = fopen (path, "rb");
	char	*data = NULL;
	long	size;

	if (!f)
		return NULL;
	if (!fseek (f, 0, SEEK_END) && (size = ftell (f)) >= 0 && size < SL_MAXCACHE && !fseek (f, 0, SEEK_SET)
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

// the next field of a line, ended; NULL at its end
static char *SL_NextField (char **p)
{
	char	*start;

	while (**p == ' ' || **p == '\t')
		(*p)++;
	if (!**p)
		return NULL;
	start = *p;
	while (**p && **p != ' ' && **p != '\t')
		(*p)++;
	if (**p)
		*(*p)++ = 0;
	return start;
}

// a line's scalars and serverinfo into s; false if its address doesn't read
static bool SL_ParseCacheLine (char *line, const int *sourcemap, slserver_t *s)
{
	char		*info = strchr (line, '\\'), *field, *value, *end;
	long long	n;

	if (info)
		*info = 0;
	memset (s, 0, sizeof(*s));
	s->info = sl_noinfo;
	s->ping = s->routeping = -1;
	s->state = SL_CACHED;

	field = SL_NextField (&line);
	if (!field || !SL_ParseAddress (field, SL_SERVERPORT, &s->address))
		return false;
	while ((field = SL_NextField (&line)))
	{
		if (!(value = strchr (field, '=')))
			continue;
		*value++ = 0;
		if (!Q_strcasecmp (field, "from"))
		{
			while (*value)
			{
				n = strtoll (value, &end, 10);
				if (end == value)
					break;
				if (n >= 0 && n < SL_MAXSOURCES && sourcemap[n] >= 0)
					s->sources |= 1ull << sourcemap[n];
				value = *end == ',' ? end + 1 : end;
			}
			continue;
		}
		n = strtoll (value, &end, 10);
		if (*end || n < 0)
			continue;
		if (!Q_strcasecmp (field, "ping"))
			s->ping = n < 0xffff ? (int)n : 0xffff;
		else if (!Q_strcasecmp (field, "seen"))
			s->seen = n;
		else if (!Q_strcasecmp (field, "players"))
			s->players = n < 255 ? (int)n : 255;
		else if (!Q_strcasecmp (field, "specs"))
			s->spectators = n < 255 ? (int)n : 255;
	}
	if (info)
	{
		*info = '\\';
		SL_SetInfo (s, info, strlen (info));
	}
	return true;
}

int SL_LoadCache (const char *path, const slsource_t *sources, int numsources, slserver_t **servers)
{
	char		*data, *line, *eol, *end, name[64], *quote;
	int			sourcemap[SL_MAXSOURCES], count = 0, max = 0, i;
	long		n;
	size_t		length;
	slserver_t	*list = NULL, *grown;

	*servers = NULL;
	if (!(data = SL_ReadFile (path, &length)))
		return 0;
	for (i = 0 ; i < SL_MAXSOURCES ; i++)
		sourcemap[i] = -1;

	for (line = data ; line < data + length && count < SL_MAXSERVERS ; line = eol + 1)
	{
		if (!(eol = strchr (line, '\n')))
			eol = data + length;
		*eol = 0;
		for (end = eol ; end > line && isspace ((byte)end[-1]) ; end--)
			;
		*end = 0;

		if (line == data)
		{
			if (strcmp (line, SL_CACHEMAGIC))
				break;		// not one we wrote, or not this version: no cache
			continue;
		}
		if (!*line)
			continue;
		if (!strncmp (line, "source ", 7))
		{
			n = strtol (line + 7, &end, 10);
			if (n < 0 || n >= SL_MAXSOURCES || !(quote = strchr (end, '"')) || !(end = strchr (quote + 1, '"')))
				continue;
			Q_strncpyz (name, quote + 1, (size_t)(end - quote) < sizeof(name) ? (size_t)(end - quote) : sizeof(name));
			for (i = 0 ; i < numsources ; i++)
				if (!strcmp (sources[i].name, name))
					sourcemap[n] = i;
			continue;
		}

		if (count == max)
		{
			max = max ? max * 2 : 256;
			if (!(grown = realloc (list, (size_t)max * sizeof(*list))))
				break;
			list = grown;
		}
		if (SL_ParseCacheLine (line, sourcemap, &list[count]))
			count++;
	}
	free (data);
	if (!count)
	{
		free (list);
		return 0;
	}
	*servers = list;
	return count;
}

bool SL_SaveCache (const char *path, const slserver_t *servers, int count, const slsource_t *sources, int numsources)
{
	char	temp[1024], address[64];
	FILE	*f;
	bool	ok, comma;
	int		i, j;

	snprintf (temp, sizeof(temp), "%s.tmp", path);
	if (!(f = fopen (temp, "wb")))
		return false;
	fprintf (f, SL_CACHEMAGIC "\n");
	for (i = 0 ; i < numsources && i < SL_MAXSOURCES ; i++)
		fprintf (f, "source %i \"%s\"\n", i, sources[i].name);
	for (i = 0 ; i < count && i < SL_MAXSERVERS ; i++)
	{
		fprintf (f, "%s ", SL_AdrToBuf (servers[i].address, address, sizeof(address)));
		if (servers[i].ping >= 0)
			fprintf (f, "ping=%i ", servers[i].ping);
		if (servers[i].seen)
			fprintf (f, "seen=%lld ", (long long)servers[i].seen);
		fprintf (f, "players=%i specs=%i ", servers[i].players, servers[i].spectators);
		if (servers[i].sources)
		{
			fprintf (f, "from=");
			for (j = 0, comma = false ; j < numsources && j < SL_MAXSOURCES ; j++)
				if (servers[i].sources & (1ull << j))
				{
					fprintf (f, comma ? ",%i" : "%i", j);
					comma = true;
				}
			fprintf (f, " ");
		}
		fprintf (f, "%s\n", servers[i].info);
	}
	ok = !ferror (f);
	if (fclose (f))
		ok = false;
	// built beside the old one and moved over it, so a run that dies on the
	// way leaves the old whole, not half a new one the next start believes
	if (ok)
	{
		remove (path);
		ok = !rename (temp, path);
	}
	if (!ok)
		remove (temp);
	return ok;
}

void SL_FreeServers (slserver_t *servers, int count)
{
	int		i;

	for (i = 0 ; i < count ; i++)
		SL_ClearServer (&servers[i]);
	free (servers);
}
