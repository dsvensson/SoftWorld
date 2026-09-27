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
// NULL if the port can't be bound
udpsocket_t	*UDP_Open (int port);
void	UDP_Close (udpsocket_t *s);
netadr_t	UDP_Address (udpsocket_t *s);	// this machine's address and the socket's port

// the next packet's length, 0 when there is none
int		UDP_Recv (udpsocket_t *s, byte *buf, int maxlen, netadr_t *from);
void	UDP_Send (udpsocket_t *s, const void *data, int length, const netadr_t *to);

// resolves a host name or dotted address, without a port
bool	UDP_Resolve (const char *host, netadr_t *a);

// TCP streams (QTV): non-blocking, and they wake Sys_WaitUntil when the
// connection is made, data arrives, or it closes
typedef struct tcpsocket_s tcpsocket_t;

enum { TCP_CONNECTING, TCP_OPEN, TCP_FAILED };

tcpsocket_t	*TCP_Connect (const netadr_t *to);	// NULL when it can't even start
void	TCP_Close (tcpsocket_t *s);
int		TCP_State (tcpsocket_t *s);
int		TCP_Recv (tcpsocket_t *s, byte *buf, int maxlen);	// 0 nothing yet, -1 closed
bool	TCP_Send (tcpsocket_t *s, const void *data, int length);
