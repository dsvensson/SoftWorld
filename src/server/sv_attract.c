// sv_attract.c -- a showcase's levels (the client's attract mode): a map run
// with its game directory's progs for no one but this program's client,
// which watches as a spectator held where the showcase looks from. The
// server is closed to the network, and nothing of the user's changes: the
// rules are the showcase's, the cvars its progs set are set back, what they
// would run or stuff (localcmd, changelevel, stuffcmd) isn't, and what the
// server says goes to the debug output only.

#include "sv_local.h"

#define	MAX_ATTRACT_CVARS	32

static struct
{
	char		map[MAX_QPATH];
	char		progs[MAX_QPATH];
	fs_chain_t	*progsdir;		// held for the level
	char		deathmatch[8];
	vec3_t		origin, angles;
	void		(*stop) (void);	// SV_SetAttractStop's
	bool		quiet;			// SV_AttractQuiet's

	// the cvars the progs set, as they were
	struct
	{
		cvar_t	*var;
		char	*value;
	} saved[MAX_ATTRACT_CVARS];
	int			numsaved;
} attract;

bool SV_Attracting (void)
{
	return svs.attract;
}

cmap_t *SV_ShareMap (const char *name, unsigned *checksum2)
{
	if (sv.state != ss_active || !sv.map || strcmp (sv.modelname, name))
		return NULL;
	return CM_ShareMap (sv.map, NULL, checksum2);
}

void SV_SetAttractStop (void (*stop) (void))
{
	attract.stop = stop;
}

/*
==================
SV_AttractStop

Before the user's own level (map, changelevel, restart, killserver, gamedir):
the showcase goes first, the client's part of it with it
==================
*/
void SV_AttractStop (void)
{
	if (svs.attract && attract.stop)
		attract.stop ();
	SV_AttractEnd ();
}

// the cvars the progs set back as they were
static void SV_AttractRestoreCvars (void)
{
	int		i;

	for (i = attract.numsaved - 1 ; i >= 0 ; i--)
	{
		Cvar_Set (attract.saved[i].var->name, attract.saved[i].value);
		Mem_Free (attract.saved[i].value);
	}
	attract.numsaved = 0;
}

/*
==================
SV_AttractQuiet

What the server says, said only to the debug output (the console with
developer 1): the progs' warnings and the joins of a showcase are no one's
business. While the server runs a frame or spawns a level; a redirect of
its own (rcon's, a client's command) comes back to it.
==================
*/
static void SV_AttractPrint (const char *msg)
{
	Con_SetPrintRedirect (NULL);
	if (developer.value)
		Con_Printf ("%s", msg);
	else
		Sys_Printf ("%s", msg);
	Con_SetPrintRedirect (SV_AttractPrint);
}

void SV_AttractQuiet (bool quiet)
{
	quiet = quiet && svs.attract;
	if (quiet == attract.quiet)
		return;
	attract.quiet = quiet;
	Con_SetPrintRedirect (quiet ? SV_AttractPrint : NULL);
}

void SV_AttractRedirectEnded (void)
{
	if (attract.quiet)
		Con_SetPrintRedirect (SV_AttractPrint);
}

/*
==================
SV_AttractLevel

The showcase on a level; false if it couldn't be spawned (said why)
==================
*/
bool SV_AttractLevel (const sv_attract_t *level)
{
	SV_AttractRestoreCvars ();
	if (attract.progsdir)
		FS_ReleaseChain (attract.progsdir);

	Q_strncpyz (attract.map, level->map, sizeof(attract.map));
	Q_strncpyz (attract.progs, level->progs, sizeof(attract.progs));
	attract.progsdir = level->progsdir;
	snprintf (attract.deathmatch, sizeof(attract.deathmatch), "%g", level->deathmatch);
	VectorCopy (level->origin, attract.origin);
	VectorCopy (level->angles, attract.angles);
	svs.attract = true;

	SV_AttractQuiet (true);
	SV_GotoLevel (attract.map, SPAWNPARMS_NEW, level->built, NULL);
	SV_AttractQuiet (false);
	return sv.state == ss_active && !strcmp (sv.name, attract.map);
}

/*
==================
SV_AttractEnd

The showcase's server goes; the user's cvars as they were
==================
*/
void SV_AttractEnd (void)
{
	if (!svs.attract)
		return;
	SV_AttractQuiet (true);
	SV_Kill ();
	SV_AttractQuiet (false);
	svs.attract = false;
	SV_AttractRestoreCvars ();
	if (attract.progsdir)
		FS_ReleaseChain (attract.progsdir);
	attract.progsdir = NULL;
}

/*
==================
SV_AttractProgs

The showcase's progs: their name and the game directory alone they are read
from; false out of one
==================
*/
bool SV_AttractProgs (char *name, size_t size, fs_chain_t **dir)
{
	if (!svs.attract)
		return false;
	Q_strncpyz (name, attract.progs, size);
	*dir = attract.progsdir;
	return true;
}

/*
==================
SV_AttractCvar

The rules of the showcase, as its progs read them: NetQuake's progs single
player's (deathmatch and coop 0), QuakeWorld's deathmatch
==================
*/
const char *SV_AttractCvar (const char *name)
{
	if (!svs.attract)
		return NULL;
	if (!Q_strcasecmp (name, "deathmatch"))
		return attract.deathmatch;
	if (!Q_strcasecmp (name, "coop"))
		return "0";
	return NULL;
}

/*
==================
SV_AttractCvarSet

A cvar the showcase's progs set, set back when it ends
==================
*/
void SV_AttractCvarSet (const char *name, const char *value)
{
	cvar_t	*var = Cvar_FindVar ((char *)name);
	int		i;

	if (!var)
		return;
	for (i = 0 ; i < attract.numsaved && attract.saved[i].var != var ; i++)
		;
	if (i == attract.numsaved)
	{
		if (attract.numsaved == MAX_ATTRACT_CVARS)
			return;		// left alone
		attract.saved[i].var = var;
		attract.saved[i].value = Mem_Alloc (strlen (var->string) + 1);
		strcpy (attract.saved[i].value, var->string);
		attract.numsaved++;
	}
	Cvar_Set (var->name, (char *)value);
}

/*
==================
SV_AttractPlace

The client watching, where the showcase looks from: still whatever its
moves, out of the world's way, its view the PVS's (no eye height)
==================
*/
void SV_AttractPlace (client_t *cl)
{
	edict_t	*ent = cl->edict;

	VectorCopy (attract.origin, ent->v.origin);
	VectorCopy (attract.angles, ent->v.angles);
	VectorCopy (attract.angles, ent->v.v_angle);
	VectorCopy (vec3_origin, ent->v.view_ofs);
	VectorCopy (vec3_origin, ent->v.velocity);
	ent->v.movetype = MOVETYPE_NONE;
	ent->v.solid = SOLID_NOT;
	ent->v.flags = (float)((int)ent->v.flags | FL_NOTARGET);
	ent->v.modelindex = 0;
}
