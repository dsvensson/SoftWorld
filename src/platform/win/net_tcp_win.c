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
// net_tcp_win.c -- TCP streams on Winsock (QTV)

#include <winsock2.h>
#include <ws2tcpip.h>

#include "mem.h"
#include "net_socket.h"
#include "win_local.h"

#include <string.h>

struct tcpsocket_s
{
	SOCKET	socket;
	HANDLE	event;		// auto reset: connected, data, or closed
	bool	connected;
};

/*
====================
TCP_Connect

Starts a non-blocking connection
====================
*/
tcpsocket_t *TCP_Connect (const netadr_t *to)
{
	tcpsocket_t			*s;
	struct sockaddr_in	addr = {.sin_family = AF_INET};
	u_long				nonblocking = 1;

	s = Mem_Calloc (1, sizeof(*s));
	s->socket = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s->socket == INVALID_SOCKET)
	{
		Mem_Free (s);
		return NULL;
	}
	setsockopt (s->socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&(int){1}, sizeof(int));
	if (ioctlsocket (s->socket, FIONBIO, &nonblocking) == SOCKET_ERROR)
	{
		closesocket (s->socket);
		Mem_Free (s);
		return NULL;
	}

	// let Sys_WaitUntil wake up when the connection is made or data arrives
	s->event = CreateEventW (NULL, FALSE, FALSE, NULL);
	if (!s->event || WSAEventSelect (s->socket, s->event, FD_CONNECT | FD_READ | FD_CLOSE) == SOCKET_ERROR)
	{
		if (s->event)
			CloseHandle (s->event);
		closesocket (s->socket);
		Mem_Free (s);
		return NULL;
	}
	Sys_AddWaitHandle (s->event);

	memcpy (&addr.sin_addr, to->ip, 4);
	addr.sin_port = to->port;
	if (connect (s->socket, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR
	 && WSAGetLastError () != WSAEWOULDBLOCK)
	{
		TCP_Close (s);
		return NULL;
	}
	return s;
}

void TCP_Close (tcpsocket_t *s)
{
	Sys_RemoveWaitHandle (s->event);
	CloseHandle (s->event);
	closesocket (s->socket);
	Mem_Free (s);
}

/*
====================
TCP_State

TCP_CONNECTING until the connection is made, then TCP_OPEN; TCP_FAILED when
it couldn't be
====================
*/
int TCP_State (tcpsocket_t *s)
{
	fd_set			writable, failed;
	struct timeval	now = {0};

	if (s->connected)
		return TCP_OPEN;
	FD_ZERO (&writable);
	FD_ZERO (&failed);
	FD_SET (s->socket, &writable);
	FD_SET (s->socket, &failed);
	if (select (0, NULL, &writable, &failed, &now) == SOCKET_ERROR || FD_ISSET (s->socket, &failed))
		return TCP_FAILED;
	if (!FD_ISSET (s->socket, &writable))
		return TCP_CONNECTING;
	s->connected = true;
	return TCP_OPEN;
}

/*
====================
TCP_Recv

The bytes there are, up to maxlen: 0 when none have arrived, -1 when the
connection closed or failed
====================
*/
int TCP_Recv (tcpsocket_t *s, byte *buf, int maxlen)
{
	int		ret;

	ret = recv (s->socket, (char *)buf, maxlen, 0);
	if (ret > 0)
		return ret;
	if (ret == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
		return 0;
	return -1;		// closed by the other end, or an error
}

/*
====================
TCP_Send

All of data, or false; a request is small enough not to fill a new
connection's buffer
====================
*/
bool TCP_Send (tcpsocket_t *s, const void *data, int length)
{
	const char	*p = data;
	int			ret;

	while (length > 0)
	{
		ret = send (s->socket, p, length, 0);
		if (ret == SOCKET_ERROR)
			return false;
		p += ret;
		length -= ret;
	}
	return true;
}
