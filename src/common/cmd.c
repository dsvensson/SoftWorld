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
// cmd.c -- Quake script command processing module

#include "args.h"
#include "cmd.h"
#include "msg.h"
#include "cvar.h"
#include "fs.h"
#include "mem.h"
#include "print.h"
#include "q_string.h"
#include "sys.h"

#include <ctype.h>
#include <stdlib.h>

static void (*cmd_forward)(void);	// sends forwarded commands to the server

#define	MAX_ALIAS_NAME	32

typedef struct cmdalias_s
{
	struct cmdalias_s	*next;
	char	name[MAX_ALIAS_NAME];
	char	*value;
} cmdalias_t;

static cmdalias_t	*cmd_alias;

static bool	cmd_wait;

static const char	*cmd_suffixfile, *cmd_suffixtext;	// Cmd_SetExecSuffix

cvar_t cl_warncmd = {.name = "cl_warncmd", .string = "0", .noreset = true,	// 1 once the configs ran
	.description = "Warns of unknown commands, and names each config file exec runs.",
	.values = (const cvar_value_t[]){{"0", "Quiet"}, {"1", "Warnings and exec's files printed"}, {0}}};

//=============================================================================

/*
============
Cmd_Wait_f

Causes execution of the remainder of the command buffer to be delayed until
next frame.  This allows commands like:
bind g "impulse 5 ; +attack ; wait ; -attack ; impulse 2"
============
*/
static void Cmd_Wait_f (void)
{
	cmd_wait = true;
}

/*
=============================================================================

						COMMAND BUFFER

=============================================================================
*/

static sizebuf_t	cmd_text;		// grows as needed

/*
============
Cbuf_Init
============
*/
void Cbuf_Init (void)
{
	cmd_text.maxsize = 8192;
	cmd_text.data = Mem_Alloc ((size_t)cmd_text.maxsize);
}

/*
============
Cbuf_AddText

Adds command text at the end of the buffer
============
*/
void Cbuf_AddText (const char *text)
{
	int		l;
	
	l = Q_strlen (text);

	if (cmd_text.cursize + l + 1 >= cmd_text.maxsize)
	{
		while (cmd_text.cursize + l + 1 >= cmd_text.maxsize)
			cmd_text.maxsize *= 2;
		cmd_text.data = Mem_Realloc (cmd_text.data, (size_t)cmd_text.maxsize);
	}
	SZ_Write (&cmd_text, text, l);
}


/*
============
Cbuf_InsertText

Adds command text immediately after the current command
Adds a \n to the text
FIXME: actually change the command buffer to do less copying
============
*/
void Cbuf_InsertText (const char *text)
{
	char	*temp;
	int		templen;

// copy off any commands still remaining in the exec buffer
	templen = cmd_text.cursize;
	if (templen)
	{
		temp = Mem_Alloc ((size_t)templen);
		Q_memcpy (temp, cmd_text.data, templen);
		SZ_Clear (&cmd_text);
	}
	else
		temp = NULL;	// shut up compiler
		
// add the entire text of the file
	Cbuf_AddText (text);
	Cbuf_AddText ("\n");
// add the copied off data
	if (templen)
	{
		if (cmd_text.cursize + templen >= cmd_text.maxsize)
		{
			while (cmd_text.cursize + templen >= cmd_text.maxsize)
				cmd_text.maxsize *= 2;
			cmd_text.data = Mem_Realloc (cmd_text.data, (size_t)cmd_text.maxsize);
		}
		SZ_Write (&cmd_text, temp, templen);
		Mem_Free (temp);
	}
}

