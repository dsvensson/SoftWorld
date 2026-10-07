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
// net_tcp_win.c -- TCP streams on Winsock (QTV, and the server's WebSocket
// port)
//
// The connections a port takes share its event: the waits have room for a few
// handles (sys_win.c), and a server reads all its connections when any wakes it.

#include "net_win.h"

#include "mem.h"
#include "net_socket.h"
#include "print.h"
#include "sys.h"
#include "win_local.h"

#include <stdlib.h>

struct tcpsocket_s
{
	SOCKET	socket;
	HANDLE	event;		// auto reset: connected, data, or closed
	bool	connected;
	bool	sharedevent;	// the listening port's, which it closes
};

struct tcplisten_s
{
	SOCKET	socket;
	HANDLE	event;		// auto reset: a connection came, or data to one taken
};

/*
====================
TCP_Connect

Starts a non-blocking connection
====================
*/
tcpsocket_t *TCP_Connect (const netadr_t *to)
{
	tcpsocket_t				*s;
	struct sockaddr_storage	addr;
	u_long					nonblocking = 1;
	int						family = NET_IsIPv4 (*to) ? AF_INET : AF_INET6;
	int						length = Win_ToSockaddr (to, family, &addr);

	s = Mem_Calloc (1, sizeof(*s));
	s->socket = socket (family, SOCK_STREAM, IPPROTO_TCP);
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

	if (connect (s->socket, (struct sockaddr *)&addr, length) == SOCKET_ERROR
	 && WSAGetLastError () != WSAEWOULDBLOCK)
	{
		TCP_Close (s);
		return NULL;
	}
	return s;
}

