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
// net_udp_posix.c -- UDP sockets over BSD sockets, on macOS and Linux

#include "args.h"
#include "mem.h"
#include "net_socket.h"
#include "print.h"
#include "sys.h"
#include "posix_local.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct udpsocket_s
{
	int			socket;
	netadr_t	address;
};

static void NetadrToSockadr (const netadr_t *a, struct sockaddr_in *s)
{
	// no sin_len: macOS takes the length from the call's
	memset (s, 0, sizeof(*s));
	s->sin_family = AF_INET;
	memcpy (&s->sin_addr, a->ip, 4);
	s->sin_port = a->port;
}

static void SockadrToNetadr (const struct sockaddr_in *s, netadr_t *a)
{
	memset (a, 0, sizeof(*a));
	a->type = NA_IP;
	memcpy (a->ip, &s->sin_addr, 4);
	a->port = s->sin_port;
}

/*
====================
UDP_Init / UDP_Shutdown
====================
*/
void UDP_Init (void)
{
}

void UDP_Shutdown (void)
{
}

/*
====================
UDP_Resolve
====================
*/
bool UDP_Resolve (const char *host, netadr_t *a)
{
	struct addrinfo	hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
	struct addrinfo	*result;

	if (getaddrinfo (host, NULL, &hints, &result) != 0 || !result)
		return false;

	SockadrToNetadr ((const struct sockaddr_in *)result->ai_addr, a);
	freeaddrinfo (result);
	return true;
}

// no URLs here: a browser's (net_ws_web.c)
bool UDP_ResolveURL (const char *s, netadr_t *a)
{
	(void)s;
	(void)a;
	return false;
}

const char *UDP_URLToString (netadr_t a, bool port)
{
	(void)a;
	(void)port;
	return "";
}

/*
====================
UDP_Open

Binds to -ip if given, otherwise to every interface
====================
*/
udpsocket_t *UDP_Open (int port)
{
	udpsocket_t	*s;
	struct sockaddr_in	address = {.sin_family = AF_INET};
	struct ifaddrs	*interfaces, *ifa;
	socklen_t	namelen;
	int		i;

	s = Mem_Calloc (1, sizeof(*s));

	s->socket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s->socket < 0)
		Sys_Error ("UDP_Open: socket: %s", strerror (errno));

	if (fcntl (s->socket, F_SETFL, fcntl (s->socket, F_GETFL) | O_NONBLOCK) < 0)
		Sys_Error ("UDP_Open: fcntl O_NONBLOCK: %s", strerror (errno));

	// room for a burst of download chunks between two reads; the default
	// holds a few dozen
	setsockopt (s->socket, SOL_SOCKET, SO_RCVBUF, &(int){1 << 21}, sizeof(int));

//ZOID -- check for interface binding option
	if ((i = COM_CheckParm("-ip")) != 0 && i + 1 < com_argc)
	{
		if (inet_pton (AF_INET, com_argv[i+1], &address.sin_addr) != 1)
			Sys_Error ("UDP_Open: bad -ip address %s", com_argv[i+1]);
		Con_Printf("Binding to IP Interface Address of %s\n", com_argv[i+1]);
	}
	else
		address.sin_addr.s_addr = INADDR_ANY;

	address.sin_port = port == PORT_ANY ? 0 : htons ((unsigned short)port);
	if (bind (s->socket, (struct sockaddr *)&address, sizeof(address)) < 0)
	{
		Con_Printf ("UDP port %i: %s\n", port, strerror (errno));
		close (s->socket);
		Mem_Free (s);
		return NULL;
	}

	// let Sys_WaitUntil wake up when a packet arrives
	Sys_AddWaitFd (s->socket);

	// this machine's address: the first interface that is up and not loopback
	UDP_Resolve ("127.0.0.1", &s->address);
	if (!getifaddrs (&interfaces))
	{
		for (ifa = interfaces ; ifa ; ifa = ifa->ifa_next)
			if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET
				&& (ifa->ifa_flags & IFF_UP) && !(ifa->ifa_flags & IFF_LOOPBACK))
			{
				SockadrToNetadr ((const struct sockaddr_in *)ifa->ifa_addr, &s->address);
				break;
			}
		freeifaddrs (interfaces);
	}
	namelen = sizeof(address);
	if (getsockname (s->socket, (struct sockaddr *)&address, &namelen) < 0)
		Sys_Error ("UDP_Open: getsockname: %s", strerror (errno));
	s->address.port = address.sin_port;

	return s;
}

void UDP_Close (udpsocket_t *s)
{
	Sys_RemoveWaitFd (s->socket);
	close (s->socket);
	Mem_Free (s);
}

netadr_t UDP_Address (udpsocket_t *s)
{
	return s->address;
}

/*
====================
UDP_Recv
====================
*/
int UDP_Recv (udpsocket_t *s, byte *buf, int maxlen, netadr_t *from)
{
	struct sockaddr_in	addr;
	socklen_t	addrlen;
	ssize_t		ret;

	while (1)
	{
		addrlen = sizeof(addr);
		ret = recvfrom (s->socket, buf, (size_t)maxlen, 0, (struct sockaddr *)&addr, &addrlen);
		if (ret < 0)
		{
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				return 0;
			if (errno == ECONNREFUSED || errno == EINTR)
				continue;		// an earlier send was refused
			Sys_Error ("UDP_Recv: %s", strerror (errno));
		}

		SockadrToNetadr (&addr, from);
		// a datagram that didn't fit is cut to maxlen
		if (ret == maxlen)
		{
			Con_Printf ("Oversize packet from %s\n", NET_AdrToString (*from));
			continue;
		}
		if (ret > 0)
			return (int)ret;
	}
}

/*
====================
UDP_Send
====================
*/
void UDP_Send (udpsocket_t *s, const void *data, int length, const netadr_t *to)
{
	struct sockaddr_in	addr;

	NetadrToSockadr (to, &addr);
	if (sendto (s->socket, data, (size_t)length, 0, (struct sockaddr *)&addr, sizeof(addr)) >= 0)
		return;

	if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS)
		return;		// silent
	if (errno == EADDRNOTAVAIL || errno == EHOSTUNREACH || errno == ENETUNREACH)
		Con_DPrintf ("UDP_Send: %s\n", strerror (errno));
	else
		Con_Printf ("UDP_Send: %s\n", strerror (errno));
}
