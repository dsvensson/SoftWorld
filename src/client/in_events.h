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
// in_events.h -- how the platform's input layer (platform/<os>/in_*.c) talks to the client
//
// The platform turns device input into these events; the client decides what they do.

#include "q_types.h"

//
// events the platform sends
//

// keys, mouse buttons, the wheel and gamepad buttons, as K_* key numbers (keys.h)
void	Key_Event (int key, bool down);

// typed text for the console and the message line, with the keyboard layout applied
void	Key_CharEvent (int ch);

// releases every held key, e.g. when the window loses focus
void	Key_ClearStates (void);

// relative mouse motion, in device counts, since the last event
void	IN_MouseMotion (int dx, int dy);

// gamepad stick positions after the dead zone, each -1 .. 1; +y is up
void	IN_GamepadSticks (float lx, float ly, float rx, float ry);

//
// what the platform asks
//

// true while playing, or flying a demo's (QTV's) or a spectator's camera, when
// the mouse should be captured instead of pointing; not in the menu or the
// console, nor while watching a player's view (a QWD's, a player followed)
bool	IN_WantsMouse (void);

// true when mouse buttons should be sent even though the mouse isn't captured
// (menus, where they can be bound, and while watching: they fly the camera or
// go to the next player)
bool	IN_WantsMouseButtons (void);

//
// what the platform provides
//

void	IN_Init (void);
void	IN_Shutdown (void);

// called every frame: polls devices that aren't message driven, updates mouse capture
void	IN_Commands (void);
