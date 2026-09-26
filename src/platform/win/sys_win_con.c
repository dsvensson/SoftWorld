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
#include "args.h"
#include "cvar.h"
#include "host.h"
#include "net.h"
#include "print.h"
#include "server.h"
#include "sys.h"

#include <stdlib.h>
#include "entry_win.h"
#include "win_local.h"
#include <direct.h>


cvar_t	sys_nostdout = {.name = "sys_nostdout", .string = "0"};

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

//    MessageBox(NULL, text, "Error", 0 /* MB_OK */ );
	printf ("ERROR: %s\n", text);

	exit (1);
}

/*
================
Sys_ConsoleInput

A line typed on the console, edited as it is typed. Ctrl+C types "quit".
================
*/
static HANDLE			con_input;		// NULL when stdin isn't a console
static HANDLE			con_output;
static volatile LONG	con_quit;

static BOOL WINAPI Sys_CtrlHandler (DWORD type)
{
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
	{
		InterlockedExchange (&con_quit, 1);
		return TRUE;
	}
	return FALSE;
}

static void Sys_Echo (const wchar_t *text, DWORD len)
{
	DWORD	written;

	if (con_output)
		WriteConsoleW (con_output, text, len, &written, NULL);
}

char *Sys_ConsoleInput (void)
{
	static char		line[256];
	static int		len;
	INPUT_RECORD	rec;
	DWORD			count;
	wchar_t			ch;

	if (InterlockedExchange (&con_quit, 0))
		return "quit";
	if (!con_input)
		return NULL;

	// drain every event, so the input handle stops waking the main loop
	while (GetNumberOfConsoleInputEvents (con_input, &count) && count)
	{
		if (!ReadConsoleInputW (con_input, &rec, 1, &count) || !count)
			break;
		if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
			continue;

		ch = rec.Event.KeyEvent.uChar.UnicodeChar;
		if (ch == L'\r')
		{
			Sys_Echo (L"\r\n", 2);
			line[len] = 0;
			len = 0;
			return line;
		}
		if (ch == L'\b')
		{
			if (len)
			{
				len--;
				Sys_Echo (L"\b \b", 3);
			}
			continue;
		}
		if (ch >= 32 && ch < 127 && len < (int)sizeof(line) - 1)
		{
			line[len++] = (char)ch;
			Sys_Echo (&ch, 1);
		}
	}
	return NULL;
}

/*
================
Sys_InitConsole
================
*/
static void Sys_InitConsole (void)
{
	DWORD	mode;

	SetConsoleCtrlHandler (Sys_CtrlHandler, TRUE);

	con_input = GetStdHandle (STD_INPUT_HANDLE);
	if (!con_input || !GetConsoleMode (con_input, &mode))
	{
		con_input = NULL;		// redirected: no typing
		return;
	}
	// raw key events; the line is edited and echoed here
	SetConsoleMode (con_input, mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
	con_output = GetStdHandle (STD_OUTPUT_HANDLE);
	if (!GetConsoleMode (con_output, &mode))
		con_output = NULL;
	Sys_AddWaitHandle (con_input);
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
	exit (0);
}


/*
=============
Sys_Init

Quake calls this so the system can register variables before host_hunklevel
is marked
=============
*/
void Sys_Init (void)
{
	Cvar_RegisterVariable (&sys_nostdout);
}

/*
==================
main

==================
*/
char	*newargv[256];

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
	SV_Init (&parms);

// run one frame immediately for first heartbeat
	SV_Frame (0.1f);		

//
// main loop
//
	oldtime = Sys_DoubleTime () - 0.1;
	while (1)
	{
	// sleep until physics is due, a packet arrives or something is typed
		Sys_WaitUntil (Sys_DoubleTime () + SV_NextFrameWait ());

	// find time passed since last cycle
		newtime = Sys_DoubleTime ();
		time = newtime - oldtime;
		oldtime = newtime;
		
		SV_Frame ((float)time);
	}	

	return true;
}


