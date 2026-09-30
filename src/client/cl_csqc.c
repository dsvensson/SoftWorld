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
// cl_csqc.c -- client-side QuakeC (CSQC), as FTE has it: csprogs.dat and
// csaddon.dat found, downloaded, checked and loaded, and their lifecycle. The
// CSQC networking (entities, the parse hooks, input) and the drawing come
// later; until they do, cl_nocsqc keeps CSQC off.

#include "cl_local.h"
#include "qcvm.h"

cvar_t	cl_nocsqc = {.name = "cl_nocsqc", .string = "1",
	.description = "Keeps client-side QuakeC (CSQC) from loading. It stays on until the client handles CSQC's "
		"networking and drawing; 0 loads a server's csprogs.dat as far as that goes."};
cvar_t	cl_download_csprogs = {.name = "cl_download_csprogs", .string = "1", .archive = true,
	.description = "Downloads the client-side QuakeC (csprogs.dat) a server offers, into csprogsvers in the game "
		"directory, named by its checksum."};

extern int	file_from_pak;

#define	CSQC_API_VERSION	1.0f
#define	SOLID_BSP			4

static struct
{
	qcvm_t			*vm;
	qc_builtins_t	*builtins;

	// what the server offers, as CSQC_Init was last given it
	bool			promiscuous;		// any csprogs will do (a demo, the server's anycsqc)
	unsigned		checksum;
	size_t			size;
	char			checkname[MAX_QPATH];

	bool			worldloaded;
	char			*entitydata;		// what getentitytoken parses next, or NULL
	char			*entitycopy;		// QuakeC's own text for it

	char			*commands[256];		// QuakeC registered them
	int				numcommands;

	int				calls;				// the client's calls into QuakeC in progress
} csqc;

static void CSQC_Destroy (void);
static void CSQC_Failed (void);
static qc_func_t CSQC_Entry (const char *name);

/*
==============================================================================

THE HOST

==============================================================================
*/

static void CSQC_Warning (void *ctx, const qc_warning_t *w)
{
	char	text[1024];

	(void)ctx;
	Con_DPrintf ("CSQC: %s\n", QC_WarningText (w, text, sizeof(text)));
}

static void CSQC_Print (void *ctx, const char *text)
{
	(void)ctx;
	Con_Printf ("%s", text);
}

static void CSQC_CenterPrint (void *ctx, const char *text)
{
	(void)ctx;
	SCR_CenterPrint ((char *)text);
}

static void CSQC_Dump (void *ctx, qc_dumpkind_t kind, const char *text)
{
	(void)kind;
	CSQC_Print (ctx, text);
}

static void CSQC_Localcmd (void *ctx, const char *text)
{
	(void)ctx;
	Cbuf_AddText ((char *)text);
}

static float CSQC_CvarFloat (void *ctx, const char *varname)
{
	(void)ctx;
	return Cvar_VariableValue ((char *)varname);
}

static const char *CSQC_CvarString (void *ctx, const char *varname)
{
	cvar_t	*var = Cvar_FindVar ((char *)varname);

	(void)ctx;
	return var ? var->string : NULL;
}

static void CSQC_CvarSet (void *ctx, const char *varname, const char *value)
{
	(void)ctx;
	if (Cvar_FindVar ((char *)varname))
		Cvar_Set ((char *)varname, (char *)value);
}

// checkcommand: 1 a command, 2 an alias, 3 a cvar
static uint32_t CSQC_CheckCommand (void *ctx, const char *cmd)
{
	(void)ctx;
	if (Cmd_Exists ((char *)cmd))
		return 1;
	if (Cmd_AliasExists (cmd))
		return 2;
	return Cvar_FindVar ((char *)cmd) ? 3 : 0;
}

// a command QuakeC registered: its whole line goes to CSQC_ConsoleCommand
static void CSQC_ConsoleCommand_f (void)
{
	qc_value_t	arg;
	qc_func_t	f;
	char		*line;
	bool		ok;

	if (!csqc.vm || !(f = CSQC_Entry ("CSQC_ConsoleCommand")))
		return;
	line = va ("%s %s", Cmd_Argv (0), Cmd_Args ());
	arg = QC_ValWord (QC_TempString (csqc.vm, line, strlen (line)));
	csqc.calls++;
	ok = QC_Call (csqc.vm, f, 1, &arg, NULL);
	csqc.calls--;
	if (!ok)
		CSQC_Failed ();
}

