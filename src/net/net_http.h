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
// net_http.h -- GETs over HTTP and HTTPS: ones that block until they are done,
// for a worker thread (the server browser's lists, maps from
// cl_download_mapsrc), and ones that run on their own while the caller polls
// (the hub's lists, demos played from a URL). A page has only the second, as
// the browser's fetch() (net_http_web.c); its blocking ones fail
// (net_http_none.c).

#ifndef NET_HTTP_H
#define NET_HTTP_H

#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAXURL			4096	// a URL's characters, with its 0
#define HTTP_MAXHEADERS		2048	// the request lines a GET adds

// a piece of the body as it comes, total the length the answer gives (-1
// where it gives none); false stops the GET
typedef bool (*httpbody_t) (void *ctx, const void *data, size_t length, long long total);

// the answer's status and head (its status line and header lines), before
// its body: the last answer's, not a redirect's
typedef void (*httphead_t) (void *ctx, int status, const char *head);

// url's body handed to body as it comes, redirects followed (from http:// to
// https:// too), given up after timeout seconds without a byte; false with
// why in error, but where body stopped it. Any 2xx is an answer.
bool	HTTP_Get (const char *url, double timeout, httpbody_t body, void *ctx, char *error, size_t errorsize);

// HTTP_Get, with request lines more (headers: each "Name: value\r\n", or
// NULL) and the answer's head told to head (NULL for none); with head, the
// body of an answer that isn't a 2xx is handed on too, before it fails
bool	HTTP_Request (const char *url, const char *headers, double timeout, httphead_t head, httpbody_t body, void *ctx,
			char *error, size_t errorsize);

// url's whole body, NUL-terminated (malloc's), its length in length; NULL
// with why in error, past max bytes too
char	*HTTP_GetAll (const char *url, size_t max, double timeout, size_t *length, char *error, size_t errorsize);

// a header's value out of an answer's head (its status line, then lines of
// "Name: value"), the name's case aside; NULL if it has none
const char	*HTTP_HeaderValue (const char *head, const char *name, char *value, size_t size);

//
// GETs that run on their own: a thread each, a page's fetch(). The caller
// polls them, on one thread.
//

typedef enum { HTTP_RUNNING, HTTP_DONE, HTTP_FAILED } httpstate_t;

typedef struct httpfetch_s httpfetch_t;

// url's GET started, with request lines more: "Name: value" each, apart by
// \n or \r\n, or NULL. Never NULL: one that can't start has failed.
httpfetch_t	*HTTP_Start (const char *url, const char *headers);
// the body's bytes come since, up to size: 0 when none have
int			HTTP_Read (httpfetch_t *f, void *buf, int size);
// done once its body has come and been read; failed, a 2xx's body cut short
// or an answer that isn't a 2xx too (what came of it can still be read)
httpstate_t	HTTP_State (httpfetch_t *f);
int			HTTP_Status (httpfetch_t *f);		// the answer's, 0 before it came
long long	HTTP_Length (httpfetch_t *f);		// its Content-Length, -1 while it isn't known
// a header of the answer, once it came; false if it has none
bool		HTTP_ResponseHeader (httpfetch_t *f, const char *name, char *value, size_t size);
const char	*HTTP_Error (httpfetch_t *f);		// why it failed, "" while it hasn't
// the GET stopped where it is still running, and gone; NULL is none
void		HTTP_Close (httpfetch_t *f);

// url's body as a file that grows as it comes (vfs_http.c): all of it kept, to
// seek back in; failed as the GET fails
vfsfile_t	*VFS_OpenURL (const char *url);

#endif
