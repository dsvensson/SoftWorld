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

extern	quakeparms_t	host_parms;
extern	bool			host_initialized;	// true if into command execution
extern	double			host_frametime;
extern	double			realtime;			// not bounded in any way, changed at
											// start of every frame, never reset

void	Host_Init (quakeparms_t *parms);
void	Host_Shutdown (void);
void	Host_Frame (float time);
[[noreturn]] void Host_Error (char *error, ...);
[[noreturn]] void Host_EndGame (char *message, ...);
