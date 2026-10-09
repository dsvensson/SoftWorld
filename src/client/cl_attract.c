// cl_attract.c -- attract mode: a showcase of maps while no game is on, as
// qualia has it. A map of cl_attract_gamedirs, picked from a shuffled queue,
// is seen from one of its info_intermission spots with the intermission's
// sway (V_CalcIntermissionRefdef), running on the local server with its game
// directory's progs (sv_attract.c); after cl_attract_time seconds it fades
// to black and the next, loaded meanwhile on the loader's thread (loader.h),
// fades in. There is no status bar: the map's file name and message are in
// the lower left. Escape opens the menu over it, the console key the
// console; the world plays on under both.

#include "cl_local.h"

static cvar_t	cl_attract = {.name = "cl_attract", .string = "1", .archive = true,
	.description = "Shows maps while no game is on, behind the menu: each from one of its intermission spots for "
		"cl_attract_time seconds, faded through black into the next.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "On"}, {0}}};
static cvar_t	cl_attract_gamedirs = {.name = "cl_attract_gamedirs", .string = "id1 qw", .archive = true,
	.description = "The game directories whose maps attract mode (cl_attract) shows, separated by spaces (in quotes); "
		"each map runs with its own directory's progs."};
static cvar_t	cl_attract_time = {.name = "cl_attract_time", .string = "12", .archive = true,
	.description = "Seconds attract mode (cl_attract) shows each map."};

#define	ATTRACT_FADE		0.5		// seconds to black, and from it
#define	ATTRACT_SIGNON		15		// seconds a level may take to come up
#define	ATTRACT_FAILURES	5		// in a row, and attract mode stops
#define	MAX_ATTRACT_SPOTS	64

typedef enum
{
	AT_OFF,
	AT_BLACK,		// black, until the next map is loaded
	AT_SIGNON,		// black, while its level comes up
	AT_FADEIN,
	AT_SHOWING,
	AT_FADEOUT
} attractstate_t;

typedef struct
{
	char	dir[MAX_QPATH];			// its game directory
	char	name[MAX_QPATH];		// without maps/ and .bsp
	bool	bad;					// couldn't be shown: passed by
} attractmap_t;

// a map loaded on the loader's thread
typedef struct
{
	loadjob_t	job;
	int			generation;			// attract.generation's when given
	int			map;
	char		dir[MAX_QPATH], name[MAX_QPATH];	// its own copies: a scan may move the maps
	uint64_t	seed;				// picks its spot
	fs_chain_t	*dirchain;			// its game directory's, the search path while it shows

	// what it found
	bool		failed;				// no info_intermission, or it couldn't be loaded
	vec3_t		origin, angles;		// the spot
	char		message[64];		// worldspawn's, its first line
	cmap_t		*cmap;				// CM_BuildMap's
	model_t		*world;				// Mod_LoadDetached's
} attractjob_t;

static struct
{
	attractstate_t	state;
	double			statetime;		// when it began (host.realtime)
	bool			ready;			// the startup's commands have run (the attract command)
	bool			hold;			// a game ended with an error: not until a key is pressed
	int				generation;		// bumped when it stops: what was loading for it is dropped
	uint64_t		rng;

	attractmap_t	*maps;
	int				nummaps;
	int				*queue;			// shuffled indices into maps
	int				queuepos;
	int				last;			// shown last, never first of the next shuffle

	attractjob_t	*loading;		// on the loader's thread
	attractjob_t	*next;			// loaded, to show
	model_t			*world;			// the level coming up's, for Model_NextDownload

	int				current;		// the map shown (maps), -1 none
	vec3_t			origin, angles;	// its spot
	char			file[MAX_QPATH], message[64];
	int				activeseq;		// the packet the level came up at
	int				failures;		// in a row
} attract = {.current = -1, .last = -1};

/*
===============================================================================

THE MAPS

===============================================================================
*/

// SplitMix64: the client's own, so rand () and the demos' runs are left alone
static uint64_t CL_AttractRand (void)
{
	uint64_t	z = (attract.rng += 0x9e3779b97f4a7c15ull);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

typedef struct
{
	const char	*dir;
} attractscan_t;

static void CL_AttractAddMap (void *ctx, const char *path)
{
	const attractscan_t	*scan = ctx;
	attractmap_t		*m;
	size_t				len = strlen (path);
	int					i;

	// maps/<name>.bsp: not a directory, nor a brush model's file (b_*)
	if (len <= 9 || Q_strncasecmp (path, "maps/", 5) || strcmp (path + len - 4, ".bsp") || strchr (path + 5, '/')
		|| !Q_strncasecmp (path + 5, "b_", 2) || len - 9 >= MAX_QPATH)
		return;
	for (i = 0 ; i < attract.nummaps ; i++)
		if (!strcmp (attract.maps[i].dir, scan->dir) && !strncmp (attract.maps[i].name, path + 5, len - 9)
			&& !attract.maps[i].name[len - 9])
			return;		// loose and in a pak too

	attract.maps = Mem_Realloc (attract.maps, (size_t)(attract.nummaps + 1) * sizeof(*attract.maps));
	m = &attract.maps[attract.nummaps++];
	memset (m, 0, sizeof(*m));
	Q_strncpyz (m->dir, scan->dir, sizeof(m->dir));
	memcpy (m->name, path + 5, len - 9);
}

// a map in id1 and in qw both is qw's, the one the search path finds
static void CL_AttractDropShadowed (void)
{
	int		i, j, n = 0;
	bool	shadowed;

	for (i = 0 ; i < attract.nummaps ; i++)
	{
		shadowed = false;
		if (!strcmp (attract.maps[i].dir, "id1"))
			for (j = 0 ; j < attract.nummaps && !shadowed ; j++)
				shadowed = !strcmp (attract.maps[j].dir, "qw") && !strcmp (attract.maps[j].name, attract.maps[i].name);
		if (!shadowed)
			attract.maps[n++] = attract.maps[i];
	}
	attract.nummaps = n;
}

// the maps of cl_attract_gamedirs; false if there are none
static bool CL_AttractScan (void)
{
	static const char *const	exts[] = {".bsp", NULL};
	char			dirs[256], *dir, *end;
	attractscan_t	scan;

	Mem_Free (attract.maps);
	Mem_Free (attract.queue);
	attract.maps = NULL;
	attract.queue = NULL;
	attract.nummaps = attract.queuepos = 0;
	attract.last = attract.current = -1;

	Q_strncpyz (dirs, cl_attract_gamedirs.string, sizeof(dirs));
	for (dir = dirs ; *dir ; dir = end)
	{
		while (*dir == ' ' || *dir == '\t')
			dir++;
		for (end = dir ; *end && *end != ' ' && *end != '\t' ; end++)
			;
		if (*end)
			*end++ = 0;
		if (!*dir || strstr (dir, "..") || strchr (dir, '/') || strchr (dir, '\\') || strchr (dir, ':'))
			continue;
		scan.dir = dir;
		FS_ListDirFiles (dir, "maps/", exts, CL_AttractAddMap, &scan);
	}
	CL_AttractDropShadowed ();
	attract.queue = Mem_Alloc ((size_t)(attract.nummaps ? attract.nummaps : 1) * sizeof(*attract.queue));
	attract.queuepos = attract.nummaps;		// shuffled at the first pick
	return attract.nummaps > 0;
}

// the next map of the queue to load, shuffled again when it runs out; -1 if
// every one has failed
static int CL_AttractPick (void)
{
	int		i, j, t, tries;

	for (tries = 0 ; tries <= attract.nummaps ; )
	{
		if (attract.queuepos == attract.nummaps)
		{
			for (i = 0 ; i < attract.nummaps ; i++)
				attract.queue[i] = i;
			for (i = attract.nummaps - 1 ; i > 0 ; i--)
			{
				j = (int)(CL_AttractRand () % (uint64_t)(i + 1));
				t = attract.queue[i];
				attract.queue[i] = attract.queue[j];
				attract.queue[j] = t;
			}
			if (attract.nummaps > 1 && attract.queue[0] == attract.last)
			{
				attract.queue[0] = attract.queue[attract.nummaps - 1];
				attract.queue[attract.nummaps - 1] = attract.last;
			}
			attract.queuepos = 0;
		}
		i = attract.queue[attract.queuepos++];
		if (!attract.maps[i].bad)
			return i;
		tries++;
	}
	return -1;
}

/*
===============================================================================

LOADING

===============================================================================
*/

// v as three numbers ("pitch yaw roll"); false if it isn't
static bool CL_AttractVector (const char *s, vec3_t v)
{
	return sscanf (s, "%f %f %f", &v[0], &v[1], &v[2]) == 3;
}

/*
=================
CL_AttractSpots

The map's entities, read alone from its file (which needn't be read whole
for a map that has none): worldspawn's message, and an info_intermission
picked by the seed. False if it has none.
=================
*/
static bool CL_AttractSpots (attractjob_t *j, const char *path)
{
	FILE		*f;
	dheader_t	header;
	int			len, ofs, size, count = 0, entity = 0, i;
	char		*text, *data, key[64], classname[64], *nl;
	vec3_t		origin, mangle, angles, spots[MAX_ATTRACT_SPOTS][2];
	bool		hasorigin, hasmangle, hasangles;
	float		angle;

	len = COM_FOpenFile (path, &f);
	if (!f)
		return false;
	if (len < (int)sizeof(header) || fread (&header, 1, sizeof(header), f) != sizeof(header))
	{
		fclose (f);
		return false;
	}
	ofs = LittleLong (header.lumps[LUMP_ENTITIES].fileofs);
	size = LittleLong (header.lumps[LUMP_ENTITIES].filelen);
	if (ofs < (int)sizeof(header) || size <= 0 || size > len - ofs)
	{
		fclose (f);
		return false;
	}
	text = Mem_Alloc ((size_t)size + 1);
	fseek (f, ofs - (int)sizeof(header), SEEK_CUR);
	i = (int)fread (text, 1, (size_t)size, f);
	fclose (f);
	text[i > 0 ? i : 0] = 0;

	// each { "key" "value" ... }
	data = text;
	while ((data = COM_Parse (data)) && com_token[0] == '{')
	{
		classname[0] = 0;
		hasorigin = hasmangle = hasangles = false;
		angle = 0;
		while ((data = COM_Parse (data)) && com_token[0] != '}')
		{
			Q_strncpyz (key, com_token, sizeof(key));
			if (!(data = COM_Parse (data)))
				break;
			if (!strcmp (key, "classname"))
				Q_strncpyz (classname, com_token, sizeof(classname));
			else if (!strcmp (key, "origin"))
				hasorigin = CL_AttractVector (com_token, origin);
			else if (!strcmp (key, "mangle"))
				hasmangle = CL_AttractVector (com_token, mangle);
			else if (!strcmp (key, "angles"))
				hasangles = CL_AttractVector (com_token, angles);
			else if (!strcmp (key, "angle"))
				angle = (float)atof (com_token);
			else if (!strcmp (key, "message") && !entity)
				Q_strncpyz (j->message, com_token, sizeof(j->message));
		}
		if (!data)
			break;
		entity++;
		if (strcmp (classname, "info_intermission") || !hasorigin || count == MAX_ATTRACT_SPOTS)
			continue;
		VectorCopy (origin, spots[count][0]);
		if (hasmangle)
		{
			VectorCopy (mangle, spots[count][1]);
		}
		else if (hasangles)
		{
			VectorCopy (angles, spots[count][1]);
		}
		else
		{
			spots[count][1][0] = 0;
			spots[count][1][1] = angle;
		}
		spots[count][1][2] = 0;		// as the sway rolls it
		count++;
	}
	Mem_Free (text);

	// the message's first line: up to a newline, or a written "\n"
	for (nl = j->message ; *nl && *nl != '\n' && *nl != '\r' && !(nl[0] == '\\' && nl[1] == 'n') ; nl++)
		;
	*nl = 0;

	if (!count)
		return false;
	i = (int)(j->seed % (uint64_t)count);
	VectorCopy (spots[i][0], j->origin);
	VectorCopy (spots[i][1], j->angles);
	return true;
}

// on the loader's thread: the map's spot, then its collision map and model
static void CL_AttractLoad (loadjob_t *job)
{
	attractjob_t	*j = (attractjob_t *)job;
	char			path[MAX_QPATH];
	byte			*buf;
	int				size;

	j->failed = true;
	snprintf (path, sizeof(path), "maps/%s.bsp", j->name);
	if (!CL_AttractSpots (j, path))
	{
		Con_DPrintf ("Attract mode: %s/%s has no info_intermission\n", j->dir, path);
		return;
	}
	buf = FS_LoadFile (path, &size);
	if (!buf)
		return;
	j->cmap = CM_BuildMap (path, buf, size);
	if (j->cmap)
		j->world = Mod_LoadDetached (path, buf, size);
	Mem_Free (buf);
	j->failed = !j->cmap || !j->world;
}

static void CL_AttractFreeJob (attractjob_t *j)
{
	if (!j)
		return;
	if (j->cmap)
		CM_DiscardMap (j->cmap);
	if (j->world)
		Mod_FreeDetached (j->world);
	FS_ReleaseChain (j->dirchain);
	Mem_Free (j);
}

static void CL_AttractLoadNext (void);

// on the main thread: the map to show next, or another tried
static void CL_AttractLoaded (loadjob_t *job)
{
	attractjob_t	*j = (attractjob_t *)job;

	if (j->generation != attract.generation)
	{	// attract mode stopped since (the maps are another scan's)
		CL_AttractFreeJob (j);
		return;
	}
	attract.loading = NULL;
	if (j->failed)
	{
		attract.maps[j->map].bad = true;
		CL_AttractFreeJob (j);
		CL_AttractLoadNext ();
		return;
	}
	attract.next = j;
}

/*
=================
CL_AttractLoadNext

The next map loaded on the loader's thread; attract mode stops when every
map has failed
=================
*/
static void CL_AttractLoadNext (void)
{
	attractjob_t	*j;
	int				map;

	if (attract.loading || attract.next || attract.state == AT_OFF)
		return;
	map = CL_AttractPick ();
	if (map < 0)
	{
		Con_Printf ("Attract mode: no map of \"%s\" has an info_intermission to be seen from\n",
			cl_attract_gamedirs.string);
		CL_AttractStop ();
		return;
	}

	j = Mem_Calloc (1, sizeof(*j));
	j->job.run = CL_AttractLoad;
	j->job.finish = CL_AttractLoaded;
	j->generation = attract.generation;
	j->map = map;
	Q_strncpyz (j->dir, attract.maps[map].dir, sizeof(j->dir));
	Q_strncpyz (j->name, attract.maps[map].name, sizeof(j->name));
	j->seed = CL_AttractRand ();
	j->dirchain = FS_OpenDirChain (attract.maps[map].dir, false);
	j->job.chain = FS_RetainChain (j->dirchain);
	attract.loading = j;
	Load_Submit (&j->job, false);
}

/*
===============================================================================

SHOWING

===============================================================================
*/

static void CL_AttractSetState (attractstate_t state)
{
	attract.state = state;
	attract.statetime = host.realtime;
}

/*
=================
CL_AttractShow

At black: the loaded map's level on the server, with its game directory's
search path and progs; the client comes up on it (CL_AttractFrame)
=================
*/
static void CL_AttractShow (void)
{
	attractjob_t	*j = attract.next;
	attractmap_t	*m = &attract.maps[j->map];
	sv_attract_t	level = {0};
	fs_chain_t		*progsdir;
	FILE			*f = NULL;
	bool			nq;

	attract.next = NULL;
	FS_SetSearchChain (j->dirchain);

	// the directory's progs: id1's NetQuake's, qw's QuakeWorld's (the one
	// built in without its own), a mod's progs.dat if it has one
	progsdir = FS_OpenDirChain (m->dir, true);
	if (!strcmp (m->dir, "id1") || !strcmp (m->dir, "qw"))
		nq = !strcmp (m->dir, "id1");
	else
	{
		FS_UseChain (progsdir);
		COM_FOpenFile ("progs.dat", &f);
		FS_UseChain (NULL);
		nq = f != NULL;
		if (f)
			fclose (f);
	}

	level.map = m->name;
	level.progs = nq ? "progs.dat" : "qwprogs.dat";
	level.progsdir = progsdir;
	level.deathmatch = nq ? 0.0f : 1.0f;
	level.built = j->cmap;
	VectorCopy (j->origin, level.origin);
	VectorCopy (j->angles, level.angles);

	// what is shown, and the world for the client's signon
	attract.current = j->map;
	attract.last = j->map;
	VectorCopy (j->origin, attract.origin);
	VectorCopy (j->angles, attract.angles);
	Q_strncpyz (attract.file, m->name, sizeof(attract.file));
	Q_strncpyz (attract.message, j->message, sizeof(attract.message));
	if (attract.world)
		Mod_FreeDetached (attract.world);
	attract.world = j->world;
	j->world = NULL;
	j->cmap = NULL;		// the server's
	CL_AttractFreeJob (j);
	attract.activeseq = -1;
	CL_AttractSetState (AT_SIGNON);

	// last: an error in the spawn goes to CL_AttractError
	if (!SV_AttractLevel (&level))
		CL_AttractError ();
}

/*
=================
CL_AttractWorld

The level coming up's world model, loaded with the rest of the map; NULL if
the model asked for is another
=================
*/
model_t *CL_AttractWorld (const char *modelname)
{
	model_t	*world = attract.world;

	if (!world || strcmp (world->name, modelname))
		return NULL;
	attract.world = NULL;
	return Mod_Install (world);
}

// the client sending nothing of its own: the user's keys drive no one
void CL_AttractCmd (usercmd_t *cmd)
{
	cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;
	cmd->buttons = 0;
	cmd->impulse = 0;
}

/*
=================
CL_AttractFrame

Each frame: started when no game is on, and on from black to the level
coming up, its fade in, the time it shows and its fade out
=================
*/
void CL_AttractFrame (void)
{
	double	t = host.realtime - attract.statetime;
	float	black;

	if (attract.state == AT_OFF)
	{
		if (attract.ready && !attract.hold && cl_attract.value && CL_AttractIdle ())
			CL_AttractStart ();
		return;
	}

	switch (attract.state)
	{
	case AT_BLACK:
		if (attract.next)
			CL_AttractShow ();
		else
			CL_AttractLoadNext ();
		break;

	case AT_SIGNON:
		if (cls.state == ca_active && attract.activeseq < 0)
			attract.activeseq = cls.netchan.incoming_sequence;
		// a couple of the level's frames in, its entities are there
		if (cls.state == ca_active && cls.netchan.incoming_sequence >= attract.activeseq + 2)
		{
			attract.failures = 0;
			CL_AttractSetState (AT_FADEIN);
			if (Load_Threaded ())
				CL_AttractLoadNext ();
		}
		else if (t > ATTRACT_SIGNON)
		{
			Con_Printf ("Attract mode: maps/%s.bsp didn't come up\n", attract.file);
			CL_AttractError ();
		}
		break;

	case AT_FADEIN:
		if (t >= ATTRACT_FADE)
			CL_AttractSetState (AT_SHOWING);
		break;

	case AT_SHOWING:
		// the time up and the next loaded (where it loads at black, now)
		if (t >= cl_attract_time.value - ATTRACT_FADE && (attract.next || !Load_Threaded ()))
			CL_AttractSetState (AT_FADEOUT);
		break;

	case AT_FADEOUT:
		if (t >= ATTRACT_FADE)
			CL_AttractSetState (AT_BLACK);
		break;

	default:
		break;
	}

	// the view from the spot, as an intermission's, whenever the level is up
	// (a reconnect clears it); once it is, as prediction stops at it
	if (cls.state == ca_active && attract.state >= AT_FADEIN)
	{
		if (!cl.intermission)
			vid.recalc_refdef = true;
		cl.intermission = 1;
		VectorCopy (attract.origin, cl.simorg);
		VectorCopy (attract.angles, cl.simangles);
		VectorCopy (vec3_origin, cl.simvel);
	}

	// the sound fades with the picture
	black = CL_AttractBlack ();
	S_SetGain (1 - black);
}

/*
=================
CL_AttractBlack

How black the screen is over the view, 0 to 1
=================
*/
float CL_AttractBlack (void)
{
	double	t = host.realtime - attract.statetime;

	switch (attract.state)
	{
	case AT_OFF:
	case AT_SHOWING:
		return 0;
	case AT_FADEIN:
		return t >= ATTRACT_FADE ? 0 : (float)(1 - t / ATTRACT_FADE);
	case AT_FADEOUT:
		return t >= ATTRACT_FADE ? 1 : (float)(t / ATTRACT_FADE);
	default:
		return 1;
	}
}

/*
=================
CL_AttractDraw

Over the view: the map's file name (gold) and the first line of its message
in the lower left, faded with the view, and the black of the fade
=================
*/
void CL_AttractDraw (void)
{
	char	line[MAX_QPATH + 64];
	int		maxchars = ((int)vid.conwidth - 16) / 8;
	float	black = CL_AttractBlack ();

	if (attract.state >= AT_FADEIN && maxchars > 0)
	{
		Q_strncpyz (line, attract.file, sizeof(line));
		if ((int)strlen (line) > maxchars)
			line[maxchars] = 0;
		Draw_Alt_String (8, (int)vid.conheight - (attract.message[0] ? 26 : 16), line);
		if (attract.message[0])
		{
			Q_strncpyz (line, attract.message, sizeof(line));
			if ((int)strlen (line) > maxchars)
				line[maxchars] = 0;
			Draw_String (8, (int)vid.conheight - 16, line);
		}
	}
	if (black > 0)
		Draw_BlendFill (0, 0, (int)vid.conwidth, (int)vid.conheight, 0, 0, 0, (int)(black * 255 + 0.5f));
}

/*
===============================================================================

STARTING AND STOPPING

===============================================================================
*/

bool CL_Attracting (void)
{
	return attract.state != AT_OFF;
}

// at startup, until the attract command after quake.rc and the command line
// says: the console isn't seen coming down for maps to be shown
bool CL_AttractPending (void)
{
	return !attract.ready && cl_attract.value;
}

// no game on: no connection, demo, QTV stream or server
bool CL_AttractIdle (void)
{
	return cls.state == ca_disconnected && !CL_Connecting () && !cls.demoplayback && !CL_QTVBusy () && !SV_Active ();
}

/*
=================
CL_AttractStart

The maps scanned, the first loaded; at black until it comes up
=================
*/
void CL_AttractStart (void)
{
	if (attract.state != AT_OFF)
		return;
	if (!CL_AttractScan ())
	{
		Con_Printf ("Attract mode: no maps in \"%s\"\n", cl_attract_gamedirs.string);
		attract.hold = true;
		return;
	}
	attract.failures = 0;
	CL_AttractSetState (AT_BLACK);
	CL_AttractLoadNext ();

	// the console out of a game goes at once, not sliding up over the black
	if (cls.key_dest != key_console)
		scr.con_current = 0;
}

/*
=================
CL_AttractStop

Before a game of the user's, a demo, or cl_attract 0: the showcase's level,
its server and its search path go; the client is out of a game
=================
*/
void CL_AttractStop (void)
{
	if (attract.state == AT_OFF)
		return;
	attract.state = AT_OFF;
	attract.generation++;		// what loads for it is dropped as it finishes
	attract.loading = NULL;
	CL_AttractFreeJob (attract.next);
	attract.next = NULL;
	if (attract.world)
		Mod_FreeDetached (attract.world);
	attract.world = NULL;
	attract.current = -1;
	S_SetGain (1);
	cl.intermission = 0;
	CL_Disconnect ();
	SV_AttractEnd ();
	FS_SetSearchChain (NULL);
}

/*
=================
CL_AttractError

A level that failed (Host_Error, Host_EndGame): in attract mode its map is
passed by from now on, and the next tried on a server of its own; after a
game of the user's, attract mode waits for a key so the error can be read
=================
*/
void CL_AttractError (void)
{
	if (attract.state == AT_OFF)
	{
		attract.hold = true;
		return;
	}
	if (attract.state == AT_BLACK)
		return;		// the last level's, which failed already (its server's last words)
	if (attract.current >= 0)
		attract.maps[attract.current].bad = true;
	attract.current = -1;
	if (attract.world)
		Mod_FreeDetached (attract.world);
	attract.world = NULL;
	if (++attract.failures >= ATTRACT_FAILURES)
	{
		Con_Printf ("Attract mode stopped: %i maps in a row failed\n", attract.failures);
		CL_AttractStop ();
		attract.hold = true;
		return;
	}
	// out of it before its server goes, which would say so
	cl.intermission = 0;
	CL_Disconnect ();
	SV_AttractEnd ();
	CL_AttractSetState (AT_BLACK);
}

// a key pressed: what an error held back can start
void CL_AttractKey (void)
{
	attract.hold = false;
}

static void CL_Attract_f (void)
{
	attract.ready = true;
	if (!cl_attract.value)
		return;
	if (CL_AttractIdle ())
		CL_AttractStart ();
}

static void CL_AttractChanged (cvar_t *var)
{
	if (var == &cl_attract && !var->value)
		CL_AttractStop ();
}

void CL_InitAttract (void)
{
	Cvar_RegisterVariable (&cl_attract);
	Cvar_RegisterVariable (&cl_attract_gamedirs);
	Cvar_RegisterVariable (&cl_attract_time);
	Cvar_AddChangeHook (CL_AttractChanged);
	Cmd_AddCommand ("attract", CL_Attract_f,
		"Starts attract mode (cl_attract) if no game is on: maps of cl_attract_gamedirs shown from their intermission "
		"spots. It runs at startup when the command line starts nothing, and again when a game ends.");
	attract.rng = (uint64_t)Sys_Seed () << 32 ^ (uint64_t)Sys_Seed ();
	SV_SetAttractStop (CL_AttractStop);
}
