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

#include "q_types.h"

#include <stddef.h>

//
// files
//

// returns 1 if the file exists, -1 if not
int		Sys_FileTime (char *path);
void	Sys_mkdir (char *path);

// each entry of a directory but . and ..: its name, and whether it is a
// directory itself; false if the directory can't be read
bool	Sys_ListDir (const char *path, void (*entry) (void *ctx, const char *name, bool isdir), void *ctx);

//
// memory
//

// reserves address space without committing memory; fatal on failure
void	*Sys_ReserveMemory (size_t size);

// commits (zero-filled) memory for the first size bytes of a reservation
void	Sys_CommitMemory (void *base, size_t size);

// the same, failing with NULL and false instead
void	*Sys_TryReserveMemory (size_t size);
bool	Sys_TryCommitMemory (void *base, size_t size);

// returns a reservation of size bytes, committed or not
void	Sys_ReleaseMemory (void *base, size_t size);

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
// arrives; may return early, so callers recheck what they wait for. exact (a
// frame's pacing) spins the end of the wait to come back on time; otherwise
// it may come back as late as the system's timer wakes (a server's frames)
void	Sys_WaitUntil (double time, bool exact);
// ends the main thread's Sys_WaitUntil now, or its next one: from any thread,
// for what came on it (WebRTC's packets, on libdatachannel's threads)
void	Sys_Wake (void);

// a line typed on the dedicated server console, or NULL
char	*Sys_ConsoleInput (void);

// pumps window/input events (Key_Event callbacks happen from here)
void	Sys_SendKeyEvents (void);

// returns the clipboard's text as a malloc'd string the caller frees, or NULL
char	*Sys_GetClipboardText (void);
// puts text (UTF-8) on the clipboard, what pastes in other programs
void	Sys_SetClipboardText (const char *text);

// the root certificates the system trusts (for HTTPS), each handed to add as
// it is: DER, or PEM text whose length counts its ending NUL (a bundle of
// them); false where there are none to give (the web). From any thread.
bool	Sys_TrustedRoots (void (*add) (void *ctx, const void *data, size_t length), void *ctx);

//
// the machine, as f_version and f_system tell it
//

typedef struct
{
	unsigned	memory;			// MB
	char		cpu[128];		// the CPU's name, "" where unknown
	int			mhz;			// its clock, 0 where unknown
} sys_info_t;

void	Sys_SystemInfo (sys_info_t *info);

// the platform as ezQuake names it: Win64, Linux64, MacOSX
const char	*Sys_Platform (void);

//
// worker threads
//

// the processor's cores (not the threads each may run at once)
int		Sys_NumCores (void);

// the worker threads besides the calling one, 0 for none; they are started
// and stopped here, between runs of Sys_Parallel
void	Sys_SetWorkers (int workers);

// job (ctx, i) for each i of 0 .. count-1, spread over the calling thread and
// the workers in no particular order; returns once all have finished. Jobs
// must not call Sys_Parallel, Sys_SetWorkers or Sys_Error.
void	Sys_Parallel (int count, void (*job) (void *ctx, int index), void *ctx);

// Sys_Parallel in two: the workers start on the jobs, and the calling thread
// goes on with something else until Sys_ParallelFinish, which takes the jobs
// left and returns once all have finished. Between the two the calling thread
// must not call Sys_Parallel, Sys_ParallelStart or Sys_SetWorkers, nor change
// what the jobs read. Without workers the jobs are all run by Finish.
void	Sys_ParallelStart (int count, void (*job) (void *ctx, int index), void *ctx);
void	Sys_ParallelFinish (void);

// while on, a worker out of jobs waits a while for the next run before it
// sleeps: on while a frame is drawn, whose runs are moments apart, where
// waking a worker took longer than its share of a run; off between frames,
// where sleeping is cheaper than spinning
void	Sys_SpinWorkers (bool on);

//
// threads of their own (the server list's), apart from the workers
//

typedef struct systhread_s systhread_t;

// func (arg) on a thread of its own, named so for debuggers; NULL if it can't
// be started (on the web, whose threads are the workers'). The thread must not
// call Sys_Error.
systhread_t	*Sys_StartThread (const char *name, void (*func) (void *arg), void *arg);
// waits for the thread's func to return, and frees it
void	Sys_JoinThread (systhread_t *t);
// lets the thread run on alone: it goes when its func returns
void	Sys_DetachThread (systhread_t *t);

// sleeps while the 32 bits at address (an atomic) hold value, or until woken;
// it may wake early. Sys_WakeAddress wakes every thread sleeping on address.
void	Sys_WaitAddress (void *address, uint32_t value);
void	Sys_WakeAddress (void *address);
