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
// net_http_none.c -- a page's GETs: none, it has no threads to wait on one

#include "net_http.h"

#include <stdio.h>

bool HTTP_Get (const char *url, double timeout, httpbody_t body, void *ctx, char *error, size_t errorsize)
{
	(void)url;
	(void)timeout;
	(void)body;
	(void)ctx;
	snprintf (error, errorsize, "no HTTP here");
	return false;
}

char *HTTP_GetAll (const char *url, size_t max, double timeout, size_t *length, char *error, size_t errorsize)
{
	(void)url;
	(void)max;
	(void)timeout;
	(void)length;
	snprintf (error, errorsize, "no HTTP here");
	return NULL;
}