void TCP_Close (tcpsocket_t *s)
{
	if (!s->sharedevent)
	{
		Sys_RemoveWaitHandle (s->event);
		CloseHandle (s->event);
	}
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

/*
====================
TCP_Write

What the socket takes without waiting
====================
*/
int TCP_Write (tcpsocket_t *s, const void *data, int length)
{
	int		ret;

	ret = send (s->socket, data, length, 0);
	if (ret != SOCKET_ERROR)
		return ret;
	return WSAGetLastError () == WSAEWOULDBLOCK ? 0 : -1;
}

/*
===============================================================================

LISTENING

===============================================================================
*/

/*
====================
TCP_Listen

Binds to -ip if given, otherwise to every interface, as UDP_Open does
====================
*/
tcplisten_t *TCP_Listen (int port)
{
	tcplisten_t				*l;
	struct sockaddr_storage	address;
	int						length;

	l = Mem_Calloc (1, sizeof(*l));
	l->socket = Win_Socket (SOCK_STREAM, port, &address, &length);
	if (l->socket == INVALID_SOCKET)
	{
		Con_Printf ("TCP port %i: Winsock error %i\n", port, WSAGetLastError ());
		Mem_Free (l);
		return NULL;
	}
	// no other program takes the port with this one
	setsockopt (l->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&(int){1}, sizeof(int));

	if (bind (l->socket, (struct sockaddr *)&address, length) == SOCKET_ERROR
		|| listen (l->socket, 16) == SOCKET_ERROR)
	{
		Con_Printf ("TCP port %i: Winsock error %i\n", port, WSAGetLastError ());
		closesocket (l->socket);
		Mem_Free (l);
		return NULL;
	}

	// let Sys_WaitUntil wake up when a connection comes
	l->event = CreateEventW (NULL, FALSE, FALSE, NULL);
	if (!l->event || WSAEventSelect (l->socket, l->event, FD_ACCEPT) == SOCKET_ERROR)
	{
		if (l->event)
			CloseHandle (l->event);
		closesocket (l->socket);
		Mem_Free (l);
		return NULL;
	}
	Sys_AddWaitHandle (l->event);
	return l;
}

void TCP_CloseListen (tcplisten_t *l)
{
	Sys_RemoveWaitHandle (l->event);
	CloseHandle (l->event);
	closesocket (l->socket);
	Mem_Free (l);
}

/*
====================
TCP_Accept

A connection come, open and non-blocking, waking with the port's event; NULL
when none has
====================
*/
tcpsocket_t *TCP_Accept (tcplisten_t *l, netadr_t *from)
{
	struct sockaddr_storage	addr;
	int					addrlen = sizeof(addr);
	tcpsocket_t			*s;
	SOCKET				accepted;

	accepted = accept (l->socket, (struct sockaddr *)&addr, &addrlen);
	if (accepted == INVALID_SOCKET)
		return NULL;
	// an event selected makes the socket non-blocking
	if (WSAEventSelect (accepted, l->event, FD_READ | FD_CLOSE) == SOCKET_ERROR)
	{
		closesocket (accepted);
		return NULL;
	}
	setsockopt (accepted, IPPROTO_TCP, TCP_NODELAY, (const char *)&(int){1}, sizeof(int));

	s = Mem_Calloc (1, sizeof(*s));
	s->socket = accepted;
	s->event = l->event;
	s->sharedevent = true;
	s->connected = true;

	Win_FromSockaddr (&addr, from);
	return s;
}

/*
===============================================================================

STREAMS WITH DEADLINES

For a thread of its own (the server list's downloads): blocking until a
deadline, waking nothing

===============================================================================
*/

struct tcpstream_s
{
	SOCKET	socket;
};

// until the socket is ready for events or the deadline; false at the deadline
static bool TCP_Poll (SOCKET socket, short events, double deadline)
{
	WSAPOLLFD	fd = {.fd = socket, .events = events};
	double		left = deadline - Sys_DoubleTime ();

	if (left <= 0)
		return false;
	return WSAPoll (&fd, 1, (INT)(left * 1000 + 0.999)) > 0;
}

tcpstream_t *TCP_StreamOpen (const netadr_t *to, double deadline)
{
	tcpstream_t				*s;
	struct sockaddr_storage	addr;
	u_long					nonblocking = 1;
	int						family = NET_IsIPv4 (*to) ? AF_INET : AF_INET6;
	int						length = Win_ToSockaddr (to, family, &addr), error = 0, errorlength = sizeof(error);

	s = calloc (1, sizeof(*s));
	if (!s)
		return NULL;
	s->socket = socket (family, SOCK_STREAM, IPPROTO_TCP);
	if (s->socket == INVALID_SOCKET)
	{
		free (s);
		return NULL;
	}
	if (ioctlsocket (s->socket, FIONBIO, &nonblocking) == SOCKET_ERROR
		|| (connect (s->socket, (struct sockaddr *)&addr, length) == SOCKET_ERROR
			&& (WSAGetLastError () != WSAEWOULDBLOCK || !TCP_Poll (s->socket, POLLWRNORM, deadline)
				|| getsockopt (s->socket, SOL_SOCKET, SO_ERROR, (char *)&error, &errorlength) == SOCKET_ERROR
				|| error)))
	{
		TCP_StreamClose (s);
		return NULL;
	}
	return s;
}

int TCP_StreamRead (tcpstream_t *s, void *buf, int size, double deadline)
{
	int		n;

	for (;;)
	{
		n = recv (s->socket, buf, size, 0);
		if (n >= 0)
			return n;
		if (WSAGetLastError () != WSAEWOULDBLOCK || !TCP_Poll (s->socket, POLLRDNORM, deadline))
			return -1;
	}
}

bool TCP_StreamWrite (tcpstream_t *s, const void *data, int size, double deadline)
{
	int		n;

	while (size > 0)
	{
		n = send (s->socket, data, size, 0);
		if (n > 0)
		{
			data = (const char *)data + n;
			size -= n;
		}
		else if (WSAGetLastError () != WSAEWOULDBLOCK || !TCP_Poll (s->socket, POLLWRNORM, deadline))
			return false;
	}
	return true;
}

void TCP_StreamClose (tcpstream_t *s)
{
	closesocket (s->socket);
	free (s);
}
