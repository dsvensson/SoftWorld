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
// cl_qtv.c  -- watching a game through QTV
//
// A QTV relay (or mvdsv itself) streams a game as an MVD over TCP. The client
// asks with a few header lines, the relay answers with its own and a blank
// line, and the MVD follows, played as a file is (cl_mvd.c) but read as it
// arrives. A level change is a new gamestate in the stream; the stream ends
// when the connection closes. qualia's request and reply handling: no
// QTV_EZQUAKE_EXT, so the relay sends the whole gamestate itself.

#include "cl_local.h"
#include "net_socket.h"

#define	QTV_PORT		27599
#define	QTV_TIMEOUT		5.0		// seconds to connect and to get the reply

enum { QTV_IDLE, QTV_CONNECTING, QTV_HEADER, QTV_STREAM };

static struct
{
	tcpsocket_t	*socket;
	int			state;
	double		started;		// when the connection or the request began
	char		source[128];	// the relay's stream, "" its default
	char		reply[8192];	// the reply header so far
	int			replylen;
} qtv;

/*
==================
CL_QTVStop

Closes the connection; the playback is stopped by whoever calls this
==================
*/
void CL_QTVStop (void)
{
	if (qtv.socket)
		TCP_Close (qtv.socket);
	qtv.socket = NULL;
	qtv.state = QTV_IDLE;
}

static void CL_QTVFail (const char *why)
{
	Con_Printf ("QTV: %s\n", why);
	CL_QTVStop ();
	if (cls.demoplayback)
		CL_StopPlayback ();
}

/*
==================
CL_QTVBegin

The relay said BEGIN: the stream after its header is an MVD
==================
*/
static void CL_QTVBegin (const byte *stream, int len)
{
	cls.demoplayback = true;
	cls.state = ca_demostart;
	Netchan_Setup (&cls.netchan, cls.net_from, 0, NS_CLIENT);
	CL_MVDStartStream ();
	if (len > 0)
		CL_MVDFeed (stream, len);
	qtv.state = QTV_STREAM;
	Con_Printf ("QTV: streaming\n");
}

/*
==================
CL_QTVReply

The reply header, once whole: "QTVSV 1", then "KEY: value" lines to a blank
line. BEGIN starts the stream; PRINT is shown; ERROR, TERROR, PERROR and AUTH
refuse. False while it isn't whole yet.
==================
*/
static bool CL_QTVReply (void)
{
	char	*line, *next, *end, *colon, *value;
	char	reason[512];
	bool	begin;

	// the header ends at a blank line; what follows is the stream
	end = NULL;
	for (line = qtv.reply ; (next = memchr (line, '\n', qtv.replylen - (line - qtv.reply))) ; line = next + 1)
	{
		if (next == line || (next == line + 1 && *line == '\r'))
		{
			end = next + 1;
			break;
		}
	}
	if (!end)
	{
		if (qtv.replylen >= 6 && strncmp (qtv.reply, "QTVSV ", 6))
			CL_QTVFail ("not a QTV relay");
		else if (qtv.replylen == (int)sizeof(qtv.reply))
			CL_QTVFail ("the reply header is too long");
		return false;
	}

	begin = false;
	reason[0] = 0;
	for (line = qtv.reply ; line < end ; line = next + 1)
	{
		next = memchr (line, '\n', end - line);
		*next = 0;
		if (next > line && next[-1] == '\r')
			next[-1] = 0;
		if (line == qtv.reply)
		{	// the version: 1 and 1.x
			if (strncmp (line, "QTVSV 1", 7) || (line[7] && line[7] != '.'))
			{
				CL_QTVFail (va("not a QTV relay (%s)", line));
				return true;
			}
			continue;
		}
		colon = strchr (line, ':');
		value = colon ? colon + 1 : line + strlen (line);
		if (colon)
			*colon = 0;
		while (*value == ' ')
			value++;
		if (!strcmp (line, "BEGIN"))
			begin = true;
		else if (!strcmp (line, "PRINT"))
			Con_Printf ("QTV: %s\n", value);
		else if (!strcmp (line, "ERROR") || !strcmp (line, "TERROR") || !strcmp (line, "PERROR"))
			Q_strncpyz (reason, value, sizeof(reason));
		else if (!strcmp (line, "AUTH") && !reason[0])
			Q_strncpyz (reason, "the stream wants a password", sizeof(reason));
	}

	if (reason[0] || !begin)
	{
		CL_QTVFail (reason[0] ? reason : "refused");
		return true;
	}
	CL_QTVBegin ((const byte *)end, qtv.replylen - (int)(end - qtv.reply));
	return true;
}