/*
============
Cbuf_Execute
============
*/
void Cbuf_Execute (void)
{
	int		i;
	char	*text;
	char	line[1024];
	int		quotes;
	
	while (cmd_text.cursize)
	{
// find a \n or ; line break
		text = (char *)cmd_text.data;

		quotes = 0;
		for (i=0 ; i< cmd_text.cursize ; i++)
		{
			if (text[i] == '"')
				quotes++;
			if ( !(quotes&1) &&  text[i] == ';')
				break;	// don't break if inside a quoted string
			if (text[i] == '\n')
				break;
		}
			

		// a longer line is cut short, not copied over the stack
		if (i < (int)sizeof(line))
		{
			memcpy (line, text, (size_t)i);
			line[i] = 0;
		}
		else
		{
			Con_Printf ("Command line over %d characters, cut short\n", (int)sizeof(line) - 1);
			memcpy (line, text, sizeof(line) - 1);
			line[sizeof(line) - 1] = 0;
		}
		
// delete the text from the command buffer and move remaining commands down
// this is necessary because commands (exec, alias) can insert data at the
// beginning of the text buffer

		if (i == cmd_text.cursize)
			cmd_text.cursize = 0;
		else
		{
			i++;
			cmd_text.cursize -= i;
			memmove (text, text+i, (size_t)cmd_text.cursize);
		}

// execute the command line
		Cmd_ExecuteString (line);
		
		if (cmd_wait)
		{	// skip out while text still remains in buffer, leaving it
			// for next frame
			cmd_wait = false;
			break;
		}
	}
}

/*
==============================================================================

						SCRIPT COMMANDS

==============================================================================
*/

/*
===============
Cmd_StuffCmds_f

Adds command line parameters as script statements
Commands lead with a +, and continue until an argument that starts with a -
or another +; a - inside a word ("+playdemo dm3-final") or before a number
("+cl_rollangle -2") doesn't end one
quake +prog jctest.qp +cmd amlev1
quake -nosound +cmd amlev1
===============
*/
static bool Cmd_StartsOption (const char *text, int i)
{
	if (i > 0 && text[i-1] != ' ')
		return false;
	if (text[i] == '+')
		return true;
	return text[i] == '-' && !isdigit ((unsigned char)text[i+1]) && text[i+1] != '.';
}

void Cmd_StuffCmds_f (void)
{
	int		i, j;
	int		s;
	char	*text, *build, c;
		
// build the combined string to parse from
	s = 0;
	for (i=1 ; i<com_argc ; i++)
	{
		if (!com_argv[i])
			continue;		// NEXTSTEP nulls out -NXHost
		s += Q_strlen (com_argv[i]) + 1;
	}
	if (!s)
		return;
		
	text = Mem_Calloc (1, (size_t)s+1);
	text[0] = 0;
	for (i=1 ; i<com_argc ; i++)
	{
		if (!com_argv[i])
			continue;		// NEXTSTEP nulls out -NXHost
		Q_strcat (text,com_argv[i]);
		if (i != com_argc-1)
			Q_strcat (text, " ");
	}
	
// pull out the commands
	build = Mem_Calloc (1, (size_t)s+1);
	build[0] = 0;
	
	for (i=0 ; i<s-1 ; i++)
	{
		if (text[i] == '+' && Cmd_StartsOption (text, i))
		{
			i++;

			for (j=i ; text[j] && !Cmd_StartsOption (text, j) ; j++)
				;

			c = text[j];
			text[j] = 0;
			
			Q_strcat (build, text+i);
			Q_strcat (build, "\n");
			text[j] = c;
			i = j-1;
		}
	}
	
	if (build[0])
		Cbuf_InsertText (build);
	
	Mem_Free (text);
	Mem_Free (build);
}


/*
===============
Cmd_Exec_f
===============
*/
static void Cmd_Exec_f (void)
{
	char	*f;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("exec <filename> : execute a script file\n");
		return;
	}

	f = (char *)FS_LoadFile (Cmd_Argv(1), NULL);
	if (!f)
	{
		Con_Printf ("couldn't exec %s\n",Cmd_Argv(1));
		return;
	}
	if (!Cvar_Command () && (cl_warncmd.value || developer.value))
		Con_Printf ("execing %s\n",Cmd_Argv(1));

	// inserted first, so it runs after the file
	if (cmd_suffixfile && !Q_strcasecmp (Cmd_Argv(1), cmd_suffixfile))
		Cbuf_InsertText (cmd_suffixtext);
	Cbuf_InsertText (f);
	Mem_Free (f);
}

/*
===============
Cmd_SetExecSuffix
===============
*/
void Cmd_SetExecSuffix (const char *file, const char *text)
{
	cmd_suffixfile = file;
	cmd_suffixtext = text;
}


/*
===============
Cmd_Echo_f

Just prints the rest of the line to the console
===============
*/
static void Cmd_Echo_f (void)
{
	int		i;
	
	for (i=1 ; i<Cmd_Argc() ; i++)
		Con_Printf ("%s ",Cmd_Argv(i));
	Con_Printf ("\n");
}

