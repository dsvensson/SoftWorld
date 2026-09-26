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
// screen.h

#include "cvar.h"
#include "q_types.h"
#include "vid.h"

void SCR_Init (void);

void SCR_UpdateScreen (void);

void SCR_CenterPrint (char *str);

// what the screen shows
typedef struct
{
	float	con_current;		// scan lines of console currently drawn
	int		sb_lines;			// scan lines of status bar
	vrect_t	vrect;				// the 3D view
	bool	disabled_for_loading;
} scr_state_t;

extern	scr_state_t	scr;

extern	cvar_t		scr_viewsize;
