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
// test_slist_loopback.c -- the list's threads against fakes on the loopback:
// a master, servers (one quiet, one answering in two datagrams, one silent,
// a qwfwd relay with its table, one a list names), a list over HTTP behind a
// redirect, and one in a file. The scan asks the master, pings what it named
// and asks what answered; the list is there before the scan ends; the silent
// server ends up gone; a describe asks again; the next start shows the cache.

#include "args.h"
#include "host.h"
#include "net_socket.h"
#include "q_endian.h"
#include "q_string.h"
#include "slist.h"
#include "sys.h"

#include <stdarg.h>
#include <stdatomic.h>
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

/*
==============================================================================

THE FAKES

==============================================================================
*/

enum { MASTER, QUIET, BUSY, SILENT, RELAY, LISTED, NUMFAKES };

static udpsocket_t	*fakes[NUMFAKES];
static netadr_t		addresses[NUMFAKES];
static atomic_bool	fakesstop;
static atomic_int	quietasked;		// the statuses the quiet one answered

static void Reply (udpsocket_t *s, netadr_t to, const char *body, size_t length)
{
	byte	packet[2048];

	memcpy (packet, "\xff\xff\xff\xffn", 5);
	memcpy (packet + 5, body, length);
	UDP_Send (s, packet, (int)(5 + length), &to);
}

static void Answer (int fake, const byte *data, int length, netadr_t from)
{
	udpsocket_t	*s = fakes[fake];
	byte		reply[64];
	char		text[512];
	int			i, n;
	bool		ping = length == 6 && !memcmp (data, "\xff\xff\xff\xffk\n", 6);
	bool		status = length >= 10 && !memcmp (data, "\xff\xff\xff\xffstatus", 10);
	bool		table = length == 14 && !memcmp (data, "\xff\xff\xff\xffpingstatus", 14);

	if (fake == MASTER)
	{
		if (length != 3 || memcmp (data, "c\n", 3))
			return;
		memcpy (reply, "\xff\xff\xff\xff" "d\n", 6);
		for (i = QUIET, n = 6 ; i <= RELAY ; i++, n += 6)
		{
			memcpy (reply + n, addresses[i].ip + 12, 4);
			memcpy (reply + n + 4, &addresses[i].port, 2);
		}
		UDP_Send (s, reply, n, &from);
		return;
	}
	if (ping)
	{
		// both of an answer's dresses
		UDP_Send (s, fake == BUSY ? "\xff\xff\xff\xffl" : "l", fake == BUSY ? 5 : 1, &from);
		return;
	}
	if (!status && !table)
		return;
	switch (fake)
	{
	case QUIET:
		n = snprintf (text, sizeof(text), "\\hostname\\quiet server%s\\map\\dm6\\maxclients\\16\n",
			atomic_fetch_add (&quietasked, 1) ? " again" : "");
		Reply (s, from, text, (size_t)n);
		break;
	case BUSY:
		n = snprintf (text, sizeof(text), "\\hostname\\busy server\\map\\dm4\\maxclients\\16\n");
		for (i = 1 ; i <= 4 ; i++)
			n += snprintf (text + n, sizeof(text) - (size_t)n, "%i %i 5 20 \"p%i\" \"\" 4 4 \"red\"\n", i, i * 3, i);
		Reply (s, from, text, (size_t)n);
		for (i = 5, n = 0 ; i <= 7 ; i++)
			n += snprintf (text + n, sizeof(text) - (size_t)n, "%i %i 5 20 \"p%i\" \"\" 4 4 \"blue\"\n", i, i * 3, i);
		Reply (s, from, text, (size_t)n);
		break;
	case RELAY:
		if (table)
		{
			memcpy (text, addresses[QUIET].ip + 12, 4);
			text[4] = (char)(BigShort ((short)addresses[QUIET].port) & 0xff);
			text[5] = (char)(BigShort ((short)addresses[QUIET].port) >> 8 & 0xff);
			text[6] = 5;
			text[7] = 0;
			Reply (s, from, text, 8);
		}
		else
			Reply (s, from, "\\hostname\\a relay\\*version\\qwfwd 1.3\n", 38);
		break;
	case LISTED:
		Reply (s, from, "\\hostname\\listed server\\map\\e1m1\n", 34);
		break;
	default:
		break;
	}
}

// the fakes' answers, on a thread of their own; the silent one never reads
static void Fakes (void *arg)
{
	byte		buf[2048];
	netadr_t	from;
	int			i, n;

	(void)arg;
	while (!atomic_load (&fakesstop))
	{
		UDP_Wait (fakes, NUMFAKES, 0.05);
		for (i = 0 ; i < NUMFAKES ; i++)
			if (i != SILENT)
				while ((n = UDP_Recv (fakes[i], buf, sizeof(buf), &from)) > 0)
					Answer (i, buf, n, from);
	}
}

