// sv_save.c -- savegames: a single player game of NetQuake's progs as
// NetQuake's version 5 .sav, the file ironwail, QuakeSpasm and FTE write and
// read. The edicts are numbered as this server has them, the client slots
// 1 .. MAX_CLIENTS and the map's after them, the slots but the player's
// written empty, which the others load as free edicts; a comment block at
// the end, which they skip, says so, and keeps the precaches.

#include <time.h>

#include "sv_local.h"

#define	SAVEGAME_VERSION	5
#define	SAVEGAME_COMMENT	39		// NetQuake's comment, FTE's date after it

/*
==============================================================================

SAVING

==============================================================================
*/

/*
==================
SV_CanSave

Why the game can't be saved now, NULL if it can: a single player game of
NetQuake's progs, this program's player alone in it, alive and not in an
intermission (NetQuake's messages)
==================
*/
const char *SV_CanSave (void)
{
	client_t	*cl;
	int			i;

	if (sv.state != ss_active || svs.attract || host.dedicated)
		return "Not playing a local game.";
	if (!pr.nq || SV_Deathmatch () || coop.value)
		return "Can't save multiplayer games.";
	for (i = 1, cl = svs.clients + 1 ; i < MAX_CLIENTS ; i++, cl++)
		if (cl->state != cs_free)
			return "Can't save multiplayer games.";
	cl = svs.clients;
	if (cl->state != cs_spawned || cl->spectator || cl->netchan.remote_address.type != NA_LOOPBACK)
		return "Not playing a local game.";
	if (sv.intermission)
		return "Can't save in intermission.";
	if (cl->edict->v.health <= 0)
		return "Can't savegame with a dead player";
	return NULL;
}

/*
==================
SV_SaveName

A savegame's name in the game directory, with .sav unless it has it, as
ironwail adds it; false, and why, for a name outside the directory
==================
*/
static bool SV_SaveName (const char *arg, char *name, size_t size)
{
	size_t	l;

	if (strstr (arg, ".."))
	{
		Con_Printf ("Relative pathnames are not allowed.\n");
		return false;
	}
	if (arg[0] == '/' || arg[0] == '\\' || strchr (arg, ':'))
	{
		Con_Printf ("Absolute pathnames are not allowed.\n");
		return false;
	}
	Q_strncpyz (name, arg, size);
	l = strlen (name);
	if (l < 4 || Q_strcasecmp (name + l - 4, ".sav"))
		Q_strncatz (name, ".sav", size);
	return true;
}

/*
==================
SV_SaveComment

NetQuake's 39 characters, the level's name (its message, else the map's)
and its kills, then FTE's date; each space an underscore, the comment being
read as a word
==================
*/
static void SV_SaveComment (char *text, size_t size)
{
	const char	*title = PR_GetString (sv.edicts->v.message);
	char		kills[32];
	time_t		now = time (NULL);
	struct tm	*local = localtime (&now);
	size_t		i, l;

	if (!*title)
		title = sv.name;
	memset (text, ' ', SAVEGAME_COMMENT);
	for (i = 0 ; i < 22 && title[i] ; i++)
		text[i] = title[i];
	snprintf (kills, sizeof(kills), "kills:%3i/%3i", (int)PR_GLOBAL(killed_monsters), (int)PR_GLOBAL(total_monsters));
	l = strlen (kills);
	memcpy (text + 22, kills, l < SAVEGAME_COMMENT - 22 ? l : SAVEGAME_COMMENT - 22);
	text[SAVEGAME_COMMENT] = 0;
	if (local)
		strftime (text + SAVEGAME_COMMENT, size - SAVEGAME_COMMENT, "%Y-%m-%d %H:%M:%S", local);
	for (i = 0 ; text[i] ; i++)
		if ((byte)text[i] <= ' ')
			text[i] = '_';
}

