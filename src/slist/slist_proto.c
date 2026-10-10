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
// slist_proto.c -- what the server list asks and what it is answered: a
// master's list, pings, statuses and a relay's table, and the serverinfo
//
// A master's query is no out-of-band packet: "c\n" and the NUL a C string
// ends with, which masters have been sent since the first client. Its reply
// is ff ff ff ff d \n and 6-byte records, an IPv4 address and a big-endian
// port, in datagrams without count or order. A ping is answered by the type
// byte 'l' alone, or out of band. A status (ff ff ff ff n) is the serverinfo
// and a line per client, in pieces of about 1400 bytes on a busy server, each
// with its own header: put together, then read. A relay's table shares the
// type byte: 8-byte records of an address, a little-endian port and the ms to
// it (the other order from a master's port).

#include "slist.h"

#include "q_endian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const byte	sl_masterquery[3] = {'c', '\n', 0};
const byte	sl_pingquery[6] = {0xff, 0xff, 0xff, 0xff, 'k', '\n'};
const byte	sl_statusquery[15] = {0xff, 0xff, 0xff, 0xff, 's', 't', 'a', 't', 'u', 's', ' ', '1', '5', '1', '\n'};
const byte	sl_tablequery[14] = {0xff, 0xff, 0xff, 0xff, 'p', 'i', 'n', 'g', 's', 't', 'a', 't', 'u', 's'};

#define	SL_SPECTATORFRAGS	-9999	// what a spectator's frags are given as

static bool SL_IsOOB (const byte *data, int length)
{
	return length > 4 && data[0] == 0xff && data[1] == 0xff && data[2] == 0xff && data[3] == 0xff;
}

int SL_ParseMasterReply (const byte *data, int length, void (*server) (void *ctx, netadr_t a), void *ctx)
{
	netadr_t	a = {.type = NA_IP};
	int			count = 0;

	if (length < 6 || !SL_IsOOB (data, length) || data[4] != 'd' || data[5] != '\n')
		return -1;
	for (data += 6, length -= 6 ; length >= 6 ; data += 6, length -= 6)
	{
		if (!data[4] && !data[5])
			break;		// how masters pad
		NET_SetIPv4 (&a, data);
		memcpy (&a.port, data + 4, 2);	// big-endian, as it stays
		server (ctx, a);
		count++;
	}
	return count;
}

bool SL_IsPingAck (const byte *data, int length)
{
	return (length > 0 && data[0] == 'l') || (SL_IsOOB (data, length) && data[4] == 'l');
}

const byte *SL_PrintPayload (const byte *data, int length, int *payloadlength)
{
	if (!SL_IsOOB (data, length) || data[4] != 'n')
		return NULL;
	*payloadlength = length - 5;
	return data + 5;
}

int SL_ParseTable (const byte *data, int length, slpeer_t *peers, int max)
{
	int		count = 0, ms, port;

	if (!SL_IsOOB (data, length) || data[4] != 'n')
		return -1;
	data += 5;
	length -= 5;
	// a serverinfo would read as a table of nonsense, and a table that isn't
	// whole records isn't one to guess at
	if (!length || data[0] == '\\' || length % 8)
		return -1;
	for ( ; length >= 8 ; data += 8, length -= 8)
	{
		port = data[4] | data[5] << 8;
		ms = data[6] | data[7] << 8;
		// negative as a short: unreachable from it too (0xffff), or as good as
		if (ms >= 0x8000 || !port || count == max)
			continue;
		peers[count].address = (netadr_t){.type = NA_IP};
		NET_SetIPv4 (&peers[count].address, data);
		peers[count].address.port = (unsigned short)BigShort ((short)port);
		peers[count].ms = ms;
		count++;
	}
	return count;
}

/*
==============================================================================

STATUS

==============================================================================
*/

typedef struct
{
	const char	*p, *end;
} slfields_t;

static void SL_SkipSpaces (slfields_t *f)
{
	while (f->p < f->end && *f->p == ' ')
		f->p++;
}

static bool SL_Int (slfields_t *f, int *value)
{
	char	text[16];
	size_t	n = 0;
	char	*end;

	SL_SkipSpaces (f);
	while (f->p < f->end && *f->p != ' ')
	{
		if (n < sizeof(text) - 1)
			text[n++] = *f->p;
		f->p++;
	}
	text[n] = 0;
	if (!n)
		return false;
	*value = (int)strtol (text, &end, 10);
	return !*end;
}

