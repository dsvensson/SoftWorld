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
// sys.h -- services every platform layer provides (platform/<os>/sys_*.c)

#include <stddef.h>

//
// files
//

// returns 1 if the file exists, -1 if not
int		Sys_FileTime (char *path);
void	Sys_mkdir (char *path);

//
// memory
//

// reserves address space without committing memory; fatal on failure
void	*Sys_ReserveMemory (size_t size);

// commits (zero-filled) memory for the first size bytes of a reservation
void	Sys_CommitMemory (void *base, size_t size);

//
// system IO
//
void	Sys_DebugLog (char *file, char *fmt, ...);

// an error will cause the entire program to exit
[[noreturn]] void Sys_Error (char *error, ...);

// send text to the console
void	Sys_Printf (char *fmt, ...);

[[noreturn]] void Sys_Quit (void);

// seconds since startup, from a monotonic high-resolution clock
double	Sys_DoubleTime (void);

// a number that differs between processes and between runs; not for secrets
unsigned	Sys_Seed (void);

// sleeps until Sys_DoubleTime () reaches time, or until input or a network packet
// arrives; may return early, so callers recheck what they wait for
void	Sys_WaitUntil (double time);

// directory holding the running executable, with '/' separators and no trailing '/'
const char *Sys_ExecutableDir (void);

// a line typed on the dedicated server console, or NULL
char	*Sys_ConsoleInput (void);

// pumps window/input events (Key_Event callbacks happen from here)
void	Sys_SendKeyEvents (void);

// returns the clipboard's text as a malloc'd string the caller frees, or NULL
char	*Sys_GetClipboardText (void);
