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
// view.h

#include "cvar.h"
#include "mathlib.h"

extern	cvar_t		v_gamma;

void V_Init (void);
void V_RenderView (void);
bool V_SetupView (void);	// V_RenderView in two: the view and gun of this frame,
void V_DrawView (bool drawcrosshair);	// and drawing them, CSQC's in between
void V_UpdateBlend (void);
