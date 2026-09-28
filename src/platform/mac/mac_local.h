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
// mac_local.h -- shared by the macOS platform files

#include "q_types.h"

// sys_mac.c: the file descriptors Sys_WaitUntil wakes up for (console input,
// sockets), in a kqueue. Edge-triggered: an fd wakes it when something new
// arrives, not again while unread data waits (the client leaves packets
// unread until its frame is due). False for what a kqueue can't wait on, such
// as /dev/null.
bool	Sys_AddWaitFd (int fd);
void	Sys_RemoveWaitFd (int fd);
int		Sys_WaitQueue (void);		// the kqueue itself, created on first use

// a timer in the kqueue for when the wait ends: critical, so not put off to
// fire with others as a plain timeout is (by over a millisecond)
void	Sys_SetWaitTimer (double until);
void	Sys_ClearWaitTimer (void);

// what the kqueue has: its timer fired, an fd has something, or a signal
// interrupted the wait; waits for one of them if block
enum { SYS_WAIT_TIMER = 1, SYS_WAIT_FD = 2, SYS_WAIT_SIGNAL = 4 };
int		Sys_ReadWaitQueue (bool block);

// each program's own: sleeps until Sys_DoubleTime () reaches until, or until
// input or a packet arrives; true if woken early
bool	Sys_WaitEvents (double until);

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
