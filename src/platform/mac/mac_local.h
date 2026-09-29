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
// mac_local.h -- shared by the macOS platform files; the waits and the
// worker threads are posix_local.h's

#include "posix_local.h"

//
// the programs with a client
//

// sys_mac_gui.m: the window has the focus; the window is minimized or hidden
extern	bool	ActiveApp, Minimized;

// vid_metal.m: Option+Enter
void	VID_ToggleFullscreen (void);

// in_mac.m: the window's keyboard and mouse
void	IN_WindowChanged (void);			// moved or resized
void	IN_WindowActivated (bool active);

// in_gamepad_mac.m
void	IN_InitGamepad (void);
void	IN_PollGamepad (void);

#ifdef __OBJC__
@class NSEvent, NSWindow;

extern	NSWindow	*vid_window;		// vid_metal.m: the main window

// an event from the queue, before AppKit gets it; true if it was input taken here
bool	IN_HandleEvent (NSEvent *event);
#endif
