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
// pr_edict.c -- the server's QuakeC: its VM, its entities, and the map's
//
// The VM (src/qcvm) runs the progs and owns the entities. Each is a block of
// its entity memory, the server's part of edict_t first, then the fields. The
// VM's slots say which are in use; edict_t.free follows them through the VM's
// spawn and remove hooks.

#include "sv_local.h"

#include <stddef.h>

pr_state_t		pr;

static bool		pr_profiling;		// the profile command started the counts

static cvar_t	sv_progs = {.name = "sv_progs", .string = "",
	.description = "The progs the server runs, from the game directory; empty chooses as FTE does: the game "
		"directory's own progs.dat or qwprogs.dat, else NetQuake's progs.dat when deathmatch is 0 or blank "
		"and QuakeWorld's qwprogs.dat when it isn't. Takes effect at the next map."};
static cvar_t	pr_checkextension = {.name = "pr_checkextension", .string = "1",
	.description = "Tells QuakeC that the server answers checkextension, as FTE's does: progs read this before "
		"they ask."};

// the cvars NetQuake keeps for its game code, which some progs use
#define	PR_SPARE	"One of the cvars NetQuake keeps for its game code to use."
#define	PR_SAVED	"One of the cvars NetQuake keeps for its game code to use, saved in the config."
static cvar_t	pr_sparecvars[] = {
	{.name = "gamecfg", .string = "0", .description = PR_SPARE},
	{.name = "savedgamecfg", .string = "0", .archive = true, .description = PR_SAVED},
	{.name = "scratch1", .string = "0", .description = PR_SPARE},
	{.name = "scratch2", .string = "0", .description = PR_SPARE},
	{.name = "scratch3", .string = "0", .description = PR_SPARE},
	{.name = "scratch4", .string = "0", .description = PR_SPARE},
	{.name = "saved1", .string = "0", .archive = true, .description = PR_SAVED},
	{.name = "saved2", .string = "0", .archive = true, .description = PR_SAVED},
	{.name = "saved3", .string = "0", .archive = true, .description = PR_SAVED},
	{.name = "saved4", .string = "0", .archive = true, .description = PR_SAVED},
	{.name = "temp1", .string = "0", .description = PR_SPARE},
};

static_assert (offsetof (edict_t, v) % 4 == 0, "the fields must start on a word");
static_assert (_Alignof (edict_t) <= 16, "the VM's entity blocks are 16-byte aligned");

/*
==============================================================================

THE VM'S HOST

==============================================================================
*/

static void PR_Warning (void *ctx, const qc_warning_t *w)
{
	char	text[1024];

	(void)ctx;
	Con_DPrintf ("QuakeC: %s\n", QC_WarningText (w, text, sizeof(text)));
}

static void PR_Trace (void *ctx, const char *line)
{
	(void)ctx;
	Con_Printf ("%s\n", line);
}

// a spawned entity: its fields are zero, the server's part is ours to reset
static void PR_OnSpawn (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	edict_t	*ed = EDICT_NUM ((int)e);

	(void)ctx;
	ed->free = false;
	ed->alpha = 0;
	VectorCopy (vec3_origin, ed->colormod);
	if ((int)QC_NumEdicts (vm) > sv.num_edicts)
		sv.num_edicts = (int)QC_NumEdicts (vm);
}

