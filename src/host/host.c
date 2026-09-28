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
// host.c -- the client's main loop, with a server in the same process. The
// client-only program runs the same loop over sv_null.c.

#include "args.h"
#include "cl_public.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"
#include "host.h"
#include "in_events.h"
#include "net.h"
#include "print.h"
#include "sv_public.h"
#include "sys.h"
#include "version.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

hoststate_t	host;

// a fixed frame time, for timedemos that must draw the same frames every run
static cvar_t	host_framerate = {.name = "host_framerate", .string = "0"};

static jmp_buf	host_abort;			// Host_Error and Host_EndGame return here
static bool		host_abort_set;		// once a frame has run

/*
================
Host_EndGame

The client leaves the game and the frame is abandoned
================
*/
void Host_EndGame (char *message, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,message);
	vsnprintf (string,sizeof(string),message,argptr);
	va_end (argptr);
	Con_Printf ("\n===========================\n");
	Con_Printf ("Host_EndGame: %s\n",string);
	Con_Printf ("===========================\n\n");

	CL_Disconnect ();

	if (!host_abort_set)
		Sys_Error ("Host_EndGame: %s", string);
	longjmp (host_abort, 1);
}

/*
================
Host_Error

The client leaves the game, the demo loop stops and the frame is abandoned.
A server that failed has already stopped itself.
================
*/
void Host_Error (char *error, ...)
{
	va_list		argptr;
	char		string[1024];
	static	bool inerror = false;

	if (inerror)
		Sys_Error ("Host_Error: recursively entered");
	inerror = true;

	va_start (argptr,error);
	vsnprintf (string,sizeof(string),error,argptr);
	va_end (argptr);
	Con_Printf ("Host_Error: %s\n",string);

	if (!host_abort_set)
		Sys_Error ("Host_Error: %s", string);

	CL_Drop ();

	inerror = false;
	longjmp (host_abort, 1);
}

/*
====================
Host_Init
====================
*/
void Host_Init (quakeparms_t *parms)
{
	COM_InitArgv (parms->argc, parms->argv);
	COM_AddParm ("-game");
	COM_AddParm ("qw");

	host.parms = *parms;

	Cbuf_Init ();
	Cmd_Init ();
	COM_Init (host.parms.basedir);
	NET_Init ();
	Cvar_RegisterVariable (&host_framerate);

	CL_Init ();
	SV_Init ();

	Cbuf_InsertText ("exec quake.rc\n");
	Cbuf_AddText ("echo Type connect <internet address> or map <name> to play.\n");
	Cbuf_AddText ("cl_warncmd 1\n");

	host.initialized = true;

	Con_Printf ("\nVersion %4.2f (Build %04d)\n\n", VERSION, build_number());

	Con_Printf ("\x80\x81\x81\x81\x81\x81\x81 QuakeWorld Initialized \x81\x81\x81\x81\x81\x81\x82\n");
}

/*
==================
Host_Frame

Commands first, then the server, then the client, which reads what the
server just sent it
==================
*/
void Host_Frame (double time)
{
	if (setjmp (host_abort))
		return;			// something bad happened, or the server disconnected
	host_abort_set = true;

	if (host_framerate.value > 0)
		time = host_framerate.value;
	host.realtime += time;

	Sys_SendKeyEvents ();
	IN_Commands ();			// gamepad buttons
	Cbuf_Execute ();

	if (SV_Active ())
		SV_Frame (time);

	CL_Frame ();
}

/*
==================
Host_FrameWait

Seconds until the client or the server needs a frame
==================
*/
double Host_FrameWait (void)
{
	double	wait, svwait;

	wait = CL_FrameWait ();
	if (SV_Active ())
	{
		svwait = SV_NextFrameWait ();
		if (svwait < wait)
			wait = svwait;
	}
	return wait;
}

/*
===============
Host_Shutdown

Called from Sys_Quit and Sys_Error
===============
*/
void Host_Shutdown (void)
{
	static bool isdown = false;

	if (isdown)
		return;
	isdown = true;

	SV_Shutdown ();
	CL_Shutdown ();
	NET_Shutdown ();
}
