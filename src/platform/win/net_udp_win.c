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
// net_udp_win.c -- UDP sockets over Winsock 2

#include <winsock2.h>
#include <ws2tcpip.h>

#include "args.h"
#include "mem.h"
#include "net_socket.h"
#include "print.h"
#include "sys.h"
#include "win_local.h"

#include <string.h>

struct udpsocket_s
{
	SOCKET	socket;
	HANDLE	event;		// auto reset, signaled when packets arrive
	netadr_t	address;
};

static void NetadrToSockadr (const netadr_t *a, struct sockaddr_in *s)
{
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
	u_long	nonblocking = 1;
	int		i, namelen;
	char	hostname[256];

	s = Mem_Calloc (1, sizeof(*s));

	s->socket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s->socket == INVALID_SOCKET)
		Sys_Error ("UDP_Open: socket: Winsock error %i", WSAGetLastError ());

	if (ioctlsocket (s->socket, FIONBIO, &nonblocking) == SOCKET_ERROR)
		Sys_Error ("UDP_Open: ioctl FIONBIO: Winsock error %i", WSAGetLastError ());

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
	if (bind (s->socket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR)
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
	if (gethostname (hostname, sizeof(hostname)) != 0 || !UDP_Resolve (hostname, &s->address))
		UDP_Resolve ("127.0.0.1", &s->address);
	namelen = sizeof(address);
	if (getsockname (s->socket, (struct sockaddr *)&address, &namelen) == SOCKET_ERROR)
		Sys_Error ("UDP_Open: getsockname: Winsock error %i", WSAGetLastError ());
	s->address.port = address.sin_port;

	return s;
}

void UDP_Close (udpsocket_t *s)
{
	Sys_RemoveWaitHandle (s->event);
	CloseHandle (s->event);
	closesocket (s->socket);
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
			if (err == WSAECONNRESET)
				continue;		// an earlier send was refused
			if (err != WSAEMSGSIZE)
				Sys_Error ("UDP_Recv: Winsock error %i", err);
			ret = maxlen;
		}

		SockadrToNetadr (&addr, from);
		if (ret == maxlen)
		{
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
	struct sockaddr_in	addr;
	int		err;

	NetadrToSockadr (to, &addr);
	if (sendto (s->socket, data, length, 0, (struct sockaddr *)&addr, sizeof(addr)) != SOCKET_ERROR)
		return;

	err = WSAGetLastError ();
	if (err == WSAEWOULDBLOCK)
		return;		// silent
	if (err == WSAEADDRNOTAVAIL)
		Con_DPrintf ("UDP_Send: Winsock error %i\n", err);
	else
		Con_Printf ("UDP_Send: Winsock error %i\n", err);
}
