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
// info.h -- key/value info strings (userinfo, serverinfo)

#define	MAX_INFO_STRING	196
#define	MAX_SERVERINFO_STRING	512
#define	MAX_LOCALINFO_STRING	32768

char *Info_ValueForKey (char *s, const char *key);
void Info_RemoveKey (char *s, const char *key);
void Info_RemovePrefixedKeys (char *start, char prefix);

// which characters are kept when storing a value in an info string
typedef enum
{
	INFO_CHARSET_USERINFO,	// client rules: high bits only in "name", "team" lowercased
	INFO_CHARSET_ASCII,		// strip high bits and control characters
	INFO_CHARSET_ANY,		// keep everything
} info_charset_t;

void Info_SetValueForKey (char *s, const char *key, const char *value, int maxsize, info_charset_t charset);

void Info_SetValueForStarKey (char *s, const char *key, const char *value, int maxsize, info_charset_t charset);
void Info_Print (char *s);
