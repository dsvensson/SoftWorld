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
// q_string.h -- string helpers shared by every module

#include "q_types.h"

#include <string.h>

#define Q_memset(d, f, c) memset((d), (f), (c))
#define Q_memcpy(d, s, c) memcpy((d), (s), (c))
#define Q_memcmp(m1, m2, c) memcmp((m1), (m2), (c))
#define Q_strcpy(d, s) strcpy((d), (s))
#define Q_strncpy(d, s, n) strncpy((d), (s), (n))
#define Q_strlen(s) ((int)strlen(s))
#define Q_strrchr(s, c) strrchr((s), (c))
#define Q_strcat(d, s) strcat((d), (s))
#define Q_strcmp(s1, s2) strcmp((s1), (s2))
#define Q_strncmp(s1, s2, n) strncmp((s1), (s2), (n))

int		Q_strcasecmp (const char *s1, const char *s2);
int		Q_strncasecmp (const char *s1, const char *s2, size_t n);

// copies at most size-1 characters and always terminates dest
void	Q_strncpyz (char *dest, const char *src, size_t size);

// appends src to dest (a buffer of size bytes), truncating and always terminating
void	Q_strncatz (char *dest, const char *src, size_t size);

int		Q_atoi (char *str);
float	Q_atof (char *str);

// does a varargs printf into a temp buffer
char	*va (char *format, ...);

// parses the next token into com_token; returns the text after it, or NULL at the end
extern	thread_local char	com_token[1024];
char	*COM_Parse (char *data);

void	COM_StripExtension (char *in, char *out);
void	COM_DefaultExtension (char *path, char *extension);