// registercommand: a console command for CSQC_ConsoleCommand, unless the name
// is taken
static void CSQC_RegisterCommand (void *ctx, const char *cmd)
{
	char	*copy;
	int		i;

	(void)ctx;
	if (!*cmd || Cmd_Exists ((char *)cmd) || Cvar_FindVar ((char *)cmd) || Cmd_AliasExists (cmd))
		return;
	for (i = 0 ; i < csqc.numcommands ; i++)
		if (!strcmp (csqc.commands[i], cmd))
			return;
	if (csqc.numcommands == (int)(sizeof(csqc.commands) / sizeof(csqc.commands[0])))
	{
		Con_Printf ("CSQC: too many commands, %s left out\n", cmd);
		return;
	}
	copy = Mem_Alloc (strlen (cmd) + 1);
	strcpy (copy, cmd);
	csqc.commands[csqc.numcommands++] = copy;
	Cmd_AddCommand (copy, CSQC_ConsoleCommand_f, "A command of the client-side QuakeC.");
}

static float CSQC_IsDemo (void *ctx)
{
	(void)ctx;
	return cls.demoplayback ? cls.mvdplayback ? 2.0f : 1.0f : 0.0f;
}

static bool CSQC_IsServer (void *ctx)
{
	(void)ctx;
	return SV_Active ();
}

// another progs for addprogs, from the game directory
static qc_progs_t *CSQC_LoadAddon (void *ctx, const char *file)
{
	byte			*data;
	int				size;
	qc_progs_t		*p;
	qc_loaderror_t	lerr;
	char			text[1024];

	(void)ctx;
	if (!*file || strstr (file, "..") || *file == '/' || *file == '\\' || strchr (file, ':'))
	{
		Con_Printf ("addprogs: refusing %s\n", file);
		return NULL;
	}
	data = FS_LoadFile (file, &size);
	if (!data)
		return NULL;
	p = QC_LoadProgs (data, (size_t)size, &lerr);
	Mem_Free (data);
	if (!p)
		Con_Printf ("%s: %s\n", file, QC_LoadErrorText (&lerr, text, sizeof(text)));
	return p;
}

static void CSQC_Trace (void *ctx, const char *line)
{
	(void)ctx;
	Con_Printf ("%s\n", line);
}

static const qc_host_t	csqc_host = {
	.warning = CSQC_Warning,
	.print = CSQC_Print,
	.dprint = CSQC_Print,
	.centerprint = CSQC_CenterPrint,
	.localcmd = CSQC_Localcmd,
	.dump = CSQC_Dump,
	.cvar_float = CSQC_CvarFloat,
	.cvar_string = CSQC_CvarString,
	.cvar_set = CSQC_CvarSet,
	.check_command = CSQC_CheckCommand,
	.register_command = CSQC_RegisterCommand,
	.is_demo = CSQC_IsDemo,
	.is_server = CSQC_IsServer,
	.load_progs = CSQC_LoadAddon,
	.trace = CSQC_Trace,
};

/*
==============================================================================

THE ENGINE'S BUILTINS

Those that need neither the CSQC networking nor drawing.

==============================================================================
*/

// a string result: null for empty text, as FTE returns them
static bool CS_ReturnText (qcvm_t *vm, const char *text)
{
	if (!*text)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	return QC_ReturnString (vm, text, strlen (text));
}

// FTE's serverkey: a few keys about the connection, else the serverinfo's
static const char *CS_ServerKey (const char *key)
{
	static char	text[64];

	if (!strcmp (key, "ip"))
		return cls.demoplayback ? "" : NET_AdrToString (cls.netchan.remote_address);
	if (!strcmp (key, "servername"))
		return cls.servername;
	if (!strcmp (key, "constate"))
		return cls.state == ca_disconnected ? "disconnected" : cls.state == ca_active ? "active" : "connecting";
	if (!strcmp (key, "pausestate"))
		return cl.paused ? "1" : "0";
	if (!strcmp (key, "protocol"))
		return cls.fteext ? "QuakeWorld FTE" : cl.z_ext ? "QuakeWorld ZQuake" : "QuakeWorld";
	if (!strcmp (key, "maxplayers"))
	{
		snprintf (text, sizeof(text), "%i", MAX_CLIENTS);
		return text;
	}
	return Info_ValueForKey (cl.serverinfo, (char *)key);
}

// string serverkey(string key)
static bool CS_ServerKeyBuiltin (qcvm_t *vm)
{
	return CS_ReturnText (vm, CS_ServerKey (QC_ArgString (vm, 0)));
}

