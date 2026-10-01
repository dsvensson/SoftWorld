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
#include "sound.h"
#include "vid.h"
#include "win_local.h"
#include "entry_win.h"
#include <direct.h>
#include <stdlib.h>



bool	ActiveApp, Minimized;
HINSTANCE	global_hInstance;


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
static void Sys_Init (void)
{
}

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];
	static bool	inerror;

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	// the console and its log see the error; an error while shutting down
	// goes straight to the message box
	if (!inerror)
	{
		inerror = true;
		Con_Printf ("Sys_Error: %s\n", text);
		Host_Shutdown ();
	}

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
		if (!GetMessage (&msg, NULL, 0, 0))
			Sys_Quit ();
		TranslateMessage (&msg);
		DispatchMessage (&msg);
	}
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
	int				code;

	global_hInstance = hInstance;

	// outside the sandbox this is the launcher, and the game a process of its own
	if (Sys_SandboxLaunch (lpCmdLine, &code))
		return code;

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

	Sys_Init ();

// because sound is off until we become active
	S_BlockSound ();

	Sys_Printf ("Host_Init\n");
	Host_Init (&parms);
	Sys_SandboxInit ();

	oldtime = Sys_DoubleTime ();

	/* main window message loop */
	while (1)
	{
		newtime = Sys_DoubleTime ();
		time = newtime - oldtime;
		Host_Frame (time);
		oldtime = newtime;

		// handle what arrived meanwhile, then sleep until the next frame is due,
		// unless input or a packet comes first
		Sys_SendKeyEvents ();
		Sys_WaitUntil (Sys_DoubleTime () + Host_FrameWait ());
	}
}