/*
==============================================================================

THE HTTP LIST

Served from the test's thread as it waits: /old moved to /list

==============================================================================
*/

static tcplisten_t	*listen_;
static int			listenport;

static void Serve (void)
{
	tcpsocket_t	*c;
	netadr_t	from;
	char		request[1024], response[512], list[256];
	double		until;
	int			n = 0, got;

	if (!(c = TCP_Accept (listen_, &from)))
		return;
	request[0] = 0;
	for (until = Sys_DoubleTime () + 2 ; Sys_DoubleTime () < until && !strstr (request, "\r\n\r\n") ; )
	{
		got = TCP_Recv (c, (byte *)request + n, (int)sizeof(request) - 1 - n);
		if (got < 0)
			break;
		n += got;
		request[n] = 0;
	}
	if (!strncmp (request, "GET /old ", 9))
		snprintf (response, sizeof(response), "HTTP/1.0 302 Found\r\nLocation: /list\r\nContent-Length: 0\r\n\r\n");
	else if (!strncmp (request, "GET /qtv ", 9))
	{
		// the QTV list: the quiet server's game on a relay
		snprintf (list, sizeof(list), "{\"Servers\":[{\"GameStates\":[{\"IpAddress\":\"127.0.0.1\",\"Port\":%i,"
			"\"Link\":\"http://qtv.example:28000/watch.qtv?sid=4\"}]}]}",
			(unsigned short)BigShort ((short)addresses[QUIET].port));
		snprintf (response, sizeof(response), "HTTP/1.0 200 OK\r\nContent-Length: %i\r\n\r\n%s", (int)strlen (list), list);
	}
	else
	{
		snprintf (list, sizeof(list), "# a list\n127.0.0.1:%i\n", (unsigned short)BigShort ((short)addresses[LISTED].port));
		snprintf (response, sizeof(response), "HTTP/1.0 200 OK\r\nContent-Length: %i\r\n\r\n%s", (int)strlen (list), list);
	}
	TCP_Send (c, response, (int)strlen (response));
	TCP_Close (c);
}

/*
==============================================================================

THE LIST

==============================================================================
*/

#define DIR		"test_slist_loopback.sb"
#define CACHE	DIR "/servers.txt"

static slsnapshot_t	*latest;

// the newest snapshot, kept until a newer comes
static void Take (void)
{
	slsnapshot_t	*s = SL_TakeSnapshot ();
	char			message[256];

	if (s)
	{
		SL_FreeSnapshot (latest);
		latest = s;
	}
	while (SL_TakeMessage (message, sizeof(message)))
		printf ("message: %s", message);
}

static const slserver_t *Find (int fake)
{
	int		i;

	for (i = 0 ; latest && i < latest->numservers ; i++)
		if (!memcmp (latest->servers[i].address.ip, addresses[fake].ip, 16)
			&& latest->servers[i].address.port == addresses[fake].port)
			return &latest->servers[i];
	return NULL;
}

static void Info (const slserver_t *s, const char *key, char *value)
{
	SL_InfoValue (s ? s->info : "", key, value, 64);
}