// float serverkeyfloat(string key, optional float default)
static bool CS_ServerKeyFloat (qcvm_t *vm)
{
	const char	*text = CS_ServerKey (QC_ArgString (vm, 0));

	QC_ReturnFloat (vm, *text ? (float)strtod (text, NULL) : QC_Argc (vm) > 1 ? QC_ArgFloat (vm, 1) : 0);
	return true;
}

// FTE's player keys: a few about the player, else their userinfo's; "" for
// an empty slot
static const char *CS_PlayerKey (int pnum, const char *key)
{
	static char		text[64];
	player_info_t	*p;

	if (pnum < 0 || pnum >= MAX_CLIENTS)
		return "";
	if (!strcmp (key, "viewentity"))
	{
		snprintf (text, sizeof(text), "%i", pnum + 1);
		return text;
	}
	p = &cl.players[pnum];
	if (!*p->name)
		return "";
	if (!strcmp (key, "ping"))
		snprintf (text, sizeof(text), "%i", p->ping);
	else if (!strcmp (key, "frags"))
		snprintf (text, sizeof(text), "%i", p->frags);
	else if (!strcmp (key, "userid"))
		snprintf (text, sizeof(text), "%i", p->userid);
	else if (!strcmp (key, "pl"))
		snprintf (text, sizeof(text), "%i", p->pl);
	else if (!strcmp (key, "activetime"))
		snprintf (text, sizeof(text), "%f", host.realtime - p->entertime);
	else
		return Info_ValueForKey (p->userinfo, (char *)key);
	return text;
}

// string getplayerkeyvalue(float playernum, string key)
static bool CS_GetPlayerKeyValue (qcvm_t *vm)
{
	return CS_ReturnText (vm, CS_PlayerKey (QC_DoubleToInt (QC_ArgFloat (vm, 0)), QC_ArgString (vm, 1)));
}

// float getplayerkeyfloat(float playernum, string key, optional float default)
static bool CS_GetPlayerKeyFloat (qcvm_t *vm)
{
	const char	*text = CS_PlayerKey (QC_DoubleToInt (QC_ArgFloat (vm, 0)), QC_ArgString (vm, 1));

	QC_ReturnFloat (vm, *text ? (float)strtod (text, NULL) : QC_Argc (vm) > 2 ? QC_ArgFloat (vm, 2) : 0);
	return true;
}

// string getlocaluserinfo(float seat, string key)
static bool CS_GetLocalUserinfo (qcvm_t *vm)
{
	const char	*text;

	if (QC_DoubleToInt (QC_ArgFloat (vm, 0)) != 0)
		return QC_Error (vm, "getlocaluserinfo: invalid seat");
	text = Info_ValueForKey (cls.userinfo, (char *)QC_ArgString (vm, 1));
	return QC_ReturnString (vm, text, strlen (text));
}

// a stat's number, or -1 with a warning
static int CS_StatNumber (qcvm_t *vm, const char *builtin, int last)
{
	int	n = QC_DoubleToInt (QC_ArgFloat (vm, 0));

	if (n < 0 || n > last)
	{
		QC_Warning (vm, "%s: invalid stat index (%i)", builtin, n);
		return -1;
	}
	return n;
}

// int getstati(float stnum)
static bool CS_GetStatI (qcvm_t *vm)
{
	int	n = CS_StatNumber (vm, "getstati", MAX_CL_STATS - 1);

	QC_ReturnInt (vm, n < 0 ? 0 : cl.stats[n]);
	return true;
}

// float getstatf(float stnum, optional float firstbit, optional float bitcount):
// the stat, or bitcount of its bits from firstbit
static bool CS_GetStatF (qcvm_t *vm)
{
	int			n = CS_StatNumber (vm, "getstatf", MAX_CL_STATS - 1), first, count;
	uint32_t	mask;

	if (n < 0)
		QC_ReturnFloat (vm, 0);
	else if (QC_Argc (vm) > 1)
	{
		first = QC_DoubleToInt (QC_ArgFloat (vm, 1));
		count = QC_Argc (vm) > 2 ? QC_DoubleToInt (QC_ArgFloat (vm, 2)) : 1;
		if (first < 0 || first > 31 || count <= 0)
		{
			QC_ReturnFloat (vm, 0);
			return true;
		}
		mask = count >= 32 ? 0xFFFFFFFFu : (1u << count) - 1;
		QC_ReturnFloat (vm, (float)(((uint32_t)cl.stats[n] >> first) & mask));
	}
	else
		QC_ReturnFloat (vm, (float)cl.stats[n]);
	return true;
}

