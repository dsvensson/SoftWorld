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

#include "q_types.h"
#include <stdio.h>
// cvar.h

/*

cvar_t variables are used to hold scalar or string variables that can be changed or displayed at the console or prog code as well as accessed directly
in C code.

A cvar_t is initialized with its name, its default string and what it does
(the console shows it; apropos searches it), and .archive for a variable saved
to the configuration file when the game is quit. A variable whose values each
do something else has .values too:

cvar_t	r_draworder = {.name = "r_draworder", .string = "0",
	.description = "Draws the world's surfaces back to front."};
cvar_t	scr_screensize = {.name = "screensize", .string = "100", .archive = true,
	.description = "How much of the screen the view takes, in percent."};
cvar_t	vid_scalemode = {.name = "vid_scalemode", .string = "0", .archive = true,
	.description = "How the frame is scaled to the window.",
	.values = (const cvar_value_t[]){{"0", "Whole multiples, letterboxed"},
		{"1", "Filling the window"}, {0}}};

Cvars must be registered before use, or they will have a 0 value instead of the float interpretation of the string.  Generally, all cvar_t declarations should be registered in the apropriate init function before any console commands are executed:
Cvar_RegisterVariable (&host_framerate);


C code usually just references a cvar in place:
if ( r_draworder.value )

It could optionally ask for the value to be looked up for a string name:
if (Cvar_VariableValue ("r_draworder"))

Interpreted prog code can access cvars with the cvar(name) or
cvar_set (name, value) internal functions:
teamplay = cvar("teamplay");
cvar_set ("registered", "1");

The user can access cvars from the console in two ways:
r_draworder			prints the current value
r_draworder 0		sets the current value to 0
Cvars are restricted from having the same names as commands to keep this
interface from being ambiguous.
*/

// what a value of a variable does, for those whose values each do something else
typedef struct
{
	const char	*value;
	const char	*meaning;
} cvar_value_t;

typedef struct cvar_s
{
	char	*name;
	char	*string;
	bool archive;		// set to true to cause it to be saved to vars.rc
	bool userinfo;		// the client's userinfo carries it
	bool serverinfo;	// the server's serverinfo carries it
	const char	*description;		// what it does, for the console
	const cvar_value_t	*values;	// what each value does, ended by {0}; or NULL
	char	*defaultstring;			// the string it was registered with
	float	value;
	struct cvar_s *next;
} cvar_t;

void 	Cvar_RegisterVariable (cvar_t *variable);
// registers a cvar that allready has the name, string, and optionally the
// archive elements set.

void 	Cvar_Set (char *var_name, char *value);

// called when a userinfo or serverinfo cvar changes; a listen server has both
typedef void (*cvar_info_hook_t) (char *name, char *value);
void	Cvar_SetUserinfoHook (cvar_info_hook_t hook);
void	Cvar_SetServerinfoHook (cvar_info_hook_t hook);

// called after any cvar changes (QuakeC's autocvars follow them); the server and
// the client each add theirs
typedef void (*cvar_change_hook_t) (cvar_t *var);
void	Cvar_AddChangeHook (cvar_change_hook_t hook);
// equivelant to "<name> <variable>" typed at the console

void	Cvar_SetValue (char *var_name, float value);
// expands value to a string and calls Cvar_Set

float	Cvar_VariableValue (char *var_name);
// returns 0 if not defined or non numeric

char 	*Cvar_CompleteVariable (char *partial);
// attempts to match a partial variable name for command line completion
// returns NULL if nothing fits

void	Cvar_ListMatches (const char *partial, void (*match) (void *ctx, const char *name), void *ctx);
// each variable whose name begins with partial (case aside)

bool Cvar_Command (void);
// called by Cmd_ExecuteString when Cmd_Argv(0) doesn't match a known
// command.  Returns true if the command was a variable reference that
// was handled. (print or change)

void 	Cvar_WriteVariables (FILE *f);
// Writes lines containing "set variable value" for all variables
// with the archive flag set to true.

cvar_t *Cvar_FindVar (char *var_name);

cvar_t *Cvar_List (void);
// the first variable registered; the rest follow by next

