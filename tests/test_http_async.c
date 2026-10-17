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
// test_http_async.c -- the GETs that run on their own (net_http.h's
// HTTP_Start) against a server on the loopback that the test serves as it
// polls: the request's lines sent, the answer's status and headers read back,
// a body that comes in two pieces, a 206 an answer, a 404's body read though it
// fails, a header that isn't one, a port no one listens on, one closed while
// it runs; HTTP/1.1's chunks put back together, cut short and corrupt, and an
// answer's end without the connection's; a gzip given only to an HTTP/1.1 GET
// that takes it (nginx's gzip_static), as it was sent; and VFS_OpenURL's file,
// with a gzip unpacked over it, and failed by a 404 without its body

#include "args.h"
#include "host.h"
#include "mem.h"
#include "net_http.h"
#include "net_socket.h"
#include "sys.h"
#include "vfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TEXTSIZE	300000

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

static udpsocket_t	*quiet;		// a socket nothing is sent to: a wait on it is a sleep

// a sleep, as test_slist_loopback's: Sys_WaitUntil is the programs' (their events)
static void Wait (double seconds)
{
	double	until = Sys_DoubleTime () + seconds, left;

	while ((left = until - Sys_DoubleTime ()) > 0)
		UDP_Wait (&quiet, 1, left);
}

/*
==============================================================================

THE SERVER

Served from the test's thread as it polls

==============================================================================
*/

static tcplisten_t	*listen_;
static int			listenport;
static tcpsocket_t	*held;			// /slow's, kept open
static tcpsocket_t	*kept;			// an answer's that is all there, kept open after
static char			*text;			// TEXTSIZE bytes of text
static byte			*gz;			// them as a gzip
static size_t		gzsize;

// all of data, as fast as the socket takes it
static bool SendAll (tcpsocket_t *c, const void *data, int length)
{
	const byte	*p = data;
	double		until = Sys_DoubleTime () + 5;
	int			n;

	while (length > 0 && Sys_DoubleTime () < until)
	{
		n = TCP_Write (c, p, length);
		if (n < 0)
			return false;
		if (!n)
			Wait (0.002);
		p += n;
		length -= n;
	}
	return length == 0;
}

