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
// sys_web_gui.c -- the system interface of the programs with a client in a
// browser: the frames as the page runs them, errors, and the clipboard
//
// A page can't wait: its own thread returns to the browser between frames, and
// the browser calls the game back for the next (sys_web.js). With vsync that is
// each animation frame, the display's; without it when the frame is due, as
// Sys_WaitUntil would wake the other systems, or sooner when input comes.

#include "args.h"
#include "cl_public.h"
#include "cmd.h"
#include "host.h"
#include "print.h"
#include "sys.h"
#include "sound.h"
#include "entry.h"
#include "vid_common.h"
#include "web_local.h"

#include <emscripten/emscripten.h>

#include <math.h>
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

	web_stop (text, 1);
	emscripten_force_exit (1);
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

	web_stop ("SoftWorld quit. Reload the page to play again.", 0);
	emscripten_force_exit (0);
}

// what the browser pasted: Ctrl+V lets it, and the key comes after
char *Sys_GetClipboardText (void)
{
	return IN_PastedText ();
}

void Sys_SetClipboardText (const char *text)
{
	IN_CopyText (text);
}

/*
===============================================================================

EVENTS

===============================================================================
*/

void Sys_SendKeyEvents (void)
{
	IN_PumpEvents ();
}

/*
===============================================================================

THE PAGE'S FRAMES

===============================================================================
*/

static double	sys_oldtime;		// when the last frame began
static double	sys_due;			// when the next is due, without vsync

/*
================
Sys_WebFrame

A frame from the page (sys_web.js): with vsync one each animation frame, the
display's, and -1 back for the next; without, a frame when it is due or
something came (woken), and the seconds back until the next is due, from
when this one began as on the other systems
================
*/
EMSCRIPTEN_KEEPALIVE double Sys_WebFrame (int woken)
{
	double	newtime = Sys_DoubleTime ();

	if (VID_Vsync ())
	{
		Host_Frame (newtime - sys_oldtime);
		sys_oldtime = newtime;
		sys_due = newtime;
		return -1;
	}

	if (newtime >= sys_due || woken)
	{
		Host_Frame (newtime - sys_oldtime);
		sys_oldtime = newtime;
		sys_due = newtime + Host_FrameWait ();
	}
	// a frame due already is due now: -1 is vsync's alone
	return fmax (sys_due - Sys_DoubleTime (), 0);
}

/*
================
Sys_WebHidden

The page is hidden, and may be closed without another frame: config.cfg is
written now, as quitting would (the file system keeps it, fs_web.js)
================
*/
EMSCRIPTEN_KEEPALIVE void Sys_WebHidden (void)
{
	CL_WriteConfiguration ();
	fflush (NULL);
}

// a command from the page's script (Module.command, the browser's console)
EMSCRIPTEN_KEEPALIVE void Sys_WebCommand (const char *text)
{
	Cbuf_AddText (text);
	Cbuf_AddText ("\n");
}

/*
===============================================================================

MAIN

===============================================================================
*/

int Sys_WebMain (int argc, char **argv)
{
	quakeparms_t	parms;

	COM_InitArgv (argc, argv);
	parms.argc = com_argc;
	parms.argv = com_argv;
	parms.basedir = ".";		// the game's files, where the page's file system has them (fs_web.js)
	parms.cachedir = NULL;

// because sound is off until we become active
	S_BlockSound ();

	Sys_Printf ("Host_Init\n");
	Host_Init (&parms);

	sys_oldtime = sys_due = Sys_DoubleTime ();
	web_start_loop ();
	emscripten_exit_with_live_runtime ();
}
