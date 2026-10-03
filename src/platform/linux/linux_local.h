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
// linux_local.h -- shared by the Linux platform files; the waits and the
// worker threads are posix_local.h's

#include "posix_local.h"

#include <stdint.h>

// sys_linux.c: the display's fd in the wait queue, which the queue reports as
// SYS_WAIT_WINDOW while it has something to read
enum { SYS_WAIT_WINDOW = 8 };
bool	Sys_AddWindowFd (int fd);

// a time on CLOCK_MONOTONIC, in nanoseconds, as Sys_DoubleTime has it
double	Sys_MonotonicToTime (uint64_t ns);

//
// the programs with a client
//

// sys_linux_gui.c: the window has the focus; the window can't be seen
extern	bool	ActiveApp, Minimized;

// vid_vulkan.c: the focus came or went, the compositor stopped or started
// showing the window, Alt+Enter
void	VID_AppActivate (bool active);
void	VID_WindowSuspended (bool suspended);
void	VID_ToggleFullscreen (void);

// in_linux.c: the keyboard's focus. Wayland needs the main loop to schedule
// key repeats. IN_NextRepeat returns the next repeat time (0 for none),
// and IN_Repeat emits repeats when due.
void	IN_WindowActivated (bool active);
double	IN_NextRepeat (void);
void	IN_Repeat (void);

// Physical keys keep bindings independent of the keyboard layout.
int		IN_EvdevKey (unsigned code);

// in_evdev.c
void	IN_InitGamepad (void);
void	IN_PollGamepad (void);
