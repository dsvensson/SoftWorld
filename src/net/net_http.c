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
// net_http.c -- GETs over HTTP or HTTPS (net_http.h)
//
// A GET as HTTP/1.0 has it (the body to the connection's end, no chunks to put
// together), a Host for the name's servers, and redirects followed, from
// http:// to https:// too. The body is handed on as it comes. TLS is mbedTLS's
// (the one a program has, WebRTC's where it is in), the server's certificate
// checked against the roots the system trusts and the name asked for (SNI).
// TLS 1.2 at most: 1.3 needs mbedTLS's PSA key store, which isn't safe from
// two threads (libdatachannel has its own on its threads).

#include "net_http.h"

#include "net.h"
#include "net_socket.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"
#include "version.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_CONNECTTIME	5		// seconds to connect
#define HTTP_REDIRECTS		5
#define HTTP_MAXHEAD		16384	// the status line and headers

typedef struct
{
	tcpstream_t		*stream;
	double			deadline;
	bool			tls;
	mbedtls_ssl_context			ssl;
	mbedtls_ssl_config			conf;
	mbedtls_ctr_drbg_context	drbg;
	mbedtls_entropy_context		entropy;
	mbedtls_x509_crt			roots;
} http_t;

static int HTTP_BioSend (void *ctx, const unsigned char *buf, size_t length)
{
	http_t	*h = ctx;
	int		n = length > INT_MAX ? INT_MAX : (int)length;

	return TCP_StreamWrite (h->stream, buf, n, h->deadline) ? n : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int HTTP_BioRecv (void *ctx, unsigned char *buf, size_t length)
{
	http_t	*h = ctx;
	int		n = TCP_StreamRead (h->stream, buf, length > INT_MAX ? INT_MAX : (int)length, h->deadline);

	return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

// a root the system trusts; one that doesn't parse is left out
static void HTTP_AddRoot (void *ctx, const void *data, size_t length)
{
	mbedtls_x509_crt_parse (ctx, data, length);
}

static void HTTP_Close (http_t *h)
{
	if (h->tls)
	{
		mbedtls_ssl_free (&h->ssl);
		mbedtls_ssl_config_free (&h->conf);
		mbedtls_ctr_drbg_free (&h->drbg);
		mbedtls_entropy_free (&h->entropy);
		mbedtls_x509_crt_free (&h->roots);
	}
	if (h->stream)
		TCP_StreamClose (h->stream);
	h->stream = NULL;
	h->tls = false;
}

// the handshake, the certificate checked; false with why in error
static bool HTTP_StartTLS (http_t *h, const char *host, char *error, size_t errorsize)
{
	static const char	personal[] = "softworld-http";
	char				why[128];
	uint32_t			flags;
	int					ret;

	h->tls = true;
	mbedtls_ssl_init (&h->ssl);
	mbedtls_ssl_config_init (&h->conf);
	mbedtls_ctr_drbg_init (&h->drbg);
	mbedtls_entropy_init (&h->entropy);
	mbedtls_x509_crt_init (&h->roots);

	if (!Sys_TrustedRoots (HTTP_AddRoot, &h->roots) || !h->roots.raw.len)
	{
		snprintf (error, errorsize, "the certificates the system trusts can't be read");
		return false;
	}
	if ((ret = mbedtls_ctr_drbg_seed (&h->drbg, mbedtls_entropy_func, &h->entropy, (const byte *)personal,
			sizeof(personal) - 1))
		|| (ret = mbedtls_ssl_config_defaults (&h->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT)))
		goto failed;
	mbedtls_ssl_conf_authmode (&h->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain (&h->conf, &h->roots, NULL);
	mbedtls_ssl_conf_rng (&h->conf, mbedtls_ctr_drbg_random, &h->drbg);
	mbedtls_ssl_conf_max_tls_version (&h->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	if ((ret = mbedtls_ssl_setup (&h->ssl, &h->conf)) || (ret = mbedtls_ssl_set_hostname (&h->ssl, host)))
		goto failed;
	mbedtls_ssl_set_bio (&h->ssl, h, HTTP_BioSend, HTTP_BioRecv, NULL);
	while ((ret = mbedtls_ssl_handshake (&h->ssl)))
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			if ((flags = mbedtls_ssl_get_verify_result (&h->ssl)) && flags != (uint32_t)-1)
			{
				mbedtls_x509_crt_verify_info (why, sizeof(why), "", flags);
				why[strcspn (why, "\n")] = 0;
				snprintf (error, errorsize, "%s's certificate isn't trusted: %s", host, why);
				return false;
			}
			goto failed;
		}
	return true;

failed:
	mbedtls_strerror (ret, why, sizeof(why));
	snprintf (error, errorsize, "TLS with %s: %s", host, why);
	return false;
}

static bool HTTP_Write (http_t *h, const char *data, size_t length)
{
	int		ret;

	if (!h->tls)
		return TCP_StreamWrite (h->stream, data, (int)length, h->deadline);
	while (length)
	{
		ret = mbedtls_ssl_write (&h->ssl, (const byte *)data, length);
		if (ret > 0)
		{
			data += ret;
			length -= (size_t)ret;
		}
		else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			return false;
	}
	return true;
}

// the bytes read, 0 at the end, -1 on an error or at the deadline
static int HTTP_Read (http_t *h, char *buf, size_t size)
{
	int		ret;

	if (!h->tls)
		return TCP_StreamRead (h->stream, buf, size > INT_MAX ? INT_MAX : (int)size, h->deadline);
	for (;;)
	{
		ret = mbedtls_ssl_read (&h->ssl, (byte *)buf, size);
		if (ret >= 0)
			return ret;
		// a server closing without saying so ends the body too (its length checks it)
		if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF)
			return 0;
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			return -1;
	}
}

/*
==============================================================================

URLS

==============================================================================
*/

typedef struct
{
	bool	tls;
	char	host[256];
	int		port;
	char	path[1024];
} httpurl_t;

static bool HTTP_ParseURL (const char *url, httpurl_t *u)
{
	const char	*authority, *end, *at, *close, *colon;
	size_t		n;
	char		*stop;
	long		port;

	if (!Q_strncasecmp (url, "http://", 7))
	{
		u->tls = false;
		u->port = 80;
		authority = url + 7;
	}
	else if (!Q_strncasecmp (url, "https://", 8))
	{
		u->tls = true;
		u->port = 443;
		authority = url + 8;
	}
	else
		return false;

	end = authority + strcspn (authority, "/?#");
	at = memchr (authority, '@', (size_t)(end - authority));
	if (at)
		authority = at + 1;		// credentials aren't sent
	colon = NULL;
	if (*authority == '[')
	{
		if (!(close = memchr (authority, ']', (size_t)(end - authority))))
			return false;
		n = (size_t)(close - authority - 1);
		if (n >= sizeof(u->host))
			return false;
		memcpy (u->host, authority + 1, n);
		if (close + 1 < end && close[1] == ':')
			colon = close + 1;
	}
	else
	{
		colon = memchr (authority, ':', (size_t)(end - authority));
		n = (size_t)((colon ? colon : end) - authority);
		if (n >= sizeof(u->host))
			return false;
		memcpy (u->host, authority, n);
	}
	u->host[n] = 0;
	if (!n)
		return false;
	if (colon && colon + 1 < end)
	{
		port = strtol (colon + 1, &stop, 10);
		if (stop != end || port <= 0 || port > 65535)
			return false;
		u->port = (int)port;
	}

	// the path and query, the fragment the client's own
	n = strcspn (end, "#");
	if (*end != '/')
		snprintf (u->path, sizeof(u->path), "/%.*s", (int)n, end);
	else
		snprintf (u->path, sizeof(u->path), "%.*s", (int)n, end);
	return true;
}

// where a redirect leads from base: a URL, or one relative to base's
static void HTTP_ResolveURL (const httpurl_t *base, const char *location, char *out, size_t size)
{
	char	origin[300], dir[1024], *slash;

	if (strstr (location, "://"))
	{
		Q_strncpyz (out, location, size);
		return;
	}
	snprintf (origin, sizeof(origin), strchr (base->host, ':') ? "%s://[%s]:%i" : "%s://%s:%i",
		base->tls ? "https" : "http", base->host, base->port);
	if (location[0] == '/' && location[1] == '/')
		snprintf (out, size, "%s:%s", base->tls ? "https" : "http", location);
	else if (location[0] == '/')
		snprintf (out, size, "%s%s", origin, location);
	else
	{
		Q_strncpyz (dir, base->path, sizeof(dir));
		dir[strcspn (dir, "?")] = 0;
		if ((slash = strrchr (dir, '/')))
			slash[1] = 0;
		snprintf (out, size, "%s%s%s", origin, dir, location);
	}
}

// a header's value of the response's head, NULL if it has none
static const char *HTTP_Header (const char *head, const char *name, char *value, size_t size)
{
	const char	*line, *end;
	size_t		namelength = strlen (name), n;

	for (line = strchr (head, '\n') ; line ; line = strchr (line, '\n'))
	{
		line++;
		if (Q_strncasecmp (line, name, namelength) || line[namelength] != ':')
			continue;
		line += namelength + 1;
		while (*line == ' ' || *line == '\t')
			line++;
		end = line + strcspn (line, "\r\n");
		n = (size_t)(end - line);
		if (n > size - 1)
			n = size - 1;
		memcpy (value, line, n);
		value[n] = 0;
		return value;
	}
	return NULL;
}

/*
==============================================================================

THE GET

==============================================================================
*/

typedef enum { HTTP_GOT, HTTP_REDIRECTED, HTTP_FAILED } httpgot_t;

// the head of the answer, to its blank line: its length with the line, the
// body that came with it after; 0 with why in error
static size_t HTTP_ReadHead (http_t *h, const httpurl_t *u, double timeout, char *head, size_t *have,
	char *error, size_t errorsize)
{
	char	*end;
	int		n;

	for (*have = 0 ; ; *have += (size_t)n)
	{
		head[*have] = 0;
		if ((end = strstr (head, "\r\n\r\n")))
			return (size_t)(end - head) + 4;
		if (*have == HTTP_MAXHEAD)
		{
			snprintf (error, errorsize, "%s's answer isn't HTTP", u->host);
			return 0;
		}
		h->deadline = Sys_DoubleTime () + timeout;
		n = HTTP_Read (h, head + *have, HTTP_MAXHEAD - *have);
		if (n <= 0)
		{
			snprintf (error, errorsize, n ? "%s stopped answering" : "%s answered nothing", u->host);
			return 0;
		}
	}
}

// one GET of the URL: its body handed to body, or where it moved to (in location)
static httpgot_t HTTP_Fetch (const httpurl_t *u, double timeout, httpbody_t body, void *ctx,
	char *location, size_t locationsize, char *error, size_t errorsize)
{
	http_t		h = {0};
	netadr_t	a;
	char		request[1536], hostport[300], value[64], head[HTTP_MAXHEAD + 1], buf[16384];
	size_t		have, headlength;
	long long	total = -1, got;
	int			n, status;

	if (!UDP_Resolve (u->host, false, &a))
	{
		snprintf (error, errorsize, "%s isn't found", u->host);
		return HTTP_FAILED;
	}
	a.type = NA_IP;
	a.port = (unsigned short)BigShort ((short)u->port);
	if (!(h.stream = TCP_StreamOpen (&a, Sys_DoubleTime () + HTTP_CONNECTTIME)))
	{
		snprintf (error, errorsize, "%s doesn't answer", u->host);
		return HTTP_FAILED;
	}
	h.deadline = Sys_DoubleTime () + timeout;
	if (u->tls && !HTTP_StartTLS (&h, u->host, error, errorsize))
	{
		HTTP_Close (&h);
		return HTTP_FAILED;
	}

	snprintf (hostport, sizeof(hostport), strchr (u->host, ':') ? "[%s]" : "%s", u->host);
	if (u->port != (u->tls ? 443 : 80))
		snprintf (hostport + strlen (hostport), sizeof(hostport) - strlen (hostport), ":%i", u->port);
	snprintf (request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SoftWorld/%4.2f\r\n"
		"Accept: */*\r\nConnection: close\r\n\r\n", u->path, hostport, VERSION);
	if (!HTTP_Write (&h, request, strlen (request)))
	{
		snprintf (error, errorsize, "%s stopped answering", u->host);
		HTTP_Close (&h);
		return HTTP_FAILED;
	}

	// the status line and headers
	if (!(headlength = HTTP_ReadHead (&h, u, timeout, head, &have, error, errorsize)))
		goto failed;
	head[headlength - 4] = 0;
	if (sscanf (head, "HTTP/%*d.%*d %d", &status) != 1)
	{
		snprintf (error, errorsize, "%s's answer isn't HTTP", u->host);
		goto failed;
	}
	if (status >= 300 && status < 400 && HTTP_Header (head, "Location", location, locationsize))
	{
		HTTP_Close (&h);
		return HTTP_REDIRECTED;
	}
	if (status < 200 || status >= 300)
	{
		snprintf (error, errorsize, "%s answered %i", u->host, status);
		goto failed;
	}
	if (HTTP_Header (head, "Content-Length", value, sizeof(value)))
		total = atoll (value);

	// the body: what came with the head, then the rest to the connection's end
	got = (long long)(have - headlength);
	if (got && !body (ctx, head + headlength, (size_t)got, total))
		goto failed;		// why is body's to say
	for (;;)
	{
		h.deadline = Sys_DoubleTime () + timeout;
		n = HTTP_Read (&h, buf, sizeof(buf));
		if (!n)
			break;
		if (n < 0)
		{
			snprintf (error, errorsize, "%s stopped answering", u->host);
			goto failed;
		}
		got += n;
		if (!body (ctx, buf, (size_t)n, total))
			goto failed;
	}
	HTTP_Close (&h);
	if (total >= 0 && got < total)
	{
		snprintf (error, errorsize, "%s's answer was cut short", u->host);
		return HTTP_FAILED;
	}
	return HTTP_GOT;

failed:
	HTTP_Close (&h);
	return HTTP_FAILED;
}

bool HTTP_Get (const char *url, double timeout, httpbody_t body, void *ctx, char *error, size_t errorsize)
{
	char		current[2048], location[2048];
	httpurl_t	u;
	int			redirects;

	Q_strncpyz (current, url, sizeof(current));
	for (redirects = 0 ; redirects <= HTTP_REDIRECTS ; redirects++)
	{
		if (!HTTP_ParseURL (current, &u))
		{
			snprintf (error, errorsize, "%s isn't an http:// or https:// URL", current);
			return false;
		}
		switch (HTTP_Fetch (&u, timeout, body, ctx, location, sizeof(location), error, errorsize))
		{
		case HTTP_GOT:
			return true;
		case HTTP_REDIRECTED:
			HTTP_ResolveURL (&u, location, current, sizeof(current));
			break;
		case HTTP_FAILED:
		default:
			return false;
		}
	}
	snprintf (error, errorsize, "%s redirected too often", url);
	return false;
}

/*
==============================================================================

THE WHOLE BODY

==============================================================================
*/

typedef struct
{
	char	*data;
	size_t	have, room, max;
} httpall_t;

static bool HTTP_Collect (void *ctx, const void *data, size_t length, long long total)
{
	httpall_t	*all = ctx;
	char		*grown;

	(void)total;
	if (length > all->max - all->have)
		return false;
	if (all->have + length + 1 > all->room)
	{
		all->room = all->room ? all->room * 2 : 65536;
		while (all->room < all->have + length + 1)
			all->room *= 2;
		if (!(grown = realloc (all->data, all->room)))
			return false;
		all->data = grown;
	}
	memcpy (all->data + all->have, data, length);
	all->have += length;
	return true;
}

char *HTTP_GetAll (const char *url, size_t max, double timeout, size_t *length, char *error, size_t errorsize)
{
	httpall_t	all = {.max = max};

	error[0] = 0;
	if (!HTTP_Get (url, timeout, HTTP_Collect, &all, error, errorsize))
	{
		if (!error[0])
			snprintf (error, errorsize, "the answer is longer than %zu bytes, or there's no memory for it", max);
		free (all.data);
		return NULL;
	}
	if (!all.data && !(all.data = malloc (1)))
	{
		snprintf (error, errorsize, "out of memory");
		return NULL;
	}
	all.data[all.have] = 0;
	*length = all.have;
	return all.data;
}
