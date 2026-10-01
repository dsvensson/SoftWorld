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
// win_local.h -- shared by the Windows platform files

#include <windows.h>

#include "q_types.h"

extern	HINSTANCE	global_hInstance;
extern	HWND		mainwindow;
extern	bool		ActiveApp, Minimized;

// sys_win_sandbox.c (or sys_win_nosandbox.c): the AppContainer. Sys_SandboxLaunch
// runs first: outside the container it starts the program again inside it as
// the game, waits, gives its exit code and is true; false when this process is
// the game. Sys_SandboxInit after Host_Init: sys_forget_sandbox
bool	Sys_SandboxLaunch (const char *cmdline, int *code);
void	Sys_SandboxInit (void);

// sys_win.c: extra handles Sys_WaitUntil wakes up for (console input, sockets)
void	Sys_AddWaitHandle (HANDLE handle);
void	Sys_RemoveWaitHandle (HANDLE handle);

// in_rawinput.c: input messages of the main window; true if consumed
bool	IN_HandleMessage (UINT msg, WPARAM wParam, LPARAM lParam);
void	IN_WindowChanged (void);
void	IN_WindowActivated (bool active);

// in_xinput.c
void	IN_InitGamepad (void);
void	IN_PollGamepad (void);