/*
==================
CL_QTVFrame

Once a frame, before the MVD is read: connects, asks, reads the reply, and
feeds what arrives to the MVD
==================
*/
void CL_QTVFrame (void)
{
	byte	buf[8192];
	char	request[MAX_INFO_STRING + 256];
	int		len;

	switch (qtv.state)
	{
	case QTV_CONNECTING:
		switch (TCP_State (qtv.socket))
		{
		case TCP_FAILED:
			CL_QTVFail ("couldn't connect");
			return;
		case TCP_CONNECTING:
			if (host.realtime - qtv.started > QTV_TIMEOUT)
				CL_QTVFail ("no answer");
			return;
		}
		snprintf (request, sizeof(request), "QTV\nVERSION: 1\n%s%s%sUSERINFO: %s\n\n",
			qtv.source[0] ? "SOURCE: " : "", qtv.source, qtv.source[0] ? "\n" : "", cls.userinfo);
		if (!TCP_Send (qtv.socket, request, (int)strlen (request)))
		{
			CL_QTVFail ("couldn't send the request");
			return;
		}
		qtv.state = QTV_HEADER;
		qtv.replylen = 0;
		qtv.started = host.realtime;
		return;

	case QTV_HEADER:
		len = TCP_Recv (qtv.socket, (byte *)qtv.reply + qtv.replylen, (int)sizeof(qtv.reply) - qtv.replylen);
		if (len < 0)
		{
			CL_QTVFail ("the relay closed the connection");
			return;
		}
		qtv.replylen += len;
		if (!CL_QTVReply () && qtv.state == QTV_HEADER && host.realtime - qtv.started > QTV_TIMEOUT)
			CL_QTVFail ("no reply");
		return;

	case QTV_STREAM:
		while ((len = TCP_Recv (qtv.socket, buf, sizeof(buf))) > 0)
			CL_MVDFeed (buf, len);
		if (len < 0)
		{	// what arrived plays to its end
			CL_MVDStreamClosed ();
			TCP_Close (qtv.socket);
			qtv.socket = NULL;
			qtv.state = QTV_IDLE;
		}
		return;
	}
}

static void CL_QTVConnect (const netadr_t *to)
{
	CL_Disconnect ();
	qtv.socket = TCP_Connect (to);
	if (!qtv.socket)
	{
		Con_Printf ("QTV: couldn't connect to %s\n", NET_AdrToString (*to));
		return;
	}
	qtv.state = QTV_CONNECTING;
	qtv.started = host.realtime;
	Con_Printf ("QTV: connecting to %s%s%s\n", qtv.source, qtv.source[0] ? "@" : "", NET_AdrToString (*to));
}

/*
==================
CL_QTVPlay_f

qtvplay [source@]host[:port], also as qw://[source@]host[:port]/qtvplay; in a
browser [source@]ws(s)://host[:port][/path] too
==================
*/
static void CL_QTVPlay_f (void)
{
	char		address[256], *at, *slash;
	const char	*arg, *url;
	netadr_t	to;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("qtvplay [stream@]host[:port]\n");
		return;
	}

	arg = Cmd_Argv (1);
	if (!Q_strncasecmp (arg, "qw://", 5))
		arg += 5;

	// a browser's: [stream@]ws(s)://host[:port][/path], the URL kept whole
	for (url = arg ; *url ; url++)
		if (!Q_strncasecmp (url, "ws://", 5) || !Q_strncasecmp (url, "wss://", 6))
			break;
	if (*url)
	{
		if (url - arg >= (int)sizeof(qtv.source))
		{
			Con_Printf ("Bad stream name\n");
			return;
		}
		Q_strncpyz (qtv.source, arg, sizeof(qtv.source));
		qtv.source[url - arg] = 0;
		at = strrchr (qtv.source, '@');
		if (at && !at[1])
			*at = 0;
		Q_strncpyz (address, url, sizeof(address));
		if (strpbrk (qtv.source, "\r\n") || !NET_StringToAdr (address, &to))
		{
			Con_Printf ("Bad address %s\n", address);
			return;
		}
		CL_QTVConnect (&to);
		return;
	}

	Q_strncpyz (address, arg, sizeof(address));
	slash = strchr (address, '/');
	if (slash)
		*slash = 0;

	// the stream is before the last @; relay chains have more of them
	qtv.source[0] = 0;
	at = strrchr (address, '@');
	if (at)
	{
		*at = 0;
		Q_strncpyz (qtv.source, address, sizeof(qtv.source));
		memmove (address, at + 1, strlen (at + 1) + 1);
	}
	if (strpbrk (qtv.source, "\r\n"))
	{
		Con_Printf ("Bad stream name\n");
		return;
	}
	// the relay's port unless given: host:port, [IPv6]:port, a URL's
	if (address[0] == '[' ? !strstr (address, "]:") : !strchr (address, ':'))
		Q_strncatz (address, va(":%i", QTV_PORT), sizeof(address));
	else if (address[0] != '[' && !strstr (address, "://") && strchr (address, ':') != strrchr (address, ':'))
		Q_strncpyz (address, va("[%s]:%i", address, QTV_PORT), sizeof(address));	// an IPv6 address alone
	if (!NET_StringToAdr (address, &to))
	{
		Con_Printf ("Bad address %s\n", address);
		return;
	}
	CL_QTVConnect (&to);
}

void CL_InitQTV (void)
{
	Cmd_AddCommand ("qtvplay", CL_QTVPlay_f,
		"Watches a game a QTV relay or mvdsv streams, on port 27599 unless one is given. "
		"Usage: qtvplay [stream@]host[:port], in a browser [stream@]ws(s)://host[:port][/path] too");
}
