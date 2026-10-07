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
// sv_public.h -- what the host and the client see of the server. The
// client-only program links sv_null.c, which has no server behind these.

#include "q_types.h"

void	SV_Init (void);			// registers the server's commands and variables
void	SV_Shutdown (void);		// ends the game and closes the logs, at exit

bool	SV_Active (void);		// a map is running
void	SV_Kill (void);			// ends the game, telling the clients

// reads packets, runs physics and answers; away: this program's player isn't in
// the game (its menu or console has the keys), which holds a game of one still
void	SV_Frame (double time, bool away);
double	SV_NextFrameWait (void);	// seconds until the server needs a frame
