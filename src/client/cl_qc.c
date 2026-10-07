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
// cl_qc.c -- what the client's QuakeC hosts share (CSQC, the menu)

#include "cl_local.h"
#include "cl_qc.h"

static clqc_t	*clqc_hosts[4];		// those whose QuakeC registered commands
static int		clqc_numhosts;

static void CLQC_Warning (void *ctx, const qc_warning_t *w)
{
	const clqc_t	*qc = ctx;
	char			text[1024];

	Con_DPrintf ("%s: %s\n", qc->name, QC_WarningText (w, text, sizeof(text)));
}

static void CLQC_Print (void *ctx, const char *text)
{
	(void)ctx;
	Con_Printf ("%s", text);
}

static void CLQC_CenterPrint (void *ctx, const char *text)
{
	(void)ctx;
	SCR_CenterPrint ((char *)text);
}

static void CLQC_Dump (void *ctx, qc_dumpkind_t kind, const char *text)
{
	(void)kind;
	CLQC_Print (ctx, text);
}

static void CLQC_Localcmd (void *ctx, const char *text)
{
	(void)ctx;
	Cbuf_AddText ((char *)text);
}

static float CLQC_CvarFloat (void *ctx, const char *varname)
{
	(void)ctx;
	return Cvar_VariableValue ((char *)varname);
}

static const char *CLQC_CvarString (void *ctx, const char *varname)
{
	cvar_t	*var = Cvar_FindVar ((char *)varname);

	(void)ctx;
	return var ? var->string : NULL;
}

static void CLQC_CvarSet (void *ctx, const char *varname, const char *value)
{
	(void)ctx;
	if (Cvar_FindVar ((char *)varname))
		Cvar_Set ((char *)varname, (char *)value);
}

// cvar_type, cvar_defstring and cvar_description: the engine's cvars all
static bool CLQC_CvarInfo (void *ctx, const char *varname, qc_cvarinfo_t *info)
{
	cvar_t	*var = Cvar_FindVar ((char *)varname);

	(void)ctx;
	if (!var)
		return false;
	info->flags = 1 | (var->archive ? 2 : 0) | 8 | (var->description ? 16 : 0);
	info->defaultvalue = var->defaultstring;
	info->description = var->description;
	return true;
}

// checkcommand: 1 a command, 2 an alias, 3 a cvar
static uint32_t CLQC_CheckCommand (void *ctx, const char *cmd)
{
	(void)ctx;
	if (Cmd_Exists ((char *)cmd))
		return 1;
	if (Cmd_AliasExists (cmd))
		return 2;
	return Cvar_FindVar ((char *)cmd) ? 3 : 0;
}

// a command QuakeC registered: its whole line to the host whose QuakeC it is
static void CLQC_Command_f (void)
{
	clqc_t	*qc;
	int		h, i;

	for (h = 0 ; h < clqc_numhosts ; h++)
	{
		qc = clqc_hosts[h];
		for (i = 0 ; i < qc->numcommands ; i++)
			if (!strcmp (qc->commands[i], Cmd_Argv (0)))
			{
				if (qc->vm)
					qc->command (qc, va ("%s %s", Cmd_Argv (0), Cmd_Args ()));
				return;
			}
	}
}

// registercommand: a console command for QuakeC, unless the name is taken
static void CLQC_RegisterCommand (void *ctx, const char *cmd)
{
	clqc_t	*qc = ctx;
	char	*copy;
	int		h, i;

	if (!*cmd || Cmd_Exists ((char *)cmd) || Cvar_FindVar ((char *)cmd) || Cmd_AliasExists (cmd))
		return;
	for (i = 0 ; i < qc->numcommands ; i++)
		if (!strcmp (qc->commands[i], cmd))
			return;
	if (qc->numcommands == (int)(sizeof(qc->commands) / sizeof(qc->commands[0])))
	{
		Con_Printf ("%s: too many commands, %s left out\n", qc->name, cmd);
		return;
	}
	for (h = 0 ; h < clqc_numhosts && clqc_hosts[h] != qc ; h++)
		;
	if (h == clqc_numhosts)
	{
		if (clqc_numhosts == (int)(sizeof(clqc_hosts) / sizeof(clqc_hosts[0])))
			return;
		clqc_hosts[clqc_numhosts++] = qc;
	}
	copy = Mem_Alloc (strlen (cmd) + 1);
	strcpy (copy, cmd);
	qc->commands[qc->numcommands++] = copy;
	Cmd_AddCommand (copy, CLQC_Command_f, qc->description);
}

static float CLQC_IsDemo (void *ctx)
{
	(void)ctx;
	return cls.demoplayback ? cls.mvdplayback ? 2.0f : 1.0f : 0.0f;
}

static bool CLQC_IsServer (void *ctx)
{
	(void)ctx;
	return SV_Active ();
}

static void CLQC_Trace (void *ctx, const char *line)
{
	(void)ctx;
	Con_Printf ("%s\n", line);
}

void CLQC_InitHost (qc_host_t *h)
{
	h->warning = CLQC_Warning;
	h->print = CLQC_Print;
	h->dprint = CLQC_Print;
	h->centerprint = CLQC_CenterPrint;
	h->localcmd = CLQC_Localcmd;
	h->dump = CLQC_Dump;
	h->cvar_float = CLQC_CvarFloat;
	h->cvar_string = CLQC_CvarString;
	h->cvar_set = CLQC_CvarSet;
	h->cvar_info = CLQC_CvarInfo;
	h->check_command = CLQC_CheckCommand;
	h->register_command = CLQC_RegisterCommand;
	h->is_demo = CLQC_IsDemo;
	h->is_server = CLQC_IsServer;
	h->trace = CLQC_Trace;
}

void CLQC_Failed (const clqc_t *qc)
{
	const qc_error_t	*e = QC_LastError (qc->vm);
	char				text[1024];
	char				*trace = Mem_Alloc (16384);

	Con_Printf ("%s", QC_BacktraceText (&e->backtrace, trace, 16384));
	Mem_Free (trace);
	Con_Printf ("%s: %s\n", qc->name, QC_ErrorText (e, text, sizeof(text)));
}

void CLQC_RemoveCommands (clqc_t *qc)
{
	int		i;

	for (i = 0 ; i < qc->numcommands ; i++)
	{
		Cmd_RemoveCommand (qc->commands[i]);
		Mem_Free (qc->commands[i]);
	}
	qc->numcommands = 0;
}

bool CLQC_ReturnText (qcvm_t *vm, const char *text)
{
	if (!*text)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	return QC_ReturnString (vm, text, strlen (text));
}
