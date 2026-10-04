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
// net_main.c -- the client's and the server's sockets, and the loopback between them

#include "cvar.h"
#include "net.h"
#include "net_rtc.h"
#include "net_socket.h"
#include "net_ws.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
=============================================================================

LOOPBACK

Packets between the client and the server in the same process. Each end
has a ring of the packets sent to it; a full ring drops the oldest, as a
network would.

=============================================================================
*/

#define	MAX_LOOPBACK	16		// packets waiting, per end

typedef struct
{
	byte	data[MAX_UDP_PACKET];
	int		datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t	msgs[MAX_LOOPBACK];
	int			get, send;
} loopback_t;

static loopback_t	loopbacks[2];		// indexed by the receiving end

static bool Loop_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg)
{
	loopback_t	*loop;
	loopmsg_t	*m;

	loop = &loopbacks[sock];
	if (loop->send - loop->get > MAX_LOOPBACK)
		loop->get = loop->send - MAX_LOOPBACK;
	if (loop->get >= loop->send)
		return false;

	m = &loop->msgs[loop->get & (MAX_LOOPBACK-1)];
	loop->get++;

	if (m->datalen > msg->maxsize)
		return false;
	memcpy (msg->data, m->data, m->datalen);
	msg->cursize = m->datalen;
	memset (from, 0, sizeof(*from));
	from->type = NA_LOOPBACK;
	return true;
}

static void Loop_SendPacket (netsrc_t sock, int length, const void *data)
{
	loopback_t	*loop;
	loopmsg_t	*m;

	if (length > MAX_UDP_PACKET)
		return;
	loop = &loopbacks[sock ^ 1];
	m = &loop->msgs[loop->send & (MAX_LOOPBACK-1)];
	loop->send++;
	memcpy (m->data, data, length);
	m->datalen = length;
}

/*
=============================================================================

SOCKETS

=============================================================================
*/

static udpsocket_t	*net_sockets[2];	// indexed by end

cvar_t	password = {.name = "password", .string = "", .userinfo = true,
	.description = "The password sent to join a server; on a server, the one players must send "
		"(empty or none: no password)."};
cvar_t	rcon_password = {.name = "rcon_password", .string = "",
	.description = "The password rcon sends with remote commands; a server runs them only with its own, "
		"and never when empty."};
// most QuakeWorld servers listen on IPv4 alone, though their host has IPv6
static cvar_t	net_prefer_ipv6 = {.name = "net_prefer_ipv6", .string = "0",
	.description = "Which address a name with both IPv4's and IPv6's is reached at. Most QuakeWorld servers "
		"listen on IPv4 alone.",
	.values = (const cvar_value_t[]){{"0", "IPv4's"}, {"1", "IPv6's"}, {0}}};
// FTE's master, a broker for its clients and servers
static cvar_t	net_webrtc_broker = {.name = "net_webrtc_broker", .string = "rtcs://master.frag-net.com",
	.description = "The WebRTC broker a public server (sv_public) hosts its room at, and invitation codes "
		"(connect 1234-5678) are rooms at: rtc://broker[:port], or rtcs:// over TLS; the port 27950 unless given."};

/*
====================
NET_Init / NET_Shutdown
====================
*/
void NET_Init (void)
{
	UDP_Init ();
	RTC_Init ();
	Netchan_Init ();
	Cvar_RegisterVariable (&password);
	Cvar_RegisterVariable (&rcon_password);
	Cvar_RegisterVariable (&net_prefer_ipv6);
	Cvar_RegisterVariable (&net_webrtc_broker);
}

void NET_Shutdown (void)
{
	NET_CloseSocket (NS_CLIENT);
	NET_CloseSocket (NS_SERVER);
	memset (loopbacks, 0, sizeof(loopbacks));
	RTC_Shutdown ();
	UDP_Shutdown ();
}

/*
====================
NET_OpenSocket
====================
*/
bool NET_OpenSocket (netsrc_t sock, int port)
{
	NET_CloseSocket (sock);
	net_sockets[sock] = UDP_Open (port);
	if (!net_sockets[sock])
		return false;
	// a browser's has no address: its packets go to URLs
	if (UDP_Address (net_sockets[sock]).type == NA_IP)
		Con_Printf ("%s UDP on %s\n", sock == NS_SERVER ? "Server" : "Client",
			NET_AdrToString (UDP_Address (net_sockets[sock])));
	return true;
}

