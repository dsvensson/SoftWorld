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
// net_ws_web.c -- the network in a browser page: WebSockets and WebRTC
// (net_ws_web.js)
//
// A page has no UDP and no DNS, so its packets go to servers by URL (NA_URL):
// ws://host[:port][/path] or wss://, and a bare host[:port] as ws:// (wss://
// on an https page, which may open nothing else), its port QuakeWorld's
// unless given. The client's "UDP socket" sends a packet as a WebSocket's
// binary message, opening one to a server on the first packet to it (the
// subprotocol "fteqw", as FTE's clients ask), and takes the messages of all
// of them. The browser connects and resolves; a connection that closes is
// opened again on a packet at most every 5 s, as the client asks for a
// challenge. QTV's streams are WebSockets too (without a subprotocol, as a
// bridge like websockify takes them). A page can't listen: its server has
// only its own client, over the loopback.
//
// rtc://broker[:port]/room and rtcs:// (the broker over TLS) go over the
// browser's WebRTC instead, as the native programs' net_rtc.c does: the
// broker (port 27950 unless given) introduces the page to the server, and a
// packet is then a message on an unordered data channel that never sends
// again, as UDP's.

#include "mem.h"
#include "net_socket.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"
#include "web_local.h"

#include <stdio.h>
#include <string.h>

// net_ws_web.js
int		web_ws_open (const char *url, const char *protocol, bool stream);
int		web_rtc_open (const char *broker, const char *stun);
bool	web_ws_send (int id, const void *data, int length);
int		web_ws_state (int id, int *code);
int		web_ws_recv (byte *buf, int max, int *id);
int		web_ws_read (int id, byte *buf, int max);
void	web_ws_close (int id);
bool	web_page_secure (void);

enum { WS_CONNECTING, WS_OPEN, WS_CLOSED };

#define	MAX_URLS		256
#define	MAX_CONNS		32
#define	REOPEN_TIME		5.0			// seconds before a closed connection is opened again
#define	IDLE_TIME		30.0		// seconds without a packet sent before one is closed
#define	RTC_BROKERPORT	27950		// FTE's (dpmaster's), unless the URL gives one
#define	RTC_PROTOCOL	"FTE-Quake"	// what FTE's brokers know Quake's games by

// the servers by URL, an NA_URL's ip[0] and ip[1] their number
typedef struct
{
	char	scheme[8];			// "ws", "wss", "rtc" or "rtcs"
	char	host[128];
	char	path[128];			// "" or from the '/' (WebRTC's room)
} weburl_t;

static weburl_t	net_urls[MAX_URLS];
static int		net_numurls;

typedef struct
{
	bool		used;
	netadr_t	to;				// NA_URL
	int			id;				// the WebSocket's, -1 while none is open
	double		sent;			// when a packet was last sent
	double		opened;			// when it was last opened
	bool		toldclosed;
} webconn_t;

static webconn_t	net_conns[MAX_CONNS];

struct udpsocket_s
{
	int		unused;
};

struct tcpsocket_s
{
	int		id;
};

static udpsocket_t	net_socket;		// the client's

/*
===============================================================================

URLS

===============================================================================
*/

static int URL_Index (netadr_t a)
{
	return a.ip[0] | a.ip[1] << 8;
}

static bool URL_HostChar (int c, bool ipv6)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
		return true;
	return ipv6 && (c == ':' || c == '[' || c == ']');
}

static bool URL_IsRTC (const weburl_t *url)
{
	return !strncmp (url->scheme, "rtc", 3);
}

// a URL's own port: the scheme's, or a WebRTC broker's
static int URL_DefaultPort (const weburl_t *url)
{
	if (URL_IsRTC (url))
		return RTC_BROKERPORT;
	return !strcmp (url->scheme, "wss") ? 443 : 80;
}

