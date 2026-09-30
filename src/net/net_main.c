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
#include "net_socket.h"
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

/*
====================
NET_Init / NET_Shutdown
====================
*/
void NET_Init (void)
{
	UDP_Init ();
	Netchan_Init ();
	Cvar_RegisterVariable (&password);
	Cvar_RegisterVariable (&rcon_password);
}

void NET_Shutdown (void)
{
	NET_CloseSocket (NS_CLIENT);
	NET_CloseSocket (NS_SERVER);
	memset (loopbacks, 0, sizeof(loopbacks));
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
	Con_Printf ("%s UDP on %s\n", sock == NS_SERVER ? "Server" : "Client",
		NET_AdrToString (UDP_Address (net_sockets[sock])));
	return true;
}

void NET_CloseSocket (netsrc_t sock)
{
	if (net_sockets[sock])
		UDP_Close (net_sockets[sock]);
	net_sockets[sock] = NULL;
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
	if (!net_sockets[sock])
		return false;

	length = UDP_Recv (net_sockets[sock], msg->data, msg->maxsize, from);
	if (length <= 0)
		return false;
	msg->cursize = length;
	return true;
}

void NET_SendPacket (netsrc_t sock, int length, const void *data, netadr_t to)
{
	if (to.type == NA_LOOPBACK)
		Loop_SendPacket (sock, length, data);
	else if (to.type == NA_IP && net_sockets[sock])
		UDP_Send (net_sockets[sock], data, length, &to);
}

/*
=============================================================================

ADDRESSES

=============================================================================
*/

bool NET_CompareBaseAdr (netadr_t a, netadr_t b)
{
	if (a.type != b.type)
		return false;
	if (a.type == NA_LOOPBACK)
		return true;
	return memcmp (a.ip, b.ip, 4) == 0;
}

bool NET_CompareAdr (netadr_t a, netadr_t b)
{
	return NET_CompareBaseAdr (a, b) && (a.type == NA_LOOPBACK || a.port == b.port);
}

bool NET_IsLocalAddress (netadr_t a)
{
	netadr_t	own;
	int			i;

	if (a.type == NA_LOOPBACK)
		return true;
	if (a.type != NA_IP)
		return false;
	if (a.ip[0] == 127)
		return true;
	for (i = 0; i < 2; i++)
	{
		own = NET_SocketAddress ((netsrc_t)i);
		if (own.type == NA_IP && !memcmp (own.ip, a.ip, 4))
			return true;
	}
	return false;
}

char *NET_AdrToString (netadr_t a)
{
	static	char	s[64];

	if (a.type == NA_LOOPBACK)
		return "loopback";
	snprintf (s, sizeof(s), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3],
		(unsigned short)BigShort ((short)a.port));
	return s;
}

char *NET_BaseAdrToString (netadr_t a)
{
	static	char	s[64];

	if (a.type == NA_LOOPBACK)
		return "loopback";
	snprintf (s, sizeof(s), "%i.%i.%i.%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
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
=============
*/
bool NET_StringToAdr (const char *s, netadr_t *a)
{
	char			copy[128];
	char			*colon;
	unsigned short	port = 0;

	if (!strcmp (s, "local") || !strcmp (s, "loopback"))
	{
		memset (a, 0, sizeof(*a));
		a->type = NA_LOOPBACK;
		return true;
	}

	Q_strncpyz (copy, s, sizeof(copy));

	// strip off a trailing :port if present
	colon = strrchr (copy, ':');
	if (colon)
	{
		*colon = 0;
		port = (unsigned short)BigShort ((short)atoi (colon + 1));
	}

	if (!UDP_Resolve (copy, a))
		return false;
	a->port = port;
	return true;
}