/*
===============
Cmd_Alias_f

Creates a new command that executes a command string (possibly ; seperated)
===============
*/

char *CopyString (char *in)
{
	char	*out;
	size_t	size;

	size = strlen(in)+1;
	out = Mem_Alloc (size);
	Q_strncpyz (out, in, size);
	return out;
}

static void Cmd_Alias_f (void)
{
	cmdalias_t	*a;
	char		cmd[1024];
	int			i, c;
	char		*s;

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("Current alias commands:\n");
		for (a = cmd_alias ; a ; a=a->next)
			Con_Printf ("%s : %s\n", a->name, a->value);
		return;
	}

	s = Cmd_Argv(1);
	if (strlen(s) >= MAX_ALIAS_NAME)
	{
		Con_Printf ("Alias name is too long\n");
		return;
	}

	// if the alias allready exists, reuse it
	for (a = cmd_alias ; a ; a=a->next)
	{
		if (!strcmp(s, a->name))
		{
			Mem_Free (a->value);
			break;
		}
	}

	if (!a)
	{
		a = Mem_Calloc (1, sizeof(cmdalias_t));
		a->next = cmd_alias;
		cmd_alias = a;
	}
	Q_strncpyz (a->name, s, sizeof(a->name));

// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	c = Cmd_Argc();
	for (i=2 ; i< c ; i++)
	{
		Q_strncatz (cmd, Cmd_Argv(i), sizeof(cmd));
		if (i != c)
			Q_strncatz (cmd, " ", sizeof(cmd));
	}
	Q_strncatz (cmd, "\n", sizeof(cmd));
	
	a->value = CopyString (cmd);
}

/*
=============================================================================

					COMMAND EXECUTION

=============================================================================
*/

typedef struct cmd_function_s
{
	struct cmd_function_s	*next;
	char					*name;
	xcommand_t				function;
	xcompletion_t			completion;		// of its argument, NULL for none
	const char				*description;	// what it does, and its arguments
} cmd_function_t;


#define	MAX_ARGS		80

static	int			cmd_argc;
static	char		*cmd_argv[MAX_ARGS];
static	char		*cmd_null_string = "";
static	char		*cmd_args = NULL;



static	cmd_function_t	*cmd_functions;		// possible commands to execute

/*
============
Cmd_Argc
============
*/
int		Cmd_Argc (void)
{
	return cmd_argc;
}

/*
============
Cmd_Argv
============
*/
char	*Cmd_Argv (int arg)
{
	if ( arg >= cmd_argc )
		return cmd_null_string;
	return cmd_argv[arg];	
}

/*
============
Cmd_Args

Returns a single string containing argv(1) to argv(argc()-1)
============
*/
char		*Cmd_Args (void)
{
	if (!cmd_args)
		return "";
	return cmd_args;
}


/*
============
Cmd_TokenizeString

Parses the given string into command line tokens.
============
*/
void Cmd_TokenizeString (char *text)
{
	int		i;
	
// clear the args from the last string
	for (i=0 ; i<cmd_argc ; i++)
		Mem_Free (cmd_argv[i]);
		
	cmd_argc = 0;
	cmd_args = NULL;
	
	while (1)
	{
// skip whitespace up to a /n
		while (*text && *text <= ' ' && *text != '\n')
		{
			text++;
		}
		
		if (*text == '\n')
		{	// a newline seperates commands in the buffer
			text++;
			break;
		}

		if (!*text)
			return;
	
		if (cmd_argc == 1)
			 cmd_args = text;
			
		text = COM_Parse (text);
		if (!text)
			return;

		if (cmd_argc < MAX_ARGS)
		{
			cmd_argv[cmd_argc] = Mem_Alloc (strlen(com_token)+1);
			Q_strcpy (cmd_argv[cmd_argc], com_token);
			cmd_argc++;
		}
	}
	
}


/*
============
Cmd_AddCommand
============
*/
void	Cmd_AddCommand (char *cmd_name, xcommand_t function, const char *description)
{
	cmd_function_t	*cmd;
	
		
// a name has one meaning; the same command from both ends is registered once
	if (Cvar_FindVar (cmd_name))
		Sys_Error ("Cmd_AddCommand: %s is a variable", cmd_name);
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcmp (cmd_name, cmd->name))
		{
			if (cmd->function == function)
				return;
			Sys_Error ("Cmd_AddCommand: %s is defined twice", cmd_name);
		}
	}

	cmd = Mem_Calloc (1, sizeof(cmd_function_t));
	cmd->name = cmd_name;
	cmd->function = function;
	cmd->description = description;
	cmd->next = cmd_functions;
	cmd_functions = cmd;
}

