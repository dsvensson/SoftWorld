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
// q_string.c -- string helpers shared by every module

#include "q_string.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

thread_local char	com_token[1024];

/*
============================================================================

					LIBRARY REPLACEMENT FUNCTIONS

============================================================================
*/


int Q_strncasecmp (const char *s1, const char *s2, size_t n)
{
	int		c1, c2;

	while (n--)
	{
		c1 = tolower ((unsigned char)*s1++);
		c2 = tolower ((unsigned char)*s2++);
		if (c1 != c2)
			return c1 < c2 ? -1 : 1;
		if (!c1)
			return 0;
	}
	return 0;
}

int Q_strcasecmp (const char *s1, const char *s2)
{
	return Q_strncasecmp (s1, s2, SIZE_MAX);
}

void Q_strncpyz (char *dest, const char *src, size_t size)
{
	size_t	len;

	if (!size)
		return;
	len = strlen (src);
	if (len >= size)
		len = size - 1;
	memcpy (dest, src, len);
	dest[len] = 0;
}

void Q_strncatz (char *dest, const char *src, size_t size)
{
	size_t	len = strlen (dest);

	if (len < size)
		Q_strncpyz (dest + len, src, size - len);
}

bool Q_snprintfz (char *dest, size_t size, const char *fmt, ...)
{
	va_list	args;
	int		n;

	va_start (args, fmt);
	n = vsnprintf (dest, size, fmt, args);
	va_end (args);
	return n >= 0 && (size_t)n < size;
}

int Q_atoi (const char *str)
{
	int		val;
	int		sign;
	int		c;
	
	if (*str == '-')
	{
		sign = -1;
		str++;
	}
	else
		sign = 1;
		
	val = 0;

//
// check for hex
//
	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X') )
	{
		str += 2;
		while (1)
		{
			c = *str++;
			if (c >= '0' && c <= '9')
				val = (val<<4) + c - '0';
			else if (c >= 'a' && c <= 'f')
				val = (val<<4) + c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				val = (val<<4) + c - 'A' + 10;
			else
				return val*sign;
		}
	}
	
//
// check for character
//
	if (str[0] == '\'')
	{
		return sign * str[1];
	}
	
//
// assume decimal
//
	while (1)
	{
		c = *str++;
		if (c <'0' || c > '9')
			return val*sign;
		val = val*10 + c - '0';
	}
	
	return 0;
}

float Q_atof (const char *str)
{
	double	val;
	int		sign;
	int		c;
	int		decimal, total;
	
	if (*str == '-')
	{
		sign = -1;
		str++;
	}
	else
		sign = 1;
		
	val = 0;

//
// check for hex
//
	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X') )
	{
		str += 2;
		while (1)
		{
			c = *str++;
			if (c >= '0' && c <= '9')
				val = (val*16) + c - '0';
			else if (c >= 'a' && c <= 'f')
				val = (val*16) + c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				val = (val*16) + c - 'A' + 10;
			else
				return (float)(val*sign);
		}
	}

//
// check for character
//
	if (str[0] == '\'')
	{
		return (float)(sign * str[1]);
	}

//
// assume decimal
//
	decimal = -1;
	total = 0;
	while (1)
	{
		c = *str++;
		if (c == '.')
		{
			decimal = total;
			continue;
		}
		if (c <'0' || c > '9')
			break;
		val = val*10 + c - '0';
		total++;
	}

	if (decimal == -1)
		return (float)(val*sign);
	while (total > decimal)
	{
		val /= 10;
		total--;
	}

	return (float)(val*sign);
}

/*
============
COM_StripExtension
============
*/
void COM_StripExtension (char *in, char *out)
{
	while (*in && *in != '.')
		*out++ = *in++;
	*out = 0;
}

/*
==================
COM_DefaultExtension
==================
*/
void COM_DefaultExtension (char *path, char *extension)
{
	char    *src;
//
// if path doesn't have a .EXT, append extension
// (extension should include the .)
//
	src = path + strlen(path) - 1;

	while (*src != '/' && src != path)
	{
		if (*src == '.')
			return;                 // it has an extension
		src--;
	}

	strcat (path, extension);
}

/*
==============
COM_Parse

Parse a token out of a string
==============
*/
char *COM_Parse (char *data)
{
	int		c;
	int		len;
	
	len = 0;
	com_token[0] = 0;
	
	if (!data)
		return NULL;
		
// skip whitespace
skipwhite:
	while ( (c = *data) <= ' ')
	{
		if (c == 0)
			return NULL;			// end of file;
		data++;
	}
	
// skip // comments
	if (c=='/' && data[1] == '/')
	{
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}
	

// handle quoted strings specially; a token longer than com_token holds is
// cut short, and a string the text ends in leaves the text at its end
	if (c == '\"')
	{
		data++;
		while (1)
		{
			c = *data++;
			if (c=='\"' || !c)
			{
				com_token[len] = 0;
				return c ? data : data - 1;
			}
			if (len < (int)sizeof(com_token) - 1)
				com_token[len++] = (char)c;
		}
	}

// parse a regular word
	do
	{
		if (len < (int)sizeof(com_token) - 1)
			com_token[len++] = (char)c;
		data++;
		c = *data;
	} while (c>32);
	
	com_token[len] = 0;
	return data;
}

/*
============
va

does a varargs printf into a temp buffer, so I don't need to have
varargs versions of all text functions.
FIXME: make this buffer size safe someday
============
*/
char	*va(char *format, ...)
{
	va_list		argptr;
	static char		string[1024];
	
	va_start (argptr, format);
	vsnprintf (string, sizeof(string), format,argptr);
	va_end (argptr);

	return string;
}
