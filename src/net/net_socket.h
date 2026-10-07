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

#pragma once
// net_socket.h -- the UDP sockets a platform provides; net_main.c builds
// the per-end sockets and the loopback on top of them

#include "net.h"
#include "q_types.h"

typedef struct udpsocket_s udpsocket_t;

void	UDP_Init (void);
void	UDP_Shutdown (void);

// a non-blocking socket that wakes Sys_WaitUntil when packets arrive;
// NULL if the port can't be bound. It is IPv6's, taking IPv4's too, where
// the system has IPv6 and -ip doesn't name an IPv4 address.
udpsocket_t	*UDP_Open (int port);
void	UDP_Close (udpsocket_t *s);
netadr_t	UDP_Address (udpsocket_t *s);	// this machine's address and the socket's port

// A socket for a thread of its own (the server list's), at a port the system
// picks: it wakes nothing, prints nothing, and its reads and sends skip what
// fails rather than stopping the program; NULL if it can't be had. Its
// address is 127.0.0.1's at its port. Not on the web.
udpsocket_t	*UDP_OpenQuiet (void);
// until a packet waits on one of the sockets (up to 32), or the seconds pass
bool	UDP_Wait (udpsocket_t *const *s, int n, double seconds);

// the next packet's length, 0 when there is none
int		UDP_Recv (udpsocket_t *s, byte *buf, int maxlen, netadr_t *from);
void	UDP_Send (udpsocket_t *s, const void *data, int length, const netadr_t *to);

// resolves a host name or numeric address, without a port: for a name with
// both, its IPv4 address, or its IPv6 one when ipv6 is preferred
bool	UDP_Resolve (const char *host, bool ipv6, netadr_t *a);

// a server by URL, ws:// or wss:// (its port as the URL has it), where the
// platform's packets go to URLs (a browser's, where a bare host[:port] is one
// too); false elsewhere. UDP_URLToString prints one back.
bool	UDP_ResolveURL (const char *s, netadr_t *a);
const char	*UDP_URLToString (netadr_t a, bool port);

// TCP streams (QTV): non-blocking, and they wake Sys_WaitUntil when the
// connection is made, data arrives, or it closes
typedef struct tcpsocket_s tcpsocket_t;

enum { TCP_CONNECTING, TCP_OPEN, TCP_FAILED };

tcpsocket_t	*TCP_Connect (const netadr_t *to);	// NULL when it can't even start
void	TCP_Close (tcpsocket_t *s);
int		TCP_State (tcpsocket_t *s);
int		TCP_Recv (tcpsocket_t *s, byte *buf, int maxlen);	// 0 nothing yet, -1 closed
bool	TCP_Send (tcpsocket_t *s, const void *data, int length);

// what the socket takes of data without waiting: the bytes, 0 for none, -1
// when the connection failed
int		TCP_Write (tcpsocket_t *s, const void *data, int length);

// a TCP port listened on (the server's WebSocket port), bound as UDP_Open
// binds (-ip); NULL when it can't be. TCP_Accept takes a connection come,
// open and non-blocking, NULL when none has; the listening and the
// connections taken wake Sys_WaitUntil.
typedef struct tcplisten_s tcplisten_t;

tcplisten_t	*TCP_Listen (int port);
void	TCP_CloseListen (tcplisten_t *l);
tcpsocket_t	*TCP_Accept (tcplisten_t *l, netadr_t *from);
