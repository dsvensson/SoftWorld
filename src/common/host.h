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
// host.h -- what the program's main loop shares with the engine modules

#include "q_types.h"

// the host system specifies the base of the directory tree and the command line
typedef struct
{
	char	*basedir;
	char	*cachedir;
	int		argc;
	char	**argv;
} quakeparms_t;

// what every program's host shares with the modules
typedef struct
{
	quakeparms_t	parms;
	bool			initialized;	// true once commands are executed
	bool			dedicated;		// a server without a client
	double			realtime;		// seconds since startup; advanced once a host frame, never paused
} host_t;

extern	host_t	host;

void	Host_Init (quakeparms_t *parms);
void	Host_Shutdown (void);
void	Host_Frame (double time);
double	Host_FrameWait (void);		// seconds until the next frame is due
[[noreturn]] void Host_Error (char *error, ...);
[[noreturn]] void Host_EndGame (char *message, ...);
