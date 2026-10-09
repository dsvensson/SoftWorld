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
// cl_public.h -- what the host sees of the client

#include "q_types.h"

void	CL_Init (void);			// brings up the client with its video, sound and input
void	CL_Start (void);		// the menu's QuakeC, after the server's init (its commands come last)
void	CL_Shutdown (void);		// writes the configuration and closes the devices
void	CL_WriteConfiguration (void);	// config.cfg, as CL_Shutdown writes it (a page leaving: sys_web_gui.c)

void	CL_Frame (void);		// packets, a command when due and a drawn frame, by host.realtime
bool	CL_KeysInGame (void);	// the keys are the game's, not the menu's or the console's
double	CL_FrameWait (void);	// seconds until CL_Frame has a frame to draw

void	CL_Drop (void);			// leaves the game after an error, stopping the demo loop
void	CL_AttractError (void);	// after it: attract mode passes its map by, or waits for a key
void	CL_Disconnect (void);