// string getstats(float stnum): four stats from stnum as sixteen characters
static bool CS_GetStatS (qcvm_t *vm)
{
	int		n = CS_StatNumber (vm, "getstats", MAX_CL_STATS - 4), i;
	char	text[17];

	if (n < 0)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	for (i = 0 ; i < 4 ; i++)
	{
		text[i * 4 + 0] = (char)(cl.stats[n + i] & 0xFF);
		text[i * 4 + 1] = (char)((cl.stats[n + i] >> 8) & 0xFF);
		text[i * 4 + 2] = (char)((cl.stats[n + i] >> 16) & 0xFF);
		text[i * 4 + 3] = (char)((cl.stats[n + i] >> 24) & 0xFF);
	}
	text[16] = 0;
	return QC_ReturnString (vm, text, strlen (text));
}

// string modelnameforindex(float index): the server's model names (the
// CSQC's own, below 0, come with the networking)
static bool CS_ModelNameForIndex (qcvm_t *vm)
{
	int	i = QC_DoubleToInt (QC_ArgFloat (vm, 0));

	QC_ReturnWord (vm, i > 0 && i < MAX_MODELS && cl.model_name[i][0] ? QC_HostString (vm, cl.model_name[i]) : 0);
	return true;
}

// string soundnameforindex(float index)
static bool CS_SoundNameForIndex (qcvm_t *vm)
{
	int	i = QC_DoubleToInt (QC_ArgFloat (vm, 0));

	QC_ReturnWord (vm, i > 0 && i < MAX_SOUNDS && cl.sound_name[i][0] ? QC_HostString (vm, cl.sound_name[i]) : 0);
	return true;
}

// string getentitytoken(optional string newdata): the next token of the
// world's entities, which CSQC_WorldLoaded may read; newdata starts over on
// that text ("" the world's again)
static bool CS_GetEntityToken (qcvm_t *vm)
{
	const char	*text;

	if (QC_Argc (vm) > 0)
	{
		text = QC_ArgString (vm, 0);
		if (csqc.entitycopy)
			Mem_Free (csqc.entitycopy);
		csqc.entitycopy = NULL;
		if (*text)
		{
			csqc.entitycopy = Mem_Alloc (strlen (text) + 1);
			strcpy (csqc.entitycopy, text);
			csqc.entitydata = csqc.entitycopy;
		}
		else
			csqc.entitydata = cl.map ? CM_EntityString (cl.map) : NULL;
		QC_ReturnWord (vm, 0);
		return true;
	}
	if (csqc.entitydata)
		csqc.entitydata = COM_Parse (csqc.entitydata);
	if (!csqc.entitydata)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	return QC_ReturnString (vm, com_token, strlen (com_token));
}

static const struct
{
	const char		*name;
	qc_builtin_t	func;
} csqc_builtins[] = {
	{"serverkey", CS_ServerKeyBuiltin},
	{"serverkeyfloat", CS_ServerKeyFloat},
	{"getplayerkeyvalue", CS_GetPlayerKeyValue},
	{"getplayerkeyfloat", CS_GetPlayerKeyFloat},
	{"getlocaluserinfo", CS_GetLocalUserinfo},
	{"getstati", CS_GetStatI},
	{"getstatf", CS_GetStatF},
	{"getstats", CS_GetStatS},
	{"modelnameforindex", CS_ModelNameForIndex},
	{"soundnameforindex", CS_SoundNameForIndex},
	{"getentitytoken", CS_GetEntityToken},
};

/*
==============================================================================

FINDING AND LOADING

==============================================================================
*/

// the size and folded MD4 the server published
static bool CSQC_Matches (const byte *data, int size, unsigned checksum, size_t checksize)
{
	if (checksize && (size_t)size != checksize)
		return false;
	return Com_BlockChecksum (data, size) == checksum;
}

// a copy of a matching loose csprogs, where the cached ones go
static void CSQC_BackUp (const char *cached, const byte *data, int size)
{
	char	path[MAX_OSPATH];
	FILE	*f;

	snprintf (path, sizeof(path), "%s/%s", com_gamedir, cached);
	COM_CreatePath (path);
	f = fopen (path, "wb");
	if (!f)
		return;
	if (fwrite (data, 1, (size_t)size, f) != (size_t)size)
		Con_Printf ("Couldn't write %s\n", path);
	fclose (f);
}

