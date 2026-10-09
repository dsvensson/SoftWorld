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

	if (sv.state != ss_active || svs.attract || host.dedicated || sv.loadgame)
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
==============================================================================

LOADING

==============================================================================
*/

/*
==================
SV_ParseSaveHeader

What comes before the globals, as NetQuake's loaders read it; false (said
why) for a file that isn't a version 5 savegame
==================
*/
static bool SV_ParseSaveHeader (char *data, sv_loadgame_t *load)
{
	int		i, version;

	data = COM_Parse (data);
	version = Q_atoi (com_token);
	if (version != SAVEGAME_VERSION)
	{
		Con_Printf ("Savegame is version %i, not %i.\n", version, SAVEGAME_VERSION);
		return false;
	}
	data = COM_Parse (data);	// the comment
	for (i = 0 ; i < NUM_SPAWN_PARMS ; i++)
	{
		data = COM_Parse (data);
		load->parms[i] = Q_atof (com_token);
	}
	data = COM_Parse (data);
	load->skill = (int)(Q_atof (com_token) + 0.1f);		// 1.06's were floats
	data = COM_Parse (data);
	Q_strncpyz (load->map, com_token, sizeof(load->map));
	data = COM_Parse (data);
	load->time = Q_atof (com_token);
	for (i = 0 ; i < MAX_LIGHTSTYLES ; i++)
	{
		data = COM_Parse (data);
		Q_strncpyz (load->lightstyles[i], com_token, sizeof(load->lightstyles[i]));
	}
	if (!data)
	{
		Con_Printf ("%s ends before its globals.\n", load->name);
		return false;
	}
	load->data = data;
	return true;
}

/*
==================
SV_FindSaveExtensions

The comment block after the edicts, and the client slots its first line
says the edicts are numbered with; NetQuake's one without it
==================
*/
static void SV_FindSaveExtensions (sv_loadgame_t *load)
{
	static const char	mark[] = "// SoftWorld extended savegame:";
	char				*p;

	load->slots = 1;
	load->extensions = NULL;
	for (p = strstr (load->data, "/*") ; p ; p = strstr (p + 2, "/*"))
		if (p[-1] == '\n')
			break;
	if (!p)
		return;
	load->extensions = p + 2;
	p = strstr (p, mark);
	if (p && sscanf (p + sizeof(mark) - 1, "%d", &load->slots) == 1)
		if (load->slots < 1 || load->slots > MAX_CLIENTS)
			load->slots = 1;
}

// a precache of the save's: one the spawn made again is there, one QuakeC
// made later during the level is added in its place
static void SV_RestorePrecache (char **list, int max, const char *kind, int index, const char *name)
{
	if (index <= 0 || index >= max - 1)	// the last stays NULL, ending the list
		return;
	if (list[index])
	{
		if (strcmp (list[index], name))
			Con_Printf ("WARNING: the savegame's %s %i is %s, the level's %s\n", kind, index, name, list[index]);
		return;
	}
	if (list[index - 1])
		list[index] = SV_LevelString (name);
	else
		Con_Printf ("WARNING: the savegame's %s %i, %s, comes after a gap\n", kind, index, name);
}

// the precaches of the comment block, line by line (other engines' lines skipped)
static void SV_RestorePrecaches (const sv_loadgame_t *load)
{
	char	line[1024], *p = load->extensions, *end, *word;
	char	key[64];
	int		index, i, models = 0;
	size_t	l;

	while (p && *p)
	{
		end = strchr (p, '\n');
		l = end ? (size_t)(end - p) : strlen (p);
		if (l >= sizeof(line))
			l = sizeof(line) - 1;
		memcpy (line, p, l);
		line[l] = 0;
		p = end ? end + 1 : NULL;
		if (strstr (line, "*/"))
			break;

		word = COM_Parse (line);
		if (!word)
			continue;
		Q_strncpyz (key, com_token, sizeof(key));
		word = COM_Parse (word);
		index = Q_atoi (com_token);
		if (!COM_Parse (word))
			continue;
		if (!strcmp (key, "sv.model_precache"))
		{
			i = sv.model_precache[index > 0 && index < MAX_MODELS ? index : 0] ? -1 : index;
			SV_RestorePrecache (sv.model_precache, MAX_MODELS, "model", index, com_token);
			if (i > 0 && sv.model_precache[i])
			{
				SV_LoadBrushModel (i);
				models++;
			}
		}
		else if (!strcmp (key, "sv.sound_precache"))
			SV_RestorePrecache (sv.sound_precache, MAX_SOUNDS, "sound", index, com_token);
	}
	if (models)
		SV_FindModelNumbers ();
}

