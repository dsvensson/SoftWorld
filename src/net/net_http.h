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
// net_http.h -- GETs over HTTP and HTTPS, which block until they are done:
// for a worker thread (the server browser's lists, maps from
// cl_download_mapsrc). A page has none (net_http_none.c).

#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdbool.h>
#include <stddef.h>

// a piece of the body as it comes, total the length the answer gives (-1
// where it gives none); false stops the GET
typedef bool (*httpbody_t) (void *ctx, const void *data, size_t length, long long total);

// url's body handed to body as it comes, redirects followed (from http:// to
// https:// too), given up after timeout seconds without a byte; false with
// why in error, but where body stopped it
bool	HTTP_Get (const char *url, double timeout, httpbody_t body, void *ctx, char *error, size_t errorsize);

// url's whole body, NUL-terminated (malloc's), its length in length; NULL
// with why in error, past max bytes too
char	*HTTP_GetAll (const char *url, size_t max, double timeout, size_t *length, char *error, size_t errorsize);

#endif
