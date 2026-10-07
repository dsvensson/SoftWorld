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

#include "q_types.h"

//
// the menu: menu QuakeC (cl_menu.c)
//
void M_Init (void);			// the commands and builtins
void M_Start (void);		// the QuakeC, once every command and cvar is registered
void M_Shutdown (void);
void M_Draw (void);
void M_Keydown (int key, int character);	// the key, and the character it types (0 if none)
void M_Keyup (int key);
void M_ToggleMenu_f (void);	// Escape and togglemenu
bool M_QuitPrompt (void);	// quit asks the menu: false if it can't

//
// the server browser's list for the menu (cl_slist.c)
//
void SB_Init (void);			// the sb_* cvars
void SB_Adopt (void);			// the newest list, at the top of M_Draw
void SB_Frame (bool connecting);	// each frame: no scan while connecting
void SB_Shutdown (void);