/*
=================
CSQC_FindMainProgs

As FTE finds the csprogs: csprogsvers/<checksum>.dat if it matches, else
the server's name for it or csprogs.dat from the game directory. A loose
file must match too, unless any will do; one that does is backed up to
csprogsvers (unless it came from a pack or the client runs the server), so
demos play with it later. The file (Mem_Free it) and where it came from.
=================
*/
static byte *CSQC_FindMainProgs (const char *file, unsigned checksum, size_t checksize, int *size, char *found,
	size_t foundsize)
{
	char		cached[MAX_QPATH];
	const char	*loose = *file ? file : "csprogs.dat";
	byte		*data;

	snprintf (cached, sizeof(cached), "csprogsvers/%x.dat", checksum);
	if (checksum && (data = FS_LoadFile (cached, size)))
	{
		if (CSQC_Matches (data, *size, checksum, checksize))
		{
			Q_strncpyz (found, cached, foundsize);
			return data;
		}
		Mem_Free (data);
	}

	data = FS_LoadFile (loose, size);
	if (!data && strcmp (loose, "csprogs.dat"))
		data = FS_LoadFile (loose = "csprogs.dat", size);
	if (!data)
		return NULL;
	if (!cls.demoplayback && checksum && !csqc.promiscuous)
	{
		if (!CSQC_Matches (data, *size, checksum, checksize))
		{
			Mem_Free (data);
			return NULL;
		}
		if (!file_from_pak && !SV_Active ())
			CSQC_BackUp (cached, data, *size);
	}
	Q_strncpyz (found, loose, foundsize);
	return data;
}

bool CSQC_CheckDownload (const char *csprogsname, unsigned checksum, size_t checksize)
{
	char	found[MAX_QPATH];
	int		size;
	byte	*data = CSQC_FindMainProgs (csprogsname, checksum, checksize, &size, found, sizeof(found));

	if (!data)
		return false;
	Mem_Free (data);
	return true;
}

// a progs from its file's bytes (freed), with the line numbers beside it
static qc_progs_t *CSQC_LoadProgs (const char *file, byte *data, int size)
{
	qc_loaderror_t		lerr;
	qc_progs_t			*p = QC_LoadProgs (data, (size_t)size, &lerr);
	const qc_loadnote_t	*notes;
	uint32_t			count, n;
	char				text[1024], lnoname[MAX_QPATH];
	byte				*lno;
	int					lnosize;

	Mem_Free (data);
	if (!p)
	{
		Con_Printf ("%s: %s\n", file, QC_LoadErrorText (&lerr, text, sizeof(text)));
		return NULL;
	}
	notes = QC_ProgsNotes (p, &count);
	for (n = 0 ; n < count ; n++)
		Con_DPrintf ("%s: %s\n", file, QC_LoadNoteText (&notes[n], text, sizeof(text)));

	COM_StripExtension ((char *)file, lnoname);
	Q_strncatz (lnoname, ".lno", sizeof(lnoname));
	if ((lno = FS_LoadFile (lnoname, &lnosize)))
	{
		if (!QC_AttachLineNumbers (p, lno, (size_t)lnosize, &lerr))
			Con_DPrintf ("%s: %s\n", lnoname, QC_LoadErrorText (&lerr, text, sizeof(text)));
		Mem_Free (lno);
	}
	return p;
}

/*
==============================================================================

RUNNING

==============================================================================
*/

// A QuakeC error: its backtrace and what went wrong, then CSQC goes; the
// client carries on without it
static void CSQC_Failed (void)
{
	const qc_error_t	*e = QC_LastError (csqc.vm);
	char				text[1024];
	char				*trace = Mem_Alloc (16384);

	Con_Printf ("%s", QC_BacktraceText (&e->backtrace, trace, 16384));
	Mem_Free (trace);
	Con_Printf ("CSQC: %s\n", QC_ErrorText (e, text, sizeof(text)));
	Con_Printf ("CSQC shut down\n");
	CSQC_Destroy ();
}

// an entry point, in the csprogs or else the add-on (FTE's PR_ANY)
static qc_func_t CSQC_Entry (const char *entry)
{
	uint32_t	pr;
	qc_func_t	f;

	for (pr = 0 ; pr < QC_NumProgs (csqc.vm) ; pr++)
		if ((f = QC_FindFunctionIn (csqc.vm, pr, entry)))
			return f;
	return 0;
}

// calls f, if not 0; false (CSQC gone) on an error
static bool CSQC_Call (qc_func_t f, int argc, const qc_value_t *args)
{
	bool	ok;

	if (!csqc.vm)
		return false;
	if (!f)
		return true;
	csqc.calls++;
	ok = QC_Call (csqc.vm, f, argc, args, NULL);
	csqc.calls--;
	if (!ok)
		CSQC_Failed ();
	return ok;
}

