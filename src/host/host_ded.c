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
// host_ded.c -- the dedicated server's main loop

#include "args.h"
#include "cmd.h"
#include "fs.h"
#include "host.h"
#include "net.h"
#include "print.h"
#include "sv_public.h"
#include "sys.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>

hoststate_t	host;

/*
================
Host_Error

A server error ends the program
================
*/
void Host_Error (char *error, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,error);
	vsnprintf (string,sizeof(string),error,argptr);
	va_end (argptr);
	Sys_Error ("%s", string);
}

/*
==================
Host_Quit_f
==================
*/
static void Host_Quit_f (void)
{
	Con_Printf ("Shutting down.\n");
	Sys_Quit ();
}

/*
====================
Host_Init
====================
*/
void Host_Init (quakeparms_t *parms)
{
	host.dedicated = true;

	COM_InitArgv (parms->argc, parms->argv);
	COM_AddParm ("-game");
	COM_AddParm ("qw");

	host.parms = *parms;

	Cbuf_Init ();
	Cmd_Init ();
	COM_Init (host.parms.basedir);
	NET_Init ();

	SV_Init ();
	Cmd_AddCommand ("quit", Host_Quit_f, "Shuts the server down and exits.");

	Cbuf_InsertText ("exec server.cfg\n");

	host.initialized = true;

	Con_Printf ("\nSoftWorld server %4.2f (Build %04d)\n\n", VERSION, build_number());
	Con_Printf ("======== QuakeWorld Initialized ========\n");

// process command line arguments
	Cmd_StuffCmds_f ();
	Cbuf_Execute ();

// if a map wasn't specified on the command line, spawn start.map
	if (!SV_Active ())
		Cmd_ExecuteString ("map start");
	if (!SV_Active ())
		Sys_Error ("Couldn't spawn a server");
}

/*
==================
Host_Frame
==================
*/
void Host_Frame (double time)
{
	char	*cmd;

	host.realtime += time;

	// commands typed on the console run as if they came from server.cfg
	while ((cmd = Sys_ConsoleInput ()) != NULL)
	{
		Cbuf_AddText (cmd);
		Cbuf_AddText ("\n");	// two lines read in a frame stay two commands
	}
	Cbuf_Execute ();

	SV_Frame (time, false);
}

/*
==================
Host_FrameWait
==================
*/
double Host_FrameWait (void)
{
	return SV_NextFrameWait ();
}

/*
===============
Host_Shutdown

Called from Sys_Quit
===============
*/
void Host_Shutdown (void)
{
	static bool isdown = false;

	if (isdown)
		return;
	isdown = true;

	SV_Shutdown ();
	NET_Shutdown ();
}