/*
==================
SV_ApplySave

On the save's level, just spawned as map spawns it (its precaches, statics
and baselines): its precaches, lightstyles, globals and edicts as saved, the
time as it was, the world held still until the player is back
==================
*/
void SV_ApplySave (const sv_loadgame_t *load)
{
	char		*data = load->data, *peek;
	edict_t		*ent;
	client_t	*cl = svs.clients;
	int			pos, n, end = 0, i;

	if (load->extensions)
		SV_RestorePrecaches (load);
	for (i = 0 ; i < MAX_LIGHTSTYLES ; i++)
		PR_SetLightstyle (i, load->lightstyles[i]);

	data = COM_Parse (data);
	if (!data || com_token[0] != '{')
		SV_Error ("%s has no globals", load->name);
	data = ED_ParseGlobals (data, load->slots);

	// the edicts by their places in the file, past the save's client slots
	// after this server's
	QC_SetTime (pr.vm, sv.time);
	for (pos = 0 ; ; pos++)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			break;
		n = pos <= load->slots ? pos : pos - load->slots + MAX_CLIENTS;
		if (n >= MAX_EDICTS)
			SV_Error ("%s has more edicts than this server's %i", load->name, MAX_EDICTS);
		end = n;

		peek = COM_Parse (data);
		if (peek && com_token[0] == '}')
		{	// a free edict, or a player's slot of no one
			data = peek;
			if (n > MAX_CLIENTS && n < (int)QC_NumEdicts (pr.vm) && !QC_IsFree (pr.vm, (qc_ent_t)n))
				QC_Remove (pr.vm, (qc_ent_t)n, true);
			continue;
		}
		if (!QC_ClaimEdict (pr.vm, (qc_ent_t)n))
			SV_Error ("%s: no edict %i", load->name, n);
		ent = EDICT_NUM (n);
		if (n)
			SV_UnlinkEdict (ent);
		data = ED_ParseSavedEdict (data, ent, load->slots);
		if (n)
			SV_LinkEdict (ent, false);
	}
	// what the spawn made past the save's edicts goes
	for (n = end + 1 > MAX_CLIENTS ? end + 1 : MAX_CLIENTS + 1 ; n < (int)QC_NumEdicts (pr.vm) ; n++)
		if (!QC_IsFree (pr.vm, (qc_ent_t)n))
			QC_Remove (pr.vm, (qc_ent_t)n, true);

	sv.time = sv.physicstime = load->time;
	QC_SetTime (pr.vm, sv.time);
	PR_GLOBAL(time) = (float)sv.time;
	svs.serverflags = (int)PR_GLOBAL(serverflags);

	// its player: the one there is, reconnecting, or the one to connect
	sv.loadgame = true;
	memcpy (sv.loadparms, load->parms, sizeof(sv.loadparms));
	if (cl->state != cs_free)
	{
		memcpy (cl->spawn_parms, load->parms, sizeof(cl->spawn_parms));
		cl->newparms = false;
	}
}

/*
==================
SV_LoadedPlayerBegins

The savegame's player back in their body, as NetQuake's spawn has it after
a load: neither ClientConnect nor PutClientInServer, but FTE's RestoreGame
(NEH_RESTOREGAME) if the progs have it, and the view turned as it was
==================
*/
void SV_LoadedPlayerBegins (client_t *cl)
{
	edict_t		*ent = cl->edict;
	qc_func_t	restore = QC_FindFunction (pr.vm, "RestoreGame");
	vec3_t		angles;

	sv.loadgame = false;
	if (restore)
	{
		PR_GLOBAL(time) = (float)sv.time;
		PR_GLOBAL(self) = EDICT_TO_PROG(ent);
		PR_ExecuteProgram ((func_t)restore);
	}
	VectorCopy (ent->v.angles, angles);
	VectorCopy (ent->v.v_angle, ent->v.angles);
	ent->v.angles[ROLL] = 0;
	SV_SendFixangle (cl, NULL);
	VectorCopy (angles, ent->v.angles);
}

/*
==================
SV_Loadgame_f

load <name>: the savegame's level as it was, the map spawned as map spawns
it, then its globals and edicts as saved; a single player game, as New
Game starts one, with the save's skill
==================
*/
static void SV_Loadgame_f (void)
{
	sv_loadgame_t	load;
	client_t		*cl;
	byte			*file;
	int				i, size;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("load <savename> : load a game\n");
		return;
	}
	if (host.dedicated)
	{
		Con_Printf ("Not playing a local game.\n");
		return;
	}
	memset (&load, 0, sizeof(load));
	if (!SV_SaveName (Cmd_Argv (1), load.name, sizeof(load.name)))
		return;
	if (!(file = FS_LoadFile (load.name, &size)))
	{
		Con_Printf ("ERROR: %s not found.\n", load.name);
		return;
	}
	if (!SV_ParseSaveHeader ((char *)file, &load))
	{
		Mem_Free (file);
		return;
	}
	SV_FindSaveExtensions (&load);

	Con_Printf ("Loading game from %s...\n", load.name);
	SV_AttractStop ();
	// the save's player is the first slot's: anyone else goes
	for (i = 0, cl = svs.clients ; i < MAX_CLIENTS ; i++, cl++)
		if (cl->state >= cs_connected && (i || cl->netchan.remote_address.type != NA_LOOPBACK))
			SV_DropClient (cl);
	Cvar_Set ("maxclients", "1");
	Cvar_Set ("deathmatch", "0");
	Cvar_Set ("coop", "0");
	Cvar_SetValue ("skill", (float)load.skill);
	SV_GotoLevel (load.map, SPAWNPARMS_NEW, NULL, &load);
	Mem_Free (file);
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
	Cmd_AddCommand ("load", SV_Loadgame_f, "Loads a savegame from <name>.sav, the game directory's or the base's "
		"(ironwail's, QuakeSpasm's and FTE's too): its level as it was, a single player game with its skill. "
		"Usage: load <name>");
	Cmd_SetCompletion ("load", SV_CompleteSave);
}
