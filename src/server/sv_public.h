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

// a showcase's level (the client's attract mode, sv_attract.c): a map run
// with its game directory's progs for no one but this program's client, which
// watches as a spectator from origin; closed to the network
struct cmap_s;
struct fs_chain_s;
typedef struct
{
	const char			*map;		// its name, without maps/ and .bsp
	const char			*progs;		// progs.dat or qwprogs.dat (the one built in if missing)
	struct fs_chain_s	*progsdir;	// the game directory alone they are read from; the server keeps it
	float				deathmatch;	// the rules: 0 single player (NetQuake's progs), 1 deathmatch
	struct cmap_s		*built;		// its collision map (CM_BuildMap), which the server adopts; or NULL
	float				origin[3], angles[3];	// where the client watches from
} sv_attract_t;

bool	SV_AttractLevel (const sv_attract_t *level);	// false if it couldn't be spawned
void	SV_AttractEnd (void);		// the showcase's server goes
bool	SV_Attracting (void);		// the server runs a showcase
// another reference to the level's collision map, if it is name's (the
// client's, from the server's), and its checksum2; NULL if not
struct cmap_s	*SV_ShareMap (const char *name, unsigned *checksum2);
void	SV_SetAttractStop (void (*stop) (void));	// called before the user's own level