/*
==================
SV_WriteSaveExtensions

After the edicts, in a comment NetQuake's loaders skip: the client slots the
edicts are numbered with (NetQuake has one), and the precaches as
DarkPlaces, QuakeSpasm-Spiked and FTE add them, quoted, those QuakeC made
during the level too
==================
*/
static void SV_WriteSaveExtensions (FILE *f)
{
	int		i;

	fprintf (f, "/*\n// SoftWorld extended savegame: %i client slots\n", MAX_CLIENTS);
	for (i = 1 ; i < MAX_MODELS && sv.model_precache[i] ; i++)
		fprintf (f, "sv.model_precache %i \"%s\"\n", i, sv.model_precache[i]);
	for (i = 1 ; i < MAX_SOUNDS && sv.sound_precache[i] ; i++)
		fprintf (f, "sv.sound_precache %i \"%s\"\n", i, sv.sound_precache[i]);
	fprintf (f, "*/\n");
}

/*
==================
SV_Savegame_f

save <name> [0]: the game to <name>.sav in the game directory, 0 saying
nothing of it (ironwail's, for autosaves)
==================
*/
static void SV_Savegame_f (void)
{
	char		name[MAX_OSPATH], path[MAX_OSPATH * 2], comment[80];
	const char	*why;
	bool		quiet = Cmd_Argc () > 2 && !strcmp (Cmd_Argv (2), "0"), failed;
	FILE		*f;
	int			i;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("save <savename> : save a game\n");
		return;
	}
	if ((why = SV_CanSave ()))
	{
		Con_Printf ("%s\n", why);
		return;
	}
	if (!SV_SaveName (Cmd_Argv (1), name, sizeof(name)))
		return;
	snprintf (path, sizeof(path), "%s/%s", com_gamedir, name);
	if (!quiet)
		Con_Printf ("Saving game to %s...\n", path);
	COM_CreatePath (path);
	f = fopen (path, "wb");
	if (!f)
	{
		Con_Printf ("ERROR: couldn't open %s.\n", path);
		return;
	}

	SV_SaveComment (comment, sizeof(comment));
	fprintf (f, "%i\n%s\n", SAVEGAME_VERSION, comment);
	for (i = 0 ; i < NUM_SPAWN_PARMS ; i++)
		fprintf (f, "%f\n", svs.clients[0].spawn_parms[i]);
	fprintf (f, "%i\n%s\n%f\n", sv.skill, sv.name, sv.time);
	for (i = 0 ; i < MAX_LIGHTSTYLES ; i++)
		fprintf (f, "%s\n", sv.lightstyles[i] && *sv.lightstyles[i] ? sv.lightstyles[i] : "m");
	ED_WriteGlobals (f);
	for (i = 0 ; i < sv.num_edicts ; i++)
	{
		if (i > 1 && i <= MAX_CLIENTS)
			fprintf (f, "{ // #%i\n}\n", i);	// a player's slot, empty in a game of one
		else
			ED_Write (f, EDICT_NUM (i), i);
	}
	SV_WriteSaveExtensions (f);
	fprintf (f, "// %i edicts\n", sv.num_edicts);

	failed = ferror (f) != 0;
	if (fclose (f) || failed)
	{
		Con_Printf ("ERROR: couldn't write %s.\n", path);
		remove (path);
	}
}

/*
==================
SV_CompleteSave

The savegames under the search path, and the directories on the way
==================
*/
static void SV_CompleteSave (const char *partial, void (*add) (void *ctx, const char *candidate), void *ctx)
{
	static const char *const	extensions[] = {".sav", NULL};

	FS_ListPaths (partial, extensions, add, ctx);
}

void SV_InitSave (void)
{
	Cmd_AddCommand ("save", SV_Savegame_f, "Saves a single player game of NetQuake's progs to <name>.sav in the "
		"game directory, as ironwail and FTE save them; 0 after the name says nothing of it. "
		"Usage: save <name> [0]");
	Cmd_SetCompletion ("save", SV_CompleteSave);
}
