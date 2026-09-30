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

static void PR_Localcmd (void *ctx, const char *text)
{
	(void)ctx;
	Cbuf_AddText ((char *)text);
}

static float PR_CvarFloat (void *ctx, const char *name)
{
	(void)ctx;
	return Cvar_VariableValue ((char *)name);
}

static const char *PR_CvarString (void *ctx, const char *name)
{
	cvar_t	*var = Cvar_FindVar ((char *)name);

	(void)ctx;
	return var ? var->string : NULL;
}

static void PR_CvarSet (void *ctx, const char *name, const char *value)
{
	(void)ctx;
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

static const qc_host_t	pr_host = {
	.warning = PR_Warning,
	.print = PR_Print,
	.dprint = PR_Print,
	.localcmd = PR_Localcmd,
	.dump = PR_Dump,
	.cvar_float = PR_CvarFloat,
	.cvar_string = PR_CvarString,
	.cvar_set = PR_CvarSet,
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

edict_t *EDICT_NUM (int n)
{
	if (n < 0 || n >= MAX_EDICTS)
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
		for (i = 0 ; QC_ProgsFieldDefAt (pr.progs, i, &d) ; i++)
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
	for (i = 1 ; QC_ProgsFieldDefAt (pr.progs, i, &d) ; i++)
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
		for (j = 1 ; QC_ProgsFieldDefAt (pr.progs, j, &d) ; j++)
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


/*
=============
ED_ParseEpair

A field's value from the map's text; false if it can't be parsed
=============
*/
static bool ED_ParseEpair (edict_t *ent, const qc_definfo_t *key, char *s)
{
	int			i, n;
	char		string[128];
	uint32_t	ofs;
	char		*v, *w;
	qc_func_t	func;
	int			*d;

	if (!ED_FieldValue (ent, key))
		return false;
	d = (int *)((char *)&ent->v + (size_t)key->ofs * 4);

	switch (key->type)
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
		if (n < 0 || n >= MAX_EDICTS)
			SV_Error ("EDICT_NUM: bad number %i", n);
		*d = n;
		break;
		
	case ev_field:
		if (!QC_FindField (pr.vm, s, &ofs, NULL))
		{
			Con_Printf ("Can't find field %s\n", s);
			return false;
		}
		// as id had it: the global at the field's offset
		*d = ofs < QC_ProgsNumGlobals (pr.progs) ? ((int *)pr.globals)[ofs] : 0;
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
				Con_Printf ("%s is not a field\n", keyname);
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
	pr.global_struct->time = (float)sv.time;

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
		if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH))
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

		pr.global_struct->self = EDICT_TO_PROG(ent);
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
	pr.global_struct = NULL;
	pr.globals = NULL;
}

// an optional field of a type: its offset, or 0 without
static int PR_OptionalField (const char *name, uint32_t want)
{
	uint32_t	ofs, type;

	return QC_FindField (pr.vm, name, &ofs, &type) && type == want ? (int)ofs : 0;
}

/*
===============
PR_LoadProgs

The game directory's qwprogs.dat, else the one the program was built with (a
progs.dat would be NetQuake's, which id1 always has), in a new VM
===============
*/
void PR_LoadProgs (void)
{
	byte				*data, *lno;
	int					size, lnosize;
	char				num[32], text[1024];
	qc_loaderror_t		lerr;
	qc_error_t			err;
	qc_config_t			config;
	const qc_loadnote_t	*notes;
	uint32_t			count, n;

	PR_FreeProgs ();
	PR_ClearLightstyles ();

	data = FS_LoadFile ("qwprogs.dat", NULL);
	size = com_filesize;
	if (!data)
	{
		size = (int)sv_qwprogs_size;
		data = Mem_Alloc (sv_qwprogs_size);
		memcpy (data, sv_qwprogs, sv_qwprogs_size);
		Con_DPrintf ("qwprogs.dat: the one built in\n");
	}
	Con_DPrintf ("Programs occupy %iK.\n", size/1024);

// add prog crc to the serverinfo
	snprintf (num, sizeof(num), "%i", CRC_Block (data, size));
	Info_SetValueForStarKey (svs.info, "*progs", num, MAX_SERVERINFO_STRING, SV_InfoCharset ());

	pr.progs = QC_LoadProgs (data, (size_t)size, &lerr);
	Mem_Free (data);
	if (!pr.progs)
		SV_Error ("qwprogs.dat: %s", QC_LoadErrorText (&lerr, text, sizeof(text)));
	notes = QC_ProgsNotes (pr.progs, &count);
	for (n = 0 ; n < count ; n++)
		Con_DPrintf ("qwprogs.dat: %s\n", QC_LoadNoteText (&notes[n], text, sizeof(text)));

	// source lines for the backtraces, when the compiler wrote them
	lno = FS_LoadFile ("qwprogs.lno", NULL);
	lnosize = com_filesize;
	if (lno)
	{
		if (!QC_AttachLineNumbers (pr.progs, lno, (size_t)lnosize, &lerr))
			Con_DPrintf ("qwprogs.lno: %s\n", QC_LoadErrorText (&lerr, text, sizeof(text)));
		Mem_Free (lno);
	}

	if (QC_ProgsCRC (pr.progs) != PROGHEADER_CRC)
		SV_Error ("You must have the progs.dat from QuakeWorld installed");
	if ((size_t)QC_ProgsEntityFields (pr.progs) * 4 < sizeof(entvars_t)
		|| (size_t)QC_ProgsNumGlobals (pr.progs) * 4 < sizeof(globalvars_t))
		SV_Error ("qwprogs.dat has fewer fields or globals than QuakeWorld's");

	QC_DefaultConfig (&config, QC_SSQC);
	config.limits.max_edicts = MAX_EDICTS;
	config.first_spawnable = MAX_CLIENTS + 1;		// the clients' are the server's
	config.entity_header_bytes = offsetof (edict_t, v);
	config.remove_clears = NULL;		// PR_OnRemove clears id's
	config.developer = developer.value != 0;
	pr.vm = QC_Create (pr.progs, pr.builtins, &config, &pr_host, NULL, &err);
	if (!pr.vm)
	{
		QC_ErrorText (&err, text, sizeof(text));
		QC_FreeError (&err);
		SV_Error ("qwprogs.dat: %s", text);
	}
	// the server reaches any entity below MAX_EDICTS
	if (!QC_CommitEdicts (pr.vm, MAX_EDICTS))
		SV_Error ("PR_LoadProgs: no memory for %i entities", MAX_EDICTS);
	if (pr_profiling)
		QC_SetProfiling (pr.vm, true);
	if (!QC_SyncAutocvars (pr.vm))
		SV_Error ("qwprogs.dat: %s", QC_ErrorText (QC_LastError (pr.vm), text, sizeof(text)));

	pr.global_struct = (globalvars_t *)QC_Globals (pr.vm);
	pr.globals = (float *)pr.global_struct;
	pr.edict_size = 1 << QC_EdictShift (pr.vm);

	pr.fofs_alpha = PR_OptionalField ("alpha", ev_float);
	pr.fofs_colormod = PR_OptionalField ("colormod", ev_vector);
	pr.fofs_gravity = PR_OptionalField ("gravity", ev_float);
	pr.fofs_maxspeed = PR_OptionalField ("maxspeed", ev_float);
	pr.fofs_teleported = PR_OptionalField ("teleported", ev_float);
	if (!pr.fofs_teleported)
		pr.fofs_teleported = PR_OptionalField ("teleported", QC_EV_INTEGER);
	pr.fofs_teleport_time = PR_OptionalField ("teleport_time", ev_float);

	// Zoid, find the spectator functions
	pr.SpectatorConnect = (func_t)QC_FindFunction (pr.vm, "SpectatorConnect");
	pr.SpectatorThink = (func_t)QC_FindFunction (pr.vm, "SpectatorThink");
	pr.SpectatorDisconnect = (func_t)QC_FindFunction (pr.vm, "SpectatorDisconnect");
}


/*
===============
PR_Init
===============
*/
void PR_Init (void)
{
	// FTE's builtins, and the engine's over them
	pr.builtins = QC_BuiltinsStandard (QC_NUMBERING_SSQC);
	if (!pr.builtins)
		Sys_Error ("PR_Init: out of memory");
	PR_InitBuiltins (pr.builtins);
	Cvar_AddChangeHook (PR_CvarChanged);

	Cmd_AddCommand ("edict", ED_PrintEdict_f, "Prints the fields of an entity of the running map. "
		"Usage: edict <number>");
	Cmd_AddCommand ("edicts", ED_PrintEdicts, "Prints the fields of every entity of the running map.");
	Cmd_AddCommand ("edictcount", ED_Count, "Counts the running map's entities: in use, with a model, solid, "
		"and stepping (MOVETYPE_STEP).");
	Cmd_AddCommand ("edictdigest", ED_Digest_f, "Prints a hash of each entity's fields and of the globals, "
		"to compare what two builds made of a map.");
	Cmd_AddCommand ("profile", PR_Profile_f, "Starts counting the statements each QuakeC function runs; "
		"after that, lists the ten that ran the most and starts the counts over.");
}