static void Serve (void)
{
	tcpsocket_t	*c;
	netadr_t	from;
	char		request[4096], head[512], apikey[128] = "", *line;
	const char	*body;
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
	if ((line = strstr (request, "\r\napikey: ")))
		sscanf (line + 10, "%127[^\r]", apikey);

	if (!strncmp (request, "GET /range ", 11))
	{	// a page of a count, its body in two pieces
		snprintf (head, sizeof(head), "HTTP/1.0 206 Partial Content\r\nContent-Length: 22\r\n"
			"Content-Range: 0-14/940\r\nX-Apikey: %s\r\n\r\nfirst half,", apikey);
		SendAll (c, head, (int)strlen (head));
		Wait (0.1);
		SendAll (c, "second half", 11);
	}
	else if (!strncmp (request, "GET /missing ", 13))
	{
		body = "{\"message\":\"no such thing\"}";
		snprintf (head, sizeof(head), "HTTP/1.0 404 Not Found\r\nContent-Length: %i\r\n\r\n%s", (int)strlen (body), body);
		SendAll (c, head, (int)strlen (head));
	}
	else if (!strncmp (request, "GET /gz ", 8))
	{
		snprintf (head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Length: %i\r\n\r\n", (int)gzsize);
		SendAll (c, head, (int)strlen (head));
		SendAll (c, gz, (int)gzsize / 2);
		Wait (0.05);
		SendAll (c, gz + gzsize / 2, (int)(gzsize - gzsize / 2));
	}
	else if (!strncmp (request, "GET /chunked ", 13))
	{	// a size line and a chunk's end split between pieces, an extension and
		// a trailer, and the connection kept open after
		static const char	first[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nfirst\r\n1";
		static const char	rest[] = "\r\n,\r\n6\r\nsecond\r\n0\r\nX-Trailer: t\r\n\r\n";

		SendAll (c, first, (int)strlen (first));
		Wait (0.05);
		SendAll (c, rest, (int)strlen (rest));
		kept = c;
		return;
	}
	else if (!strncmp (request, "GET /cutchunks ", 15))
	{
		snprintf (head, sizeof(head), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nfirst\r\n");
		SendAll (c, head, (int)strlen (head));
	}
	else if (!strncmp (request, "GET /badchunks ", 15))
	{
		snprintf (head, sizeof(head), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nfirst\r\n0\r\n\r\n");
		SendAll (c, head, (int)strlen (head));
	}
	else if (!strncmp (request, "GET /kept ", 10))
	{	// all of its length, and the connection kept open after
		snprintf (head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
		SendAll (c, head, (int)strlen (head));
		kept = c;
		return;
	}
	else if (!strncmp (request, "GET /static ", 12))
	{	// gzip_static's: only the gzip, to an HTTP/1.1 GET that takes it
		if (!strstr (request, " HTTP/1.1\r\n") || !strstr (request, "\r\nAccept-Encoding: gzip\r\n"))
		{
			snprintf (head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
			SendAll (c, head, (int)strlen (head));
		}
		else
		{
			snprintf (head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %i\r\n\r\n",
				(int)gzsize);
			SendAll (c, head, (int)strlen (head));
			SendAll (c, gz, (int)gzsize);
		}
	}
	else if (!strncmp (request, "GET /slow ", 10))
	{	// its head and a little, and then nothing while it is held
		snprintf (head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\n0123456789");
		SendAll (c, head, (int)strlen (head));
		held = c;
		return;
	}
	else
	{
		snprintf (head, sizeof(head), "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
		SendAll (c, head, (int)strlen (head));
	}
	TCP_Close (c);
}

static void MakeText (void)
{
	z_stream	z = {0};
	int			i;

	text = Mem_Alloc (TEXTSIZE);
	for (i = 0 ; i < TEXTSIZE ; i++)
		text[i] = "a demo's bytes, more or less "[i % 29] + (char)((i / 1000) & 1);
	gz = Mem_Alloc (TEXTSIZE + 1024);
	CHECK (deflateInit2 (&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
	z.next_in = (byte *)text;
	z.avail_in = TEXTSIZE;
	z.next_out = gz;
	z.avail_out = TEXTSIZE + 1024;
	CHECK (deflate (&z, Z_FINISH) == Z_STREAM_END);
	gzsize = z.total_out;
	deflateEnd (&z);
}

/*
==============================================================================

THE GETS

==============================================================================
*/

static char	url[64];

static const char *URL (const char *path)
{
	snprintf (url, sizeof(url), "http://127.0.0.1:%i%s", listenport, path);
	return url;
}

// f run to its end as the test serves, its body read into out (ended)
static size_t Finish (httpfetch_t *f, char *out, size_t room)
{
	double	until = Sys_DoubleTime () + 10;
	size_t	have = 0;
	int		n;

	while (Sys_DoubleTime () < until)
	{
		Serve ();
		while ((n = HTTP_Read (f, out + have, (int)(room - 1 - have))) > 0)
			have += (size_t)n;
		if (HTTP_State (f) != HTTP_RUNNING)
			break;
		Wait (0.002);
	}
	out[have] = 0;
	return have;
}

static void TestAnswer (void)
{
	httpfetch_t	*f = HTTP_Start (URL ("/range"), "apikey: k1\nPrefer: count=exact");
	char		body[256], value[64];

	CHECK (Finish (f, body, sizeof(body)) == 22);
	CHECK (HTTP_State (f) == HTTP_DONE);
	CHECK (HTTP_Status (f) == 206);
	CHECK (!strcmp (body, "first half,second half"));
	CHECK (HTTP_ResponseHeader (f, "Content-Range", value, sizeof(value)) && !strcmp (value, "0-14/940"));
	CHECK (HTTP_ResponseHeader (f, "x-apikey", value, sizeof(value)) && !strcmp (value, "k1"));
	CHECK (!HTTP_ResponseHeader (f, "X-None", value, sizeof(value)));
	CHECK (HTTP_Length (f) == 22);
	CHECK (*HTTP_Error (f) == 0);
	HTTP_Close (f);
}

static void TestRefused (void)
{
	httpfetch_t	*f = HTTP_Start (URL ("/missing"), NULL);
	char		body[256];

	Finish (f, body, sizeof(body));
	CHECK (HTTP_State (f) == HTTP_FAILED);
	CHECK (HTTP_Status (f) == 404);
	CHECK (!strcmp (body, "{\"message\":\"no such thing\"}"));
	CHECK (strstr (HTTP_Error (f), "404") != NULL);
	HTTP_Close (f);

	// a header that isn't one fails it at once
	f = HTTP_Start (URL ("/range"), "not a header");
	CHECK (HTTP_State (f) == HTTP_FAILED && strstr (HTTP_Error (f), "isn't a header"));
	HTTP_Close (f);
	f = HTTP_Start (URL ("/range"), "X-Bad: a\x01" "b");
	CHECK (HTTP_State (f) == HTTP_FAILED);
	HTTP_Close (f);
}

// the GET's end once its body is all there, the connection still open
static void TestChunks (void)
{
	httpfetch_t	*f = HTTP_Start (URL ("/chunked"), NULL);
	double		start = Sys_DoubleTime ();
	char		body[256];

	CHECK (Finish (f, body, sizeof(body)) == 12);
	CHECK (HTTP_State (f) == HTTP_DONE && !strcmp (body, "first,second"));
	CHECK (Sys_DoubleTime () - start < 3);
	HTTP_Close (f);
	if (kept)
		TCP_Close (kept);
	kept = NULL;

	f = HTTP_Start (URL ("/cutchunks"), NULL);
	Finish (f, body, sizeof(body));
	CHECK (HTTP_State (f) == HTTP_FAILED && strstr (HTTP_Error (f), "cut short"));
	HTTP_Close (f);

	f = HTTP_Start (URL ("/badchunks"), NULL);
	Finish (f, body, sizeof(body));
	CHECK (HTTP_State (f) == HTTP_FAILED && strstr (HTTP_Error (f), "corrupt"));
	HTTP_Close (f);

	f = HTTP_Start (URL ("/kept"), NULL);
	start = Sys_DoubleTime ();
	CHECK (Finish (f, body, sizeof(body)) == 5);
	CHECK (HTTP_State (f) == HTTP_DONE && !strcmp (body, "hello"));
	CHECK (Sys_DoubleTime () - start < 3);
	HTTP_Close (f);
	if (kept)
		TCP_Close (kept);
	kept = NULL;
}

// a gzip asked for comes as it was sent, Content-Encoding telling; not asked
// for, there is none (gzip_static's 404)
static void TestEncoded (void)
{
	httpfetch_t	*f = HTTP_Start (URL ("/static"), "Accept-Encoding: gzip");
	char		*out = Mem_Alloc (TEXTSIZE + 1), value[32];

	CHECK (Finish (f, out, TEXTSIZE + 1) == gzsize && !memcmp (out, gz, gzsize));
	CHECK (HTTP_State (f) == HTTP_DONE);
	CHECK (HTTP_ResponseHeader (f, "Content-Encoding", value, sizeof(value)) && !strcmp (value, "gzip"));
	CHECK (HTTP_Length (f) == (long long)gzsize);
	HTTP_Close (f);

	f = HTTP_Start (URL ("/static"), NULL);
	Finish (f, out, TEXTSIZE + 1);
	CHECK (HTTP_State (f) == HTTP_FAILED && HTTP_Status (f) == 404);
	HTTP_Close (f);
	Mem_Free (out);
}

static void TestNobody (void)
{
	tcplisten_t	*l;
	httpfetch_t	*f;
	char		where[64], body[16];
	int			port;

	// a port that was listened on a moment ago
	for (port = listenport + 1, l = NULL ; port < listenport + 40 && !(l = TCP_Listen (port)) ; port++)
		;
	CHECK (l != NULL);
	TCP_CloseListen (l);
	snprintf (where, sizeof(where), "http://127.0.0.1:%i/", port);
	f = HTTP_Start (where, NULL);
	Finish (f, body, sizeof(body));
	CHECK (HTTP_State (f) == HTTP_FAILED);
	CHECK (HTTP_Status (f) == 0);
	CHECK (*HTTP_Error (f) != 0);
	HTTP_Close (f);
}

static void TestClosed (void)
{
	httpfetch_t	*f = HTTP_Start (URL ("/slow"), NULL);
	double		until = Sys_DoubleTime () + 5;
	char		body[64];
	int			have = 0;

	while (Sys_DoubleTime () < until && have < 10)
	{
		Serve ();
		have += HTTP_Read (f, body + have, (int)sizeof(body) - have);
		Wait (0.002);
	}
	CHECK (have == 10 && HTTP_Status (f) == 200 && HTTP_State (f) == HTTP_RUNNING);
	// closed while it runs: its thread ends as the connection does, and frees it
	HTTP_Close (f);
	if (held)
		TCP_Close (held);
	held = NULL;
	Wait (0.3);
}

static void TestFile (void)
{
	vfsfile_t	*f = VFS_OpenGunzip (VFS_OpenURL (URL ("/gz")));
	double		until = Sys_DoubleTime () + 10;
	char		*out = Mem_Alloc (TEXTSIZE + 1);
	size_t		have = 0;
	int			n;
	bool		live = false;

	while (Sys_DoubleTime () < until)
	{
		Serve ();
		while ((n = VFS_Read (f, out + have, (int)(TEXTSIZE + 1 - have))) > 0)
			have += (size_t)n;
		if (VFS_Status (f) != VFS_LIVE)
			break;
		live = true;
		Wait (0.002);
	}
	CHECK (live);
	CHECK (VFS_Status (f) == VFS_ENDED);
	CHECK (have == TEXTSIZE && !memcmp (out, text, TEXTSIZE));
	CHECK (VFS_Length (f) == TEXTSIZE);
	CHECK (VFS_Seek (f, 1000) && VFS_Read (f, out, 10) == 10 && !memcmp (out, text + 1000, 10));
	VFS_Close (f);

	// a 404: failed, its body no part of the file
	f = VFS_OpenURL (URL ("/missing"));
	for (until = Sys_DoubleTime () + 5 ; Sys_DoubleTime () < until && VFS_Status (f) == VFS_LIVE ; )
	{
		Serve ();
		Wait (0.002);
	}
	CHECK (VFS_Status (f) == VFS_FAILED);
	CHECK (strstr (VFS_Error (f), "404") != NULL);
	CHECK (VFS_Read (f, out, 16) == 0);
	VFS_Close (f);
	Mem_Free (out);
}

int main (void)
{
	// bound to the loopback, as -ip binds the programs': a socket on every
	// interface has the firewall ask whether the test may listen
	COM_InitArgv (3, (char *[]){"test", "-ip", "127.0.0.1"});
	UDP_Init ();
	quiet = UDP_OpenQuiet ();
	CHECK (quiet != NULL);
	if (!quiet)
		return 1;
	for (listenport = 27960 ; listenport < 28000 && !listen_ ; listenport++)
		listen_ = TCP_Listen (listenport);
	listenport--;
	CHECK (listen_ != NULL);
	if (!listen_)
		return 1;
	MakeText ();

	TestAnswer ();
	TestRefused ();
	TestChunks ();
	TestEncoded ();
	TestNobody ();
	TestClosed ();
	TestFile ();

	TCP_CloseListen (listen_);
	UDP_Close (quiet);
	Mem_Free (text);
	Mem_Free (gz);
	if (failures)
	{
		printf ("%d failed\n", failures);
		return 1;
	}
	printf ("http_async: all passed\n");
	return 0;
}