/*
============
Cmd_RemoveCommand

For commands that come and go (client-side QuakeC's); the name stays the
caller's
============
*/
void	Cmd_RemoveCommand (const char *cmd_name)
{
	cmd_function_t	**link, *cmd;

	for (link = &cmd_functions ; (cmd = *link) ; link = &cmd->next)
		if (!Q_strcmp (cmd_name, cmd->name))
		{
			*link = cmd->next;
			Mem_Free (cmd);
			return;
		}
}

/*
============
Cmd_Exists
============
*/
bool	Cmd_Exists (char *cmd_name)
{
	cmd_function_t	*cmd;

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcmp (cmd_name,cmd->name))
			return true;
	}

	return false;
}

/*
============
Cmd_AliasExists
============
*/
bool Cmd_AliasExists (const char *name)
{
	cmdalias_t	*a;

	for (a = cmd_alias ; a ; a = a->next)
		if (!strcmp (name, a->name))
			return true;
	return false;
}



/*
============
Cmd_SetCompletion
============
*/
void Cmd_SetCompletion (char *cmd_name, xcompletion_t completion)
{
	cmd_function_t	*cmd;

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!Q_strcmp (cmd_name, cmd->name))
		{
			cmd->completion = completion;
			return;
		}
	Sys_Error ("Cmd_SetCompletion: no command %s", cmd_name);
}

/*
============
Cmd_CompleteArgument
============
*/
bool Cmd_CompleteArgument (const char *cmd_name, const char *partial, void (*add) (void *ctx, const char *candidate),
	void *ctx)
{
	cmd_function_t	*cmd;

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!Q_strcasecmp (cmd_name, cmd->name))
		{
			if (!cmd->completion)
				return false;
			cmd->completion (partial, add, ctx);
			return true;
		}
	return false;
}

/*
============
Cmd_ListMatches
============
*/
void Cmd_ListMatches (const char *partial, void (*match) (void *ctx, const char *name), void *ctx)
{
	cmd_function_t	*cmd;
	cmdalias_t		*a;
	size_t			len = strlen (partial);

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!Q_strncasecmp (partial, cmd->name, len))
			match (ctx, cmd->name);
	for (a=cmd_alias ; a ; a=a->next)
		if (!Q_strncasecmp (partial, a->name, len))
			match (ctx, a->name);
}

/*
============
Cmd_CompleteCommand
============
*/
char *Cmd_CompleteCommand (char *partial)
{
	cmd_function_t	*cmd;
	int				len;
	cmdalias_t		*a;
	
	len = Q_strlen(partial);
	
	if (!len)
		return NULL;
		
// check for exact match
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!strcmp (partial,cmd->name))
			return cmd->name;
	for (a=cmd_alias ; a ; a=a->next)
		if (!strcmp (partial, a->name))
			return a->name;

// check for partial match
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!strncmp (partial,cmd->name, len))
			return cmd->name;
	for (a=cmd_alias ; a ; a=a->next)
		if (!strncmp (partial, a->name, len))
			return a->name;

	return NULL;
}


/*
===============================================================================

APROPOS

===============================================================================
*/

// a name apropos found: a variable, a command or an alias
typedef struct
{
	const char		*name;
	cvar_t			*var;
	cmd_function_t	*cmd;
	cmdalias_t		*alias;
} apropos_t;

// text has sub in it, case aside
static bool Cmd_HasText (const char *text, const char *sub)
{
	size_t	len = strlen (sub);

	for ( ; text && *text ; text++)
		if (!Q_strncasecmp (text, sub, len))
			return true;
	return false;
}

static int Cmd_CompareApropos (const void *a, const void *b)
{
	return Q_strcasecmp (((const apropos_t *)a)->name, ((const apropos_t *)b)->name);
}