// a float global of the csprogs, if it has one
static void CSQC_SetFloat (const char *global, float value)
{
	uint32_t	word, type;

	if (QC_FindGlobal (csqc.vm, global, &word, &type) && type == QC_EV_FLOAT)
		QC_Globals (csqc.vm)[word].f = value;
}

static void CSQC_SetString (const char *global, const char *text)
{
	uint32_t	word, type;

	if (QC_FindGlobal (csqc.vm, global, &word, &type) && type == QC_EV_STRING)
		QC_Globals (csqc.vm)[word].u = QC_Intern (csqc.vm, text, strlen (text));
}

// a field of the world, if the progs has it
static void CSQC_SetWorldField (const char *field, uint32_t want, qc_value_t value)
{
	uint32_t	ofs, type;

	if (QC_FindField (csqc.vm, field, &ofs, &type) && type == want)
		QC_SetField (csqc.vm, 0, ofs, 1, value.w);
}

// init(float prevprogs) and initents(float prevprogs) of each progs, as FTE
// calls them
static bool CSQC_CallEach (const char *entry, uint32_t from)
{
	uint32_t	pr;
	qc_value_t	arg;

	for (pr = from ; csqc.vm && pr < QC_NumProgs (csqc.vm) ; pr++)
	{
		arg = QC_ValFloat ((float)pr - 1);
		if (!CSQC_Call (QC_FindFunctionIn (csqc.vm, pr, entry), 1, &arg))
			return false;
	}
	return csqc.vm != NULL;
}

// a cvar changed: the autocvars that follow it (QuakeC may be running: its
// cvar_set)
static void CSQC_CvarChanged (cvar_t *var)
{
	char	text[1024];

	if (csqc.vm && !QC_SyncAutocvar (csqc.vm, var->name))
		Con_Printf ("CSQC: autocvar_%s: %s\n", var->name, QC_ErrorText (QC_LastError (csqc.vm), text, sizeof(text)));
}

