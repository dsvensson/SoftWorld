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
// test_stream.c -- the TCP streams with deadlines (the server list's downloads)
// on the loopback: one connects, writes and reads what a listening port sends
// back until it closes; a read with nothing coming gives up at its deadline,
// and a connection to a port no one listens on fails by its own. And the roots
// the system trusts, which HTTPS checks servers by.

#include "args.h"
#include "host.h"
#include "net_socket.h"
#include "q_endian.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static netadr_t Loopback (int port)
{
	netadr_t	a = {.type = NA_IP};

	NET_SetIPv4 (&a, (const byte[4]){127, 0, 0, 1});
	a.port = (unsigned short)BigShort ((short)port);
	return a;
}

// the connection the port takes, waiting up to two seconds for it
static tcpsocket_t *Accept (tcplisten_t *l)
{
	double		until = Sys_DoubleTime () + 2;
	tcpsocket_t	*s;
	netadr_t	from;

	while (!(s = TCP_Accept (l, &from)) && Sys_DoubleTime () < until)
		;
	return s;
}

// what the taken connection reads, waiting up to two seconds for size bytes
static int Recv (tcpsocket_t *s, byte *buf, int size)
{
	double	until = Sys_DoubleTime () + 2;
	int		n, got = 0;

	while (got < size && Sys_DoubleTime () < until)
		if ((n = TCP_Recv (s, buf + got, size - got)) < 0)
			break;
		else
			got += n;
	return got;
}

static void TestStream (void)
{
	tcplisten_t	*l = NULL;
	tcpsocket_t	*server;
	tcpstream_t	*s;
	netadr_t	to;
	byte		buf[64];
	double		start;
	int			port, n, got;

	for (port = 27960 ; port < 28000 && !l ; port++)
		l = TCP_Listen (port);
	CHECK (l != NULL);
	if (!l)
		return;
	to = Loopback (port - 1);

	s = TCP_StreamOpen (&to, Sys_DoubleTime () + 2);
	CHECK (s != NULL);
	server = Accept (l);
	CHECK (server != NULL);
	if (!s || !server)
		return;

	// nothing sent: the read gives up at its deadline, not before
	start = Sys_DoubleTime ();
	CHECK (TCP_StreamRead (s, buf, sizeof(buf), start + 0.1) == -1);
	CHECK (Sys_DoubleTime () - start >= 0.09);

	CHECK (TCP_StreamWrite (s, "GET", 3, Sys_DoubleTime () + 2));
	CHECK (Recv (server, buf, 3) == 3 && !memcmp (buf, "GET", 3));
	CHECK (TCP_Send (server, "hello", 5));
	TCP_Close (server);
	for (got = 0 ; (n = TCP_StreamRead (s, buf + got, (int)sizeof(buf) - got, Sys_DoubleTime () + 2)) > 0 ; )
		got += n;
	CHECK (n == 0 && got == 5 && !memcmp (buf, "hello", 5));
	TCP_StreamClose (s);

	// no one listens: refused, or no answer by the deadline
	TCP_CloseListen (l);
	start = Sys_DoubleTime ();
	CHECK (TCP_StreamOpen (&to, start + 0.5) == NULL);
	CHECK (Sys_DoubleTime () - start < 1.5);
}

static void CountRoot (void *ctx, const void *data, size_t length)
{
	CHECK (data && length > 0);
	(*(int *)ctx)++;
}

static void TestRoots (void)
{
	int		roots = 0;

	CHECK (Sys_TrustedRoots (CountRoot, &roots));
	CHECK (roots > 0);
	printf ("roots: %d handed over\n", roots);
}

int main (void)
{
	// bound to the loopback, as -ip binds the programs': a socket on every
	// interface has the firewall ask whether the test may listen
	COM_InitArgv (3, (char *[]){"test", "-ip", "127.0.0.1"});
	UDP_Init ();
	TestStream ();
	TestRoots ();
	UDP_Shutdown ();
	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("stream: TCP streams with deadlines on the loopback, and the system's roots\n");
	return 0;
}
