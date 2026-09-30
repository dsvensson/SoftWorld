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
// cvar.c -- dynamic variable tracking

#include "cmd.h"
#include "cvar.h"
#include "info.h"
#include "mem.h"
#include "print.h"
#include "q_string.h"
#include "sys.h"

static cvar_t	*cvar_vars;

/*
============
Cvar_FindVar
============
*/
cvar_t *Cvar_FindVar (char *var_name)
{
	cvar_t	*var;
	
	for (var=cvar_vars ; var ; var=var->next)
		if (!Q_strcmp (var_name, var->name))
			return var;

	return NULL;
}

/*
============
Cvar_VariableValue
============
*/
float	Cvar_VariableValue (char *var_name)
{
	cvar_t	*var;
	
	var = Cvar_FindVar (var_name);
	if (!var)
		return 0;
	return Q_atof (var->string);
}


/*
============
Cvar_CompleteVariable
============
*/
char *Cvar_CompleteVariable (char *partial)
{
	cvar_t		*cvar;
	int			len;
	
	len = Q_strlen(partial);
	
	if (!len)
		return NULL;
		
	// check exact match
	for (cvar=cvar_vars ; cvar ; cvar=cvar->next)
		if (!strcmp (partial,cvar->name))
			return cvar->name;

	// check partial match
	for (cvar=cvar_vars ; cvar ; cvar=cvar->next)
		if (!Q_strncmp (partial,cvar->name, len))
			return cvar->name;

	return NULL;
}


/*
============
Cvar_ListMatches
============
*/
void Cvar_ListMatches (const char *partial, void (*match) (void *ctx, const char *name), void *ctx)
{
	cvar_t	*cvar;
	size_t	len = strlen (partial);

	for (cvar=cvar_vars ; cvar ; cvar=cvar->next)
		if (!Q_strncasecmp (partial, cvar->name, len))
			match (ctx, cvar->name);
}


static cvar_info_hook_t	cvar_userinfo_hook;		// the client's
static cvar_info_hook_t	cvar_serverinfo_hook;	// the server's

#define MAX_CHANGE_HOOKS	4
static cvar_change_hook_t	cvar_change_hooks[MAX_CHANGE_HOOKS];

/*
============
Cvar_SetUserinfoHook / Cvar_SetServerinfoHook

Called whenever a cvar flagged as userinfo or serverinfo changes
============
*/
void Cvar_SetUserinfoHook (cvar_info_hook_t hook)
{
	cvar_userinfo_hook = hook;
}

void Cvar_SetServerinfoHook (cvar_info_hook_t hook)
{
	cvar_serverinfo_hook = hook;
}

/*
============
Cvar_AddChangeHook

Called after any cvar changes; adding one twice adds it once
============
*/
void Cvar_AddChangeHook (cvar_change_hook_t hook)
{
	int		i;

	for (i = 0 ; i < MAX_CHANGE_HOOKS ; i++)
		if (cvar_change_hooks[i] == hook)
			return;
	for (i = 0 ; i < MAX_CHANGE_HOOKS ; i++)
		if (!cvar_change_hooks[i])
		{
			cvar_change_hooks[i] = hook;
			return;
		}
	Sys_Error ("Cvar_AddChangeHook: too many hooks");
}

/*
============
Cvar_Set
============
*/
void Cvar_Set (char *var_name, char *value)
{
	cvar_t	*var;
	int		i;
	
	var = Cvar_FindVar (var_name);
	if (!var)
	{	// there is an error in C code if this happens
		Con_Printf ("Cvar_Set: variable %s not found\n", var_name);
		return;
	}

	if (var->userinfo && cvar_userinfo_hook)
		cvar_userinfo_hook (var_name, value);
	if (var->serverinfo && cvar_serverinfo_hook)
		cvar_serverinfo_hook (var_name, value);
	
	Mem_Free (var->string);	// free the old value string
	
	var->string = Mem_Alloc (strlen(value)+1);
	Q_strcpy (var->string, value);
	var->value = Q_atof (var->string);

	for (i = 0 ; i < MAX_CHANGE_HOOKS && cvar_change_hooks[i] ; i++)
		cvar_change_hooks[i] (var);
}

/*
============
Cvar_SetValue
============
*/
void Cvar_SetValue (char *var_name, float value)
{
	char	val[32];
	
	snprintf (val, sizeof(val), "%f",value);
	Cvar_Set (var_name, val);
}


/*
============
Cvar_RegisterVariable

Adds a freestanding variable to the variable list.
============
*/
void Cvar_RegisterVariable (cvar_t *variable)
{
	char	value[512];

// a variable both ends of a listen server use is registered by both;
// two variables with one name are a bug
	if (Cvar_FindVar (variable->name) == variable)
		return;
	if (Cvar_FindVar (variable->name))
		Sys_Error ("Cvar_RegisterVariable: %s is defined twice", variable->name);
	if (Cmd_Exists (variable->name))
		Sys_Error ("Cvar_RegisterVariable: %s is a command", variable->name);
		
// link the variable in
	variable->next = cvar_vars;
	cvar_vars = variable;

// copy the value off, because future sets will Mem_Free it
	Q_strncpyz (value, variable->string, sizeof(value));
	variable->defaultstring = Mem_Alloc (strlen (value) + 1);
	strcpy (variable->defaultstring, value);
	variable->string = Mem_Calloc (1, 1);
	
// set it through the function to be consistant
	Cvar_Set (variable->name, value);
}

cvar_t *Cvar_List (void)
{
	return cvar_vars;
}

/*
============
Cvar_Describe

What a variable does, what its values do, and its default and current values,
as ezQuake shows them: the values and their heading in the other charset
============
*/
static void Cvar_Describe (cvar_t *v)
{
	const cvar_value_t	*val;

	if (v->description)
		Con_Printf ("  %s\n", v->description);
	if (v->values)
	{
		Con_Printf ("\n^avalues^a\n");
		for (val = v->values ; val->value ; val++)
			Con_Printf ("  ^a%s^a - %s\n", val->value, val->meaning);
	}
	if (v->description || v->values)
		Con_Printf ("\n");
	Con_Printf ("%s : default value is \"%s\"\n", v->name, v->defaultstring);
	Con_Printf ("%*s current value is \"%s\"\n", (int)strlen (v->name) + 2, "", v->string);
}

/*
============
Cvar_Command

Handles variable inspection and changing from the console
============
*/
bool	Cvar_Command (void)
{
	cvar_t			*v;

// check variables
	v = Cvar_FindVar (Cmd_Argv(0));
	if (!v)
		return false;
		
// perform a variable print or set
	if (Cmd_Argc() == 1)
	{
		Cvar_Describe (v);
		return true;
	}

	Cvar_Set (v->name, Cmd_Argv(1));
	return true;
}


/*
============
Cvar_WriteVariables

Writes lines containing "set variable value" for all variables
with the archive flag set to true.
============
*/
void Cvar_WriteVariables (FILE *f)
{
	cvar_t	*var;
	
	for (var = cvar_vars ; var ; var = var->next)
		if (var->archive)
			fprintf (f, "%s \"%s\"\n", var->name, var->string);
}