/*
=================
CSQC_Init

As FTE's: the csprogs the server offers (a NULL name when it offers none),
checked unless any will do, and csaddon.dat after it where cheats apply (a
demo, *cheats, or a local server for one) or any will do; csaddon.dat alone
without a csprogs. Then the autocvars, each progs' init and initents, the
globals FTE sets, and CSQC_Init. Whether CSQC runs.
=================
*/
bool CSQC_Init (bool anycsqc, const char *csprogsname, unsigned checksum, size_t size)
{
	bool			offered = csprogsname != NULL, cheats, access, ok;
	const char		*s;
	char			found[MAX_QPATH], text[1024];
	byte			*data;
	int				len;
	qc_progs_t		*main = NULL, *addon = NULL;
	qc_config_t		config;
	qc_error_t		err;
	qc_value_t		args[3];
	uint32_t		pr;

	if (!csprogsname || !*csprogsname)
		csprogsname = "csprogs.dat";
	if (csqc.promiscuous != anycsqc || csqc.checksum != checksum || csqc.size != size
		|| strcmp (csqc.checkname, csprogsname))
		CSQC_Shutdown ();
	csqc.promiscuous = anycsqc;
	csqc.checksum = checksum;
	csqc.size = size;
	Q_strncpyz (csqc.checkname, csprogsname, sizeof(csqc.checkname));

	s = Info_ValueForKey (cl.serverinfo, "*cheats");
	cheats = cls.demoplayback || !Q_strcasecmp ((char *)s, "ON") || atoi (s)
		|| (SV_Active () && atoi (Info_ValueForKey (cl.serverinfo, "maxclients")) == 1);

	if (csqc.vm)
		return true;
	if (cl_nocsqc.value)
	{
		if (checksum || size)
			Con_Printf ("The server uses CSQC, which %s keeps off\n", cl_nocsqc.name);
		return false;
	}

	// the csprogs, when the server offers one or cheats allow any
	access = offered || cheats || anycsqc;
	if (access)
	{
		if ((data = CSQC_FindMainProgs (csqc.checkname, checksum, size, &len, found, sizeof(found))))
			main = CSQC_LoadProgs (found, data, len);
		else if (checksum || size)
			Con_Printf ("Unable to load csprogsvers/%x.dat\n", checksum);
	}
	if (main && !Q_strcasecmp (csqc.checkname, "csaddon.dat"))
		;	// the add-on is the csprogs
	else if (cheats || anycsqc)
	{
		if ((data = FS_LoadFile ("csaddon.dat", &len)))
			addon = CSQC_LoadProgs ("csaddon.dat", data, len);
		Con_DPrintf (addon ? "Loaded csaddon.dat\n" : "No csaddon.dat\n");
	}
	else
		Con_DPrintf ("Skipping csaddon.dat: cheats don't apply\n");
	if (!main)
	{
		main = addon;
		addon = NULL;
	}
	if (!main)
		return false;

	QC_DefaultConfig (&config, QC_CSQC);
	config.developer = developer.value != 0;
	csqc.vm = QC_Create (main, csqc.builtins, &config, &csqc_host, NULL, &err);
	QC_ReleaseProgs (main);
	if (!csqc.vm)
	{
		Con_Printf ("CSQC: %s\n", QC_ErrorText (&err, text, sizeof(text)));
		QC_FreeError (&err);
		if (addon)
			QC_ReleaseProgs (addon);
		return false;
	}

	// the add-on's init runs as it is added, after the csprogs' own
	if (!QC_SyncAutocvars (csqc.vm))
	{
		CSQC_Failed ();
		if (addon)
			QC_ReleaseProgs (addon);
		return false;
	}
	if (!CSQC_CallEach ("init", 0))
	{
		if (addon)
			QC_ReleaseProgs (addon);
		return false;
	}
	if (addon)
	{
		csqc.calls++;
		ok = QC_AddProgs (csqc.vm, addon, &pr);
		csqc.calls--;
		QC_ReleaseProgs (addon);
		if (!ok)
		{
			CSQC_Failed ();
			return false;
		}
		if (!QC_SyncAutocvars (csqc.vm))
		{
			CSQC_Failed ();
			return false;
		}
	}
	if (!CSQC_CallEach ("initents", 0))
		return false;

	// what FTE sets before CSQC_Init
	CSQC_SetWorldField ("message", QC_EV_STRING, QC_ValWord (QC_Intern (csqc.vm, cl.levelname, strlen (cl.levelname))));
	s = Info_ValueForKey (cl.serverinfo, "map");
	CSQC_SetString ("mapname", *s ? s : *cl.model_name[1] ? cl.model_name[1] : "unknown");
	CSQC_SetFloat ("deathmatch", (float)atoi (Info_ValueForKey (cl.serverinfo, "deathmatch")));
	CSQC_SetFloat ("coop", !atoi (Info_ValueForKey (cl.serverinfo, "deathmatch"))
		&& atoi (Info_ValueForKey (cl.serverinfo, "maxclients")) > 1 ? 1.0f : 0.0f);
	CSQC_SetFloat ("maxclients", MAX_CLIENTS);
	CSQC_SetFloat ("player_localnum", (float)cl.playernum);
	CSQC_SetFloat ("player_localentnum", (float)(cl.playernum + 1));
	CSQC_SetFloat ("time", (float)cl.time);

	args[0] = QC_ValFloat (CSQC_API_VERSION);
	args[1] = QC_ValWord (QC_TempString (csqc.vm, "SoftWorld", 9));
	args[2] = QC_ValFloat ((float)VERSION);
	if (!CSQC_Call (CSQC_Entry ("CSQC_Init"), 3, args))
		return false;
	Con_DPrintf ("Loaded CSQC\n");
	return true;
}

/*
=================
CSQC_WorldLoaded

The world model is loaded: the world entity is the map's, and QuakeC may read
its entities with getentitytoken while CSQC_WorldLoaded runs; after, the
world is read only
=================
*/
void CSQC_WorldLoaded (void)
{
	if (!csqc.vm || csqc.worldloaded)
		return;
	csqc.worldloaded = true;

	QC_SetProtected (csqc.vm, 0, false);
	CSQC_SetWorldField ("solid", QC_EV_FLOAT, QC_ValFloat (SOLID_BSP));
	CSQC_SetWorldField ("modelindex", QC_EV_FLOAT, QC_ValFloat (1));
	CSQC_SetWorldField ("model", QC_EV_STRING, QC_ValWord (QC_HostString (csqc.vm, cl.model_name[1])));

	csqc.entitydata = cl.map ? CM_EntityString (cl.map) : NULL;
	CSQC_Call (CSQC_Entry ("CSQC_WorldLoaded"), 0, NULL);
	csqc.entitydata = NULL;
	if (csqc.vm)
		QC_SetProtected (csqc.vm, 0, true);
}

