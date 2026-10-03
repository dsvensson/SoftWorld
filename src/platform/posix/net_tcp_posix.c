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
// net_tcp_posix.c -- TCP streams on BSD sockets (QTV, and the server's
// WebSocket port), on macOS and Linux

#include "args.h"
#include "mem.h"
#include "net_socket.h"
#include "print.h"
#include "posix_local.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct tcpsocket_s
{
	int		socket;
	bool	connected;
};

struct tcplisten_s
{
	int		socket;
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

	s = Mem_Calloc (1, sizeof(*s));
	s->socket = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s->socket < 0)
	{
		Mem_Free (s);
		return NULL;
	}
	setsockopt (s->socket, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
	// a write to a closed connection is an error, not SIGPIPE: the process
	// ignores it (SO_NOSIGPIPE, which did it for the socket, is macOS's alone)
	signal (SIGPIPE, SIG_IGN);
	if (fcntl (s->socket, F_SETFL, fcntl (s->socket, F_GETFL) | O_NONBLOCK) < 0)
	{
		close (s->socket);
		Mem_Free (s);
		return NULL;
	}

	// let Sys_WaitUntil wake up when the connection is made or data arrives
	Sys_AddWaitFd (s->socket);

	memcpy (&addr.sin_addr, to->ip, 4);
	addr.sin_port = to->port;
	if (connect (s->socket, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS)
	{
		TCP_Close (s);
		return NULL;
	}
	return s;
}

void TCP_Close (tcpsocket_t *s)
{
	Sys_RemoveWaitFd (s->socket);
	close (s->socket);
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
	struct pollfd	p = {.fd = s->socket, .events = POLLOUT};
	socklen_t		len = sizeof(int);
	int				err = 0;

	if (s->connected)
		return TCP_OPEN;
	if (poll (&p, 1, 0) < 0)
		return TCP_FAILED;
	if (!(p.revents & (POLLOUT | POLLERR | POLLHUP)))
		return TCP_CONNECTING;
	// a connection that failed is writable too; its error tells
	if (getsockopt (s->socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err)
		return TCP_FAILED;
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
	ssize_t	ret;

	ret = recv (s->socket, buf, (size_t)maxlen, 0);
	if (ret > 0)
		return (int)ret;
	if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR))
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
	ssize_t		ret;

	while (length > 0)
	{
		ret = send (s->socket, p, (size_t)length, 0);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		p += ret;
		length -= (int)ret;
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
	ssize_t	ret;

	do
		ret = send (s->socket, data, (size_t)length, 0);
	while (ret < 0 && errno == EINTR);
	if (ret >= 0)
		return (int)ret;
	return errno == EWOULDBLOCK || errno == EAGAIN ? 0 : -1;
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
	tcplisten_t			*l;
	struct sockaddr_in	address = {.sin_family = AF_INET};
	int					i;

	l = Mem_Calloc (1, sizeof(*l));
	l->socket = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (l->socket < 0)
	{
		Con_Printf ("TCP port %i: %s\n", port, strerror (errno));
		Mem_Free (l);
		return NULL;
	}
	// a server started again takes its port at once, though the last one's
	// connections linger
	setsockopt (l->socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
	signal (SIGPIPE, SIG_IGN);

	if ((i = COM_CheckParm ("-ip")) != 0 && i + 1 < com_argc)
		inet_pton (AF_INET, com_argv[i + 1], &address.sin_addr);
	else
		address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons ((unsigned short)port);
	if (fcntl (l->socket, F_SETFL, fcntl (l->socket, F_GETFL) | O_NONBLOCK) < 0
		|| bind (l->socket, (struct sockaddr *)&address, sizeof(address)) < 0 || listen (l->socket, 16) < 0)
	{
		Con_Printf ("TCP port %i: %s\n", port, strerror (errno));
		close (l->socket);
		Mem_Free (l);
		return NULL;
	}

	// let Sys_WaitUntil wake up when a connection comes
	Sys_AddWaitFd (l->socket);
	return l;
}

void TCP_CloseListen (tcplisten_t *l)
{
	Sys_RemoveWaitFd (l->socket);
	close (l->socket);
	Mem_Free (l);
}

/*
====================
TCP_Accept

A connection come, open and non-blocking; NULL when none has
====================
*/
tcpsocket_t *TCP_Accept (tcplisten_t *l, netadr_t *from)
{
	struct sockaddr_in	addr;
	socklen_t			addrlen = sizeof(addr);
	tcpsocket_t			*s;
	int					accepted;

	do
		accepted = accept (l->socket, (struct sockaddr *)&addr, &addrlen);
	while (accepted < 0 && (errno == EINTR || errno == ECONNABORTED));
	if (accepted < 0)
		return NULL;
	if (fcntl (accepted, F_SETFL, fcntl (accepted, F_GETFL) | O_NONBLOCK) < 0)
	{
		close (accepted);
		return NULL;
	}
	setsockopt (accepted, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));

	s = Mem_Calloc (1, sizeof(*s));
	s->socket = accepted;
	s->connected = true;
	Sys_AddWaitFd (accepted);

	memset (from, 0, sizeof(*from));
	from->type = NA_IP;
	memcpy (from->ip, &addr.sin_addr, 4);
	from->port = addr.sin_port;
	return s;
}