/*
============
Cmd_Apropos_f

The variables, commands and aliases with the text in their name or in what
they do, by name, as FTE lists them
============
*/
static void Cmd_Apropos_f (void)
{
	const char		*query = Cmd_Argv (1);
	apropos_t		*found = NULL;
	cmd_function_t	*cmd;
	cmdalias_t		*a;
	cvar_t			*var;
	int				count = 0, size = 0, i;

	if (Cmd_Argc () != 2 || !query[0])
	{
		Con_Printf ("apropos <text> : the variables and commands with the text in their name or description\n");
		return;
	}

#define APROPOS_ADD(field, item, itemname) \
	do { \
		if (count == size) \
			found = Mem_Realloc (found, (size_t)(size = size ? size * 2 : 64) * sizeof(*found)); \
		found[count] = (apropos_t){.name = (itemname), .field = (item)}; \
		count++; \
	} while (0)

	for (var = Cvar_List () ; var ; var = var->next)
		if (Cmd_HasText (var->name, query) || Cmd_HasText (var->description, query))
			APROPOS_ADD (var, var, var->name);
	for (cmd = cmd_functions ; cmd ; cmd = cmd->next)
		if (Cmd_HasText (cmd->name, query) || Cmd_HasText (cmd->description, query))
			APROPOS_ADD (cmd, cmd, cmd->name);
	for (a = cmd_alias ; a ; a = a->next)
		if (Cmd_HasText (a->name, query) || Cmd_HasText (a->value, query))
			APROPOS_ADD (alias, a, a->name);
#undef APROPOS_ADD

	if (!count)
	{
		Con_Printf ("Nothing has \"%s\" in its name or description.\n", query);
		return;
	}
	qsort (found, (size_t)count, sizeof(*found), Cmd_CompareApropos);
	for (i = 0 ; i < count ; i++)
	{
		if (found[i].var)
			Con_Printf ("cvar ^2%s^7: \"%s\" : ^3%s\n", found[i].name, found[i].var->string,
				found[i].var->description ? found[i].var->description : "no description");
		else if (found[i].cmd)
			Con_Printf ("command ^2%s^7: ^3%s\n", found[i].name,
				found[i].cmd->description ? found[i].cmd->description : "no description");
		else
			Con_Printf ("alias ^2%s^7: ^3%s", found[i].name, found[i].alias->value);
	}
	Mem_Free (found);
}

/*
============
Cmd_SetForwardHandler

Commands registered without a function are passed to this handler.
============
*/
void Cmd_SetForwardHandler (void (*forward)(void))
{
	cmd_forward = forward;
}

/*
============
Cmd_ExecuteString

A complete command line has been parsed, so try to execute it
FIXME: lookupnoadd the token to speed search?
============
*/
void	Cmd_ExecuteString (char *text)
{	
	cmd_function_t	*cmd;
	cmdalias_t		*a;

	Cmd_TokenizeString (text);
			
// execute the command line
	if (!Cmd_Argc())
		return;		// no tokens

// check functions
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcasecmp (cmd_argv[0],cmd->name))
		{
			if (!cmd->function)
			{
				if (cmd_forward)
					cmd_forward ();
			}
			else
				cmd->function ();
			return;
		}
	}

// check alias
	for (a=cmd_alias ; a ; a=a->next)
	{
		if (!Q_strcasecmp (cmd_argv[0], a->name))
		{
			Cbuf_InsertText (a->value);
			return;
		}
	}
	
// check cvars
	if (!Cvar_Command () && (cl_warncmd.value || developer.value))
		Con_Printf ("Unknown command \"%s\"\n", Cmd_Argv(0));
	
}

/*
============
Cmd_Init
============
*/
void Cmd_Init (void)
{
//
// register our commands
//
	Cmd_AddCommand ("stuffcmds",Cmd_StuffCmds_f, "Runs the +commands given on the program's command line.");
	Cmd_AddCommand ("exec",Cmd_Exec_f, "Runs the commands in a config file. Usage: exec <file>");
	Cmd_AddCommand ("echo",Cmd_Echo_f, "Prints its arguments to the console. Usage: echo [text]");
	Cmd_AddCommand ("alias",Cmd_Alias_f, "Makes a command that runs the given commands; "
		"without arguments, lists the aliases. Usage: alias [<name> <commands>]");
	Cmd_AddCommand ("wait", Cmd_Wait_f, "Holds the rest of the command buffer until the next frame.");
	Cmd_AddCommand ("apropos", Cmd_Apropos_f,
		"Lists the variables, commands and aliases with the text in their name or description. Usage: apropos <text>");
}

