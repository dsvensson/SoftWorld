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
// csaddon.dat found, downloaded, checked and loaded, and their lifecycle; the
// server's CSQC entities and events, the stats, input frames, sounds and temp
// entities offered to CSQC first, and the view CSQC draws.

#include "cl_local.h"
#include "cl_qc.h"

cvar_t	cl_nocsqc = {.name = "cl_nocsqc", .string = "0",
	.description = "Keeps client-side QuakeC (CSQC) from loading: a server's csprogs.dat, which some servers need "
		"to be played on."};
cvar_t	cl_download_csprogs = {.name = "cl_download_csprogs", .string = "1", .archive = true,
	.description = "Downloads the client-side QuakeC (csprogs.dat) a server offers, into csprogsvers in the game "
		"directory, named by its checksum."};

extern int	file_from_pak;

#define	CSQC_API_VERSION	1.0f
#define	SOLID_BSP			4
#define	MAX_CSMODELS		1024	// FTE's: CSQC's own models, beside the server's

// FTE's: addentities' masks, renderflags, predraw's results
#define	MASK_ENGINE			1		// the client's own entities
#define	MASK_VIEWMODEL		2		// and its gun
#define	RF_VIEWMODEL		1		// the entity is a gun, placed in the view's axes
#define	PREDRAW_AUTOADD		0

// FTE's view properties (setproperty, getproperty)
enum
{
	VF_MIN = 1, VF_MIN_X, VF_MIN_Y, VF_SIZE, VF_SIZE_X, VF_SIZE_Y, VF_VIEWPORT, VF_FOV, VF_FOV_X, VF_FOV_Y,
	VF_ORIGIN, VF_ORIGIN_X, VF_ORIGIN_Y, VF_ORIGIN_Z, VF_ANGLES, VF_ANGLES_X, VF_ANGLES_Y, VF_ANGLES_Z,
	VF_DRAWWORLD, VF_DRAWENGINESBAR, VF_DRAWCROSSHAIR,
	VF_CL_VIEWANGLES = 33, VF_CL_VIEWANGLES_X, VF_CL_VIEWANGLES_Y, VF_CL_VIEWANGLES_Z,
	VF_AFOV = 203
};

static struct
{
	clqc_t			qc;					// the VM, the calls in progress, the commands
	qc_builtins_t	*builtins;

	// what the server offers, as CSQC_Init was last given it
	bool			promiscuous;		// any csprogs will do (a demo, the server's anycsqc)
	unsigned		checksum;
	size_t			size;
	char			checkname[MAX_QPATH];

	bool			worldloaded;
	char			*entitydata;		// what getentitytoken parses next, or NULL
	char			*entitycopy;		// QuakeC's own text for it

	qc_ent_t		ents[MAX_EDICTS];	// the server's entities CSQC holds, by number; 0 none
	bool			mayread;			// QuakeC reads the server's message (the Read builtins)

	// CSQC's own models, as FTE has them: index -i is modelnames[i]
	char			modelnames[MAX_CSMODELS][MAX_QPATH];
	model_t			*models[MAX_CSMODELS];

	double			starttime;			// of the map: cltime counts from it
	float			*trailcarry;		// each entity's trail: how far on its next particle is due

	// the fields the client reads each frame (NOFIELD if the progs lacks one)
	struct
	{
		uint32_t	modelindex, origin, angles, frame, frame2, lerpfrac, skin, colormap, alpha,
					renderflags, drawmask, predraw;
	} f;

	// the view CSQC_UpdateView draws: the client's entities of the frame and
	// its view, which clearscene goes back to, and what CSQC adds
	bool			drawing;			// in CSQC_UpdateView
	bool			rendered;			// renderscene drew
	entity_t		engine[MAX_VISEDICTS];
	int				numengine;
	vec3_t			vieworg, viewangles;
	bool			enginegun;			// the client draws its gun
	bool			sbar, crosshair;	// VF_DRAWENGINESBAR, VF_DRAWCROSSHAIR
	bool			drewsbar;			// as renderscene had it
	entity_t		viewent;			// an RF_VIEWMODEL entity's, as the gun
	dlight_t		lights[MAX_DLIGHTS];	// dynamiclight_add's, until clearscene
	int				numlights;
} csqc;

#define	NOFIELD		UINT32_MAX

static void CSQC_Destroy (void);
static void CSQC_Failed (void);
static qc_func_t CSQC_Entry (const char *name);
static void CSQC_SetFloat (const char *global, float value);
static void CSQC_SetVector (const char *global, const vec3_t v);
static void CSQC_SetGlobalWord (const char *global, uint32_t word);

/*
==============================================================================

THE HOST

==============================================================================
*/

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

// an entity going: its trail ends, and if it is one of the server's, CSQC no
// longer holds it (its next update makes it anew)
static bool CSQC_OnRemove (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	uint32_t	ofs, type;
	qc_word_t	n = {0};

	(void)ctx;
	if (csqc.trailcarry && e < QC_MaxEdicts (vm))
		csqc.trailcarry[e] = 0;
	if (QC_FindField (vm, "entnum", &ofs, &type) && type == QC_EV_FLOAT && QC_GetField (vm, e, ofs, 1, &n.u)
		&& n.f > 0 && n.f < MAX_EDICTS && csqc.ents[(int)n.f] == e)
		csqc.ents[(int)n.f] = 0;
	return true;
}

// the shared host's callbacks (cl_qc.c), and these (CSQC_RegisterVariables)
static qc_host_t	csqc_host;

/*
==============================================================================

THE ENGINE'S BUILTINS

Those that need neither the CSQC networking nor drawing.

==============================================================================
*/

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
	return CLQC_ReturnText (vm, CS_ServerKey (QC_ArgString (vm, 0)));
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
	return CLQC_ReturnText (vm, CS_PlayerKey (QC_DoubleToInt (QC_ArgFloat (vm, 0)), QC_ArgString (vm, 1)));
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
// the stat as a float (svc_fte_updatestatfloat's), or bitcount bits of the
// integer from firstbit
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
		QC_ReturnFloat (vm, cl.statsf[n]);
	return true;
}

