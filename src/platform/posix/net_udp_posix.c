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
// net_udp_posix.c -- UDP sockets over BSD sockets, on macOS and Linux, IPv4's
// and IPv6's (net_posix.h)

#include "args.h"
#include "mem.h"
#include "net_posix.h"
#include "net_socket.h"
#include "print.h"
#include "sys.h"
#include "posix_local.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct udpsocket_s
{
	int			socket;
	int			family;		// AF_INET6, taking IPv4's too, or AF_INET
	netadr_t	address;
	bool		quiet;		// UDP_OpenQuiet's: no wake, no print, no error
};

socklen_t Posix_ToSockaddr (const netadr_t *a, int family, struct sockaddr_storage *s)
{
	struct sockaddr_in	*in = (struct sockaddr_in *)s;
	struct sockaddr_in6	*in6 = (struct sockaddr_in6 *)s;

	// no sin_len: macOS takes the length from the call's
	memset (s, 0, sizeof(*s));
	if (family == AF_INET6)
	{
		in6->sin6_family = AF_INET6;
		memcpy (&in6->sin6_addr, a->ip, 16);
		in6->sin6_port = a->port;
		return sizeof(*in6);
	}
	if (!NET_IsIPv4 (*a))
		return 0;
	in->sin_family = AF_INET;
	memcpy (&in->sin_addr, a->ip + 12, 4);
	in->sin_port = a->port;
	return sizeof(*in);
}

void Posix_FromSockaddr (const struct sockaddr_storage *s, netadr_t *a)
{
	memset (a, 0, sizeof(*a));
	a->type = NA_IP;
	if (s->ss_family == AF_INET6)
	{
		memcpy (a->ip, &((const struct sockaddr_in6 *)s)->sin6_addr, 16);
		a->port = ((const struct sockaddr_in6 *)s)->sin6_port;
	}
	else
	{
		NET_SetIPv4 (a, &((const struct sockaddr_in *)s)->sin_addr);
		a->port = ((const struct sockaddr_in *)s)->sin_port;
	}
}

int Posix_Socket (int type, int port, struct sockaddr_storage *address, socklen_t *length)
{
	netadr_t	a = {.type = NA_IP};
	int			s, i, family = AF_INET6;

	//ZOID -- check for interface binding option
	if ((i = COM_CheckParm ("-ip")) != 0 && i + 1 < com_argc)
	{
		if (!NET_ParseIP (com_argv[i + 1], a.ip))
			Sys_Error ("Bad -ip address %s", com_argv[i + 1]);
		family = NET_IsIPv4 (a) ? AF_INET : AF_INET6;
		s = socket (family, type, 0);
	}
	// every interface's, IPv6's (::) and IPv4's, where the system has IPv6
	else if ((s = socket (AF_INET6, type, 0)) >= 0)
		setsockopt (s, IPPROTO_IPV6, IPV6_V6ONLY, &(int){0}, sizeof(int));
	else
	{
		family = AF_INET;
		NET_SetIPv4 (&a, (const byte[4]){0});
		s = socket (AF_INET, type, 0);
	}
	if (s < 0)
		return -1;
	if (fcntl (s, F_SETFL, fcntl (s, F_GETFL) | O_NONBLOCK) < 0)
	{
		close (s);
		return -1;
	}
	a.port = port == PORT_ANY ? 0 : htons ((unsigned short)port);
	*length = Posix_ToSockaddr (&a, family, address);
	return s;
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
bool UDP_Resolve (const char *host, bool ipv6, netadr_t *a)
{
	struct addrinfo	hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
	struct addrinfo	*result, *r;

	if (getaddrinfo (host, NULL, &hints, &result) != 0 || !result)
		return false;

	// the family preferred, or else the other
	for (r = result ; r && r->ai_family != (ipv6 ? AF_INET6 : AF_INET) ; r = r->ai_next)
		;
	if (!r)
		for (r = result ; r && r->ai_family != AF_INET && r->ai_family != AF_INET6 ; r = r->ai_next)
			;
	if (r)
		Posix_FromSockaddr ((const struct sockaddr_storage *)r->ai_addr, a);
	freeaddrinfo (result);
	return r != NULL;
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
	struct sockaddr_storage	address;
	struct ifaddrs	*interfaces, *ifa;
	netadr_t	bound;
	socklen_t	length, namelen;

	s = Mem_Calloc (1, sizeof(*s));

	s->socket = Posix_Socket (SOCK_DGRAM, port, &address, &length);
	if (s->socket < 0)
		Sys_Error ("UDP_Open: socket: %s", strerror (errno));
	s->family = address.ss_family;
	if (COM_CheckParm ("-ip"))
		Con_Printf ("Binding to IP Interface Address of %s\n", com_argv[COM_CheckParm ("-ip") + 1]);

	// room for a burst of download chunks between two reads; the default
	// holds a few dozen
	setsockopt (s->socket, SOL_SOCKET, SO_RCVBUF, &(int){1 << 21}, sizeof(int));

	if (bind (s->socket, (struct sockaddr *)&address, length) < 0)
	{
		Con_Printf ("UDP port %i: %s\n", port, strerror (errno));
		close (s->socket);
		Mem_Free (s);
		return NULL;
	}

	// let Sys_WaitUntil wake up when a packet arrives
	Sys_AddWaitFd (s->socket);

	// this machine's address: the first IPv4 interface that is up and not
	// loopback
	UDP_Resolve ("127.0.0.1", false, &s->address);
	if (!getifaddrs (&interfaces))
	{
		for (ifa = interfaces ; ifa ; ifa = ifa->ifa_next)
			if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET
				&& (ifa->ifa_flags & IFF_UP) && !(ifa->ifa_flags & IFF_LOOPBACK))
			{
				Posix_FromSockaddr ((const struct sockaddr_storage *)ifa->ifa_addr, &s->address);
				break;
			}
		freeifaddrs (interfaces);
	}
	namelen = sizeof(address);
	if (getsockname (s->socket, (struct sockaddr *)&address, &namelen) < 0)
		Sys_Error ("UDP_Open: getsockname: %s", strerror (errno));
	Posix_FromSockaddr (&address, &bound);
	s->address.port = bound.port;

	return s;
}

