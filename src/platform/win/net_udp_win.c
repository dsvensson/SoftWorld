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
// net_udp_win.c -- UDP sockets over Winsock 2, IPv4's and IPv6's (net_win.h)

#include "net_win.h"

#include "args.h"
#include "mem.h"
#include "net_socket.h"
#include "print.h"
#include "sys.h"
#include "win_local.h"

#include <mstcpip.h>
#include <stdlib.h>
#include <string.h>

// ICMP's refusals and expiries reported to a socket's reads, which a quiet one
// turns off (mstcpip.h has them for later Windows versions only)
#ifndef SIO_UDP_CONNRESET
#define	SIO_UDP_CONNRESET	_WSAIOW (IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define	SIO_UDP_NETRESET	_WSAIOW (IOC_VENDOR, 15)
#endif

struct udpsocket_s
{
	SOCKET	socket;
	int		family;		// AF_INET6, taking IPv4's too, or AF_INET
	HANDLE	event;		// auto reset, signaled when packets arrive; NULL for a quiet socket
	netadr_t	address;
	bool	quiet;		// UDP_OpenQuiet's: no wake, no print, no error
};

int Win_ToSockaddr (const netadr_t *a, int family, struct sockaddr_storage *s)
{
	struct sockaddr_in	*in = (struct sockaddr_in *)s;
	struct sockaddr_in6	*in6 = (struct sockaddr_in6 *)s;

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

void Win_FromSockaddr (const struct sockaddr_storage *s, netadr_t *a)
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

SOCKET Win_Socket (int type, int port, struct sockaddr_storage *address, int *length)
{
	netadr_t	a = {.type = NA_IP};
	SOCKET		s;
	u_long		nonblocking = 1;
	int			i, family = AF_INET6;

	//ZOID -- check for interface binding option
	if ((i = COM_CheckParm ("-ip")) != 0 && i + 1 < com_argc)
	{
		if (!NET_ParseIP (com_argv[i + 1], a.ip))
			Sys_Error ("Bad -ip address %s", com_argv[i + 1]);
		family = NET_IsIPv4 (a) ? AF_INET : AF_INET6;
		s = socket (family, type, 0);
	}
	// every interface's, IPv6's (::) and IPv4's, where the system has IPv6
	else if ((s = socket (AF_INET6, type, 0)) != INVALID_SOCKET)
		setsockopt (s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&(int){0}, sizeof(int));
	else
	{
		family = AF_INET;
		NET_SetIPv4 (&a, (const byte[4]){0});
		s = socket (AF_INET, type, 0);
	}
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;
	if (ioctlsocket (s, FIONBIO, &nonblocking) == SOCKET_ERROR)
	{
		closesocket (s);
		return INVALID_SOCKET;
	}
	a.port = port == PORT_ANY ? 0 : htons ((unsigned short)port);
	*length = Win_ToSockaddr (&a, family, address);
	return s;
}

/*
====================
UDP_Init / UDP_Shutdown
====================
*/
void UDP_Init (void)
{
	WSADATA	winsockdata;

	if (WSAStartup (MAKEWORD(2, 2), &winsockdata))
		Sys_Error ("Winsock initialization failed.");
}

void UDP_Shutdown (void)
{
	WSACleanup ();
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
		Win_FromSockaddr ((const struct sockaddr_storage *)r->ai_addr, a);
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
	netadr_t	bound;
	int		length, namelen;
	char	hostname[256];

	s = Mem_Calloc (1, sizeof(*s));

	s->socket = Win_Socket (SOCK_DGRAM, port, &address, &length);
	if (s->socket == INVALID_SOCKET)
		Sys_Error ("UDP_Open: socket: Winsock error %i", WSAGetLastError ());
	s->family = address.ss_family;
	if (COM_CheckParm ("-ip"))
		Con_Printf ("Binding to IP Interface Address of %s\n", com_argv[COM_CheckParm ("-ip") + 1]);

	// room for a burst of download chunks between two reads; the default
	// 64 KB holds about 60
	setsockopt (s->socket, SOL_SOCKET, SO_RCVBUF, (const char *)&(int){1 << 21}, sizeof(int));

	if (bind (s->socket, (struct sockaddr *)&address, length) == SOCKET_ERROR)
	{
		Con_Printf ("UDP port %i: Winsock error %i\n", port, WSAGetLastError ());
		closesocket (s->socket);
		Mem_Free (s);
		return NULL;
	}

	// let Sys_WaitUntil wake up when a packet arrives
	s->event = CreateEventW (NULL, FALSE, FALSE, NULL);
	if (!s->event || WSAEventSelect (s->socket, s->event, FD_READ) == SOCKET_ERROR)
		Sys_Error ("UDP_Open: couldn't create the socket event");
	Sys_AddWaitHandle (s->event);

	// determine my name & address
	if (gethostname (hostname, sizeof(hostname)) != 0 || !UDP_Resolve (hostname, false, &s->address))
		UDP_Resolve ("127.0.0.1", false, &s->address);
	namelen = sizeof(address);
	if (getsockname (s->socket, (struct sockaddr *)&address, &namelen) == SOCKET_ERROR)
		Sys_Error ("UDP_Open: getsockname: Winsock error %i", WSAGetLastError ());
	Win_FromSockaddr (&address, &bound);
	s->address.port = bound.port;

	return s;
}

/*
====================
UDP_OpenQuiet

A socket for a thread of its own (the server list's): no wake, no print, no
error; ICMP's refusals and expiries don't reach its reads
====================
*/
udpsocket_t *UDP_OpenQuiet (void)
{
	udpsocket_t	*s;
	struct sockaddr_storage	address;
	netadr_t	bound;
	int		length, namelen;
	DWORD	bytes;

	s = calloc (1, sizeof(*s));
	if (!s)
		return NULL;
	s->quiet = true;
	s->socket = Win_Socket (SOCK_DGRAM, PORT_ANY, &address, &length);
	if (s->socket == INVALID_SOCKET)
	{
		free (s);
		return NULL;
	}
	s->family = address.ss_family;
	setsockopt (s->socket, SOL_SOCKET, SO_RCVBUF, (const char *)&(int){1 << 18}, sizeof(int));
	WSAIoctl (s->socket, SIO_UDP_CONNRESET, &(BOOL){FALSE}, sizeof(BOOL), NULL, 0, &bytes, NULL, NULL);
	WSAIoctl (s->socket, SIO_UDP_NETRESET, &(BOOL){FALSE}, sizeof(BOOL), NULL, 0, &bytes, NULL, NULL);
	namelen = sizeof(address);
	if (bind (s->socket, (struct sockaddr *)&address, length) == SOCKET_ERROR
		|| getsockname (s->socket, (struct sockaddr *)&address, &namelen) == SOCKET_ERROR)
	{
		closesocket (s->socket);
		free (s);
		return NULL;
	}
	Win_FromSockaddr (&address, &bound);
	NET_SetIPv4 (&s->address, (const byte[4]){127, 0, 0, 1});
	s->address.type = NA_IP;
	s->address.port = bound.port;
	return s;
}

void UDP_Close (udpsocket_t *s)
{
	if (s->quiet)
	{
		closesocket (s->socket);
		free (s);
		return;
	}
	Sys_RemoveWaitHandle (s->event);
	CloseHandle (s->event);
	closesocket (s->socket);
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
	WSAPOLLFD	fds[32];
	int			i;

	if (n > 32)
		n = 32;
	for (i = 0 ; i < n ; i++)
		fds[i] = (WSAPOLLFD){.fd = s[i]->socket, .events = POLLRDNORM};
	return WSAPoll (fds, (ULONG)n, seconds > 0 ? (INT)(seconds * 1000 + 0.999) : 0) > 0;
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
	int		ret, addrlen, err;

	while (1)
	{
		addrlen = sizeof(addr);
		ret = recvfrom (s->socket, (char *)buf, maxlen, 0, (struct sockaddr *)&addr, &addrlen);
		if (ret == SOCKET_ERROR)
		{
			err = WSAGetLastError ();
			if (err == WSAEWOULDBLOCK)
				return 0;
			if (err == WSAECONNRESET || err == WSAENETRESET)
				continue;		// an earlier send was refused, or expired on the way
			if (err != WSAEMSGSIZE)
			{
				if (s->quiet)
					return 0;
				Sys_Error ("UDP_Recv: Winsock error %i", err);
			}
			ret = maxlen;
		}

		Win_FromSockaddr (&addr, from);
		if (ret == maxlen)
		{
			if (!s->quiet)
				Con_Printf ("Oversize packet from %s\n", NET_AdrToString (*from));
			continue;
		}
		if (ret > 0)
			return ret;
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
	int		addrlen, err;

	// an IPv6 address, and this socket IPv4's alone
	addrlen = Win_ToSockaddr (to, s->family, &addr);
	if (!addrlen)
		return;
	if (sendto (s->socket, data, length, 0, (struct sockaddr *)&addr, addrlen) != SOCKET_ERROR)
		return;

	err = WSAGetLastError ();
	if (err == WSAEWOULDBLOCK || s->quiet)
		return;		// silent
	if (err == WSAEADDRNOTAVAIL)
		Con_DPrintf ("UDP_Send: Winsock error %i\n", err);
	else
		Con_Printf ("UDP_Send: Winsock error %i\n", err);
}