static bool SL_Quoted (slfields_t *f, char *out, size_t size)
{
	const char	*close;
	size_t		n;

	SL_SkipSpaces (f);
	if (f->p == f->end || *f->p != '"')
		return false;
	close = memchr (f->p + 1, '"', (size_t)(f->end - f->p - 1));
	if (!close)
		return false;
	n = (size_t)(close - f->p - 1);
	if (n > size - 1)
		n = size - 1;
	memcpy (out, f->p + 1, n);
	out[n] = 0;
	f->p = close + 1;
	return true;
}

// a client's line: userid frags time ping "name" "skin" top bottom ["team"
// ["type"]], the type mvdsv's b for a bot, h for a human
static bool SL_ParsePlayer (const char *line, const char *end, slplayer_t *p)
{
	slfields_t	f = {line, end};
	size_t		n;
	char		type[4];

	memset (p, 0, sizeof(*p));
	if (!SL_Int (&f, &p->userid) || !SL_Int (&f, &p->frags) || !SL_Int (&f, &p->time) || !SL_Int (&f, &p->ping)
		|| !SL_Quoted (&f, p->name, sizeof(p->name)) || !SL_Quoted (&f, p->skin, sizeof(p->skin))
		|| !SL_Int (&f, &p->topcolor) || !SL_Int (&f, &p->bottomcolor))
		return false;
	// a server that ignored the team bit ends here, one older than mvdsv's
	// client type bit after the team
	if (SL_Quoted (&f, p->team, sizeof(p->team)) && SL_Quoted (&f, type, sizeof(type)))
		p->bot = type[0] == 'b';

	// a spectator's ping comes negative, its frags as SL_SPECTATORFRAGS, and its
	// name in \s\ (older servers: name(s), only believed of a spectator's line)
	p->spectator = p->ping < 0 || p->frags == SL_SPECTATORFRAGS;
	if (p->ping < 0)
		p->ping = -p->ping;
	if (!strncmp (p->name, "\\s\\", 3))
	{
		memmove (p->name, p->name + 3, strlen (p->name + 3) + 1);
		p->spectator = true;
	}
	else if (p->spectator && (n = strlen (p->name)) >= 3 && !strcmp (p->name + n - 3, "(s)"))
		p->name[n - 3] = 0;
	return true;
}

void SL_ParseStatus (const char *text, size_t length, char *info, size_t infosize,
	slplayer_t *roster, int max, int *count)
{
	const char	*end, *line, *eol, *last;
	size_t		n;

	// servers end it with a NUL, a newline, or both; counted down, as a length
	// is what the trailing ones come off and what memchr is given
	while (length && (text[length - 1] == 0 || text[length - 1] == '\n'
		|| text[length - 1] == '\r' || text[length - 1] == ' '))
		length--;
	end = text + length;

	*count = 0;
	eol = memchr (text, '\n', length);
	if (!eol)
		eol = end;
	last = eol > text && eol[-1] == '\r' ? eol - 1 : eol;
	n = (size_t)(last - text);
	if (n > infosize - 1)
		n = infosize - 1;
	memcpy (info, text, n);
	info[n] = 0;

	for (line = eol + 1 ; line < end && *count < max ; line = eol + 1)
	{
		eol = memchr (line, '\n', (size_t)(end - line));
		if (!eol)
			eol = end;
		last = eol > line && eol[-1] == '\r' ? eol - 1 : eol;
		if (SL_ParsePlayer (line, last, &roster[*count]))
			(*count)++;
	}
}

/*
==============================================================================

SERVERINFO

==============================================================================
*/

const char *SL_InfoValue (const char *info, const char *key, char *value, size_t size)
{
	size_t		keylength = strlen (key), n;
	const char	*k, *v, *next;

	*value = 0;
	for (k = info ; *k ; k = next)
	{
		if (*k == '\\')
			k++;
		v = strchr (k, '\\');
		if (!v)
			break;
		v++;
		next = strchr (v, '\\');
		if (!next)
			next = v + strlen (v);
		if ((size_t)(v - 1 - k) == keylength && !strncmp (k, key, keylength))
		{
			n = (size_t)(next - v);
			if (n > size - 1)
				n = size - 1;
			memcpy (value, v, n);
			value[n] = 0;
			break;
		}
	}
	return value;
}

int SL_InfoInt (const char *info, const char *key)
{
	char	value[32];

	return atoi (SL_InfoValue (info, key, value, sizeof(value)));
}

int SL_Ping (const slserver_t *s)
{
	if (s->routeping >= 0 && (s->ping < 0 || s->routeping < s->ping))
		return s->routeping;
	return s->ping;
}

bool SL_Relayed (const slserver_t *s)
{
	return s->routeping >= 0 && (s->ping < 0 || s->routeping < s->ping);
}

int SL_MaxClients (const slserver_t *s)
{
	int		max = SL_InfoInt (s->info, "maxclients");

	return max > 0 ? max : 0;
}