/*
====================
UDP_OpenQuiet

A socket for a thread of its own (the server list's): no wake, no print, no
error
====================
*/
udpsocket_t *UDP_OpenQuiet (void)
{
	udpsocket_t	*s;
	struct sockaddr_storage	address;
	netadr_t	bound;
	socklen_t	length, namelen;

	s = calloc (1, sizeof(*s));
	if (!s)
		return NULL;
	s->quiet = true;
	s->socket = Posix_Socket (SOCK_DGRAM, PORT_ANY, &address, &length);
	if (s->socket < 0)
	{
		free (s);
		return NULL;
	}
	s->family = address.ss_family;
	setsockopt (s->socket, SOL_SOCKET, SO_RCVBUF, &(int){1 << 18}, sizeof(int));
	namelen = sizeof(address);
	if (bind (s->socket, (struct sockaddr *)&address, length) < 0
		|| getsockname (s->socket, (struct sockaddr *)&address, &namelen) < 0)
	{
		close (s->socket);
		free (s);
		return NULL;
	}
	Posix_FromSockaddr (&address, &bound);
	NET_SetIPv4 (&s->address, (const byte[4]){127, 0, 0, 1});
	s->address.type = NA_IP;
	s->address.port = bound.port;
	return s;
}

void UDP_Close (udpsocket_t *s)
{
	if (s->quiet)
	{
		close (s->socket);
		free (s);
		return;
	}
	Sys_RemoveWaitFd (s->socket);
	close (s->socket);
	Mem_Free (s);
}

/*
====================
UDP_Wait

Until a packet waits on one of the sockets, or the seconds pass
====================
*/
bool UDP_Wait (udpsocket_t *const *s, int n, double seconds)
{
	struct pollfd	fds[32];
	int				i;

	if (n > 32)
		n = 32;
	for (i = 0 ; i < n ; i++)
		fds[i] = (struct pollfd){.fd = s[i]->socket, .events = POLLIN};
	return poll (fds, (nfds_t)n, seconds > 0 ? (int)(seconds * 1000 + 0.999) : 0) > 0;
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
	struct sockaddr_storage	addr;
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
			if (s->quiet)
				return 0;
			Sys_Error ("UDP_Recv: %s", strerror (errno));
		}

		Posix_FromSockaddr (&addr, from);
		// a datagram that didn't fit is cut to maxlen
		if (ret == maxlen)
		{
			if (!s->quiet)
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
	struct sockaddr_storage	addr;
	socklen_t	addrlen;

	// an IPv6 address, and this socket IPv4's alone
	addrlen = Posix_ToSockaddr (to, s->family, &addr);
	if (!addrlen)
		return;
	if (sendto (s->socket, data, (size_t)length, 0, (struct sockaddr *)&addr, addrlen) >= 0)
		return;

	if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS || s->quiet)
		return;		// silent
	if (errno == EADDRNOTAVAIL || errno == EHOSTUNREACH || errno == ENETUNREACH)
		Con_DPrintf ("UDP_Send: %s\n", strerror (errno));
	else
		Con_Printf ("UDP_Send: %s\n", strerror (errno));
}
