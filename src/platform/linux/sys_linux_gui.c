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
// sys_linux_gui.c -- the Linux system interface for the programs with a client:
// the main loop and its waits, errors, and the clipboard
//
// The main loop is the game's, as on Windows; the display's events are taken
// between frames. The wait for the next frame is on the epoll (sys_linux.c),
// which has the display's fd with the sockets and the timer that ends the wait.

#include "args.h"
#include "cmd.h"
#include "host.h"
#include "print.h"
#include "sys.h"
#include "sound.h"
#include "entry.h"
#include "window.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

bool	ActiveApp, Minimized;

/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];
	static bool	inerror;

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	// the console and its log see the error; an error while shutting down
	// goes straight out
	if (!inerror)
	{
		inerror = true;
		Con_Printf ("Sys_Error: %s\n", text);
		Host_Shutdown ();
	}
	fprintf (stderr, "Error: %s\n", text);

	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;

	va_start (argptr, fmt);
	vprintf (fmt, argptr);
	va_end (argptr);
}

void Sys_Quit (void)
{
	Host_Shutdown ();

	exit (0);
}

char *Sys_GetClipboardText (void)
{
	return window ? window->GetClipboardText () : NULL;
}

/*
===============================================================================

EVENTS

===============================================================================
*/

void Sys_SendKeyEvents (void)
{
	if (!window)
		return;
	window->ReadEvents ();
	IN_Repeat ();
}

/*
================
Sys_WaitEvents

Input or another event for the window, a packet, a key's repeat, or the
timer at until
================
*/
bool Sys_WaitEvents (double until)
{
	double	repeat = IN_NextRepeat ();
	int		what = 0;
	bool	early = false;

	if (repeat && repeat < until)
		until = repeat;
	Sys_SetWaitTimer (until);
	for (;;)
	{
		// the display's events queued already end the wait at once
		if (window->PrepareRead ())
		{
			window->FinishRead (false);
			early = true;
			break;
		}
		what = Sys_ReadWaitQueue (true);
		if (window->FinishRead ((what & SYS_WAIT_WINDOW) != 0) || (what & (SYS_WAIT_FD | SYS_WAIT_SIGNAL)))
		{
			early = true;
			break;
		}
		if (what & SYS_WAIT_TIMER)
			break;
		// only the Vulkan driver's events came: wait on
	}
	if (!(what & SYS_WAIT_TIMER))
		Sys_ClearWaitTimer ();

	if (repeat && Sys_DoubleTime () >= repeat)
	{
		IN_Repeat ();
		early = true;
	}
	return early;
}

/*
===============================================================================

MAIN

===============================================================================
*/

int Sys_LinuxMain (int argc, char **argv)
{
	quakeparms_t	parms;
	double			time, oldtime, newtime;

	// the console's echo a line at a time, also into a pipe or a file
	setvbuf (stdout, NULL, _IOLBF, 0);

	COM_InitArgv (argc, argv);
	parms.argc = com_argc;
	parms.argv = com_argv;
	parms.basedir = ".";
	parms.cachedir = NULL;

// because sound is off until we become active
	S_BlockSound ();

	Sys_Printf ("Host_Init\n");
	Host_Init (&parms);

	oldtime = Sys_DoubleTime ();

	/* main loop */
	while (1)
	{
		newtime = Sys_DoubleTime ();
		time = newtime - oldtime;
		Host_Frame (time);
		oldtime = newtime;

		// handle what arrived meanwhile, then sleep until the next frame is due,
		// unless input or a packet comes first; due from when this frame began,
		// as host.realtime was then
		Sys_SendKeyEvents ();
		Sys_WaitUntil (newtime + Host_FrameWait (), true);
	}
}