// An entity about to be freed, as FTE's server has it (ED_CanFree): the
// players' stay; the rest leave the world, with id's fields and the classname
// cleared (the VM refuses the world itself)
static bool PR_OnRemove (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	edict_t	*ed;

	(void)ctx;
	if (e <= MAX_CLIENTS)
	{
		QC_Warning (vm, "cannot free player entities");
		return false;
	}
	ed = EDICT_NUM ((int)e);
	SV_UnlinkEdict (ed);		// unlink from world bsp
	ed->free = true;
	ed->v.model = 0;
	ed->v.takedamage = 0;
	ed->v.modelindex = 0;
	ed->v.colormap = 0;
	ed->v.skin = 0;
	ed->v.frame = 0;
	VectorCopy (vec3_origin, ed->v.origin);
	VectorCopy (vec3_origin, ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;
	ed->v.classname = 0;
	return true;
}

// print, dprint (in developer mode), and the dumps of eprint, coredump and objerror
static void PR_Print (void *ctx, const char *text)
{
	(void)ctx;
	Con_Printf ("%s", text);
}

static void PR_Dump (void *ctx, qc_dumpkind_t kind, const char *text)
{
	(void)kind;
	PR_Print (ctx, text);
}

// a showcase's progs (sv_attract.c) run nothing of the user's: their
// localcmds are passed by, their rules are the showcase's, and the cvars they
// set are set back
static void PR_Localcmd (void *ctx, const char *text)
{
	(void)ctx;
	if (svs.attract)
		Con_DPrintf ("localcmd passed by in a showcase: %s", text);
	else
		Cbuf_AddText ((char *)text);
}

static float PR_CvarFloat (void *ctx, const char *name)
{
	const char	*rule = SV_AttractCvar (name);

	(void)ctx;
	return rule ? (float)atof (rule) : Cvar_VariableValue ((char *)name);
}

static const char *PR_CvarString (void *ctx, const char *name)
{
	cvar_t		*var = Cvar_FindVar ((char *)name);
	const char	*rule = SV_AttractCvar (name);

	(void)ctx;
	return rule ? rule : var ? var->string : NULL;
}

static void PR_CvarSet (void *ctx, const char *name, const char *value)
{
	(void)ctx;
	if (svs.attract)
		SV_AttractCvarSet (name, value);
	else
		Cvar_Set ((char *)name, (char *)value);
}

// checkcommand: 1 a command, 2 an alias, 3 a cvar
static uint32_t PR_CheckCommand (void *ctx, const char *name)
{
	(void)ctx;
	if (Cmd_Exists ((char *)name))
		return 1;
	if (Cmd_AliasExists (name))
		return 2;
	return Cvar_FindVar ((char *)name) ? 3 : 0;
}

static bool PR_IsServer (void *ctx)
{
	(void)ctx;
	return true;
}

// another progs for addprogs, from the game directory
static qc_progs_t *PR_LoadAddon (void *ctx, const char *name)
{
	byte			*data;
	int				size;
	qc_progs_t		*p;
	qc_loaderror_t	lerr;
	char			text[1024];

	(void)ctx;
	if (!*name || strstr (name, "..") || *name == '/' || *name == '\\' || strchr (name, ':'))
	{
		Con_Printf ("addprogs: refusing %s\n", name);
		return NULL;
	}
	data = FS_LoadFile (name, &size);
	if (!data)
		return NULL;
	p = QC_LoadProgs (data, (size_t)size, &lerr);
	Mem_Free (data);
	if (!p)
		Con_Printf ("%s: %s\n", name, QC_LoadErrorText (&lerr, text, sizeof(text)));
	return p;
}

// what checkextension answers: the standard builtins' extensions, and the
// particle effects the server sends (sv_part.c) and FTE's client scripts
static bool PR_CheckExtension (void *ctx, const char *name)
{
	static const char *const	own[] =
	{
		"FTE_SV_POINTPARTICLES", "FTE_PART_SCRIPT", "FTE_PART_NAMESPACES", "FTE_PART_NAMESPACE_EFFECTINFO",
		"DP_ENT_TRAILEFFECTNUM", "DP_TE_PARTICLERAIN", "DP_TE_PARTICLESNOW",
	};
	size_t	i;

	(void)ctx;
	for (i = 0 ; i < sizeof(own) / sizeof(own[0]) ; i++)
		if (!Q_strcasecmp (name, own[i]))
			return true;
	return QC_StandardExtension (name);
}

static const qc_host_t	pr_host = {
	.warning = PR_Warning,
	.print = PR_Print,
	.dprint = PR_Print,
	.localcmd = PR_Localcmd,
	.dump = PR_Dump,
	.cvar_float = PR_CvarFloat,
	.cvar_string = PR_CvarString,
	.cvar_set = PR_CvarSet,
	.check_extension = PR_CheckExtension,
	.check_command = PR_CheckCommand,
	.is_server = PR_IsServer,
	.load_progs = PR_LoadAddon,
	.trace = PR_Trace,
	.on_spawn = PR_OnSpawn,
	.on_remove = PR_OnRemove,
};

/*
==============================================================================

RUNNING QUAKEC

==============================================================================
*/

/*
============
PR_RunError

The error QuakeC stopped with, and where; the server goes down
============
*/
[[noreturn]] static void PR_RunError (void)
{
	const qc_error_t	*e = QC_LastError (pr.vm);
	char				text[1024];
	char				*trace = Mem_Alloc (16384);

	Con_Printf ("%s", QC_BacktraceText (&e->backtrace, trace, 16384));
	Mem_Free (trace);
	Con_Printf ("%s\n", QC_ErrorText (e, text, sizeof(text)));
	SV_Error ("Program error");
}

void PR_ExecuteProgram (func_t fnum)
{
	if (!pr.vm)
		SV_Error ("PR_ExecuteProgram: no progs");
	QC_SetTime (pr.vm, sv.time);
	if (!QC_Call (pr.vm, (qc_func_t)fnum, 0, NULL, NULL))
		PR_RunError ();
}

// fnum (s)
void PR_ExecuteProgramString (func_t fnum, const char *s)
{
	qc_value_t	arg;

	if (!pr.vm)
		SV_Error ("PR_ExecuteProgram: no progs");
	QC_SetTime (pr.vm, sv.time);
	arg = QC_ValWord (QC_TempString (pr.vm, s, strlen (s)));
	if (!QC_Call (pr.vm, (qc_func_t)fnum, 1, &arg, NULL))
		PR_RunError ();
}

/*
============
PR_RunThreads

Wakes the QuakeC threads (sleep, fork) whose time has come, as FTE's server
does after StartFrame
============
*/
void PR_RunThreads (void)
{
	if (!pr.vm || !QC_SleepingThreads (pr.vm))
		return;
	QC_SetTime (pr.vm, sv.time);
	if (!QC_RunThreads (pr.vm, NULL))
		PR_RunError ();
}

/*
============
PR_CvarChanged

The autocvars that follow a cvar (FTE's PR_AutoCvar)
============
*/
static void PR_CvarChanged (cvar_t *var)
{
	char	text[1024];

	if (pr.vm && !QC_SyncAutocvar (pr.vm, var->name))
		Con_Printf ("autocvar_%s: %s\n", var->name, QC_ErrorText (QC_LastError (pr.vm), text, sizeof(text)));
}

/*
============
PR_ResetStack

For an error the server longjmped out of while QuakeC ran
============
*/
void PR_ResetStack (void)
{
	if (pr.vm)
		QC_Abandon (pr.vm);
}

const char *PR_GetString (int num)
{
	return pr.vm ? QC_String (pr.vm, (qc_str_t)num) : "";
}

string_t PR_SetString (const char *s)
{
	return (string_t)QC_HostString (pr.vm, s);
}

/*
============
PR_Builtins_f

The builtins the running progs calls that the server doesn't have (with
"all", those it declares)
============
*/
static void PR_Builtins_f (void)
{
	qc_unbound_t	*list;
	bool			all = Cmd_Argc () > 1 && !strcmp (Cmd_Argv (1), "all");
	uint32_t		n, i;

	if (!pr.vm)
	{
		Con_Printf ("No progs is running\n");
		return;
	}
	n = QC_UnboundBuiltins (pr.vm, !all, NULL, 0);
	list = Mem_Alloc (((size_t)n + 1) * sizeof(*list));
	QC_UnboundBuiltins (pr.vm, !all, list, n);
	for (i = 0 ; i < n ; i++)
		if (list[i].number)
			Con_Printf ("#%-4u %s\n", list[i].number, list[i].name);
		else
			Con_Printf ("      %s\n", list[i].name);
	Con_Printf ("%u builtins %s the server lacks\n", n, all ? "declared" : "called");
	Mem_Free (list);
}

/*
============
PR_Profile_f

The busiest QuakeC functions since the counts started
============
*/
static void PR_Profile_f (void)
{
	const uint64_t	*counts;
	uint64_t		best;
	bool			*listed;
	uint32_t		count, i, pick, num;
	qc_funcinfo_t	info;

	if (!pr.vm)
		return;
	if (!pr_profiling)
	{
		pr_profiling = true;
		QC_SetProfiling (pr.vm, true);
		Con_Printf ("Counting QuakeC statements: profile again to list the busiest functions.\n");
		return;
	}
	counts = QC_Profile (pr.vm, 0, &count);
	if (!counts)
		return;
	listed = Mem_Calloc (count + 1, sizeof(*listed));
	for (num = 0 ; num < 10 ; num++)
	{
		best = 0;
		pick = 0;
		for (i = 0 ; i < count ; i++)
			if (!listed[i] && counts[i] > best)
			{
				best = counts[i];
				pick = i;
			}
		if (!best)
			break;
		listed[pick] = true;
		Con_Printf ("%9llu %s\n", (unsigned long long)best,
			QC_ProgsFunction (pr.progs, pick, &info) ? info.name : "?");
	}
	Mem_Free (listed);
	QC_ClearProfile (pr.vm);
}

/*
==============================================================================

ENTITIES

==============================================================================
*/

// an entity the VM has a slot for (its memory is committed as slots are)
edict_t *EDICT_NUM (int n)
{
	if (n < 0 || n >= MAX_EDICTS || (pr.vm && (uint32_t)n >= QC_NumEdicts (pr.vm)))
		SV_Error ("EDICT_NUM: bad number %i", n);
	return (edict_t *)((byte *)sv.edicts + (size_t)n * (size_t)pr.edict_size);
}

int NUM_FOR_EDICT (edict_t *e)
{
	ptrdiff_t	b;

	b = ((byte *)e - (byte *)sv.edicts) / pr.edict_size;
	if (b < 0 || b >= sv.num_edicts)
		SV_Error ("NUM_FOR_EDICT: bad pointer");
	return (int)b;
}

edict_t *PROG_TO_EDICT (int n)
{
	if (n < 0 || n >= sv.num_edicts)
		return sv.edicts;
	return EDICT_NUM (n);
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one. The VM avoids reusing an
entity that was freed recently, because the client could take it for the old
one morphed instead of removed and made again (interpolated angles, bad
trails).
=================
*/
edict_t *ED_Alloc (void)
{
	qc_ent_t	e;
	edict_t		*ed;

	QC_SetTime (pr.vm, sv.time);
	if (QC_Spawn (pr.vm, &e))
		return EDICT_NUM ((int)e);

	Con_Printf ("WARNING: ED_Alloc: no free edicts\n");
	ed = EDICT_NUM (sv.num_edicts - 1);		// step on whatever is the last edict
	SV_UnlinkEdict (ed);
	memset (&ed->v, 0, QC_FieldWords (pr.vm) * 4);
	ed->alpha = 0;
	VectorCopy (vec3_origin, ed->colormod);
	ed->free = false;
	return ed;
}

/*
=================
ED_Free

Marks the edict as free; the VM refuses the world, an entity that is free
already, and (PR_OnRemove) the players'
=================
*/
void ED_Free (edict_t *ed)
{
	QC_SetTime (pr.vm, sv.time);
	QC_Remove (pr.vm, (qc_ent_t)NUM_FOR_EDICT (ed), false);
}

//===========================================================================

// the words a definition's value takes (one for void)
static uint32_t PR_TypeWords (uint32_t type)
{
	int	words = QC_TypeWords (type);

	return words > 0 ? (uint32_t)words : 1;
}

// the progs' field definition i, at the word the field has in the VM (the
// server's struct has id's); false past the last
static bool ED_ProgsField (uint32_t i, qc_definfo_t *d)
{
	if (!QC_ProgsFieldDefAt (pr.progs, i, d))
		return false;
	QC_FindField (pr.vm, d->name, &d->ofs, NULL);
	return true;
}

// a definition's value as text
static const char *PR_ValueString (uint32_t type, const int *val)
{
	static char		line[256];
	qc_funcinfo_t	f;
	qc_definfo_t	d;
	uint32_t		i;

	switch (type)
	{
	case ev_string:
		snprintf (line, sizeof(line), "%s", PR_GetString (val[0]));
		break;
	case ev_entity:
		snprintf (line, sizeof(line), "entity %i", val[0]);
		break;
	case ev_function:
		snprintf (line, sizeof(line), "%s()", QC_ProgsFunction (pr.progs, (uint32_t)QC_FUNC_INDEX (val[0]), &f)
			? f.name : "?");
		break;
	case ev_field:
		snprintf (line, sizeof(line), ".?");
		for (i = 0 ; ED_ProgsField (i, &d) ; i++)
			if (d.ofs == (uint32_t)val[0])
			{
				snprintf (line, sizeof(line), ".%s", d.name);
				break;
			}
		break;
	case ev_void:
		snprintf (line, sizeof(line), "void");
		break;
	case ev_float:
		snprintf (line, sizeof(line), "%5.1f", *(const float *)val);
		break;
	case ev_vector:
		snprintf (line, sizeof(line), "'%5.1f %5.1f %5.1f'", ((const float *)val)[0], ((const float *)val)[1],
			((const float *)val)[2]);
		break;
	case ev_pointer:
		snprintf (line, sizeof(line), "pointer");
		break;
	default:
		snprintf (line, sizeof(line), "bad type %u", type);
		break;
	}
	return line;
}

// a field's value in an entity, or NULL if the entity is too small for it
static const int *ED_FieldValue (edict_t *ed, const qc_definfo_t *d)
{
	if ((uint64_t)d->ofs + PR_TypeWords (d->type) > QC_FieldWords (pr.vm))
		return NULL;
	return (const int *)((const char *)&ed->v + (size_t)d->ofs * 4);
}

/*
=============
ED_Print

For debugging
=============
*/
void ED_Print (edict_t *ed)
{
	qc_definfo_t	d;
	const int		*v;
	uint32_t		i, j, words;
	size_t			l;

	if (ed->free)
	{
		Con_Printf ("FREE\n");
		return;
	}
	for (i = 1 ; ED_ProgsField (i, &d) ; i++)
	{
		l = strlen (d.name);
		if (l >= 2 && d.name[l - 2] == '_')
			continue;	// skip _x, _y, _z vars
		v = ED_FieldValue (ed, &d);
		if (!v)
			continue;

	// if the value is still all 0, skip the field
		words = PR_TypeWords (d.type);
		for (j = 0 ; j < words ; j++)
			if (v[j])
				break;
		if (j == words)
			continue;

		Con_Printf ("%s", d.name);
		while (l++ < 15)
			Con_Printf (" ");
		Con_Printf ("%s\n", PR_ValueString (d.type, v));
	}
}

void ED_PrintNum (int ent)
{
	ED_Print (EDICT_NUM(ent));
}

/*
=============
ED_PrintEdicts

For debugging, prints all the entities in the current server
=============
*/
void ED_PrintEdicts (void)
{
	int		i;
	
	if (sv.state == ss_dead)
		return;
	Con_Printf ("%i entities\n", sv.num_edicts);
	for (i=0 ; i<sv.num_edicts ; i++)
	{
		Con_Printf ("\nEDICT %i:\n",i);
		ED_PrintNum (i);
	}
}

/*
=============
ED_PrintEdict_f

For debugging, prints a single edict
=============
*/
static void ED_PrintEdict_f (void)
{
	int		i;
	
	if (sv.state == ss_dead)
		return;
	i = Q_atoi (Cmd_Argv(1));
	if (i < 0 || i >= sv.num_edicts)
	{
		Con_Printf ("No entity %i: the map has %i\n", i, sv.num_edicts);
		return;
	}
	Con_Printf ("\n EDICT %i:\n",i);
	ED_PrintNum (i);
}

/*
=============
ED_Count

For debugging
=============
*/
static void ED_Count (void)
{
	int		i;
	edict_t	*ent;
	int		active, models, solid, step;

	if (sv.state == ss_dead)
		return;
	active = models = solid = step = 0;
	for (i=0 ; i<sv.num_edicts ; i++)
	{
		ent = EDICT_NUM(i);
		if (ent->free)
			continue;
		active++;
		if (ent->v.solid)
			solid++;
		if (ent->v.model)
			models++;
		if (ent->v.movetype == MOVETYPE_STEP)
			step++;
	}

	Con_Printf ("num_edicts:%3i\n", sv.num_edicts);
	Con_Printf ("active    :%3i\n", active);
	Con_Printf ("view      :%3i\n", models);
	Con_Printf ("touch     :%3i\n", solid);
	Con_Printf ("step      :%3i\n", step);

}

/*
=============
ED_Hash

FNV-1a, 64 bits
=============
*/
static uint64_t ED_Hash (uint64_t h, const void *data, size_t len)
{
	const byte	*p = data;
	size_t		i;

	for (i = 0 ; i < len ; i++)
		h = (h ^ p[i]) * 0x100000001B3ull;
	return h;
}

// a value normalized so another VM's can be compared: entities as numbers,
// strings and functions by their text, the rest as their bits
static uint64_t ED_HashValue (uint64_t h, uint32_t type, const int *v)
{
	qc_funcinfo_t	f;
	const char		*s;

	switch (type)
	{
	case ev_string:
		s = PR_GetString (v[0]);
		return ED_Hash (h, s, strlen (s) + 1);
	case ev_entity:
		return ED_Hash (h, v, 4);
	case ev_function:
		s = v[0] > 0 && QC_ProgsFunction (pr.progs, (uint32_t)QC_FUNC_INDEX (v[0]), &f) ? f.name : "";
		return ED_Hash (h, s, strlen (s) + 1);
	case ev_vector:
		return ED_Hash (h, v, 12);
	default:
		return ED_Hash (h, v, 4);
	}
}

/*
=============
ED_Digest_f

A hash of each entity's fields and of the globals, to compare what two VMs
made of the same map
=============
*/
static void ED_Digest_f (void)
{
	uint64_t		h, total = 0xCBF29CE484222325ull;
	edict_t			*ed;
	qc_definfo_t	d;
	const int		*v;
	uint32_t		j;
	int				i;

	if (sv.state == ss_dead)
		return;
	for (i = 0 ; i < sv.num_edicts ; i++)
	{
		ed = EDICT_NUM (i);
		h = 0xCBF29CE484222325ull;
		for (j = 1 ; ED_ProgsField (j, &d) ; j++)
			if ((v = ED_FieldValue (ed, &d)))
				h = ED_HashValue (h, d.type, v);
		Con_Printf ("edict %4i %s%016llx\n", i, ed->free ? "free " : "", (unsigned long long)h);
		total = ED_Hash (total, &h, 8);
	}
	h = 0xCBF29CE484222325ull;
	for (j = 0 ; QC_ProgsGlobalDefAt (pr.progs, j, &d) ; j++)
		if ((uint64_t)d.ofs + PR_TypeWords (d.type) <= QC_ProgsNumGlobals (pr.progs))
			h = ED_HashValue (h, d.type, (const int *)&pr.globals[d.ofs]);
	total = ED_Hash (total, &h, 8);
	Con_Printf ("globals %016llx\n", (unsigned long long)h);
	Con_Printf ("edictdigest %i %016llx\n", sv.num_edicts, (unsigned long long)total);
}

/*
==============================================================================

SAVEGAMES

==============================================================================
*/

/*
=============
ED_SaveValue

A value as NetQuake's savegames have it (PR_UglyValueString): entities by
number, functions and fields by name, strings as they are; NULL for one
that can't be written (a function of a progs added after the first)
=============
*/
static const char *ED_SaveValue (uint32_t type, const int *val)
{
	static char		line[128];
	qc_funcinfo_t	f;
	qc_definfo_t	d;
	uint32_t		i;

	switch (type)
	{
	case ev_string:
		return PR_GetString (val[0]);
	case ev_entity:
		snprintf (line, sizeof(line), "%i", val[0]);
		return line;
	case ev_function:
		if (QC_FUNC_PROGS (val[0]) || !QC_ProgsFunction (pr.progs, QC_FUNC_INDEX (val[0]), &f))
			return NULL;
		return f.name;
	case ev_field:
		for (i = 1 ; ED_ProgsField (i, &d) ; i++)
			if (d.ofs == (uint32_t)val[0])
				return d.name;
		return NULL;
	case ev_float:
		snprintf (line, sizeof(line), "%f", *(const float *)val);
		return line;
	case ev_vector:
		snprintf (line, sizeof(line), "%f %f %f", ((const float *)val)[0], ((const float *)val)[1],
			((const float *)val)[2]);
		return line;
	default:
		return NULL;
	}
}

/*
=============
ED_WriteGlobals

The globals a savegame keeps: those the compiler marks to save, of the
types NetQuake writes (a vector's come as its _x, _y and _z)
=============
*/
void ED_WriteGlobals (FILE *f)
{
	qc_definfo_t	d;
	const char		*v;
	uint32_t		i;

	fprintf (f, "{\n");
	for (i = 0 ; QC_ProgsGlobalDefAt (pr.progs, i, &d) ; i++)
	{
		if (!d.save || (d.type != ev_string && d.type != ev_float && d.type != ev_entity))
			continue;
		if ((uint64_t)d.ofs + PR_TypeWords (d.type) > QC_ProgsNumGlobals (pr.progs))
			continue;
		if ((v = ED_SaveValue (d.type, (const int *)&pr.globals[d.ofs])))
			fprintf (f, "\"%s\" \"%s\"\n", d.name, v);
	}
	fprintf (f, "}\n");
}

/*
=============
ED_WriteEdicts

The edicts as a savegame keeps them, each its fields with a value but the
parts of vectors (as ED_Print shows them) and the alpha and color the server
holds for progs without those fields; nothing for a free one, nor for those
from empty to empty_end (players' slots of no one). The fields are looked up
once, for all.
=============
*/
void ED_WriteEdicts (FILE *f, int empty, int empty_end)
{
	qc_definfo_t	d, *fields;
	const int		*v;
	const char		*s;
	edict_t			*ed;
	uint32_t		i, j, words, count = 0;
	size_t			l;
	int				e;

	fields = Mem_Alloc ((QC_ProgsNumFieldDefs (pr.progs) + 1) * sizeof(*fields));
	for (i = 1 ; ED_ProgsField (i, &d) ; i++)
	{
		l = strlen (d.name);
		if (l < 2 || d.name[l - 2] != '_')	// not the parts of a vector
			fields[count++] = d;
	}

	for (e = 0 ; e < sv.num_edicts ; e++)
	{
		fprintf (f, "{ // #%i\n", e);
		ed = EDICT_NUM (e);
		if (!ed->free && (e < empty || e > empty_end))
		{
			for (i = 0 ; i < count ; i++)
			{
				if (!(v = ED_FieldValue (ed, &fields[i])))
					continue;
				words = PR_TypeWords (fields[i].type);
				for (j = 0 ; j < words && !v[j] ; j++)
					;
				if (j < words && (s = ED_SaveValue (fields[i].type, v)))
					fprintf (f, "\"%s\" \"%s\"\n", fields[i].name, s);
			}
			if (!pr.fofs_alpha && ed->alpha)
				fprintf (f, "\"alpha\" \"%f\"\n", ed->alpha);
			if (!pr.fofs_colormod && (ed->colormod[0] || ed->colormod[1] || ed->colormod[2]))
				fprintf (f, "\"colormod\" \"%f %f %f\"\n", ed->colormod[0], ed->colormod[1], ed->colormod[2]);
		}
		fprintf (f, "}\n");
	}
	Mem_Free (fields);
}

/*
==============================================================================

THE MAP'S ENTITIES

==============================================================================
*/

/*
=============
ED_NewString

The map's text as a string for the level, its \n escapes made newlines
=============
*/
static string_t ED_NewString (const char *string)
{
	size_t		l = strlen (string), i;
	char		*copy = Mem_Alloc (l + 1), *p = copy;
	string_t	s;

	for (i = 0 ; i < l ; i++)
	{
		if (string[i] == '\\' && i < l - 1)
		{
			i++;
			*p++ = string[i] == 'n' ? '\n' : '\\';
		}
		else
			*p++ = string[i];
	}
	*p = 0;
	s = (string_t)QC_Intern (pr.vm, copy, (size_t)(p - copy));
	Mem_Free (copy);
	return s;
}


// the client slots the edicts of a savegame being read were numbered with
// (NetQuake's 1); 0 for a map's own entities
static int	ed_saveslots;

/*
=============
ED_ParseValue

A value of the type from the map's or a savegame's text; false if it can't
be parsed. A savegame's entities are numbered as this server has them, its
edicts past its client slots after this server's.
=============
*/
static bool ED_ParseValue (int *d, uint32_t type, char *s)
{
	int				i, n;
	char			string[128];
	qc_definfo_t	field;
	char			*v, *w;
	qc_func_t		func;

	switch (type)
	{
	case ev_string:
		*d = ED_NewString (s);
		break;
		
	case ev_float:
		*(float *)d = (float)atof (s);
		break;

	case ev_vector:
		Q_strncpyz (string, s, sizeof(string));
		v = string;
		w = string;
		for (i=0 ; i<3 ; i++)
		{
			while (*v && *v != ' ')
				v++;
			*v = 0;
			((float *)d)[i] = (float)atof (w);
			w = v = v+1;
		}
		break;
		
	case ev_entity:
		n = atoi (s);
		if (ed_saveslots && n > ed_saveslots)
			n += MAX_CLIENTS - ed_saveslots;
		if (n < 0 || n >= MAX_EDICTS)
			SV_Error ("EDICT_NUM: bad number %i", n);
		*d = n;
		break;
		
	case ev_field:
		if (!QC_ProgsFieldDef (pr.progs, s, &field))
		{
			Con_Printf ("Can't find field %s\n", s);
			return false;
		}
		// as id had it: the global at the field's offset in the progs
		*d = field.ofs < QC_ProgsNumGlobals (pr.progs) ? ((int *)pr.globals)[field.ofs] : 0;
		break;
	
	case ev_function:
		func = QC_FindFunction (pr.vm, s);
		if (!func)
		{
			Con_Printf ("Can't find function %s\n", s);
			return false;
		}
		*d = (int)func;
		break;

	default:
		break;
	}
	return true;
}

// a field's value from the text; false if it can't be parsed
static bool ED_ParseEpair (edict_t *ent, const qc_definfo_t *key, char *s)
{
	if (!ED_FieldValue (ent, key))
		return false;
	return ED_ParseValue ((int *)((char *)&ent->v + (size_t)key->ofs * 4), key->type, s);
}

/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
====================
*/
static char *ED_ParseEdict (char *data, edict_t *ent)
{
	qc_definfo_t	key;
	bool			anglehack;
	char			keyname[256];

// clear it
	if (ent != sv.edicts)	// hack
		memset (&ent->v, 0, QC_FieldWords (pr.vm) * 4);
	ent->alpha = 0;
	memset (ent->colormod, 0, sizeof(ent->colormod));

// go through all the dictionary pairs
	while (1)
	{	
	// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			SV_Error ("ED_ParseEntity: EOF without closing brace");
		
// anglehack is to allow QuakeEd to write single scalar angles
// and allow them to be turned into vectors. (FIXME...)
if (!strcmp(com_token, "angle"))
{
	Q_strncpyz (com_token, "angles", sizeof(com_token));
	anglehack = true;
}
else
	anglehack = false;

// FIXME: change light to _light to get rid of this hack
if (!strcmp(com_token, "light"))
	Q_strncpyz (com_token, "light_lev", sizeof(com_token));	// hack for single light def

		Q_strncpyz (keyname, com_token, sizeof(keyname));
		
	// parse value	
		data = COM_Parse (data);
		if (!data)
			SV_Error ("ED_ParseEntity: EOF without closing brace");

		if (com_token[0] == '}')
			SV_Error ("ED_ParseEntity: closing brace without data");

// keynames with a leading underscore are used for utility comments,
// and are immediately discarded by quake
		if (keyname[0] == '_')
			continue;
		
		key = (qc_definfo_t){.name = keyname};
		if (!QC_FindField (pr.vm, keyname, &key.ofs, &key.type))
		{
			// FTE's entity alpha and color, kept by the server when the
			// progs have no such fields
			if (!strcmp (keyname, "alpha"))
				ent->alpha = Q_atof (com_token);
			else if (!strcmp (keyname, "colormod"))
				sscanf (com_token, "%f %f %f", &ent->colormod[0], &ent->colormod[1], &ent->colormod[2]);
			else
				Con_DPrintf ("%s is not a field\n", keyname);
			continue;
		}

if (anglehack)
{
char	temp[32];
Q_strncpyz (temp, com_token, sizeof(temp));
snprintf (com_token, sizeof(com_token), "0 %s 0", temp);
}

		if (!ED_ParseEpair (ent, &key, com_token))
			SV_Error ("ED_ParseEdict: parse error");
	}

	return data;
}

/*
=============
ED_ParseGlobals

A savegame's globals, after the opening brace, by name; slots is the client
slots its edicts were numbered with
=============
*/
char *ED_ParseGlobals (char *data, int slots)
{
	qc_definfo_t	d;
	char			keyname[64];

	ed_saveslots = slots;
	while (1)
	{
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			SV_Error ("ED_ParseGlobals: EOF without closing brace");
		Q_strncpyz (keyname, com_token, sizeof(keyname));

		data = COM_Parse (data);
		if (!data)
			SV_Error ("ED_ParseGlobals: EOF without closing brace");
		if (com_token[0] == '}')
			SV_Error ("ED_ParseGlobals: closing brace without data");

		if (!QC_ProgsGlobalDef (pr.progs, keyname, &d)
			|| (uint64_t)d.ofs + PR_TypeWords (d.type) > QC_ProgsNumGlobals (pr.progs))
		{
			Con_DPrintf ("'%s' is not a global\n", keyname);
			continue;
		}
		if (!ED_ParseValue ((int *)&pr.globals[d.ofs], d.type, com_token))
			SV_Error ("ED_ParseGlobals: parse error");
	}
	ed_saveslots = 0;
	return data;
}

/*
=============
ED_ParseSavedEdict

A savegame's edict, after the opening brace, into ent, which is in use
=============
*/
char *ED_ParseSavedEdict (char *data, edict_t *ent, int slots)
{
	ed_saveslots = slots;
	data = ED_ParseEdict (data, ent);
	ed_saveslots = 0;
	return data;
}


/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.
================
*/
void ED_LoadFromFile (char *data)
{	
	edict_t		*ent;
	int			inhibit;
	qc_func_t	func;
	
	ent = NULL;
	inhibit = 0;
	PR_GLOBAL(time) = (float)sv.time;

// parse ents
	while (1)
	{
// parse the opening brace	
		data = COM_Parse (data);
		if (!data)
			break;
		if (com_token[0] != '{')
			SV_Error ("ED_LoadFromFile: found %s when expecting {",com_token);

		if (!ent)
			ent = EDICT_NUM(0);
		else
			ent = ED_Alloc ();
		data = ED_ParseEdict (data, ent);
		
// remove things from different skill levels or deathmatch
		if (SV_Deathmatch () ? (int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH
			: (int)ent->v.spawnflags & (skill.value == 0 ? SPAWNFLAG_NOT_EASY
				: skill.value == 1 ? SPAWNFLAG_NOT_MEDIUM : SPAWNFLAG_NOT_HARD))
		{
			ED_Free (ent);	
			inhibit++;
			continue;
		}

//
// immediately call spawn function
//
		if (!ent->v.classname)
		{
			Con_Printf ("No classname for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}
		
	// look for the spawn function
		func = QC_FindFunction (pr.vm, PR_GetString (ent->v.classname));

		if (!func)
		{
			Con_Printf ("No spawn function for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

		PR_GLOBAL(self) = EDICT_TO_PROG(ent);
		PR_ExecuteProgram ((func_t)func);
		SV_FlushSignon();
	}	

	Con_DPrintf ("%i entities inhibited\n", inhibit);
}


/*
==============================================================================

LOADING

==============================================================================
*/

/*
===============
PR_FreeProgs

The VM and its progs, gone
===============
*/
void PR_FreeProgs (void)
{
	if (pr.vm)
		QC_Destroy (pr.vm);
	pr.vm = NULL;
	QC_ReleaseProgs (pr.progs);
	pr.progs = NULL;
	memset (&pr.g, 0, sizeof(pr.g));
	pr.globals = NULL;
}

// an optional field of a type: its offset, or 0 without
static int PR_OptionalField (const char *name, uint32_t want)
{
	uint32_t	ofs, type;

	return QC_FindField (pr.vm, name, &ofs, &type) && type == want ? (int)ofs : 0;
}

// id's fields where entvars_t has them: the VM moves a progs' there
static const qc_hostfield_t	pr_hostfields[] = {
#define X(type, qctype, name)	{#name, qctype, offsetof (entvars_t, name) / 4},
	PR_ENTITY_FIELDS (X)
#undef X
};

// id's globals a progs lacks, kept by the server
static struct
{
#define X(type, qctype, name)	type name;
	PR_GLOBALS (X)
#undef X
	float	parm[NUM_SPAWN_PARMS];
} pr_ownglobals;

// the progs' global of the name if it has one as big as the type, else NULL
static void *PR_ProgsGlobal (const char *name, uint32_t want)
{
	uint32_t	word, type;

	if (!QC_FindGlobal (pr.vm, name, &word, &type) || QC_TypeWords (type) != QC_TypeWords (want))
		return NULL;
	return &QC_Globals (pr.vm)[word];
}

// points id's globals at the progs' own, or at the server's for those it lacks
// (QuakeWorld's newmis, NetQuake's deathmatch, coop and teamplay)
static void PR_BindGlobals (void)
{
	char	parm[16];
	void	*g;
	int		i;

	memset (&pr_ownglobals, 0, sizeof(pr_ownglobals));
#define X(type, qctype, name)	pr.g.name = (g = PR_ProgsGlobal (#name, qctype)) ? g : &pr_ownglobals.name;
	PR_GLOBALS (X)
#undef X
	for (i = 0 ; i < NUM_SPAWN_PARMS ; i++)
	{
		snprintf (parm, sizeof(parm), "parm%i", i + 1);
		pr.g.parm[i] = (g = PR_ProgsGlobal (parm, ev_float)) ? g : &pr_ownglobals.parm[i];
	}
}

// the functions the server calls, which every progs has
static void PR_CheckEntryPoints (void)
{
	const struct
	{
		const char	*name;
		func_t		f;
	} entries[] = {
		{"StartFrame", PR_GLOBAL(StartFrame)}, {"PlayerPreThink", PR_GLOBAL(PlayerPreThink)},
		{"PlayerPostThink", PR_GLOBAL(PlayerPostThink)}, {"ClientKill", PR_GLOBAL(ClientKill)},
		{"ClientConnect", PR_GLOBAL(ClientConnect)}, {"PutClientInServer", PR_GLOBAL(PutClientInServer)},
		{"ClientDisconnect", PR_GLOBAL(ClientDisconnect)}, {"SetNewParms", PR_GLOBAL(SetNewParms)},
		{"SetChangeParms", PR_GLOBAL(SetChangeParms)},
	};
	size_t	i;

	for (i = 0 ; i < sizeof(entries) / sizeof(entries[0]) ; i++)
		if (!entries[i].f)
			SV_Error ("%s has no %s", pr.name, entries[i].name);
}

/*
===============
PR_ChooseProgs

The progs to run, as FTE chooses (Q_InitProgs): sv_progs' if set; else the
game directory's own progs.dat or qwprogs.dat over the base's (id1's and qw's,
and the qwprogs.dat built in); else NetQuake's progs.dat for single player
and coop (deathmatch 0 or blank), QuakeWorld's qwprogs.dat for the rest
===============
*/
static void PR_ChooseProgs (fs_chain_t **dir)
{
	bool		nq, qw;
	const char	*base;

	// a showcase's, from its map's game directory alone
	*dir = NULL;
	if (SV_AttractProgs (pr.name, sizeof(pr.name), dir))
		return;
	if (sv_progs.string[0])
	{
		Q_strncpyz (pr.name, sv_progs.string, sizeof(pr.name));
		base = strrchr (pr.name, '/');
		if (!strchr (base ? base : pr.name, '.'))
			Q_strncatz (pr.name, ".dat", sizeof(pr.name));
		return;
	}
	nq = FS_InGameDir ("progs.dat");
	qw = FS_InGameDir ("qwprogs.dat");
	if (nq != qw)
		Q_strncpyz (pr.name, nq ? "progs.dat" : "qwprogs.dat", sizeof(pr.name));
	else	// as asked for, not as the last level's progs chose (blank)
		Q_strncpyz (pr.name, Q_atof (Cvar_UserString (&deathmatch)) ? "qwprogs.dat" : "progs.dat", sizeof(pr.name));
}

// the progs' file, else for qwprogs.dat the one built in; NULL without
static byte *PR_LoadProgsFile (int *size)
{
	byte	*data = FS_LoadFile (pr.name, size);

	if (data || strcmp (pr.name, "qwprogs.dat"))
		return data;
	*size = (int)sv_qwprogs_size;
	data = Mem_Alloc (sv_qwprogs_size);
	memcpy (data, sv_qwprogs, sv_qwprogs_size);
	Con_DPrintf ("qwprogs.dat: the one built in\n");
	return data;
}

/*
===============
PR_LoadProgs

The progs PR_ChooseProgs names (QuakeWorld's when it is missing), in a new VM:
QuakeWorld's or NetQuake's by its header's CRC, its fields moved to the
server's struct and its globals bound by name
===============
*/
void PR_LoadProgs (void)
{
	byte				*data, *lno;
	int					size, lnosize;
	char				num[32], text[1024], lnoname[MAX_QPATH];
	fs_chain_t			*dir;
	qc_loaderror_t		lerr;
	qc_error_t			err;
	qc_config_t			config;
	const qc_loadnote_t	*notes;
	uint32_t			count, n;

	PR_FreeProgs ();
	PR_ClearLightstyles ();

	PR_ChooseProgs (&dir);
	FS_UseChain (dir);
	data = PR_LoadProgsFile (&size);
	if (!data)
	{	// as FTE: QuakeWorld's then
		Con_Printf ("%s: not found, running qwprogs.dat\n", pr.name);
		Q_strncpyz (pr.name, "qwprogs.dat", sizeof(pr.name));
		data = PR_LoadProgsFile (&size);
	}
	Con_DPrintf ("Programs occupy %iK.\n", size/1024);

// add prog crc to the serverinfo
	snprintf (num, sizeof(num), "%i", CRC_Block (data, size));
	Info_SetValueForStarKey (svs.info, "*progs", num, MAX_SERVERINFO_STRING, SV_InfoCharset ());

	pr.progs = QC_LoadProgs (data, (size_t)size, &lerr);
	Mem_Free (data);
	if (!pr.progs)
		SV_Error ("%s: %s", pr.name, QC_LoadErrorText (&lerr, text, sizeof(text)));
	notes = QC_ProgsNotes (pr.progs, &count);
	for (n = 0 ; n < count ; n++)
		Con_DPrintf ("%s: %s\n", pr.name, QC_LoadNoteText (&notes[n], text, sizeof(text)));

	// source lines for the backtraces, when the compiler wrote them
	COM_StripExtension (pr.name, lnoname);
	Q_strncatz (lnoname, ".lno", sizeof(lnoname));
	lno = FS_LoadFile (lnoname, &lnosize);
	FS_UseChain (NULL);
	if (lno)
	{
		if (!QC_AttachLineNumbers (pr.progs, lno, (size_t)lnosize, &lerr))
			Con_DPrintf ("%s: %s\n", lnoname, QC_LoadErrorText (&lerr, text, sizeof(text)));
		Mem_Free (lno);
	}

	// FTE's PROG_UNKNOWN acts as NetQuake's
	pr.nq = QC_ProgsCRC (pr.progs) != PROGHEADER_CRC;
	Con_DPrintf ("%s: %s's\n", pr.name, pr.nq ? "NetQuake" : "QuakeWorld");

	QC_DefaultConfig (&config, QC_SSQC);
	config.limits.max_edicts = MAX_EDICTS;
	config.first_spawnable = MAX_CLIENTS + 1;		// the clients' are the server's
	config.entity_header_bytes = offsetof (edict_t, v);
	config.host_fields = pr_hostfields;
	config.num_host_fields = sizeof(pr_hostfields) / sizeof(pr_hostfields[0]);
	config.remove_clears = NULL;		// PR_OnRemove clears id's
	config.developer = developer.value != 0;
	pr.vm = QC_Create (pr.progs, pr.builtins, &config, &pr_host, NULL, &err);
	if (!pr.vm)
	{
		QC_ErrorText (&err, text, sizeof(text));
		QC_FreeError (&err);
		SV_Error ("%s: %s", pr.name, text);
	}
	if (pr_profiling)
		QC_SetProfiling (pr.vm, true);
	if (!QC_SyncAutocvars (pr.vm))
		SV_Error ("%s: %s", pr.name, QC_ErrorText (QC_LastError (pr.vm), text, sizeof(text)));

	PR_BindGlobals ();
	PR_CheckEntryPoints ();
	pr.globals = (float *)QC_Globals (pr.vm);
	pr.edict_size = 1 << QC_EdictShift (pr.vm);

	pr.fofs_alpha = PR_OptionalField ("alpha", ev_float);
	pr.fofs_colormod = PR_OptionalField ("colormod", ev_vector);
	pr.fofs_gravity = PR_OptionalField ("gravity", ev_float);
	pr.fofs_maxspeed = PR_OptionalField ("maxspeed", ev_float);
	pr.fofs_teleported = PR_OptionalField ("teleported", ev_float);
	if (!pr.fofs_teleported)
		pr.fofs_teleported = PR_OptionalField ("teleported", QC_EV_INTEGER);
	pr.fofs_teleport_time = PR_OptionalField ("teleport_time", ev_float);
	pr.fofs_traileffectnum = PR_OptionalField ("traileffectnum", ev_float);
	pr.fofs_emiteffectnum = PR_OptionalField ("emiteffectnum", ev_float);

	// Zoid, find the spectator functions
	pr.SpectatorConnect = (func_t)QC_FindFunction (pr.vm, "SpectatorConnect");
	pr.SpectatorThink = (func_t)QC_FindFunction (pr.vm, "SpectatorThink");
	pr.SpectatorDisconnect = (func_t)QC_FindFunction (pr.vm, "SpectatorDisconnect");
	pr.ParseClientCommand = (func_t)QC_FindFunction (pr.vm, "SV_ParseClientCommand");
}


/*
===============
PR_Init
===============
*/
void PR_Init (void)
{
	size_t	i;

	// FTE's builtins, and the engine's over them
	pr.builtins = QC_BuiltinsStandard (QC_NUMBERING_SSQC);
	if (!pr.builtins)
		Sys_Error ("PR_Init: out of memory");
	PR_InitBuiltins (pr.builtins);
	Cvar_AddChangeHook (PR_CvarChanged);
	Cvar_RegisterVariable (&sv_progs);
	Cvar_RegisterVariable (&pr_checkextension);
	for (i = 0 ; i < sizeof(pr_sparecvars) / sizeof(pr_sparecvars[0]) ; i++)
		Cvar_RegisterVariable (&pr_sparecvars[i]);

	Cmd_AddCommand ("edict", ED_PrintEdict_f, "Prints the fields of an entity of the running map. "
		"Usage: edict <number>");
	Cmd_AddCommand ("edicts", ED_PrintEdicts, "Prints the fields of every entity of the running map.");
	Cmd_AddCommand ("edictcount", ED_Count, "Counts the running map's entities: in use, with a model, solid, "
		"and stepping (MOVETYPE_STEP).");
	Cmd_AddCommand ("edictdigest", ED_Digest_f, "Prints a hash of each entity's fields and of the globals, "
		"to compare what two builds made of a map.");
	Cmd_AddCommand ("profile", PR_Profile_f, "Starts counting the statements each QuakeC function runs; "
		"after that, lists the ten that ran the most and starts the counts over.");
	Cmd_AddCommand ("pr_builtins", PR_Builtins_f, "Lists the builtins the running progs calls that the server "
		"lacks; with all, those it declares. Usage: pr_builtins [all]");
}
