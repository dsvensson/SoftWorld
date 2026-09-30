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
// sys_con_posix.c -- the dedicated server's console on macOS and Linux: lines
// typed in the terminal, which edits them, and Ctrl+C

#include "args.h"
#include "cvar.h"
#include "host.h"
#include "print.h"
#include "sys.h"
#include "entry.h"
#include "posix_local.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static cvar_t	sys_nostdout = {.name = "sys_nostdout", .string = "0",
	.description = "Stops the dedicated server printing its console output to standard output.",
	.values = (const cvar_value_t[]){{"0", "Output printed"}, {"1", "Nothing printed"}, {0}}};

/*
================
Sys_Error
================
*/
void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];

	va_start (argptr,error);
	vsnprintf (text, sizeof(text), error,argptr);
	va_end (argptr);

	printf ("ERROR: %s\n", text);

	exit (1);
}

/*
================
Sys_ConsoleInput

A line typed on the console, without its newline. Ctrl+C types "quit".
================
*/
static volatile sig_atomic_t	con_quit;
static bool		con_input;			// stdin is there to read, until it ends
static bool		con_waited;			// stdin wakes Sys_WaitUntil (not /dev/null)

static void Sys_Interrupt (int sig)
{
	(void)sig;
	con_quit = 1;
}

char *Sys_ConsoleInput (void)
{
	static char		line[256];
	static char		pending[1024];	// read, not yet returned
	static size_t	numpending;
	struct pollfd	p = {.fd = STDIN_FILENO, .events = POLLIN};
	char			*newline;
	size_t			len;
	ssize_t			got;

	if (con_quit)
	{
		con_quit = 0;
		return "quit";
	}

	// read what there is (a terminal gives a whole line at a time)
	if (con_input && numpending < sizeof(pending) && poll (&p, 1, 0) > 0)
	{
		got = read (STDIN_FILENO, pending + numpending, sizeof(pending) - numpending);
		if (got > 0)
			numpending += (size_t)got;
		else if (got == 0 || (errno != EINTR && errno != EAGAIN))
		{	// the end of the input: nothing more to wait for
			if (con_waited)
				Sys_RemoveWaitFd (STDIN_FILENO);
			con_input = false;
		}
	}

	newline = memchr (pending, '\n', numpending);
	if (!newline)
	{
		if (numpending < sizeof(pending) && con_input)
			return NULL;
		if (!numpending)
			return NULL;
		newline = pending + numpending - 1;		// a full buffer, or the last line: all of it
	}
	len = (size_t)(newline - pending);
	if (len > sizeof(line) - 1)
		len = sizeof(line) - 1;
	memcpy (line, pending, len);
	line[len] = 0;
	if (len && line[len - 1] == '\r')
		line[len - 1] = 0;
	numpending -= (size_t)(newline + 1 - pending);
	memmove (pending, newline + 1, numpending);
	return line;
}

/*
================
Sys_InitConsole
================
*/
static void Sys_InitConsole (void)
{
	struct sigaction	sa = {.sa_handler = Sys_Interrupt};

	// no SA_RESTART: Ctrl+C also ends the wait for the next frame
	sigemptyset (&sa.sa_mask);
	sigaction (SIGINT, &sa, NULL);
	sigaction (SIGTERM, &sa, NULL);

	con_input = true;
	con_waited = Sys_AddWaitFd (STDIN_FILENO);
}

/*
================
Sys_WaitEvents

The console's input, the sockets, or a signal
================
*/
bool Sys_WaitEvents (double until)
{
	int		what;

	Sys_SetWaitTimer (until);
	what = Sys_ReadWaitQueue (true);
	if (!(what & SYS_WAIT_TIMER))
		Sys_ClearWaitTimer ();
	return what != SYS_WAIT_TIMER;
}

/*
================
Sys_Printf
================
*/
void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;

	if (sys_nostdout.value)
		return;

	va_start (argptr,fmt);
	vprintf (fmt,argptr);
	va_end (argptr);
	fflush (stdout);
}

/*
================
Sys_Quit
================
*/
void Sys_Quit (void)
{
	Host_Shutdown ();
	exit (0);
}

/*
=============
Sys_Init

Quake calls this so the system can register variables before host_hunklevel
is marked
=============
*/
static void Sys_Init (void)
{
	Cvar_RegisterVariable (&sys_nostdout);
}

/*
==================
Sys_ConsoleMain
==================
*/
int Sys_ConsoleMain (int argc, char **argv)
{
	quakeparms_t	parms;
	double			newtime, time, oldtime;

	COM_InitArgv (argc, argv);

	parms.argc = com_argc;
	parms.argv = com_argv;

	parms.basedir = ".";
	parms.cachedir = NULL;

	Sys_InitConsole ();
	Sys_Init ();
	Host_Init (&parms);

// run one frame immediately for first heartbeat
	Host_Frame (0.1);

//
// main loop
//
	oldtime = Sys_DoubleTime () - 0.1;
	while (1)
	{
	// sleep until physics is due, a packet arrives or something is typed
		Sys_WaitUntil (Sys_DoubleTime () + Host_FrameWait ());

	// find time passed since last cycle
		newtime = Sys_DoubleTime ();
		time = newtime - oldtime;
		oldtime = newtime;

		Host_Frame (time);
	}

	return 0;
}