void NET_CloseSocket (netsrc_t sock)
{
	if (net_sockets[sock])
		UDP_Close (net_sockets[sock]);
	net_sockets[sock] = NULL;
	if (sock == NS_SERVER)
	{
		NET_CloseTCP ();
		RTC_Host (NULL, NULL);
	}
}

bool NET_HostRTC (const char *room)
{
	return RTC_Host (room ? net_webrtc_broker.string : NULL, room);
}

bool NET_RoomURL (const char *broker, const char *room, char *url, int size)
{
	const char	*host, *slash;
	int			length = (int)strlen (broker);

	if (!Q_strncasecmp (broker, "rtcs://", 7))
		host = broker + 7;
	else if (!Q_strncasecmp (broker, "rtc://", 6))
		host = broker + 6;
	else
		return false;
	// the broker's address alone, a '/' after it or none
	if (length && broker[length - 1] == '/')
		length--;
	slash = strchr (host, '/');
	if (!*host || (slash && slash - broker < length))
		return false;
	return snprintf (url, (size_t)size, "%.*s/%s", length, broker, room) < size;
}

// an invitation code, 1234-5678, as the room it is (its eight digits)
static bool NET_InvitationRoom (const char *s, char *room)
{
	int		i, n = 0;

	for (i = 0 ; i < 9 ; i++)
		if (i == 4 ? s[i] != '-' : (s[i] < '0' || s[i] > '9'))
			return false;
		else if (i != 4)
			room[n++] = s[i];
	room[n] = 0;
	return !s[9];
}

void NET_RTCInfo (const char *info)
{
	RTC_HostInfo (info);
}

netadr_t NET_SocketAddress (netsrc_t sock)
{
	netadr_t	none = {.type = NA_INVALID};

	return net_sockets[sock] ? UDP_Address (net_sockets[sock]) : none;
}

/*
====================
NET_GetPacket / NET_SendPacket
====================
*/
bool NET_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg)
{
	int		length;

	if (Loop_GetPacket (sock, from, msg))
		return true;
	if (net_sockets[sock])
	{
		length = UDP_Recv (net_sockets[sock], msg->data, msg->maxsize, from);
		if (length > 0)
		{
			msg->cursize = length;
			return true;
		}
	}
	// the server's browsers' clients, and the peers over WebRTC (the server's
	// clients, or the client's servers)
	if (sock == NS_SERVER && WS_GetPacket (from, msg))
		return true;
	return RTC_GetPacket (sock, from, msg);
}

void NET_SendPacket (netsrc_t sock, int length, const void *data, netadr_t to)
{
	if (to.type == NA_LOOPBACK)
		Loop_SendPacket (sock, length, data);
	else if (to.type == NA_WS)
	{
		if (sock == NS_SERVER)
			WS_SendPacket (data, length, &to);
	}
	else if (to.type == NA_RTC || to.type == NA_RTCCLIENT)
		RTC_SendPacket (sock, data, length, &to);
	else if ((to.type == NA_IP || to.type == NA_URL) && net_sockets[sock])
		UDP_Send (net_sockets[sock], data, length, &to);
}

/*
=============================================================================

ADDRESSES

=============================================================================
*/

int NET_FragmentMTU (netadr_t a)
{
	// as FTE's web client asks: the packet, and the SCTP and DTLS around it,
	// in one UDP datagram under 1500 bytes
	if (a.type == NA_RTC || (a.type == NA_URL && !Q_strncasecmp (UDP_URLToString (a, false), "rtc", 3)))
		return 1384;
	return 0;
}

bool NET_CompareBaseAdr (netadr_t a, netadr_t b)
{
	if (a.type != b.type)
		return false;
	if (a.type == NA_LOOPBACK)
		return true;
	return memcmp (a.ip, b.ip, sizeof(a.ip)) == 0;
}