/*
=============
UDP_ResolveURL

ws://host[:port][/path], wss://, or a bare host[:port] as ws:// (wss:// on an
https page); rtc://broker[:port]/room or rtcs://, over WebRTC. A URL's port
is the URL's (80, 443, or the broker's 27950) unless given, a bare host's
QuakeWorld's (left 0 here for the caller's). No credentials, nothing a
userinfo or a request would take for more.
=============
*/
bool UDP_ResolveURL (const char *s, netadr_t *a)
{
	weburl_t	url = {0};
	const char	*p = s, *host, *end;
	int			port = 0, i;
	bool		bare = false;

	if (!Q_strncasecmp (p, "wss://", 6))
	{
		strcpy (url.scheme, "wss");
		p += 6;
	}
	else if (!Q_strncasecmp (p, "ws://", 5))
	{
		strcpy (url.scheme, "ws");
		p += 5;
	}
	else if (!Q_strncasecmp (p, "rtcs://", 7))
	{
		strcpy (url.scheme, "rtcs");
		p += 7;
	}
	else if (!Q_strncasecmp (p, "rtc://", 6))
	{
		strcpy (url.scheme, "rtc");
		p += 6;
	}
	else if (strstr (p, "://"))
		return false;
	else
	{
		strcpy (url.scheme, web_page_secure () ? "wss" : "ws");
		bare = true;
	}

	// the host, a name or an address, [ ] around IPv6's
	host = p;
	if (*p == '[')
	{
		end = strchr (p, ']');
		if (!end)
			return false;
		end++;
	}
	else
		for (end = p ; *end && *end != ':' && *end != '/' ; end++)
			;
	if (end == host || end - host >= (int)sizeof(url.host))
		return false;
	for (p = host ; p < end ; p++)
		if (!URL_HostChar (*p, *host == '['))
			return false;		// credentials (@), spaces, quotes
	for (i = 0 ; host + i < end ; i++)
		url.host[i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] + 'a' - 'A' : host[i]);
	p = end;

	if (*p == ':')
	{
		for (p++ ; *p >= '0' && *p <= '9' ; p++)
			port = port * 10 + *p - '0';
		if (port <= 0 || port > 65535)
			return false;
	}
	else if (!bare)
		port = URL_DefaultPort (&url);

	if (*p == '/')
	{
		if (bare || strlen (p) >= sizeof(url.path) || strpbrk (p, " \t\r\n\"\\;"))
			return false;
		if (URL_IsRTC (&url) && (!p[1] || strpbrk (p, "?#")))
			return false;
		strcpy (url.path, p);
	}
	else if (*p || URL_IsRTC (&url))
		return false;		// WebRTC's room is needed

	// the URL's number, the same for the same server
	for (i = 0 ; i < net_numurls ; i++)
		if (!strcmp (net_urls[i].scheme, url.scheme) && !strcmp (net_urls[i].host, url.host)
			&& !strcmp (net_urls[i].path, url.path))
			break;
	if (i == net_numurls)
	{
		if (net_numurls == MAX_URLS)
			return false;
		net_urls[net_numurls++] = url;
	}

	memset (a, 0, sizeof(*a));
	a->type = NA_URL;
	a->ip[0] = (byte)i;
	a->ip[1] = (byte)(i >> 8);
	a->port = (unsigned short)BigShort ((short)port);
	return true;
}

// scheme://host[:port]/path, the port left out where the scheme's own or
// none, or when port is false
const char *UDP_URLToString (netadr_t a, bool port)
{
	static char	s[2][320];
	static int	which;
	weburl_t	*url;
	int			number = (unsigned short)BigShort ((short)a.port);
	char		*out = s[which ^= 1];

	if (a.type != NA_URL || URL_Index (a) >= net_numurls)
		return "";
	url = &net_urls[URL_Index (a)];
	if (port && number && number != URL_DefaultPort (url))
		snprintf (out, sizeof(s[0]), "%s://%s:%i%s", url->scheme, url->host, number, url->path);
	else
		snprintf (out, sizeof(s[0]), "%s://%s%s", url->scheme, url->host, url->path);
	return out;
}

