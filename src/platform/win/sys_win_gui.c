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
// sys_win.c -- Win32 system interface for the client

#include "args.h"
#include "cmd.h"
#include "cvar.h"
#include "host.h"
#include "print.h"
#include "q_string.h"
#include "sys.h"
#include "client.h"
#include "keys.h"
#include "screen.h"
#include "vid.h"
#include "winquake.h"
#include "entry_win.h"
#include <direct.h>


#define PAUSE_SLEEP		50				// sleep time on pause or minimization
#define NOT_FOCUS_SLEEP	20				// sleep time when not focus

bool	ActiveApp, Minimized;
HINSTANCE	global_hInstance;

static HANDLE	tevent;

/*
===============================================================================

SYSTEM IO

===============================================================================
*/

/*
================
Sys_Init
================
*/
void Sys_Init (void)
{
	// make sure waits and timer events have 1 ms resolution
	timeBeginPeriod (1);
}

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];

	Host_Shutdown ();

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	MessageBox (NULL, text, "Error", MB_OK | MB_ICONERROR);

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
	if (tevent)
		CloseHandle (tevent);

	exit (0);
}

/*
================
Sys_GetClipboardText
================
*/
char *Sys_GetClipboardText (void)
{
	HANDLE	data;
	char	*text, *copy = NULL;

	if (!OpenClipboard (NULL))
		return NULL;

	data = GetClipboardData (CF_TEXT);
	if (data)
	{
		text = GlobalLock (data);
		if (text)
		{
			size_t	len = strlen (text) + 1;

			copy = malloc (len);
			if (copy)
				memcpy (copy, text, len);
			GlobalUnlock (data);
		}
	}
	CloseClipboard ();

	return copy;
}

void Sys_SendKeyEvents (void)
{
	MSG		msg;

	while (PeekMessage (&msg, NULL, 0, 0, PM_NOREMOVE))
	{
	// we always update if there are any event, even if we're paused
		scr_skipupdate = 0;

		if (!GetMessage (&msg, NULL, 0, 0))
			Sys_Quit ();
		TranslateMessage (&msg);
		DispatchMessage (&msg);
	}
}

/*
==================
SleepUntilInput
==================
*/
static void SleepUntilInput (int time)
{
	MsgWaitForMultipleObjects (1, &tevent, FALSE, (DWORD)time, QS_ALLINPUT);
}

/*
==================
Sys_WinMain
==================
*/
static char	*argv[MAX_NUM_ARGVS];
static char	*empty_string = "";

int Sys_WinMain (HINSTANCE hInstance, LPSTR lpCmdLine, [[maybe_unused]] int nCmdShow)
{
	quakeparms_t	parms;
	double			time, oldtime, newtime;
	static	char	cwd[1024];
	size_t			len;

	global_hInstance = hInstance;

	if (!GetCurrentDirectory (sizeof(cwd), cwd))
		Sys_Error ("Couldn't determine current directory");

	len = strlen (cwd);
	if (len && cwd[len-1] == '/')
		cwd[len-1] = 0;

	parms.basedir = cwd;
	parms.cachedir = NULL;

	parms.argc = 1;
	argv[0] = empty_string;

	while (*lpCmdLine && (parms.argc < MAX_NUM_ARGVS))
	{
		while (*lpCmdLine && ((*lpCmdLine <= 32) || (*lpCmdLine > 126)))
			lpCmdLine++;

		if (*lpCmdLine)
		{
			argv[parms.argc] = lpCmdLine;
			parms.argc++;

			while (*lpCmdLine && ((*lpCmdLine > 32) && (*lpCmdLine <= 126)))
				lpCmdLine++;

			if (*lpCmdLine)
			{
				*lpCmdLine = 0;
				lpCmdLine++;
			}
		}
	}

	parms.argv = argv;

	COM_InitArgv (parms.argc, parms.argv);

	parms.argc = com_argc;
	parms.argv = com_argv;

	tevent = CreateEvent (NULL, FALSE, FALSE, NULL);

	if (!tevent)
		Sys_Error ("Couldn't create event");

	Sys_Init ();

// because sound is off until we become active
	S_BlockSound ();

	Sys_Printf ("Host_Init\n");
	Host_Init (&parms);

	oldtime = Sys_DoubleTime ();

	/* main window message loop */
	while (1)
	{
	// yield the CPU for a little while when paused, minimized, or not the focus
		if ((cl.paused && !ActiveApp) || Minimized || block_drawing)
		{
			SleepUntilInput (PAUSE_SLEEP);
			scr_skipupdate = 1;		// no point in bothering to draw
		}
		else if (!ActiveApp)
		{
			SleepUntilInput (NOT_FOCUS_SLEEP);
		}

		newtime = Sys_DoubleTime ();
		time = newtime - oldtime;
		Host_Frame ((float)time);
		oldtime = newtime;
	}
}