bool NET_CompareAdr (netadr_t a, netadr_t b)
{
	return NET_CompareBaseAdr (a, b) && (a.type == NA_LOOPBACK || a.port == b.port);
}

bool NET_IsLoopback (netadr_t a)
{
	static const byte	ipv6loopback[16] = {[15] = 1};

	if (a.type == NA_LOOPBACK)
		return true;
	if (a.type != NA_IP && a.type != NA_WS && a.type != NA_RTCCLIENT)
		return false;
	return NET_IsIPv4 (a) ? a.ip[12] == 127 : !memcmp (a.ip, ipv6loopback, sizeof(a.ip));
}

bool NET_IsLocalAddress (netadr_t a)
{
	netadr_t	own;
	int			i;

	if (a.type == NA_LOOPBACK)
		return true;
	if (a.type != NA_IP)
		return false;
	if (NET_IsLoopback (a))
		return true;
	for (i = 0; i < 2; i++)
	{
		own = NET_SocketAddress ((netsrc_t)i);
		if (own.type == NA_IP && !memcmp (own.ip, a.ip, sizeof(a.ip)))
			return true;
	}
	return false;
}

char *NET_AdrToString (netadr_t a)
{
	static	char	s[64];

	if (a.type == NA_LOOPBACK)
		return "loopback";
	if (a.type == NA_URL)
		return (char *)UDP_URLToString (a, true);
	if (a.type == NA_RTC)
		return (char *)RTC_AdrToString (a, true);
	// IPv6's in [ ], as its colons aren't the port's
	snprintf (s, sizeof(s), NET_IsIPv4 (a) ? "%s:%i" : "[%s]:%i", NET_IPToString (a.ip),
		(unsigned short)BigShort ((short)a.port));
	return s;
}

char *NET_BaseAdrToString (netadr_t a)
{
	static	char	s[64];

	if (a.type == NA_LOOPBACK)
		return "loopback";
	if (a.type == NA_URL)
		return (char *)UDP_URLToString (a, false);
	if (a.type == NA_RTC)
		return (char *)RTC_AdrToString (a, false);
	Q_strncpyz (s, NET_IPToString (a.ip), sizeof(s));
	return s;
}

/*
=============
NET_StringToAdr

local
idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
2001:db8::1, [2001:db8::1]:28000 (IPv6's, its port after [ ])
ws://idnewt:28000/path, wss://idnewt/path (where the platform's packets go to URLs: a browser's)
rtc://broker/room, rtcs://broker:27950/udp/192.246.40.70:28000 (WebRTC)
1234-5678 (an invitation code: room 12345678 at net_webrtc_broker)
=============
*/
bool NET_StringToAdr (const char *s, netadr_t *a)
{
	char			copy[128], room[9], url[256];
	char			*host = copy, *colon, *end;
	unsigned short	port = 0;

	if (!strcmp (s, "local") || !strcmp (s, "loopback"))
	{
		memset (a, 0, sizeof(*a));
		a->type = NA_LOOPBACK;
		return true;
	}
	if (NET_InvitationRoom (s, room))
		return NET_RoomURL (net_webrtc_broker.string, room, url, sizeof(url)) && NET_StringToAdr (url, a);
	if (strstr (s, "://"))
		return RTC_ResolveURL (s, a) || UDP_ResolveURL (s, a);

	Q_strncpyz (copy, s, sizeof(copy));

	// strip off a trailing :port if present: after [ ] around an IPv6
	// address, else after a host's one colon (an IPv6 address alone has more)
	colon = NULL;
	if (copy[0] == '[')
	{
		end = strchr (copy, ']');
		if (!end || (end[1] && end[1] != ':'))
			return false;
		*end = 0;
		host = copy + 1;
		if (end[1])
			colon = end + 1;
	}
	else if (strchr (copy, ':') == strrchr (copy, ':'))
		colon = strchr (copy, ':');
	if (colon)
	{
		*colon = 0;
		port = (unsigned short)BigShort ((short)atoi (colon + 1));
	}

	// a browser has no UDP, nor DNS: a host there is a URL's
	if (!UDP_Resolve (host, net_prefer_ipv6.value != 0, a))
		return UDP_ResolveURL (s, a);
	a->port = port;
	return true;
}