/*
===============================================================================

DATAGRAMS

===============================================================================
*/

void UDP_Init (void)
{
}

void UDP_Shutdown (void)
{
	int		i;

	for (i = 0 ; i < MAX_CONNS ; i++)
		if (net_conns[i].used && net_conns[i].id >= 0)
			web_ws_close (net_conns[i].id);
	memset (net_conns, 0, sizeof(net_conns));
}

// a page's socket goes anywhere, from no port of its own
udpsocket_t *UDP_Open (int port)
{
	return port == PORT_ANY ? &net_socket : NULL;
}

void UDP_Close (udpsocket_t *s)
{
	(void)s;
	UDP_Shutdown ();
}

netadr_t UDP_Address (udpsocket_t *s)
{
	netadr_t	none = {.type = NA_INVALID};

	(void)s;
	return none;
}

// no DNS in a page: hosts are a URL's (UDP_ResolveURL)
bool UDP_Resolve (const char *host, bool ipv6, netadr_t *a)
{
	(void)host;
	(void)ipv6;
	memset (a, 0, sizeof(*a));
	return false;
}

static webconn_t *NET_FindConn (const netadr_t *to)
{
	int		i;

	for (i = 0 ; i < MAX_CONNS ; i++)
		if (net_conns[i].used && NET_CompareAdr (net_conns[i].to, *to))
			return &net_conns[i];
	return NULL;
}

// the connections to servers no longer sent to, closed
static void NET_CloseIdle (double now)
{
	int		i;

	for (i = 0 ; i < MAX_CONNS ; i++)
		if (net_conns[i].used && now - net_conns[i].sent > IDLE_TIME)
		{
			if (net_conns[i].id >= 0)
				web_ws_close (net_conns[i].id);
			net_conns[i].used = false;
		}
}

static void NET_Open (webconn_t *c, double now)
{
	weburl_t	*url = &net_urls[URL_Index (c->to)];
	char		address[400], stun[300];
	int			port = (unsigned short)BigShort ((short)c->to.port);

	if (URL_IsRTC (url))
	{
		// the broker's WebSocket at /FTE-Quake/<room>, and the broker the
		// first STUN server
		snprintf (address, sizeof(address), "%s://%s:%i/" RTC_PROTOCOL "%s", url->scheme[3] ? "wss" : "ws",
			url->host, port, url->path);
		snprintf (stun, sizeof(stun), "stun:%s:%i", url->host, port);
		c->id = web_rtc_open (address, stun);
	}
	else
	{
		snprintf (address, sizeof(address), "%s", UDP_URLToString (c->to, true));
		c->id = web_ws_open (address, "fteqw", false);
	}
	c->opened = now;
	c->toldclosed = false;
	if (c->id < 0 && URL_IsRTC (url) && !url->scheme[3] && web_page_secure ())
		Con_Printf ("Can't open %s from an https page: rtcs:// it must be\n", UDP_URLToString (c->to, true));
	else if (c->id < 0)
		Con_Printf ("Can't open %s%s\n", address, !strncmp (address, "ws:", 3) && web_page_secure ()
			? " from an https page: wss:// it must be" : URL_IsRTC (url) ? ": no WebRTC in this browser" : "");
}

// why a connection closed: a WebSocket's code, or net_ws_web.js's for
// WebRTC's
static const char *NET_ClosedWhy (const weburl_t *url, int code)
{
	if (!URL_IsRTC (url))
		return code == 1006 ? ": couldn't connect, or the connection was lost" : "";
	switch (code)
	{
	case 4000:	return ": the broker lost the server";
	case 4001:	return ": the broker refused the room";
	case 4002:	return ": the peers couldn't reach each other";
	case 4003:	return ": the connection was lost";
	case 4004:	return ": no connection in time";
	default:	return ": the broker's connection closed";
	}
}