// string getstats(float stnum): the string stat with FTE's CSQC protocol, else
// four stats from stnum as sixteen characters
static bool CS_GetStatS (qcvm_t *vm)
{
	int		n = CS_StatNumber (vm, "getstats", MAX_CL_STATS - 1), i;
	char	text[17];

	if (n >= 0 && (cls.fteext & FTE_PEXT_CSQC))
		return CLQC_ReturnText (vm, cl.statsstr[n] ? cl.statsstr[n] : "");
	if (n < 0 || n > MAX_CL_STATS - 4)
	{
		if (n >= 0)
			QC_Warning (vm, "getstats: invalid packed-string stat index (%i)", n);
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

// string modelnameforindex(float index): the server's model names, CSQC's own
// below 0
static bool CS_ModelNameForIndex (qcvm_t *vm)
{
	int	i = QC_DoubleToInt (QC_ArgFloat (vm, 0));

	if (i < 0 && -i < MAX_CSMODELS && csqc.modelnames[-i][0])
		QC_ReturnWord (vm, QC_HostString (vm, csqc.modelnames[-i]));
	else
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

/*
==============================================================================

MODELS, SOUNDS AND ENTITIES

==============================================================================
*/

// an entity's field, if the progs has it with that type
static bool CS_Field (const char *field, uint32_t type, uint32_t *ofs)
{
	uint32_t	t;

	return QC_FindField (csqc.qc.vm, field, ofs, &t) && t == type;
}

static float CS_GetFloat (qc_ent_t e, const char *field)
{
	uint32_t	ofs;
	qc_word_t	w = {0};

	if (CS_Field (field, QC_EV_FLOAT, &ofs))
		QC_GetField (csqc.qc.vm, e, ofs, 1, &w.u);
	return w.f;
}

static void CS_GetVector (qc_ent_t e, const char *field, float v[3])
{
	uint32_t	ofs;
	qc_word_t	w[3] = {0};

	if (CS_Field (field, QC_EV_VECTOR, &ofs))
		QC_GetField (csqc.qc.vm, e, ofs, 3, &w[0].u);
	v[0] = w[0].f;
	v[1] = w[1].f;
	v[2] = w[2].f;
}

static void CS_SetVector (qc_ent_t e, const char *field, const float v[3])
{
	uint32_t	ofs;
	qc_word_t	w[3];

	w[0].f = v[0];
	w[1].f = v[1];
	w[2].f = v[2];
	if (CS_Field (field, QC_EV_VECTOR, &ofs))
		QC_SetField (csqc.qc.vm, e, ofs, 3, &w[0].u);
}

static void CS_SetWord (qc_ent_t e, const char *field, uint32_t type, uint32_t word)
{
	uint32_t	ofs;

	if (CS_Field (field, type, &ofs))
		QC_SetField (csqc.qc.vm, e, ofs, 1, &word);
}

/*
=================
CS_ModelIndex

As FTE finds a model: CSQC's own first (index -i), then the server's; 0 if
neither has it, with *freeslot the first of CSQC's slots free (0 if none)
=================
*/
static int CS_ModelIndex (const char *model, int *freeslot)
{
	int		i;

	*freeslot = 0;
	if (!*model)
		return 0;
	for (i = 1 ; i < MAX_CSMODELS ; i++)
	{
		if (!csqc.modelnames[i][0])
		{
			*freeslot = -i;
			break;
		}
		if (!strcmp (csqc.modelnames[i], model))
			return -i;
	}
	for (i = 1 ; i < MAX_MODELS && cl.model_name[i][0] ; i++)
		if (!strcmp (cl.model_name[i], model))
			return i;
	return 0;
}

// the model of an index, server's or CSQC's, loaded; NULL for none
static model_t *CS_Model (int index)
{
	if (index < 0)
	{
		if (-index >= MAX_CSMODELS || !csqc.modelnames[-index][0])
			return NULL;
		if (!csqc.models[-index])
			csqc.models[-index] = Mod_ForName (csqc.modelnames[-index], false);
		return csqc.models[-index];
	}
	return index < MAX_MODELS ? cl.model_precache[index] : NULL;
}

// a model's index in *index, given CSQC's own slot unless the server has it or
// queryonly; false (an error) when CSQC's slots are all taken
static bool CS_PrecacheModel (qcvm_t *vm, const char *model, bool queryonly, int *index)
{
	int		freeslot;

	*index = CS_ModelIndex (model, &freeslot);
	if (*index || queryonly || !*model)
		return true;
	if (!freeslot)
		return QC_HostError (vm, "CSQC ran out of model slots");
	Q_strncpyz (csqc.modelnames[-freeslot], model, sizeof(csqc.modelnames[0]));
	csqc.models[-freeslot] = Mod_ForName (csqc.modelnames[-freeslot], false);
	*index = freeslot;
	return true;
}

// string precache_model(string name)
static bool CS_PrecacheModelBuiltin (qcvm_t *vm)
{
	int		index;
	bool	ok = CS_PrecacheModel (vm, QC_ArgString (vm, 0), false, &index);

	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	return ok;
}

// float getmodelindex(string name, optional float queryonly)
static bool CS_GetModelIndex (qcvm_t *vm)
{
	bool	queryonly = QC_Argc (vm) > 1 && QC_ArgFloat (vm, 1) != 0;
	int		index;

	if (!CS_PrecacheModel (vm, QC_ArgString (vm, 0), queryonly, &index))
		return false;
	QC_ReturnFloat (vm, (float)index);
	return true;
}

// string precache_sound(string name)
static bool CS_PrecacheSound (qcvm_t *vm)
{
	const char	*sample = QC_ArgString (vm, 0);

	if (*sample)
		S_PrecacheSound ((char *)sample);
	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	return true;
}

/*
=================
CS_SetModel

void setmodel(entity e, string name): its modelindex, model, size and (FTE's)
modelflags, the model's own flags; a model nobody precached is given CSQC's
next slot
=================
*/
static bool CS_SetModel (qcvm_t *vm)
{
	qc_ent_t	e = QC_ArgWord (vm, 0);
	const char	*model = QC_ArgString (vm, 1);
	int			index;
	model_t		*mod;
	vec3_t		size;
	qc_word_t	w;

	if (e == 0 && QC_IsProtected (vm, 0))
		return QC_Error (vm, "setmodel on the world");
	if (!CS_PrecacheModel (vm, model, false, &index))
		return false;
	mod = CS_Model (index);

	w.f = (float)index;
	CS_SetWord (e, "modelindex", QC_EV_FLOAT, w.u);
	CS_SetWord (e, "model", QC_EV_STRING, index < 0 ? QC_HostString (vm, csqc.modelnames[-index])
		: index ? QC_HostString (vm, cl.model_name[index]) : 0);
	if (mod)
	{
		CS_SetVector (e, "mins", mod->mins);
		CS_SetVector (e, "maxs", mod->maxs);
		VectorSubtract (mod->maxs, mod->mins, size);
		CS_SetVector (e, "size", size);
		w.f = (float)mod->flags;
		CS_SetWord (e, "modelflags", QC_EV_FLOAT, w.u);
	}
	return true;
}

// void setorigin(entity e, vector origin): with its absolute bounds
static bool CS_SetOrigin (qcvm_t *vm)
{
	qc_ent_t	e = QC_ArgWord (vm, 0);
	vec3_t		org, mins, maxs;

	QC_ArgVector (vm, 1, org);
	CS_SetVector (e, "origin", org);
	CS_GetVector (e, "mins", mins);
	CS_GetVector (e, "maxs", maxs);
	VectorAdd (org, mins, mins);
	VectorAdd (org, maxs, maxs);
	CS_SetVector (e, "absmin", mins);
	CS_SetVector (e, "absmax", maxs);
	return true;
}

/*
=================
CS_Sound

void sound(entity e, float channel, string sample, float volume, float
attenuation, ...): from the entity (a brush model's middle), numbered below 0
as FTE numbers CSQC's entities' sounds
=================
*/
static bool CS_Sound (qcvm_t *vm)
{
	qc_ent_t	e = QC_ArgWord (vm, 0);
	int			channel = QC_FloatToInt (QC_ArgFloat (vm, 1));
	const char	*sample = QC_ArgString (vm, 2);
	float		vol = QC_ArgFloat (vm, 3), attenuation = QC_ArgFloat (vm, 4);
	vec3_t		org, mins, maxs;
	sfx_t		*sfx;
	int			i;

	if (!*sample || !(sfx = S_PrecacheSound ((char *)sample)))
		return true;
	CS_GetVector (e, "origin", org);
	if (CS_GetFloat (e, "solid") == SOLID_BSP)
	{
		CS_GetVector (e, "mins", mins);
		CS_GetVector (e, "maxs", maxs);
		for (i = 0 ; i < 3 ; i++)
			org[i] += (mins[i] + maxs[i]) * 0.5f;
	}
	S_StartSound (-(int)e, channel, sfx, org, vol, attenuation);
	return true;
}

/*
==============================================================================

THE WORLD

==============================================================================
*/

// float pointcontents(vector point): the world's
static bool CS_PointContents (qcvm_t *vm)
{
	vec3_t		p;
	hull_t		*hull;
	int			contents = CONTENTS_EMPTY;

	QC_ArgVector (vm, 0, p);
	if (cl.map)
	{
		hull = &CM_WorldModel (cl.map)->hulls[0];
		contents = CM_HullPointContents (hull, hull->firstclipnode, p);
	}
	QC_ReturnFloat (vm, (float)contents);
	return true;
}

/*
=================
CS_TraceLine

void traceline(vector v1, vector v2, float nomonsters, entity forent): through
the world alone (no entity is solid to CSQC here), into the trace_ globals as
the server's traceline sets them
=================
*/
static bool CS_TraceLine (qcvm_t *vm)
{
	vec3_t		start, end;
	trace_t		trace;
	hull_t		*hull;

	QC_ArgVector (vm, 0, start);
	QC_ArgVector (vm, 1, end);
	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1;
	VectorCopy (end, trace.endpos);
	if (cl.map)
	{
		trace.allsolid = true;
		hull = &CM_WorldModel (cl.map)->hulls[0];
		CM_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, start, end, &trace);
	}

	CSQC_SetFloat ("trace_allsolid", trace.allsolid);
	CSQC_SetFloat ("trace_startsolid", trace.startsolid);
	CSQC_SetFloat ("trace_fraction", trace.fraction);
	CSQC_SetFloat ("trace_inwater", trace.inwater);
	CSQC_SetFloat ("trace_inopen", trace.inopen);
	CSQC_SetVector ("trace_endpos", trace.endpos);
	CSQC_SetVector ("trace_plane_normal", trace.plane.normal);
	CSQC_SetFloat ("trace_plane_dist", trace.plane.dist);
	CSQC_SetGlobalWord ("trace_ent", 0);
	return true;
}

/*
==============================================================================

EFFECTS

==============================================================================
*/

// the particle trails, by R_RocketTrail's type, by FTE's names for them
static const char *const cs_trails[] = {"TR_ROCKET", "TR_GRENADE", "TR_BLOOD", "TR_WIZSPIKE", "TR_SLIGHTBLOOD",
	"TR_KNIGHTSPIKE", "TR_VORESPIKE"};

// float particleeffectnum(string name): one of the trails, 0 for an effect
// the client lacks
static bool CS_ParticleEffectNum (qcvm_t *vm)
{
	const char	*effect = QC_ArgString (vm, 0);
	size_t		i;

	for (i = 0 ; i < sizeof(cs_trails) / sizeof(cs_trails[0]) ; i++)
		if (!Q_strcasecmp ((char *)effect, (char *)cs_trails[i]))
			break;
	QC_ReturnFloat (vm, i < sizeof(cs_trails) / sizeof(cs_trails[0]) ? (float)(i + 1) : 0.0f);
	return true;
}

// void trailparticles(float effect, entity ent, vector start, vector end): the
// entity's trail goes on, its particles evenly spaced across the stretches
// QuakeC draws, as FTE keeps a trail per entity
static bool CS_TrailParticles (qcvm_t *vm)
{
	int			effect = QC_FloatToInt (QC_ArgFloat (vm, 0)) - 1;
	qc_ent_t	e = QC_ArgWord (vm, 1);
	vec3_t		start, end;

	QC_ArgVector (vm, 2, start);
	QC_ArgVector (vm, 3, end);
	if (effect >= 0 && effect < (int)(sizeof(cs_trails) / sizeof(cs_trails[0])))
		R_RocketTrail (start, end, effect, e && e < QC_MaxEdicts (vm) ? &csqc.trailcarry[e] : NULL);
	return true;
}

// float dynamiclight_add(vector org, float radius, vector color, ...): a light
// for the scene, until clearscene
static bool CS_DynamicLightAdd (qcvm_t *vm)
{
	dlight_t	*dl;
	float		radius = QC_ArgFloat (vm, 1);

	QC_ReturnFloat (vm, 0);
	if (radius <= 0 || csqc.numlights == MAX_DLIGHTS)
		return true;
	dl = &csqc.lights[csqc.numlights++];
	memset (dl, 0, sizeof(*dl));
	QC_ArgVector (vm, 0, dl->origin);
	dl->radius = radius;
	QC_ArgVector (vm, 2, dl->color);
	QC_ReturnFloat (vm, (float)csqc.numlights);
	return true;
}

// te_lightning1/2/3(entity own, vector start, vector end): the beam of an
// entity of CSQC's, kept apart from the server's as FTE keys it
static bool CS_Lightning (qcvm_t *vm, const char *model)
{
	qc_ent_t	e = QC_ArgWord (vm, 0);
	vec3_t		start, end;
	model_t		*m = Mod_ForName ((char *)model, false);

	QC_ArgVector (vm, 1, start);
	QC_ArgVector (vm, 2, end);
	if (m)
		CL_AddBeam (m, e ? (int)e + MAX_EDICTS : 0, start, end);
	return true;
}

static bool CS_Lightning1 (qcvm_t *vm)
{
	return CS_Lightning (vm, "progs/bolt.mdl");
}

static bool CS_Lightning2 (qcvm_t *vm)
{
	return CS_Lightning (vm, "progs/bolt2.mdl");
}

static bool CS_Lightning3 (qcvm_t *vm)
{
	return CS_Lightning (vm, "progs/bolt3.mdl");
}

/*
==============================================================================

THE SCENE

CSQC_UpdateView draws the view: clearscene starts it from the client's view of
the frame, addentities and addentity bring in entities, renderscene draws it.
Outside CSQC_UpdateView they do nothing.
==============================================================================
*/

static float CS_EntFloat (qc_ent_t e, uint32_t ofs)
{
	qc_word_t	w = {0};

	if (ofs != NOFIELD)
		QC_GetField (csqc.qc.vm, e, ofs, 1, &w.u);
	return w.f;
}

static void CS_EntVector (qc_ent_t e, uint32_t ofs, vec3_t v)
{
	qc_word_t	w[3] = {0};

	if (ofs != NOFIELD)
		QC_GetField (csqc.qc.vm, e, ofs, 3, &w[0].u);
	v[0] = w[0].f;
	v[1] = w[1].f;
	v[2] = w[2].f;
}

static uint32_t CS_FieldOfs (const char *field, uint32_t type)
{
	uint32_t	ofs;

	return CS_Field (field, type, &ofs) ? ofs : NOFIELD;
}

// the fields the scene reads
static void CS_FindFields (void)
{
	csqc.f.modelindex = CS_FieldOfs ("modelindex", QC_EV_FLOAT);
	csqc.f.origin = CS_FieldOfs ("origin", QC_EV_VECTOR);
	csqc.f.angles = CS_FieldOfs ("angles", QC_EV_VECTOR);
	csqc.f.frame = CS_FieldOfs ("frame", QC_EV_FLOAT);
	csqc.f.frame2 = CS_FieldOfs ("frame2", QC_EV_FLOAT);
	csqc.f.lerpfrac = CS_FieldOfs ("lerpfrac", QC_EV_FLOAT);
	csqc.f.skin = CS_FieldOfs ("skin", QC_EV_FLOAT);
	csqc.f.colormap = CS_FieldOfs ("colormap", QC_EV_FLOAT);
	csqc.f.alpha = CS_FieldOfs ("alpha", QC_EV_FLOAT);
	csqc.f.renderflags = CS_FieldOfs ("renderflags", QC_EV_FLOAT);
	csqc.f.drawmask = CS_FieldOfs ("drawmask", QC_EV_FLOAT);
	csqc.f.predraw = CS_FieldOfs ("predraw", QC_EV_FUNCTION);
}

/*
=================
CS_RenderEntity

A CSQC entity as the renderer draws it, as FTE's CopyCSQCEdictToEntity has it:
frame2 is the frame it turns from, lerpfrac how much of frame2 shows; alpha 0
is opaque. False without a model.
=================
*/
static bool CS_RenderEntity (qc_ent_t e, entity_t *ent)
{
	model_t	*model = CS_Model (QC_FloatToInt (CS_EntFloat (e, csqc.f.modelindex)));
	int		colormap;
	float	f;

	if (!model)
		return false;
	memset (ent, 0, sizeof(*ent));
	ent->model = model;
	CS_EntVector (e, csqc.f.origin, ent->origin);
	CS_EntVector (e, csqc.f.angles, ent->angles);
	ent->frame = QC_FloatToInt (CS_EntFloat (e, csqc.f.frame));
	ent->oldframe = QC_FloatToInt (CS_EntFloat (e, csqc.f.frame2));
	f = CS_EntFloat (e, csqc.f.lerpfrac);
	ent->backlerp = f < 0 ? 0 : f > 1 ? 1 : f;
	ent->skinnum = QC_FloatToInt (CS_EntFloat (e, csqc.f.skin));
	f = CS_EntFloat (e, csqc.f.alpha);
	if (f > 0 && f < 1)
		ent->alpha = (byte)(f * 254 < 1 ? 1 : f * 254);

	// a player's colors, as the client gives its own players them
	colormap = QC_FloatToInt (CS_EntFloat (e, csqc.f.colormap));
	if (colormap > 0 && colormap <= MAX_CLIENTS && !strcmp (model->name, "progs/player.mdl"))
	{
		ent->translate = cl.players[colormap-1].translate;
		ent->palette = cl.players[colormap-1].palette;
		ent->skin = Skin_ForPlayer (&cl.players[colormap-1]);
	}
	return true;
}

static void CS_AddToScene (const entity_t *ent)
{
	if (cl.numvisedicts < MAX_VISEDICTS)
		cl.visedicts[cl.numvisedicts++] = *ent;
}

/*
=================
CS_AddEntity

A CSQC entity into the scene. An RF_VIEWMODEL one is the gun: its origin is in
the view's axes (forward, left, up) from where the client holds its own gun,
bobbing with it, and its angles add to the gun's.
=================
*/
static void CS_AddEntity (qc_ent_t e)
{
	entity_t	ent;
	vec3_t		forward, right, up;
	int			i;

	if (!CS_RenderEntity (e, &ent))
		return;
	if (!(QC_FloatToInt (CS_EntFloat (e, csqc.f.renderflags)) & RF_VIEWMODEL))
	{
		CS_AddToScene (&ent);
		return;
	}
	AngleVectors (r_refdef.viewangles, forward, right, up);
	csqc.viewent = ent;
	for (i = 0 ; i < 3 ; i++)
	{
		csqc.viewent.origin[i] = cl.viewent.origin[i] + forward[i] * ent.origin[0] - right[i] * ent.origin[1]
			+ up[i] * ent.origin[2];
		csqc.viewent.angles[i] = cl.viewent.angles[i] + ent.angles[i];
	}
	r_scene.viewent = &csqc.viewent;
	r_scene.drawviewmodel = true;
}

// void clearscene(): the client's view of the frame, without entities
static bool CS_ClearScene (qcvm_t *vm)
{
	(void)vm;
	if (!csqc.drawing)
		return true;
	cl.numvisedicts = 0;
	csqc.numlights = 0;
	VectorCopy (csqc.vieworg, r_refdef.vieworg);
	VectorCopy (csqc.viewangles, r_refdef.viewangles);
	r_scene.viewent = &cl.viewent;
	r_scene.drawviewmodel = false;
	csqc.sbar = false;
	csqc.crosshair = false;
	return true;
}

/*
=================
CS_AddEntities

void addentities(float mask): with MASK_ENGINE the client's entities, then (as
FTE has it) its beams and explosions, so beams added before show this frame;
CSQC's entities whose drawmask has a bit of the mask, each after its predraw
(as self) unless that removes it or returns nonzero; with MASK_VIEWMODEL the
client's gun
=================
*/
static bool CS_AddEntities (qcvm_t *vm)
{
	int			mask = QC_FloatToInt (QC_ArgFloat (vm, 0));
	int			i;
	uint32_t	e, last;
	qc_word_t	w;
	qc_value_t	ret;

	if (!csqc.drawing)
		return true;
	if (mask & MASK_ENGINE)
	{
		for (i = 0 ; i < csqc.numengine ; i++)
			CS_AddToScene (&csqc.engine[i]);
		CL_UpdateTEnts ();
	}

	last = QC_NumEdicts (vm);
	for (e = 1 ; e < last ; e++)
	{
		if (QC_IsFree (vm, e) || !(QC_FloatToInt (CS_EntFloat (e, csqc.f.drawmask)) & mask))
			continue;
		w.u = 0;
		if (csqc.f.predraw != NOFIELD)
			QC_GetField (vm, e, csqc.f.predraw, 1, &w.u);
		if (w.u)
		{
			if (!QC_CallAs (vm, e, w.u, 0, NULL, &ret))
				return false;
			w.u = ret.w[0];
			if (QC_IsFree (vm, e) || w.f != PREDRAW_AUTOADD)
				continue;
		}
		CS_AddEntity (e);
	}

	if (mask & MASK_VIEWMODEL)
	{
		r_scene.viewent = &cl.viewent;
		r_scene.drawviewmodel = csqc.enginegun;
	}
	return true;
}

// void addentity(entity e)
static bool CS_AddEntityBuiltin (qcvm_t *vm)
{
	qc_ent_t	e = QC_ArgWord (vm, 0);

	if (csqc.drawing && !QC_IsFree (vm, e))
		CS_AddEntity (e);
	return true;
}

/*
=================
CS_RenderScene

void renderscene(): the view, with CSQC's lights in the client's free slots
for it; the status bar and crosshair as VF_DRAWENGINESBAR and
VF_DRAWCROSSHAIR ask (the status bar after CSQC_UpdateView)
=================
*/
static bool CS_RenderScene (qcvm_t *vm)
{
	int		slots[MAX_DLIGHTS], numslots = 0, i, j;

	(void)vm;
	if (!csqc.drawing)
		return true;
	for (i = 0, j = 0 ; i < csqc.numlights && j < MAX_DLIGHTS ; j++)
		if (cl.dlights[j].die < r_scene.time || !cl.dlights[j].radius)
		{
			cl.dlights[j] = csqc.lights[i++];
			cl.dlights[j].die = (float)(r_scene.time + 1);
			slots[numslots++] = j;
		}

	V_DrawView (csqc.crosshair);

	for (i = 0 ; i < numslots ; i++)
		memset (&cl.dlights[slots[i]], 0, sizeof(cl.dlights[0]));
	csqc.rendered = true;
	csqc.drewsbar = csqc.sbar;
	return true;
}

/*
=================
CS_SetProperty

float setproperty(float property, ...): the view's origin and angles, the
player's view angles, whether the status bar and crosshair are drawn; 0 for a
property the client can't set
=================
*/
static bool CS_SetProperty (qcvm_t *vm)
{
	int		prop = QC_FloatToInt (QC_ArgFloat (vm, 0));
	float	f = QC_ArgFloat (vm, 1);
	vec3_t	v;

	QC_ArgVector (vm, 1, v);
	QC_ReturnFloat (vm, 1);
	switch (prop)
	{
	case VF_ORIGIN:
		VectorCopy (v, r_refdef.vieworg);
		break;
	case VF_ORIGIN_X:
	case VF_ORIGIN_Y:
	case VF_ORIGIN_Z:
		r_refdef.vieworg[prop - VF_ORIGIN_X] = f;
		break;
	case VF_ANGLES:
		VectorCopy (v, r_refdef.viewangles);
		break;
	case VF_ANGLES_X:
	case VF_ANGLES_Y:
	case VF_ANGLES_Z:
		r_refdef.viewangles[prop - VF_ANGLES_X] = f;
		break;
	case VF_CL_VIEWANGLES:
		VectorCopy (v, cl.viewangles);
		break;
	case VF_CL_VIEWANGLES_X:
	case VF_CL_VIEWANGLES_Y:
	case VF_CL_VIEWANGLES_Z:
		cl.viewangles[prop - VF_CL_VIEWANGLES_X] = f;
		break;
	case VF_DRAWENGINESBAR:
		csqc.sbar = f != 0;
		break;
	case VF_DRAWCROSSHAIR:
		csqc.crosshair = f != 0;
		break;
	case VF_DRAWWORLD:
		break;			// the world is always drawn
	default:
		Con_DPrintf ("CSQC: setproperty %i isn't supported\n", prop);
		QC_ReturnFloat (vm, 0);
		break;
	}
	return true;
}

// vector getproperty(float property): what setproperty sets, and the fov
static bool CS_GetProperty (qcvm_t *vm)
{
	int		prop = QC_FloatToInt (QC_ArgFloat (vm, 0));
	vec3_t	v = {0, 0, 0};

	switch (prop)
	{
	case VF_ORIGIN:
		VectorCopy (r_refdef.vieworg, v);
		break;
	case VF_ORIGIN_X:
	case VF_ORIGIN_Y:
	case VF_ORIGIN_Z:
		v[0] = r_refdef.vieworg[prop - VF_ORIGIN_X];
		break;
	case VF_ANGLES:
		VectorCopy (r_refdef.viewangles, v);
		break;
	case VF_ANGLES_X:
	case VF_ANGLES_Y:
	case VF_ANGLES_Z:
		v[0] = r_refdef.viewangles[prop - VF_ANGLES_X];
		break;
	case VF_CL_VIEWANGLES:
		VectorCopy (cl.viewangles, v);
		break;
	case VF_CL_VIEWANGLES_X:
	case VF_CL_VIEWANGLES_Y:
	case VF_CL_VIEWANGLES_Z:
		v[0] = cl.viewangles[prop - VF_CL_VIEWANGLES_X];
		break;
	case VF_FOV:
		v[0] = r_refdef.fov_x;
		v[1] = r_refdef.fov_y;
		break;
	case VF_FOV_X:
		v[0] = r_refdef.fov_x;
		break;
	case VF_FOV_Y:
		v[0] = r_refdef.fov_y;
		break;
	case VF_AFOV:
		v[0] = Cvar_VariableValue ("fov");
		break;
	case VF_DRAWWORLD:
		v[0] = 1;
		break;
	case VF_DRAWENGINESBAR:
		v[0] = csqc.sbar;
		break;
	case VF_DRAWCROSSHAIR:
		v[0] = csqc.crosshair;
		break;
	default:
		Con_DPrintf ("CSQC: getproperty %i isn't supported\n", prop);
		break;
	}
	QC_ReturnVector (vm, v);
	return true;
}

/*
==============================================================================

INPUT

==============================================================================
*/

// a command into the input_ globals, as FTE has them
static void CS_SetInput (const usercmd_t *cmd, int sequence)
{
	vec3_t	move;

	move[0] = cmd->forwardmove;
	move[1] = cmd->sidemove;
	move[2] = cmd->upmove;
	CSQC_SetFloat ("input_sequence", (float)sequence);
	CSQC_SetFloat ("input_timelength", cmd->msec / 1000.0f);
	CSQC_SetVector ("input_angles", cmd->angles);
	CSQC_SetVector ("input_movevalues", move);
	CSQC_SetFloat ("input_buttons", cmd->buttons);
	CSQC_SetFloat ("input_impulse", cmd->impulse);
}

/*
=================
CS_GetInputState

float getinputstate(float frame): the command of an input frame into the
input_ globals, while the client holds it; the frame being made
(clientcommandframe) as far as it is, as FTE has it: the last command turned to
the view now and as long as it has been since
=================
*/
static bool CS_GetInputState (qcvm_t *vm)
{
	int			f = QC_FloatToInt (QC_ArgFloat (vm, 0));
	int			made = cls.netchan.outgoing_sequence;
	frame_t		*last;
	usercmd_t	cmd;
	double		msec;

	QC_ReturnFloat (vm, 0);
	if (cls.demoplayback || f > made || f <= made - UPDATE_BACKUP || f < 1
		|| (cl.paused && f > cls.netchan.incoming_acknowledged))
		return true;
	if (f == made)
	{
		last = &cl.frames[(f - 1) & UPDATE_MASK];
		cmd = last->cmd;
		VectorCopy (cl.viewangles, cmd.angles);
		msec = (host.realtime - last->senttime) * 1000;
		cmd.msec = (byte)(msec < 0 ? 0 : msec > 255 ? 255 : msec);
	}
	else
		cmd = cl.frames[f & UPDATE_MASK].cmd;
	CS_SetInput (&cmd, f);
	QC_ReturnFloat (vm, 1);
	return true;
}

/*
==============================================================================

READING THE SERVER'S MESSAGE

While CSQC_Ent_Update or CSQC_Parse_Event runs: what the server's QuakeC wrote,
read as it wrote it (coordinates and angles as the connection has them)

==============================================================================
*/

static bool CS_MayRead (qcvm_t *vm, const char *builtin)
{
	if (csqc.mayread)
		return true;
	QC_Error (vm, "%s is not valid at this time", builtin);
	return false;
}

static bool CS_ReadByte (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadByte"))
		QC_ReturnFloat (vm, (float)MSG_ReadByte ());
	return csqc.mayread;
}

static bool CS_ReadChar (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadChar"))
		QC_ReturnFloat (vm, (float)(signed char)MSG_ReadByte ());
	return csqc.mayread;
}

static bool CS_ReadShort (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadShort"))
		QC_ReturnFloat (vm, (float)MSG_ReadShort ());
	return csqc.mayread;
}

static bool CS_ReadEntityNum (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadEntityNum"))
		QC_ReturnFloat (vm, (float)(MSG_ReadShort () & 0xffff));
	return csqc.mayread;
}

static bool CS_ReadLong (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadLong"))
		QC_ReturnFloat (vm, (float)MSG_ReadLong ());
	return csqc.mayread;
}

static bool CS_ReadCoord (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadCoord"))
		QC_ReturnFloat (vm, MSG_ReadCoord ());
	return csqc.mayread;
}

static bool CS_ReadAngle (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadAngle"))
		QC_ReturnFloat (vm, MSG_ReadAngle ());
	return csqc.mayread;
}

static bool CS_ReadFloat (qcvm_t *vm)
{
	if (CS_MayRead (vm, "ReadFloat"))
		QC_ReturnFloat (vm, MSG_ReadFloat ());
	return csqc.mayread;
}

static bool CS_ReadString (qcvm_t *vm)
{
	const char	*s;

	if (!CS_MayRead (vm, "ReadString"))
		return false;
	s = MSG_ReadString ();
	QC_ReturnWord (vm, QC_TempString (vm, s, strlen (s)));
	return true;
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
	{"precache_model", CS_PrecacheModelBuiltin},
	{"getmodelindex", CS_GetModelIndex},
	{"precache_sound", CS_PrecacheSound},
	{"setmodel", CS_SetModel},
	{"setorigin", CS_SetOrigin},
	{"sound", CS_Sound},
	{"pointcontents", CS_PointContents},
	{"traceline", CS_TraceLine},
	{"particleeffectnum", CS_ParticleEffectNum},
	{"trailparticles", CS_TrailParticles},
	{"dynamiclight_add", CS_DynamicLightAdd},
	{"te_lightning1", CS_Lightning1},
	{"te_lightning2", CS_Lightning2},
	{"te_lightning3", CS_Lightning3},
	{"clearscene", CS_ClearScene},
	{"addentities", CS_AddEntities},
	{"addentity", CS_AddEntityBuiltin},
	{"renderscene", CS_RenderScene},
	{"setproperty", CS_SetProperty},
	{"getproperty", CS_GetProperty},
	{"getinputstate", CS_GetInputState},
	{"ReadByte", CS_ReadByte},
	{"ReadChar", CS_ReadChar},
	{"ReadShort", CS_ReadShort},
	{"ReadEntityNum", CS_ReadEntityNum},
	{"ReadLong", CS_ReadLong},
	{"ReadCoord", CS_ReadCoord},
	{"ReadAngle", CS_ReadAngle},
	{"ReadFloat", CS_ReadFloat},
	{"ReadString", CS_ReadString},
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
	CLQC_Failed (&csqc.qc);
	Con_Printf ("CSQC shut down\n");
	CSQC_Destroy ();
}

// an entry point, in the csprogs or else the add-on (FTE's PR_ANY)
static qc_func_t CSQC_Entry (const char *entry)
{
	uint32_t	pr;
	qc_func_t	f;

	for (pr = 0 ; pr < QC_NumProgs (csqc.qc.vm) ; pr++)
		if ((f = QC_FindFunctionIn (csqc.qc.vm, pr, entry)))
			return f;
	return 0;
}

// calls f, if not 0, its result in *ret (if not NULL; zero if f is 0); false
// (CSQC gone) on an error
static bool CSQC_CallRet (qc_func_t f, int argc, const qc_value_t *args, qc_value_t *ret)
{
	bool	ok;

	if (ret)
		memset (ret, 0, sizeof(*ret));
	if (!csqc.qc.vm)
		return false;
	if (!f)
		return true;
	csqc.qc.calls++;
	ok = QC_Call (csqc.qc.vm, f, argc, args, ret);
	csqc.qc.calls--;
	if (!ok)
		CSQC_Failed ();
	return ok;
}

static bool CSQC_Call (qc_func_t f, int argc, const qc_value_t *args)
{
	return CSQC_CallRet (f, argc, args, NULL);
}

// a command QuakeC registered: its whole line goes to CSQC_ConsoleCommand
static void CSQC_Command (clqc_t *qc, const char *line)
{
	qc_value_t	arg = QC_ValWord (QC_TempString (qc->vm, line, strlen (line)));

	CSQC_Call (CSQC_Entry ("CSQC_ConsoleCommand"), 1, &arg);
}

// a float result
static float CSQC_RetFloat (const qc_value_t *ret)
{
	qc_word_t	w = {.u = ret->w[0]};

	return w.f;
}

// a float global of the csprogs, if it has one
static void CSQC_SetFloat (const char *global, float value)
{
	uint32_t	word, type;

	if (QC_FindGlobal (csqc.qc.vm, global, &word, &type) && type == QC_EV_FLOAT)
		QC_Globals (csqc.qc.vm)[word].f = value;
}

static void CSQC_SetVector (const char *global, const vec3_t v)
{
	uint32_t	word, type;
	qc_word_t	*g;

	if (QC_FindGlobal (csqc.qc.vm, global, &word, &type) && type == QC_EV_VECTOR)
	{
		g = QC_Globals (csqc.qc.vm) + word;
		g[0].f = v[0];
		g[1].f = v[1];
		g[2].f = v[2];
	}
}

// an entity, string or function global
static void CSQC_SetGlobalWord (const char *global, uint32_t value)
{
	uint32_t	word, type;

	if (QC_FindGlobal (csqc.qc.vm, global, &word, &type) && type != QC_EV_FLOAT && type != QC_EV_VECTOR)
		QC_Globals (csqc.qc.vm)[word].u = value;
}

static void CSQC_SetString (const char *global, const char *text)
{
	uint32_t	word, type;

	if (QC_FindGlobal (csqc.qc.vm, global, &word, &type) && type == QC_EV_STRING)
		QC_Globals (csqc.qc.vm)[word].u = QC_Intern (csqc.qc.vm, text, strlen (text));
}

// a field of the world, if the progs has it
static void CSQC_SetWorldField (const char *field, uint32_t want, qc_value_t value)
{
	uint32_t	ofs, type;

	if (QC_FindField (csqc.qc.vm, field, &ofs, &type) && type == want)
		QC_SetField (csqc.qc.vm, 0, ofs, 1, value.w);
}

// a float field of an entity, if the progs has it
static void CSQC_SetEntityFloat (qc_ent_t e, const char *field, float value)
{
	uint32_t	ofs, type;
	qc_value_t	v = QC_ValFloat (value);

	if (QC_FindField (csqc.qc.vm, field, &ofs, &type) && type == QC_EV_FLOAT)
		QC_SetField (csqc.qc.vm, e, ofs, 1, v.w);
}

/*
=================
CSQC_SetFrameGlobals

What FTE sets before calling into CSQC for the server's entities and events,
and each frame:
- time: the client's clock of the game (cl.time, the server's time as the
  client shows it, which FTE's interpolated servertime is too)
- cltime: real time since the map started
- clientcommandframe: the input frame being made, the next one sent (each
  command goes out in the packet of its number); servercommandframe: the last
  the server acknowledged, which what it sends has run
- player_localnum: the player's slot; player_localentnum: the entity of whose
  view is shown (the player followed, spectating)
- the intermission, the predicted origin (pmove_org) and the view's angles
=================
*/
static void CSQC_SetFrameGlobals (void)
{
	int		viewed = cl.playernum;

	if (cls.mvdplayback)
		viewed = cl.viewplayer;
	else if (cl.spectator && Cam_TrackNum () >= 0)
		viewed = Cam_TrackNum ();

	CSQC_SetFloat ("time", (float)cl.time);
	CSQC_SetFloat ("cltime", (float)(host.realtime - csqc.starttime));
	CSQC_SetFloat ("frametime", (float)cls.frametime);
	CSQC_SetFloat ("clientcommandframe", (float)cls.netchan.outgoing_sequence);
	CSQC_SetFloat ("servercommandframe", (float)cls.netchan.incoming_acknowledged);
	CSQC_SetFloat ("player_localnum", (float)(cls.mvdplayback ? cl.viewplayer : cl.playernum));
	CSQC_SetFloat ("player_localentnum", (float)(viewed + 1));
	CSQC_SetFloat ("intermission", (float)cl.intermission);
	CSQC_SetFloat ("intermission_time", (float)cl.completed_time);
	CSQC_SetVector ("pmove_org", cl.simorg);
	CSQC_SetVector ("view_angles", cl.viewangles);
}

// init(float prevprogs) and initents(float prevprogs) of each progs, as FTE
// calls them
static bool CSQC_CallEach (const char *entry, uint32_t from)
{
	uint32_t	pr;
	qc_value_t	arg;

	for (pr = from ; csqc.qc.vm && pr < QC_NumProgs (csqc.qc.vm) ; pr++)
	{
		arg = QC_ValFloat ((float)pr - 1);
		if (!CSQC_Call (QC_FindFunctionIn (csqc.qc.vm, pr, entry), 1, &arg))
			return false;
	}
	return csqc.qc.vm != NULL;
}

/*
=================
CSQC_RegisterAutocvars

As FTE does: a cvar for each autocvar_<name> global of progs pr that names
none (a vector's _x, _y and _z floats aside), holding the progs' default, so
the player can set it; it stays when CSQC goes, as FTE's do
=================
*/
static void CSQC_RegisterAutocvars (uint32_t pr)
{
	const qc_progs_t	*p = QC_LoadedProgs (csqc.qc.vm, pr);
	qc_definfo_t		d;
	qc_word_t			v[3];
	uint32_t			i, j;
	const char			*cvar;
	char				value[128];
	size_t				len;
	cvar_t				*var;

	for (i = 0 ; p && i < QC_ProgsNumGlobalDefs (p) ; i++)
	{
		if (!QC_ProgsGlobalDefAt (p, i, &d) || strncmp (d.name, "autocvar_", 9))
			continue;
		cvar = d.name + 9;
		len = strlen (cvar);
		if (!len || Cvar_FindVar ((char *)cvar) || Cmd_Exists ((char *)cvar))
			continue;
		for (j = 0 ; j < 3 ; j++)
			if (!QC_ProgsInitialGlobal (p, d.ofs + j, &v[j].u))
				v[j].u = 0;
		switch (d.type)
		{
		case QC_EV_FLOAT:
			if (len >= 2 && cvar[len-2] == '_' && strchr ("xyz", cvar[len-1]))
				continue;
			snprintf (value, sizeof(value), "%g", v[0].f);
			break;
		case QC_EV_INTEGER:
			snprintf (value, sizeof(value), "%i", v[0].i);
			break;
		case QC_EV_VECTOR:
			snprintf (value, sizeof(value), "%g %g %g", v[0].f, v[1].f, v[2].f);
			break;
		case QC_EV_STRING:
			Q_strncpyz (value, QC_ProgsString (p, v[0].u), sizeof(value));
			break;
		default:
			continue;
		}
		var = Mem_Calloc (1, sizeof(*var));
		var->name = Mem_Alloc (len + 1);
		strcpy (var->name, cvar);
		var->string = value;
		var->description = "A setting of the server's client-side QuakeC (CSQC), as it reads it.";
		Cvar_RegisterVariable (var);
	}
}

// a cvar changed: the autocvars that follow it (QuakeC may be running: its
// cvar_set)
static void CSQC_CvarChanged (cvar_t *var)
{
	char	text[1024];

	if (csqc.qc.vm && !QC_SyncAutocvar (csqc.qc.vm, var->name))
		Con_Printf ("CSQC: autocvar_%s: %s\n", var->name, QC_ErrorText (QC_LastError (csqc.qc.vm), text, sizeof(text)));
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

	if (csqc.qc.vm)
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
	csqc.qc.vm = QC_Create (main, csqc.builtins, &config, &csqc_host, &csqc.qc, &err);
	QC_ReleaseProgs (main);
	if (!csqc.qc.vm)
	{
		Con_Printf ("CSQC: %s\n", QC_ErrorText (&err, text, sizeof(text)));
		QC_FreeError (&err);
		if (addon)
			QC_ReleaseProgs (addon);
		return false;
	}
	csqc.trailcarry = Mem_Calloc (QC_MaxEdicts (csqc.qc.vm), sizeof(*csqc.trailcarry));

	// the add-on's init runs as it is added, after the csprogs' own
	CSQC_RegisterAutocvars (0);
	if (!QC_SyncAutocvars (csqc.qc.vm))
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
		csqc.qc.calls++;
		ok = QC_AddProgs (csqc.qc.vm, addon, &pr);
		csqc.qc.calls--;
		QC_ReleaseProgs (addon);
		if (!ok)
		{
			CSQC_Failed ();
			return false;
		}
		CSQC_RegisterAutocvars (pr);
		if (!QC_SyncAutocvars (csqc.qc.vm))
		{
			CSQC_Failed ();
			return false;
		}
	}
	if (!CSQC_CallEach ("initents", 0))
		return false;
	CS_FindFields ();
	csqc.starttime = host.realtime;

	// what FTE sets before CSQC_Init
	CSQC_SetWorldField ("message", QC_EV_STRING, QC_ValWord (QC_Intern (csqc.qc.vm, cl.levelname, strlen (cl.levelname))));
	s = Info_ValueForKey (cl.serverinfo, "map");
	CSQC_SetString ("mapname", *s ? s : *cl.model_name[1] ? cl.model_name[1] : "unknown");
	CSQC_SetFloat ("deathmatch", (float)atoi (Info_ValueForKey (cl.serverinfo, "deathmatch")));
	CSQC_SetFloat ("coop", !atoi (Info_ValueForKey (cl.serverinfo, "deathmatch"))
		&& atoi (Info_ValueForKey (cl.serverinfo, "maxclients")) > 1 ? 1.0f : 0.0f);
	CSQC_SetFloat ("maxclients", MAX_CLIENTS);
	CSQC_SetFrameGlobals ();

	args[0] = QC_ValFloat (CSQC_API_VERSION);
	args[1] = QC_ValWord (QC_TempString (csqc.qc.vm, "SoftWorld", 9));
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
	if (!csqc.qc.vm || csqc.worldloaded)
		return;
	csqc.worldloaded = true;

	QC_SetProtected (csqc.qc.vm, 0, false);
	CSQC_SetWorldField ("solid", QC_EV_FLOAT, QC_ValFloat (SOLID_BSP));
	CSQC_SetWorldField ("modelindex", QC_EV_FLOAT, QC_ValFloat (1));
	CSQC_SetWorldField ("model", QC_EV_STRING, QC_ValWord (QC_HostString (csqc.qc.vm, cl.model_name[1])));

	csqc.entitydata = cl.map ? CM_EntityString (cl.map) : NULL;
	CSQC_Call (CSQC_Entry ("CSQC_WorldLoaded"), 0, NULL);
	csqc.entitydata = NULL;
	if (csqc.qc.vm)
		QC_SetProtected (csqc.qc.vm, 0, true);
}

/*
=================
CSQC_Announce

As FTE does as the world has loaded: the server sends CSQC's entities and
events (and takes its own entities out of the rest) to a client that says
CSQC runs, again on each map
=================
*/
void CSQC_Announce (void)
{
	if (!(cls.fteext & FTE_PEXT_CSQC) || cls.demoplayback)
		return;
	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
	MSG_WriteString (&cls.netchan.message, csqc.qc.vm ? "enablecsqc" : "disablecsqc");
}

bool CSQC_Inited (void)
{
	return csqc.qc.vm != NULL;
}

/*
==============================================================================

THE SERVER'S ENTITIES AND EVENTS

==============================================================================
*/

static void CSQC_SetSelf (qc_ent_t e)
{
	uint32_t	word, type;

	if (QC_FindGlobal (csqc.qc.vm, "self", &word, &type))
		QC_Globals (csqc.qc.vm)[word].u = e;
}

static qc_ent_t CSQC_Self (void)
{
	uint32_t	word, type;

	return QC_FindGlobal (csqc.qc.vm, "self", &word, &type) ? QC_Globals (csqc.qc.vm)[word].u : 0;
}

// an entity the server removed: CSQC_Ent_Remove's to remove, else gone
static void CSQC_EntRemove (qc_ent_t e)
{
	qc_func_t	f = CSQC_Entry ("CSQC_Ent_Remove");

	if (!f)
	{
		QC_Remove (csqc.qc.vm, e, false);
		return;
	}
	CSQC_SetSelf (e);
	CSQC_Call (f, 0, NULL);
}

// a sized message's end: what QuakeC left unread is skipped, what it read past
// is read again
static void CSQC_EndSized (const char *what, unsigned num, int start, int size)
{
	if (msg_readcount != start + size)
		Con_DPrintf ("CSQC %s %u: %i bytes, %i read\n", what, num, size, msg_readcount - start);
	msg_readcount = start + size;
}

/*
=================
CSQC_ParseEntities

svc_fte_csqcentities, as FTE reads it: each entity's number (with 0x8000 the
server removed it) and what its SendEntity wrote, which CSQC_Ent_Update reads
with self the entity, isnew if CSQC didn't hold it; 0 ends it. sized has the
data's size after the number. CSQC failing leaves the message unreadable.
=================
*/
void CSQC_ParseEntities (bool sized)
{
	unsigned	num;
	int			start = 0, size = 0;
	bool		remove;
	qc_func_t	update, spawn;
	qc_value_t	arg;
	qc_ent_t	e;

	if (!csqc.qc.vm)
		Host_EndGame ("The server sends CSQC entities, but CSQC isn't running");
	update = CSQC_Entry ("CSQC_Ent_Update");
	if (!update)
		Host_EndGame ("CSQC has no CSQC_Ent_Update");
	if (!csqc.worldloaded)
		Host_EndGame ("CSQC entities came before the world");
	spawn = CSQC_Entry ("CSQC_Ent_Spawn");
	CSQC_SetFrameGlobals ();

	for (;;)
	{
		num = (unsigned)MSG_ReadShort () & 0xffff;
		remove = (num & 0x8000) != 0;
		num &= ~0x8000u;
		if ((!num && !remove) || msg_badread)
			break;
		if (num >= MAX_EDICTS)
			Host_EndGame ("CSQC entity %u is out of range", num);

		if (remove)
		{
			if (!num)
				Host_EndGame ("CSQC can't remove the world");
			e = csqc.ents[num];
			csqc.ents[num] = 0;
			if (e)
				CSQC_EntRemove (e);
			if (!csqc.qc.vm)
				Host_EndGame ("CSQC failed removing entity %u", num);
			continue;
		}

		if (sized)
		{
			size = MSG_ReadShort () & 0xffff;
			start = msg_readcount;
		}
		e = csqc.ents[num];
		arg = QC_ValFloat (e ? 0.0f : 1.0f);
		if (!e)
		{
			if (spawn)
			{
				// CSQC_Ent_Spawn makes it, in self
				qc_value_t	n = QC_ValFloat ((float)num);

				CSQC_SetSelf (0);
				if (!CSQC_Call (spawn, 1, &n))
					Host_EndGame ("CSQC failed making entity %u", num);
				e = CSQC_Self ();
			}
			else if (!QC_Spawn (csqc.qc.vm, &e))
				Host_EndGame ("CSQC has no room for entity %u", num);
			else
				CSQC_SetEntityFloat (e, "entnum", (float)num);
			csqc.ents[num] = e;
		}

		CSQC_SetSelf (e);
		csqc.mayread = true;
		CSQC_Call (update, 1, &arg);
		csqc.mayread = false;
		if (!csqc.qc.vm)
			Host_EndGame ("CSQC failed reading entity %u", num);
		if (spawn)
			csqc.ents[num] = CSQC_Self ();	// it may have made another
		if (sized)
			CSQC_EndSized ("entity", num, start, size);
	}
}

/*
=================
CSQC_ParseEvent

svc_fte_cgamepacket: what the server's QuakeC wrote to CSQC, all of it read by
CSQC_Parse_Event (or, as FTE has it, CSQC_Parse_TempEntity without one); sized
has its size first, so without CSQC it is skipped
=================
*/
void CSQC_ParseEvent (bool sized)
{
	qc_func_t	f = 0;
	int			start = 0, size = 0;

	if (csqc.qc.vm && !(f = CSQC_Entry ("CSQC_Parse_Event")))
		f = CSQC_Entry ("CSQC_Parse_TempEntity");

	if (sized)
	{
		size = MSG_ReadShort () & 0xffff;
		start = msg_readcount;
	}
	if (!f)
	{
		if (!sized)
			Host_EndGame ("The server sends CSQC events, but %s", csqc.qc.vm ? "CSQC has no CSQC_Parse_Event"
				: "CSQC isn't running");
		msg_readcount = start + size;
		return;
	}
	CSQC_SetFrameGlobals ();
	csqc.mayread = true;
	CSQC_Call (f, 0, NULL);
	csqc.mayread = false;
	if (!csqc.qc.vm && !sized)
		Host_EndGame ("CSQC failed reading an event");
	if (sized)
		CSQC_EndSized ("event", 0, start, size);
}

/*
=================
CSQC_ParseTempEntity

svc_temp_entity, to CSQC_Parse_TempEntity first: it returns true if it read
the temp entity; else the client reads it from the start, as FTE has it
=================
*/
bool CSQC_ParseTempEntity (void)
{
	qc_func_t	f = csqc.qc.vm ? CSQC_Entry ("CSQC_Parse_TempEntity") : 0;
	int			start = msg_readcount;
	qc_value_t	ret;
	bool		ok;

	if (!f)
		return false;
	CSQC_SetFrameGlobals ();
	csqc.mayread = true;
	ok = CSQC_CallRet (f, 0, NULL, &ret);
	csqc.mayread = false;
	if (ok && CSQC_RetFloat (&ret))
		return true;
	msg_readcount = start;
	msg_badread = false;
	return false;
}

/*
=================
CSQC_EventSound

A sound the server plays, to CSQC_Event_Sound (entity, channel, sample,
volume, attenuation, origin, pitch percent, flags), with self the entity if
CSQC holds it, as FTE calls it; true if CSQC took the sound
=================
*/
bool CSQC_EventSound (int ent, int channel, const char *sample, float vol, float attenuation, const vec3_t pos)
{
	qc_func_t	f = csqc.qc.vm ? CSQC_Entry ("CSQC_Event_Sound") : 0;
	qc_value_t	args[8], ret;

	if (!f)
		return false;
	CSQC_SetFrameGlobals ();
	CSQC_SetSelf (ent > 0 && ent < MAX_EDICTS ? csqc.ents[ent] : 0);
	args[0] = QC_ValFloat ((float)ent);
	args[1] = QC_ValFloat ((float)channel);
	args[2] = QC_ValWord (QC_TempString (csqc.qc.vm, sample, strlen (sample)));
	args[3] = QC_ValFloat (vol);
	args[4] = QC_ValFloat (attenuation);
	args[5] = QC_ValVector (pos[0], pos[1], pos[2]);
	args[6] = QC_ValFloat (100);
	args[7] = QC_ValFloat (0);
	return CSQC_CallRet (f, 8, args, &ret) && CSQC_RetFloat (&ret);
}

/*
=================
CSQC_InputFrame

A command about to be sent, through CSQC_Input_Frame in the input_ globals,
which it may change, as FTE has it
=================
*/
void CSQC_InputFrame (usercmd_t *cmd)
{
	qc_func_t	f = csqc.qc.vm ? CSQC_Entry ("CSQC_Input_Frame") : 0;
	uint32_t	word, type;
	qc_word_t	*g;
	float		msec;

	if (!f)
		return;
	CSQC_SetFrameGlobals ();
	CS_SetInput (cmd, cls.netchan.outgoing_sequence);
	if (!CSQC_Call (f, 0, NULL))
		return;

	g = QC_Globals (csqc.qc.vm);
	if (QC_FindGlobal (csqc.qc.vm, "input_timelength", &word, &type) && type == QC_EV_FLOAT)
	{
		msec = g[word].f * 1000;
		cmd->msec = (byte)(msec < 0 ? 0 : msec > 255 ? 255 : msec);
	}
	if (QC_FindGlobal (csqc.qc.vm, "input_angles", &word, &type) && type == QC_EV_VECTOR)
	{
		cmd->angles[0] = g[word].f;
		cmd->angles[1] = g[word + 1].f;
		cmd->angles[2] = g[word + 2].f;
	}
	if (QC_FindGlobal (csqc.qc.vm, "input_movevalues", &word, &type) && type == QC_EV_VECTOR)
	{
		cmd->forwardmove = (short)g[word].f;
		cmd->sidemove = (short)g[word + 1].f;
		cmd->upmove = (short)g[word + 2].f;
	}
	if (QC_FindGlobal (csqc.qc.vm, "input_buttons", &word, &type) && type == QC_EV_FLOAT)
		cmd->buttons = (byte)g[word].f;
	if (QC_FindGlobal (csqc.qc.vm, "input_impulse", &word, &type) && type == QC_EV_FLOAT)
		cmd->impulse = (byte)g[word].f;
}

/*
==============================================================================

THE VIEW

==============================================================================
*/

// CSQC draws the view (it has CSQC_UpdateView)
bool CSQC_DrawsView (void)
{
	return csqc.qc.vm && CSQC_Entry ("CSQC_UpdateView");
}

/*
=================
CSQC_DrawView

CSQC_UpdateView (width, height, notmenu) draws the view, from the client's view
of the frame and its entities (which clearscene goes back to); false when
CSQC doesn't, for the client to. If CSQC fails, the client's view is drawn
after all. *sbar is whether the status bar is drawn.
=================
*/
bool CSQC_DrawView (bool *sbar)
{
	qc_func_t	f = csqc.qc.vm ? CSQC_Entry ("CSQC_UpdateView") : 0;
	qc_value_t	args[3];
	bool		ok;

	*sbar = true;
	if (!f || !V_SetupView ())
		return false;

	csqc.numengine = cl.numvisedicts;
	memcpy (csqc.engine, cl.visedicts, cl.numvisedicts * sizeof(cl.visedicts[0]));
	VectorCopy (r_refdef.vieworg, csqc.vieworg);
	VectorCopy (r_refdef.viewangles, csqc.viewangles);
	csqc.enginegun = r_scene.drawviewmodel;
	csqc.sbar = false;
	csqc.crosshair = false;
	csqc.rendered = false;
	csqc.numlights = 0;

	CSQC_SetFrameGlobals ();
	args[0] = QC_ValFloat ((float)vid.conwidth);
	args[1] = QC_ValFloat ((float)vid.conheight);
	args[2] = QC_ValFloat (cls.key_dest != key_menu ? 1.0f : 0.0f);
	csqc.drawing = true;
	ok = CSQC_Call (f, 3, args);
	csqc.drawing = false;

	if (!ok && !csqc.rendered)
	{
		// the client's view after all
		cl.numvisedicts = csqc.numengine;
		memcpy (cl.visedicts, csqc.engine, csqc.numengine * sizeof(cl.visedicts[0]));
		CL_UpdateTEnts ();
		VectorCopy (csqc.vieworg, r_refdef.vieworg);
		VectorCopy (csqc.viewangles, r_refdef.viewangles);
		r_scene.viewent = &cl.viewent;
		r_scene.drawviewmodel = csqc.enginegun;
		V_DrawView (true);
		csqc.drewsbar = true;
	}
	*sbar = csqc.rendered ? csqc.drewsbar : !ok;
	r_scene.viewent = &cl.viewent;
	return true;
}

/*
=================
CSQC_DrawHud, CSQC_DrawScores

QuakeSpasm-Spiked's simple CSQC, as FTE runs it: a progs without
CSQC_UpdateView may draw the status bar, CSQC_DrawHud (virtsize, showscores)
in place of the client's, and the scores, CSQC_DrawScores, where the client
draws its scoreboard or intermission. False when it doesn't, for the client to.
=================
*/
static bool CSQC_DrawSimple (const char *entry)
{
	qc_func_t	f;
	qc_value_t	args[2];

	if (!csqc.qc.vm || CSQC_Entry ("CSQC_UpdateView") || !(f = CSQC_Entry (entry)))
		return false;
	CSQC_SetFrameGlobals ();
	args[0] = QC_ValVector ((float)vid.conwidth, (float)vid.conheight, 0);
	args[1] = QC_ValFloat (Sbar_ShowingScores () ? 1.0f : 0.0f);
	Draw_ResetClipArea ();
	CSQC_Call (f, 2, args);
	Draw_ResetClipArea ();
	return true;
}

bool CSQC_DrawHud (void)
{
	return CSQC_DrawSimple ("CSQC_DrawHud");
}

bool CSQC_DrawScores (void)
{
	return CSQC_DrawSimple ("CSQC_DrawScores");
}

// the VM and everything of it gone
static void CSQC_Destroy (void)
{
	QC_Destroy (csqc.qc.vm);
	csqc.qc.vm = NULL;
	if (csqc.trailcarry)
		Mem_Free (csqc.trailcarry);
	csqc.trailcarry = NULL;
	csqc.worldloaded = false;
	csqc.mayread = false;
	csqc.drawing = false;
	csqc.numlights = 0;
	memset (csqc.ents, 0, sizeof(csqc.ents));
	memset (csqc.modelnames, 0, sizeof(csqc.modelnames));
	memset (csqc.models, 0, sizeof(csqc.models));
	csqc.entitydata = NULL;
	if (csqc.entitycopy)
		Mem_Free (csqc.entitycopy);
	csqc.entitycopy = NULL;
	CLQC_RemoveCommands (&csqc.qc);
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
	if (!csqc.qc.vm)
		return;
	// an error the client longjmped out of while QuakeC ran: its calls are
	// abandoned, and QuakeC isn't called again
	if (csqc.qc.calls)
	{
		QC_Abandon (csqc.qc.vm);
		csqc.qc.calls = 0;
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
	qcvm_t			*vm = csqc.qc.vm;
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
	if (vm != csqc.qc.vm)
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

	csqc.qc.name = "CSQC";
	csqc.qc.description = "A command of the client-side QuakeC.";
	csqc.qc.command = CSQC_Command;
	CLQC_InitHost (&csqc_host);
	csqc_host.load_progs = CSQC_LoadAddon;
	csqc_host.on_remove = CSQC_OnRemove;

	csqc.builtins = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	if (!csqc.builtins || !CLQC_DrawBuiltins (csqc.builtins))
		Sys_Error ("CSQC_RegisterVariables: out of memory");
	for (i = 0 ; i < sizeof(csqc_builtins) / sizeof(csqc_builtins[0]) ; i++)
		if (!QC_BuiltinsSet (csqc.builtins, csqc_builtins[i].name, csqc_builtins[i].func))
			Sys_Error ("CSQC_RegisterVariables: out of memory");
}
