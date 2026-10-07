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
// slist_http.c -- a url source's list, over HTTP or HTTPS
//
// A GET as HTTP/1.0 has it (the body to the connection's end, no chunks to put
// together), a Host for the name's servers, and redirects followed, from
// http:// to https:// too. TLS is mbedTLS's (the one a program has, WebRTC's
// where it is in), the server's certificate checked against the roots the
// system trusts and the name asked for (SNI). TLS 1.2 at most: 1.3 needs
// mbedTLS's PSA key store, which isn't safe from two threads (libdatachannel
// has its own on its threads).

#include "slist_local.h"

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

#define SL_CONNECTTIME	5		// seconds to connect
#define SL_FETCHTIME	20		// to have it all
#define SL_REDIRECTS	5
#define SL_MAXHEADERS	16384

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
} slhttp_t;

static int SL_BioSend (void *ctx, const unsigned char *buf, size_t length)
{
	slhttp_t	*h = ctx;
	int			n = length > INT_MAX ? INT_MAX : (int)length;

	return TCP_StreamWrite (h->stream, buf, n, h->deadline) ? n : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int SL_BioRecv (void *ctx, unsigned char *buf, size_t length)
{
	slhttp_t	*h = ctx;
	int			n = TCP_StreamRead (h->stream, buf, length > INT_MAX ? INT_MAX : (int)length, h->deadline);

	return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

// a root the system trusts; one that doesn't parse is left out
static void SL_AddRoot (void *ctx, const void *data, size_t length)
{
	mbedtls_x509_crt_parse (ctx, data, length);
}

static void SL_Close (slhttp_t *h)
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
static bool SL_StartTLS (slhttp_t *h, const char *host, char *error, size_t errorsize)
{
	static const char	personal[] = "softworld-slist";
	char				why[128];
	uint32_t			flags;
	int					ret;

	h->tls = true;
	mbedtls_ssl_init (&h->ssl);
	mbedtls_ssl_config_init (&h->conf);
	mbedtls_ctr_drbg_init (&h->drbg);
	mbedtls_entropy_init (&h->entropy);
	mbedtls_x509_crt_init (&h->roots);

	if (!Sys_TrustedRoots (SL_AddRoot, &h->roots) || !h->roots.raw.len)
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
	mbedtls_ssl_set_bio (&h->ssl, h, SL_BioSend, SL_BioRecv, NULL);
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

static bool SL_Write (slhttp_t *h, const char *data, size_t length)
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
static int SL_Read (slhttp_t *h, char *buf, size_t size)
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
} slurl_t;

static bool SL_ParseURL (const char *url, slurl_t *u)
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
static void SL_ResolveURL (const slurl_t *base, const char *location, char *out, size_t size)
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
static const char *SL_Header (const char *head, const char *name, char *value, size_t size)
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

typedef enum { SL_GOT, SL_REDIRECTED, SL_FAILED } slgot_t;

// one GET of the URL: its body, or where it moved to (in location)
static slgot_t SL_Get (const slurl_t *u, size_t max, char **body, size_t *length, char *location, size_t locationsize,
	char *error, size_t errorsize, double started)
{
	slhttp_t	h = {0};
	netadr_t	a;
	char		request[1536], hostport[300], value[64], *data = NULL, *grown, *head;
	size_t		have = 0, room = 0, headlength;
	int			n, status;
	long		contentlength;

	if (!SL_ParseAddress (u->host, u->port, &a) && !UDP_Resolve (u->host, false, &a))
	{
		snprintf (error, errorsize, "%s isn't found", u->host);
		return SL_FAILED;
	}
	a.type = NA_IP;
	a.port = (unsigned short)BigShort ((short)u->port);
	if (!(h.stream = TCP_StreamOpen (&a, Sys_DoubleTime () + SL_CONNECTTIME)))
	{
		snprintf (error, errorsize, "%s doesn't answer", u->host);
		return SL_FAILED;
	}
	h.deadline = started + SL_FETCHTIME;
	if (u->tls && !SL_StartTLS (&h, u->host, error, errorsize))
	{
		SL_Close (&h);
		return SL_FAILED;
	}

	snprintf (hostport, sizeof(hostport), strchr (u->host, ':') ? "[%s]" : "%s", u->host);
	if (u->port != (u->tls ? 443 : 80))
		snprintf (hostport + strlen (hostport), sizeof(hostport) - strlen (hostport), ":%i", u->port);
	snprintf (request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SoftWorld/%4.2f\r\n"
		"Accept: */*\r\nConnection: close\r\n\r\n", u->path, hostport, VERSION);
	if (!SL_Write (&h, request, strlen (request)))
	{
		snprintf (error, errorsize, "%s stopped answering", u->host);
		SL_Close (&h);
		return SL_FAILED;
	}

	// all of it, to the connection's end
	for (;;)
	{
		if (have + 4096 + 1 > room)
		{
			if (have > max + SL_MAXHEADERS)
			{
				snprintf (error, errorsize, "the list is too long");
				goto failed;
			}
			room = room ? room * 2 : 65536;
			if (!(grown = realloc (data, room)))
			{
				snprintf (error, errorsize, "out of memory");
				goto failed;
			}
			data = grown;
		}
		n = SL_Read (&h, data + have, room - have - 1);
		if (!n)
			break;
		if (n < 0)
		{
			snprintf (error, errorsize, "%s stopped answering", u->host);
			goto failed;
		}
		have += (size_t)n;
	}
	SL_Close (&h);
	if (!data)
	{
		snprintf (error, errorsize, "%s answered nothing", u->host);
		return SL_FAILED;
	}
	data[have] = 0;

	// the status line and headers, then the body
	if (!(head = strstr (data, "\r\n\r\n")) || sscanf (data, "HTTP/%*d.%*d %d", &status) != 1)
	{
		snprintf (error, errorsize, "%s's answer isn't HTTP", u->host);
		free (data);
		return SL_FAILED;
	}
	*head = 0;
	headlength = (size_t)(head - data) + 4;
	if (status >= 300 && status < 400 && SL_Header (data, "Location", location, locationsize))
	{
		free (data);
		return SL_REDIRECTED;
	}
	if (status < 200 || status >= 300)
	{
		snprintf (error, errorsize, "%s answered %i", u->host, status);
		free (data);
		return SL_FAILED;
	}
	if (SL_Header (data, "Content-Length", value, sizeof(value)) && (contentlength = atol (value)) >= 0
		&& (size_t)contentlength > have - headlength)
	{
		snprintf (error, errorsize, "%s's list was cut short", u->host);
		free (data);
		return SL_FAILED;
	}
	*length = have - headlength;
	if (*length > max)
		*length = max;
	memmove (data, data + headlength, *length);
	data[*length] = 0;
	*body = data;
	return SL_GOT;

failed:
	SL_Close (&h);
	free (data);
	return SL_FAILED;
}

char *SL_HttpGet (const char *url, size_t max, size_t *length, char *error, size_t errorsize)
{
	char		current[2048], location[2048];
	char		*body = NULL;
	double		started = Sys_DoubleTime ();
	slurl_t		u;
	int			redirects;

	Q_strncpyz (current, url, sizeof(current));
	for (redirects = 0 ; redirects <= SL_REDIRECTS ; redirects++)
	{
		if (!SL_ParseURL (current, &u))
		{
			snprintf (error, errorsize, "%s isn't an http:// or https:// URL", current);
			return NULL;
		}
		switch (SL_Get (&u, max, &body, length, location, sizeof(location), error, errorsize, started))
		{
		case SL_GOT:
			return body;
		case SL_REDIRECTED:
			SL_ResolveURL (&u, location, current, sizeof(current));
			break;
		case SL_FAILED:
		default:
			return NULL;
		}
	}
	snprintf (error, errorsize, "%s redirected too often", url);
	return NULL;
}