int main (void)
{
	slsource_t			sources[3];
	slconfig_t			c;
	systhread_t			*thread;
	const slserver_t	*s;
	FILE				*f;
	char				value[64];
	double				until;
	bool				partial = false;
	int					i;

	// bound to the loopback, as -ip binds the programs': a socket on every
	// interface has the firewall ask whether the test may listen
	COM_InitArgv (3, (char *[]){"test", "-ip", "127.0.0.1"});
	UDP_Init ();
	for (i = 0 ; i < NUMFAKES ; i++)
	{
		fakes[i] = UDP_OpenQuiet ();
		CHECK (fakes[i] != NULL);
		if (!fakes[i])
			return 1;
		addresses[i] = UDP_Address (fakes[i]);
	}
	for (listenport = 27960 ; listenport < 28000 && !listen_ ; listenport++)
		listen_ = TCP_Listen (listenport);
	listenport--;
	CHECK (listen_ != NULL);
	if (!listen_)
		return 1;
	thread = Sys_StartThread ("fakes", Fakes, NULL);
	CHECK (thread != NULL);

	Sys_mkdir (DIR);
	remove (CACHE);
	if ((f = fopen (DIR "/list.txt", "wb")))
	{
		fprintf (f, "127.0.0.1:%i \\hostname\\from the file\n", (unsigned short)BigShort ((short)addresses[QUIET].port));
		fclose (f);
	}
	memset (sources, 0, sizeof(sources));
	sources[0] = (slsource_t){SL_MASTER, "Fake master", "", true};
	snprintf (sources[0].location, sizeof(sources[0].location), "127.0.0.1:%i",
		(unsigned short)BigShort ((short)addresses[MASTER].port));
	sources[1] = (slsource_t){SL_URL, "Fake list", "", true};
	snprintf (sources[1].location, sizeof(sources[1].location), "http://127.0.0.1:%i/old", listenport);
	sources[2] = (slsource_t){SL_FILE, "Fake file", "list.txt", true};

	SL_DefaultConfig (&c);
	c.pingrate = c.inforate = 1000;
	c.proxyrate = 100;
	c.pings = 2;
	c.pingtimeout = c.infotimeout = c.proxytimeout = c.mastertimeout = 0.3;
	c.inforetries = c.proxyretries = c.masterretries = 2;
	snprintf (c.qtvlist, sizeof(c.qtvlist), "http://127.0.0.1:%i/qtv", listenport);

	// no cache: nothing, before a packet goes
	CHECK (SL_Start (DIR, CACHE, sources, 3));
	Take ();
	CHECK (latest && !latest->numservers && !latest->scanning);

	// the scan, the list readable while it runs
	SL_Refresh (sources, 3, &c);
	for (until = Sys_DoubleTime () + 10 ; Sys_DoubleTime () < until ; )
	{
		Serve ();
		Take ();
		if (latest && latest->scanning && latest->numservers)
			partial = true;
		if (latest && !latest->scanning && latest->generation > 2)
			break;
		UDP_Wait (&fakes[SILENT], 1, 0.005);
	}
	CHECK (partial);
	CHECK (latest && !latest->scanning && latest->numservers == 5);

	s = Find (QUIET);
	Info (s, "hostname", value);
	CHECK (s && s->state == SL_ALIVE && s->ping >= 0 && s->samples >= 1 && !strcmp (value, "quiet server"));
	CHECK (s && s->players == 0 && s->sources == 5);		// the master's and the file's
	s = Find (BUSY);
	Info (s, "hostname", value);
	CHECK (s && s->players == 7 && s->numroster == 7 && !strcmp (value, "busy server"));
	CHECK (s && !strcmp (s->roster[6].name, "p7") && !strcmp (s->roster[6].team, "blue"));
	s = Find (SILENT);
	CHECK (s && s->state == SL_DEAD && s->ping == -1 && !*s->info);
	s = Find (RELAY);
	CHECK (s && s->proxy == SL_QWFWD);
	s = Find (LISTED);
	Info (s, "hostname", value);
	CHECK (s && s->state == SL_ALIVE && s->sources == 2 && !strcmp (value, "listed server"));
	CHECK (latest && latest->sources[0].state == SLSRC_ANSWERED && latest->sources[1].state == SLSRC_ANSWERED
		&& latest->sources[2].state == SLSRC_ANSWERED);
	CHECK (latest && latest->sources[0].servers == 4 && latest->sources[1].servers == 1 && latest->sources[2].servers == 1);

	// the QTV list, read after the sources: the quiet server's game a stream
	for (until = Sys_DoubleTime () + 3 ; Sys_DoubleTime () < until && (!(s = Find (QUIET)) || !s->qtv[0]) ; )
	{
		Serve ();
		Take ();
		UDP_Wait (&fakes[SILENT], 1, 0.005);
	}
	s = Find (QUIET);
	CHECK (s && !strcmp (s->qtv, "4@qtv.example:28000"));
	s = Find (BUSY);
	CHECK (s && !s->qtv[0]);

	// asked again ahead of the rest
	SL_Describe (addresses[QUIET]);
	for (until = Sys_DoubleTime () + 3, value[0] = 0 ; Sys_DoubleTime () < until && strcmp (value, "quiet server again") ; )
	{
		Serve ();
		Take ();
		Info (Find (QUIET), "hostname", value);
		UDP_Wait (&fakes[SILENT], 1, 0.005);
	}
	CHECK (!strcmp (value, "quiet server again"));

	// stopped, the cache saved; the next start shows it at once
	SL_Shutdown ();
	SL_FreeSnapshot (latest);
	latest = NULL;
	CHECK (SL_Start (DIR, CACHE, sources, 3));
	Take ();
	CHECK (latest && latest->numservers == 5);
	s = Find (BUSY);
	Info (s, "hostname", value);
	CHECK (s && s->state == SL_CACHED && s->ping >= 0 && s->players == 7 && !s->numroster && !strcmp (value, "busy server"));
	CHECK (s && s->sources == 1);
	s = Find (QUIET);
	CHECK (s && !strcmp (s->qtv, "4@qtv.example:28000"));
	SL_Shutdown ();
	SL_FreeSnapshot (latest);

	atomic_store (&fakesstop, true);
	if (thread)
		Sys_JoinThread (thread);
	for (i = 0 ; i < NUMFAKES ; i++)
		UDP_Close (fakes[i]);
	TCP_CloseListen (listen_);
	remove (CACHE);
	remove (DIR "/list.txt");
	UDP_Shutdown ();
	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("slist_loopback: a scan of fakes on the loopback, its cache next time\n");
	return 0;
}
