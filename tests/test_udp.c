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
// test_udp.c -- the quiet UDP sockets (the server list's) on the loopback: what
// one sends the other reads from its address, UDP_Wait wakes for it and times
// out without it, and a send no one takes troubles no read

#include "args.h"
#include "host.h"
#include "net_socket.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int	failures;

hoststate_t	host;		// the netchan's clock, which sw_net brings

#define CHECK(x)	do { if (!(x)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

// the next datagram on s, waiting up to a second for it
static int Read (udpsocket_t *s, byte *buf, int size, netadr_t *from)
{
	time_t	until = time (NULL) + 2;
	int		n;

	while (!(n = UDP_Recv (s, buf, size, from)) && time (NULL) < until)
		UDP_Wait (&s, 1, 0.1);
	return n;
}

int main (void)
{
	udpsocket_t	*a, *b, *c;
	netadr_t	from, nobody;
	byte		buf[64];

	// bound to the loopback, as -ip binds the programs': a socket on every
	// interface has the firewall ask whether the test may listen
	COM_InitArgv (3, (char *[]){"test", "-ip", "127.0.0.1"});
	UDP_Init ();
	a = UDP_OpenQuiet ();
	b = UDP_OpenQuiet ();
	CHECK (a && b);
	if (!a || !b)
		return 1;
	CHECK (UDP_Address (a).port != UDP_Address (b).port);

	// nothing waits: UDP_Wait gives up
	CHECK (!UDP_Wait (&b, 1, 0.05));

	// a to b, and b reads it from a's port on the loopback
	UDP_Send (a, "ping", 4, &(netadr_t){.type = NA_IP, .ip = {[10] = 0xff, [11] = 0xff, 127, 0, 0, 1},
		.port = UDP_Address (b).port});
	CHECK (Read (b, buf, sizeof(buf), &from) == 4 && !memcmp (buf, "ping", 4));
	CHECK (from.port == UDP_Address (a).port && NET_IsIPv4 (from));

	// a send to a port no one has: the refusal reaches no read, and the next
	// datagram does
	c = UDP_OpenQuiet ();
	CHECK (c != NULL);
	if (c)
	{
		nobody = UDP_Address (c);
		UDP_Close (c);
		UDP_Send (a, "lost", 4, &nobody);
		UDP_Wait (&a, 1, 0.1);
		CHECK (UDP_Recv (a, buf, sizeof(buf), &from) == 0);
	}
	UDP_Send (b, "pong", 4, &(netadr_t){.type = NA_IP, .ip = {[10] = 0xff, [11] = 0xff, 127, 0, 0, 1},
		.port = UDP_Address (a).port});
	CHECK (Read (a, buf, sizeof(buf), &from) == 4 && !memcmp (buf, "pong", 4));

	UDP_Close (a);
	UDP_Close (b);
	UDP_Shutdown ();
	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("udp: quiet sockets send, read and wait on the loopback\n");
	return 0;
}