void UDP_Send (udpsocket_t *s, const void *data, int length, const netadr_t *to)
{
	double		now = Sys_DoubleTime ();
	webconn_t	*c;
	int			i, code;

	(void)s;
	if (to->type != NA_URL)
		return;
	NET_CloseIdle (now);
	c = NET_FindConn (to);
	if (!c)
	{
		for (i = 0 ; i < MAX_CONNS && net_conns[i].used ; i++)
			;
		if (i == MAX_CONNS)
			return;
		c = &net_conns[i];
		memset (c, 0, sizeof(*c));
		c->used = true;
		c->to = *to;
		NET_Open (c, now);
	}
	c->sent = now;

	// closed: told once, and opened again in a while
	if (c->id >= 0 && web_ws_state (c->id, &code) == WS_CLOSED)
	{
		if (!c->toldclosed)
			Con_Printf ("%s to %s closed (%i%s)\n", URL_IsRTC (&net_urls[URL_Index (*to)]) ? "WebRTC" : "WebSocket",
				UDP_URLToString (*to, true), code, NET_ClosedWhy (&net_urls[URL_Index (*to)], code));
		c->toldclosed = true;
		web_ws_close (c->id);
		c->id = -1;
	}
	if (c->id < 0)
	{
		if (now - c->opened < REOPEN_TIME)
			return;
		NET_Open (c, now);
		if (c->id < 0)
			return;
	}
	web_ws_send (c->id, data, length);
}

int UDP_Recv (udpsocket_t *s, byte *buf, int maxlen, netadr_t *from)
{
	int		length, id, i;

	(void)s;
	for (;;)
	{
		length = web_ws_recv (buf, maxlen, &id);
		if (!length)
			return 0;
		for (i = 0 ; i < MAX_CONNS ; i++)
			if (net_conns[i].used && net_conns[i].id == id)
				break;
		if (i == MAX_CONNS)
			continue;		// a connection closed since
		*from = net_conns[i].to;
		if (length < 0)
		{
			Con_Printf ("Oversize packet from %s\n", NET_AdrToString (*from));
			continue;
		}
		return length;
	}
}

/*
===============================================================================

STREAMS (QTV)

===============================================================================
*/

tcpsocket_t *TCP_Connect (const netadr_t *to)
{
	tcpsocket_t	*s;
	int			id;

	if (to->type != NA_URL || URL_IsRTC (&net_urls[URL_Index (*to)]))
		return NULL;
	id = web_ws_open (UDP_URLToString (*to, true), NULL, true);
	if (id < 0)
		return NULL;
	s = Mem_Calloc (1, sizeof(*s));
	s->id = id;
	return s;
}

void TCP_Close (tcpsocket_t *s)
{
	web_ws_close (s->id);
	Mem_Free (s);
}

int TCP_State (tcpsocket_t *s)
{
	int		code;

	switch (web_ws_state (s->id, &code))
	{
	case WS_CONNECTING:	return TCP_CONNECTING;
	case WS_OPEN:		return TCP_OPEN;
	default:			return TCP_FAILED;
	}
}

int TCP_Recv (tcpsocket_t *s, byte *buf, int maxlen)
{
	return web_ws_read (s->id, buf, maxlen);
}

bool TCP_Send (tcpsocket_t *s, const void *data, int length)
{
	return web_ws_send (s->id, data, length);
}

int TCP_Write (tcpsocket_t *s, const void *data, int length)
{
	return web_ws_send (s->id, data, length) ? length : -1;
}

// a page can't listen
tcplisten_t *TCP_Listen (int port)
{
	(void)port;
	return NULL;
}

void TCP_CloseListen (tcplisten_t *l)
{
	(void)l;
}

tcpsocket_t *TCP_Accept (tcplisten_t *l, netadr_t *from)
{
	(void)l;
	(void)from;
	return NULL;
}