bool CSQC_Inited (void)
{
	return csqc.vm != NULL;
}

// the VM and everything of it gone
static void CSQC_Destroy (void)
{
	int		i;

	QC_Destroy (csqc.vm);
	csqc.vm = NULL;
	csqc.worldloaded = false;
	csqc.entitydata = NULL;
	if (csqc.entitycopy)
		Mem_Free (csqc.entitycopy);
	csqc.entitycopy = NULL;
	for (i = 0 ; i < csqc.numcommands ; i++)
	{
		Cmd_RemoveCommand (csqc.commands[i]);
		Mem_Free (csqc.commands[i]);
	}
	csqc.numcommands = 0;
}

/*
=================
CSQC_Shutdown

At serverdata (the next map's CSQC_Init brings it back), disconnect and quit;
QuakeC's CSQC_Shutdown runs first, unless a longjmp left it running
=================
*/
void CSQC_Shutdown (void)
{
	if (!csqc.vm)
		return;
	// an error the client longjmped out of while QuakeC ran: its calls are
	// abandoned, and QuakeC isn't called again
	if (csqc.calls)
	{
		QC_Abandon (csqc.vm);
		csqc.calls = 0;
		CSQC_Destroy ();
		return;
	}
	if (CSQC_Call (CSQC_Entry ("CSQC_Shutdown"), 0, NULL))
		CSQC_Destroy ();
}

/*
=================
CSQC_Builtins_f

The builtins the csprogs calls that the client doesn't have yet (with "all",
those it declares): the checklist for CSQC's networking and drawing. Of the
running CSQC, or of a progs file.
=================
*/
static void CSQC_Builtins_f (void)
{
	qcvm_t			*vm = csqc.vm;
	qc_progs_t		*p = NULL;
	qc_config_t		config;
	qc_unbound_t	*list;
	const char		*file = NULL;
	bool			all = false;
	uint32_t		n, i;
	byte			*data;
	int				size, arg;

	for (arg = 1 ; arg < Cmd_Argc () ; arg++)
		if (!strcmp (Cmd_Argv (arg), "all"))
			all = true;
		else
			file = Cmd_Argv (arg);
	if (file)
	{
		if (!(data = FS_LoadFile (file, &size)) || !(p = CSQC_LoadProgs (file, data, size)))
		{
			Con_Printf ("Couldn't load %s\n", file);
			return;
		}
		QC_DefaultConfig (&config, QC_CSQC);
		vm = QC_Create (p, csqc.builtins, &config, NULL, NULL, NULL);
		QC_ReleaseProgs (p);
		if (!vm)
		{
			Con_Printf ("Couldn't make a VM for %s\n", file);
			return;
		}
	}
	else if (!vm)
	{
		Con_Printf ("No CSQC is running. Usage: csqc_builtins [<progs.dat>] [all]\n");
		return;
	}

	n = QC_UnboundBuiltins (vm, !all, NULL, 0);
	list = Mem_Alloc (((size_t)n + 1) * sizeof(*list));
	QC_UnboundBuiltins (vm, !all, list, n);
	for (i = 0 ; i < n ; i++)
		if (list[i].number)
			Con_Printf ("#%-4u %s\n", list[i].number, list[i].name);
		else
			Con_Printf ("      %s\n", list[i].name);
	Con_Printf ("%u builtins %s the client lacks\n", n, all ? "declared" : "called");
	Mem_Free (list);
	if (vm != csqc.vm)
		QC_Destroy (vm);
}

void CSQC_RegisterVariables (void)
{
	size_t	i;

	Cvar_RegisterVariable (&cl_nocsqc);
	Cvar_RegisterVariable (&cl_download_csprogs);
	Cmd_AddCommand ("csqc_builtins", CSQC_Builtins_f, "Lists the builtins the client-side QuakeC calls that the "
		"client lacks, of the running CSQC or of a progs file; with all, those it declares. "
		"Usage: csqc_builtins [<progs.dat>] [all]");
	Cvar_AddChangeHook (CSQC_CvarChanged);

	csqc.builtins = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	if (!csqc.builtins)
		Sys_Error ("CSQC_RegisterVariables: out of memory");
	for (i = 0 ; i < sizeof(csqc_builtins) / sizeof(csqc_builtins[0]) ; i++)
		if (!QC_BuiltinsSet (csqc.builtins, csqc_builtins[i].name, csqc_builtins[i].func))
			Sys_Error ("CSQC_RegisterVariables: out of memory");
}
