/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

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
// p_script.c -- the particle scripts: FTE's (particles/<name>.cfg, blocks of
// r_part) and DarkPlaces' (effectinfo.txt) read into particle types, each
// named in its script's namespace. r_particledesc names the scripts loaded;
// one a QuakeC names as "<script>.<effect>" is loaded then, weakly: it doesn't
// replace an effect r_particledesc's have.

#include "p_local.h"

p_host_t	p_host;
p_type_t	*p_types;
int			p_numtypes;
p_type_t	*p_runlist;
bool		p_looksdirty;

// id's particles of a palette color, by count (P_RunPaletteEffect), and
// trails of one
int		pe_default = P_INVALID, pe_size2 = P_INVALID, pe_size3 = P_INVALID, pe_defaulttrail = P_INVALID;

cvar_t	r_particledesc = {.name = "r_particledesc", .string = "classic", .archive = true,
	.description = "The particle scripts loaded, by name: particles/<name>.cfg or <name>.cfg, or effectinfo "
		"(DarkPlaces' effectinfo.txt); classic loads none, leaving id's particles."};
cvar_t	r_part_rain = {.name = "r_part_rain", .string = "1", .archive = true,
	.description = "Particles from the surfaces scripts give effects, as rain from the sky."};
cvar_t	r_part_rain_quantity = {.name = "r_part_rain_quantity", .string = "1", .archive = true,
	.description = "How many particles surfaces make, as a multiple."};
cvar_t	r_part_density = {.name = "r_part_density", .string = "1", .archive = true,
	.description = "How many particles scripted effects make, as a multiple."};
cvar_t	r_part_maxparticles = {.name = "r_part_maxparticles", .string = "65536", .archive = true,
	.description = "Scripted particles at most at once, from the next level."};
cvar_t	r_part_maxdecals = {.name = "r_part_maxdecals", .string = "8192", .archive = true,
	.description = "Pieces of decals at most at once, from the next level."};
cvar_t	r_part_sparks = {.name = "r_part_sparks", .string = "1", .archive = true,
	.description = "Sparks: lines along their way; 0 none, as textured sparks or triangles when those are off."};
cvar_t	r_part_sparks_trifan = {.name = "r_part_sparks_trifan", .string = "1", .archive = true,
	.description = "Sparks drawn as triangles, else as lines."};
cvar_t	r_part_sparks_textured = {.name = "r_part_sparks_textured", .string = "1", .archive = true,
	.description = "Sparks drawn with their images, else as lines."};
cvar_t	r_part_beams = {.name = "r_part_beams", .string = "1", .archive = true,
	.description = "Particle beams drawn."};
cvar_t	r_part_contentswitch = {.name = "r_part_contentswitch", .string = "1", .archive = true,
	.description = "Effects look at what they start in: water, slime or lava."};
cvar_t	r_bouncysparks = {.name = "r_bouncysparks", .string = "1", .archive = true,
	.description = "Particles hit walls: they bounce, splash or leave a mark."};
cvar_t	r_particle_tracelimit = {.name = "r_particle_tracelimit", .string = "2147483647",
	.description = "Particles a frame at most that look for walls."};
cvar_t	r_decal_noperpendicular = {.name = "r_decal_noperpendicular", .string = "1",
	.description = "Decals only on the surfaces they face, without fading."};
cvar_t	r_lightflicker = {.name = "r_lightflicker", .string = "1", .archive = true,
	.description = "Effects' lights flicker."};

// the scripts loaded, by name (namespace)
typedef struct p_config_s
{
	struct p_config_s	*next;
	char	name[MAX_QPATH];
} p_config_t;

static p_config_t	*p_configs;
static char			p_mapname[MAX_QPATH];
static char			p_lastdesc[256];

// FTE's old names
static const struct
{
	const char	*from, *to;
} p_legacynames[] =
{
	{"t_rocket",	"TR_ROCKET"},
	{"t_grenade",	"TR_GRENADE"},
	{"t_gib",		"TR_BLOOD"},
	{"te_plasma",	"TE_TEI_PLASMAHIT"},
	{"te_smoke",	"TE_TEI_SMOKE"},
};

// r_partredirect's: an effect's name looked up as another's
typedef struct p_alias_s
{
	struct p_alias_s	*next;
	char	from[MAX_QPATH];
	char	to[MAX_QPATH];
} p_alias_t;

static p_alias_t	*p_aliases;

// r_trail's and r_effect's: a model's effects
typedef struct p_modeleffect_s
{
	struct p_modeleffect_s	*next;
	char		model[MAX_QPATH];
	char		effect[MAX_QPATH];
	bool		trail;
	unsigned	flags;				// P_EMIT
} p_modeleffect_t;

static p_modeleffect_t	*p_modeleffects;

static bool	P_LoadParticleSet (const char *name, bool weak, bool warn);

/*
==============================================================================

LINES AND WORDS

==============================================================================
*/

#define	P_MAXARGS	16

// a line of a script as words
typedef struct
{
	int		argc;
	char	*argv[P_MAXARGS];
	char	text[1024];
} p_args_t;

// the words of a line: a quoted string is one, and // ends the line
static void P_Tokenize (p_args_t *args, const char *line)
{
	char	*out = args->text, *end = args->text + sizeof(args->text) - 1;

	args->argc = 0;
	while (args->argc < P_MAXARGS && out < end)
	{
		while (*line && (byte)*line <= ' ')
			line++;
		if (!*line || (line[0] == '/' && line[1] == '/'))
			break;
		args->argv[args->argc++] = out;
		if (*line == '"')
		{
			for (line++ ; *line && *line != '"' && out < end ; )
				*out++ = *line++;
			if (*line == '"')
				line++;
		}
		else
			while ((byte)*line > ' ' && out < end)
				*out++ = *line++;
		*out++ = 0;
	}
}

static const char *P_Arg (const p_args_t *args, int i)
{
	return i < args->argc ? args->argv[i] : "";
}

static float P_ArgF (const p_args_t *args, int i)
{
	return (float)atof (P_Arg (args, i));
}

// the next line of data from *offset, without its leading white space; NULL at the end
static const char *P_ReadLine (char *line, size_t size, const char *data, size_t length, size_t *offset)
{
	const char	*start = data + *offset, *p = start, *end = data + length;
	size_t		n;

	if (p >= end)
		return NULL;
	while (p < end && *p++ != '\n')
		;
	*offset = (size_t)(p - data);
	n = (size_t)(p - start) < size - 1 ? (size_t)(p - start) : size - 1;
	memcpy (line, start, n);
	line[n] = 0;
	for (p = line ; *p && (byte)*p <= ' ' ; p++)
		;
	return p;
}

/*
==============================================================================

TYPES

==============================================================================
*/

static const char *P_LegacyName (const char *name)
{
	int		i;

	for (i = 0 ; i < (int)(sizeof(p_legacynames) / sizeof(p_legacynames[0])) ; i++)
		if (!strcmp (name, p_legacynames[i].from))
			return p_legacynames[i].to;
	return name;
}

// "<script>.<effect>" into the script, else config, and the effect
static const char *P_SplitName (const char *name, const char *config, char *buf)
{
	const char	*dot = strchr (name, '.');

	if (dot && dot - name < MAX_QPATH - 1)
	{
		memcpy (buf, name, (size_t)(dot - name));
		buf[dot - name] = 0;
		return dot + 1;
	}
	Q_strncpyz (buf, config, MAX_QPATH);
	return name;
}

/*
=================
P_GetParticleType

The type of a name in a script, made if there is none. The types may move:
pointers into them are stale after.
=================
*/
static p_type_t *P_GetParticleType (const char *config, const char *name)
{
	p_type_t	*t, *old = p_types;
	char		cfg[MAX_QPATH];
	int			i;

	name = P_LegacyName (P_SplitName (name, config, cfg));
	for (i = 0 ; i < p_numtypes ; i++)
		if (!Q_strcasecmp (p_types[i].name, name) && !Q_strcasecmp (p_types[i].config, cfg))
			return &p_types[i];

	p_types = Mem_Realloc (p_types, sizeof(*p_types) * (size_t)(p_numtypes + 1));
	if (old && p_types != old)
	{
		// the run list points into them
		if (p_runlist)
			p_runlist = p_types + (p_runlist - old);
		for (i = 0 ; i < p_numtypes ; i++)
			if (p_types[i].nexttorun)
				p_types[i].nexttorun = p_types + (p_types[i].nexttorun - old);
	}
	t = &p_types[p_numtypes++];
	memset (t, 0, sizeof(*t));
	Q_strncpyz (t->name, name, sizeof(t->name));
	Q_strncpyz (t->config, cfg, sizeof(t->config));
	t->assoc = t->inwater = t->cliptype = t->emit = P_INVALID;
	p_looksdirty = true;
	return t;
}

int P_AllocateParticleType (const char *config, const char *name)
{
	return (int)(P_GetParticleType (config, name) - p_types);
}

// to as a copy of from, but in colors codes "<index>_<count>" names: id's
// TE_EXPLOSION2 of a palette range
static void P_RetintEffect (p_type_t *to, const p_type_t *from, const char *codes)
{
	char	name[sizeof(to->name)], config[sizeof(to->config)];
	char	*end;

	memcpy (name, to->name, sizeof(name));
	memcpy (config, to->config, sizeof(config));
	*to = *from;
	memcpy (to->name, name, sizeof(name));
	memcpy (to->config, config, sizeof(config));

	// lists of its own, and none of the particles from's are
	if (to->sounds)
	{
		to->sounds = Mem_Alloc (sizeof(*to->sounds) * (size_t)to->numsounds);
		memcpy (to->sounds, from->sounds, sizeof(*to->sounds) * (size_t)to->numsounds);
	}
	if (to->ramp)
	{
		to->ramp = Mem_Alloc (sizeof(*to->ramp) * (size_t)to->rampindexes);
		memcpy (to->ramp, from->ramp, sizeof(*to->ramp) * (size_t)to->rampindexes);
	}
	to->nexttorun = NULL;
	to->inrunlist = false;
	to->particles = NULL;
	to->clippeddecals = NULL;
	to->beams = NULL;
	p_looksdirty = true;

	to->colorindex = (int)strtoul (codes, &end, 10);
	if (*end == '_')
		end++;
	to->colorrand = (int)strtoul (end, NULL, 10);
}

/*
=================
P_FindParticleType

The type of an effect's name: r_partredirect's alias, else the name in its
namespace, which is loaded when it isn't, or without one any loaded type of
the name; te_explosion2_<index>_<count> is te_explosion2 in those colors
=================
*/
int P_FindParticleType (const char *fullname)
{
	const p_alias_t	*a;
	const char		*name = fullname;
	char			cfg[MAX_QPATH];
	int				i, found = P_INVALID, from, to, depth = 5;

	for (a = p_aliases ; a ; )
	{
		if (Q_strcasecmp (a->from, name))
		{
			a = a->next;
			continue;
		}
		if (depth-- <= 0)
			return P_INVALID;
		name = a->to;
		a = p_aliases;
	}

	name = P_LegacyName (P_SplitName (name, "", cfg));
	for (i = 0 ; i < p_numtypes ; i++)
	{
		if (Q_strcasecmp (p_types[i].name, name))
			continue;
		if (*cfg)
		{
			if (!Q_strcasecmp (p_types[i].config, cfg))
			{
				found = i;
				break;
			}
		}
		else
		{
			found = i;
			if (p_types[i].loaded)
				break;
		}
	}
	if (found != P_INVALID && p_types[found].loaded)
		return found;

	if (!Q_strncasecmp (name, "te_explosion2_", 14))
	{
		from = P_FindParticleType (va ("%s.te_explosion2", cfg));
		if (from != P_INVALID)
		{
			to = P_AllocateParticleType (cfg, name);
			P_RetintEffect (&p_types[to], &p_types[from], name + 14);
			return to;
		}
	}
	if (*cfg && P_LoadParticleSet (cfg, true, true))
		return P_FindParticleType (fullname);
	return P_INVALID;
}

// to of config as an assoc of type from: P_INVALID if that loops
static int P_CheckAssociation (const char *config, const char *name, int from)
{
	int		to, first;

	first = to = P_AllocateParticleType (config, name);
	for ( ; to != P_INVALID ; to = p_types[to].assoc)
		if (to == from)
		{
			Con_Printf ("Association of %s would loop\n", name);
			return P_INVALID;
		}
	return first;
}

// the type's image, else the one QuakeSpasm-Spiked makes for its kind
static void P_LoadImage (p_type_t *t)
{
	int		fallback;
	bool	found;

	if (t->looks.type == PT_SPARK)
		fallback = RPI_WHITE;
	else if (t->looks.type == PT_BEAM)
		fallback = RPI_BEAM;
	else if (t->looks.type == PT_SPARKFAN)
		fallback = RPI_FAN;
	else if (strstr (t->texname, "classicparticle"))
		fallback = RPI_CLASSIC;
	else if (strstr (t->texname, "glow") || strstr (t->texname, "ball") || t->looks.type == PT_TEXTUREDSPARK)
		fallback = RPI_BALL;
	else
		fallback = RPI_FUZZY;
	t->looks.image = R_ParticleImage (t->texname, fallback, &found);
	if (!found)
	{
		t->s1 = t->t1 = 0;
		t->s2 = t->t2 = fallback == RPI_CLASSIC ? 0.5f : 1;
		t->randsmax = 1;
	}
}

// out of the run list, if it is in it
static void P_RemoveFromRunList (p_type_t *t)
{
	p_type_t	**link;

	if (!t->inrunlist)
		return;
	for (link = &p_runlist ; *link ; link = &(*link)->nexttorun)
		if (*link == t)
		{
			*link = t->nexttorun;
			break;
		}
	t->nexttorun = NULL;
	t->inrunlist = false;
}

/*
=================
P_ResetToDefaults

The type as a script starts it: its particles gone, its beams dead
=================
*/
static void P_ResetToDefaults (p_type_t *t)
{
	p_particle_t	*p;
	p_beamseg_t		*b, *beams;
	char			name[sizeof(t->name)], config[sizeof(t->config)];

	// the beams are freed as they are run next
	for (b = t->beams ; b ; b = b->next)
		b->flags |= BS_DEAD;
	while ((p = t->particles))
	{
		t->particles = p->next;
		p->next = p_freeparticles;
		p_freeparticles = p;
	}
	P_RemoveFromRunList (t);

	beams = t->beams;
	memcpy (name, t->name, sizeof(name));
	memcpy (config, t->config, sizeof(config));
	Mem_Free (t->ramp);
	Mem_Free (t->sounds);
	memset (t, 0, sizeof(*t));

	t->beams = beams;
	memcpy (t->name, name, sizeof(name));
	memcpy (t->config, config, sizeof(config));
	t->rainfrequency = 1;
	t->assoc = t->inwater = t->cliptype = t->emit = P_INVALID;
	t->fluidmask = P_CONT_FLUID;
	t->alpha = 1;
	t->alphachange = 1;
	t->clipbounce = 0.8f;
	t->clipcount = 1;
	t->colorindex = -1;
	t->rotationstartmin = (float)-Q_PI;		// a random angle
	t->rotationstartrand = (float)(2 * Q_PI);
	t->spawnchance = 1;
	VectorSet (t->dl_rgb, 1, 1, 1);
	t->looks.stretch = 0.05f;
	t->randsmax = 1;
	t->s2 = 1;
	t->t2 = 1;
}

// what scripts leave to it, worked out; and the cvars' choices of shapes
static void P_FinishParticleType (p_type_t *t)
{
	if (t->gravity || t->veladd || t->spawnvel || t->spawnvelvert || DotProduct (t->velwrand, t->velwrand)
		|| DotProduct (t->velbias, t->velbias) || t->flurry)
		t->flags |= PT_VELOCITY;
	if (DotProduct (t->velbias, t->velbias) || DotProduct (t->velwrand, t->velwrand)
		|| DotProduct (t->orgwrand, t->orgwrand))
		t->flags |= PT_WORLDSPACERAND;
	if (t->friction[0] || t->friction[1] || t->friction[2])
		t->flags |= PT_FRICTION;

	P_LoadImage (t);
	if (t->dl_decay && !t->dl_time)
		t->dl_time = t->dl_radius[0] / t->dl_decay;
	if (t->looks.scalefactor > 1 && !t->looks.invscalefactor)
	{
		t->scale *= t->looks.scalefactor;
		t->scalerand *= t->looks.scalefactor;
		t->looks.scalefactor = 1;		// ramps keep their sizes
	}
	t->looks.invscalefactor = 1 - t->looks.scalefactor;

	if (t->looks.type == PT_TEXTUREDSPARK && !t->looks.stretch)
		t->looks.stretch = 0.05f;
	if (t->looks.type == PT_SPARK && r_part_sparks.value < 0)
		t->looks.type = PT_INVISIBLE;
	if (t->looks.type == PT_TEXTUREDSPARK && !r_part_sparks_textured.value)
		t->looks.type = PT_SPARK;
	if (t->looks.type == PT_SPARKFAN && !r_part_sparks_trifan.value)
		t->looks.type = PT_SPARK;
	if (t->looks.type == PT_SPARK && !r_part_sparks.value)
		t->looks.type = PT_INVISIBLE;
	if (t->looks.type == PT_BEAM && r_part_beams.value <= 0)
		t->looks.type = PT_INVISIBLE;

	if (t->rampmode && !t->ramp)
	{
		t->rampmode = RAMP_NONE;
		Con_Printf ("%s.%s: a ramp mode but no ramp\n", t->config, t->name);
	}
	else if (t->ramp && !t->rampmode)
		Con_Printf ("%s.%s: a ramp but no ramp mode\n", t->config, t->name);
	p_looksdirty = true;
}

/*
==============================================================================

MODEL EFFECTS AND ALIASES

==============================================================================
*/

// r_trail <model> <effect>, r_effect <model> <effect> [replace] [forwards]
static void P_AssociateEffect (const p_args_t *args)
{
	const char		*model = P_Arg (args, 1), *effect = P_Arg (args, 2), *flag;
	p_modeleffect_t	*me;
	bool			trail = !strcmp (P_Arg (args, 0), "r_trail");
	unsigned		flags = 0;
	int				i;

	for (i = 3 ; !trail && i < args->argc ; i++)
	{
		flag = args->argv[i];
		if (!strcmp (flag, "replace") || !strcmp (flag, "1"))
			flags |= P_EMITREPLACE;
		else if (!strcmp (flag, "forwards") || !strcmp (flag, "forward"))
			flags |= P_EMITFORWARDS;
		else if (strcmp (flag, "0"))
			Con_DPrintf ("%s %s: unknown flag %s\n", P_Arg (args, 0), model, flag);
	}

	// big effects on these would show where they are through walls
	if (strstr (model, "player") || strstr (model, "eyes") || strstr (model, "flag") || strstr (model, "tf_stan")
		|| strstr (model, ".bsp") || strstr (model, "turr"))
	{
		Con_Printf ("Effects can't be given to \"%s\"\n", model);
		return;
	}
	if (strlen (model) >= MAX_QPATH || strlen (effect) >= MAX_QPATH)
		return;

	for (me = p_modeleffects ; me ; me = me->next)
		if (!strcmp (me->model, model) && me->trail == trail)
			break;
	if (!me)
	{
		me = Mem_Calloc (1, sizeof(*me));
		Q_strncpyz (me->model, model, sizeof(me->model));
		me->trail = trail;
		me->next = p_modeleffects;
		p_modeleffects = me;
	}
	Q_strncpyz (me->effect, effect, sizeof(me->effect));
	me->flags = flags;
	p_looksdirty = true;
}

int P_ModelTrail (const char *modelname)
{
	const p_modeleffect_t	*me;

	for (me = p_modeleffects ; me ; me = me->next)
		if (me->trail && !strcmp (me->model, modelname))
			return P_FindParticleType (me->effect);
	return P_INVALID;
}

int P_ModelEmit (const char *modelname, unsigned *emitflags)
{
	const p_modeleffect_t	*me;

	*emitflags = 0;
	for (me = p_modeleffects ; me ; me = me->next)
		if (!me->trail && !strcmp (me->model, modelname))
		{
			*emitflags = me->flags;
			return P_FindParticleType (me->effect);
		}
	return P_INVALID;
}

// r_partredirect <from> <to>: from looked up as to; without to, what from is;
// with nothing, all of them
static void P_Redirect (const p_args_t *args)
{
	const char	*from = P_Arg (args, 1), *to = P_Arg (args, 2);
	p_alias_t	**link, *a;

	if (!*from)
	{
		for (a = p_aliases ; a ; a = a->next)
			Con_Printf ("%s -> %s\n", a->from, a->to);
		return;
	}
	for (link = &p_aliases ; (a = *link) ; link = &a->next)
		if (!Q_strcasecmp (a->from, from))
		{
			if (args->argc == 2)
			{
				Con_Printf ("%s is %s\n", a->from, a->to);
				return;
			}
			*link = a->next;
			Mem_Free (a);
			break;
		}
	if (*to && Q_strcasecmp (from, to))
	{
		a = Mem_Calloc (1, sizeof(*a));
		Q_strncpyz (a->from, from, sizeof(a->from));
		Q_strncpyz (a->to, to, sizeof(a->to));
		a->next = p_aliases;
		p_aliases = a;
	}
	p_looksdirty = true;
}

static void P_Redirect_f (void)
{
	p_args_t	args;
	int			i;

	args.argc = Cmd_Argc () < P_MAXARGS ? Cmd_Argc () : P_MAXARGS;
	for (i = 0 ; i < args.argc ; i++)
		args.argv[i] = Cmd_Argv (i);
	P_Redirect (&args);
}

/*
==============================================================================

FTE'S SCRIPTS

==============================================================================
*/

// "underwater" and "notunderwater": in or out of the contents named, else any fluid
static void P_ParseFluid (p_type_t *t, const p_args_t *args, unsigned flag)
{
	static const struct
	{
		const char	*name;
		unsigned	bits;
	} contents[] =
	{
		{"water", P_CONT_WATER}, {"slime", P_CONT_SLIME}, {"lava", P_CONT_LAVA}, {"sky", P_CONT_SKY},
		{"fluid", P_CONT_FLUID}, {"solid", P_CONT_SOLID}, {"playerclip", 0}, {"none", 0},
	};
	int		i, k;

	t->flags |= flag;
	if ((t->flags & (PT_TRUNDERWATER | PT_TROVERWATER)) == (PT_TRUNDERWATER | PT_TROVERWATER))
	{
		t->flags &= ~PT_TRUNDERWATER;
		Con_Printf ("%s.%s: both over and under water\n", t->config, t->name);
	}
	if (args->argc == 1)
	{
		t->fluidmask = P_CONT_FLUID;
		return;
	}
	t->fluidmask = 0;
	for (i = 1 ; i < args->argc ; i++)
	{
		for (k = 0 ; k < (int)(sizeof(contents) / sizeof(contents[0])) ; k++)
			if (!strcmp (args->argv[i], contents[k].name))
				break;
		if (k < (int)(sizeof(contents) / sizeof(contents[0])))
			t->fluidmask |= contents[k].bits;
		else
			Con_Printf ("%s.%s: unknown contents %s\n", t->config, t->name, args->argv[i]);
	}
}

// "sound <name> [vol atten pitch delay weight]", or with named arguments
static void P_ParseSound (p_type_t *t, const p_args_t *args)
{
	p_sound_t	*s;
	const char	*arg, *value;
	char		*end;
	int			i;

	t->sounds = Mem_Realloc (t->sounds, sizeof(*t->sounds) * (size_t)(t->numsounds + 1));
	s = &t->sounds[t->numsounds++];
	Q_strncpyz (s->name, P_Arg (args, 1), sizeof(s->name));
	if (*s->name && p_host.precachesound)
		p_host.precachesound (s->name);
	s->vol = s->atten = 1;
	s->weight = 0;

	strtoul (P_Arg (args, 2), &end, 0);
	if (*end)
	{
		for (i = 2 ; i < args->argc ; i++)
		{
			arg = args->argv[i];
			value = strchr (arg, '=');
			value = value ? value + 1 : "";
			if (!Q_strncasecmp (arg, "vol=", 4) || !Q_strncasecmp (arg, "volume=", 7))
				s->vol = (float)atof (value);
			else if (!Q_strncasecmp (arg, "attn=", 5) || !Q_strncasecmp (arg, "atten=", 6)
				|| !Q_strncasecmp (arg, "attenuation=", 12))
				s->atten = !strcmp (value, "none") ? 0 : !strcmp (value, "normal") ? 1 : (float)atof (value);
			else if (!Q_strncasecmp (arg, "weight=", 7))
				s->weight = (float)atof (value);
			else if (Q_strncasecmp (arg, "pitch=", 6) && Q_strncasecmp (arg, "delay=", 6))
				Con_Printf ("%s.%s: unknown sound argument %s\n", t->config, t->name, arg);
		}
	}
	else
	{
		s->vol = P_ArgF (args, 2) ? P_ArgF (args, 2) : 1;
		s->atten = P_ArgF (args, 3) ? P_ArgF (args, 3) : 1;
		s->weight = P_ArgF (args, 6);
	}
	if (!s->weight)
		s->weight = 1;
}

// a step of a ramp of palette colors: index past 255 is half seen
static p_ramp_t *P_NewRamp (p_type_t *t)
{
	t->ramp = Mem_Realloc (t->ramp, sizeof(*t->ramp) * (size_t)(t->rampindexes + 1));
	memset (&t->ramp[t->rampindexes], 0, sizeof(*t->ramp));
	return &t->ramp[t->rampindexes++];
}

static void P_PaletteRamp (p_ramp_t *r, int index)
{
	r->alpha = index > 255 ? 0.5f : 1;
	P_PaletteColor (index & 255, r->rgb);
}

static const struct
{
	const char		*name;
	r_partblend_t	mode;
	int				premul;
} p_blends[] =
{
	{"adda", RPB_ADDA, 0}, {"add", RPB_ADDA, 0}, {"addc", RPB_ADDC, 0}, {"subtract", RPB_SUBTRACT, 0},
	{"invmoda", RPB_INVMODA, 0}, {"invmod", RPB_INVMODA, 0}, {"invmodc", RPB_INVMODC, 0},
	{"blendcolour", RPB_BLENDCOLOR, 0}, {"blendcolor", RPB_BLENDCOLOR, 0},
	{"blendalpha", RPB_BLEND, 0}, {"blend", RPB_BLEND, 0},
	{"premul_subtract", RPB_INVMODC, 1}, {"premul_add", RPB_PREMUL, 2}, {"premul_blend", RPB_PREMUL, 1},
};

static const struct
{
	const char		*name;
	p_spawnmode_t	mode;
} p_spawnmodes[] =
{
	{"circle", SM_CIRCLE}, {"ball", SM_BALL}, {"spiral", SM_SPIRAL}, {"tracer", SM_TRACER},
	{"telebox", SM_TELEBOX}, {"lavasplash", SM_LAVASPLASH}, {"uniformcircle", SM_UNICIRCLE},
	{"syncfield", SM_FIELD}, {"distball", SM_DISTBALL}, {"box", SM_BOX},
};

static const struct
{
	const char	*name;
	p_shape_t	type;
} p_shapes[] =
{
	{"beam", PT_BEAM}, {"spark", PT_SPARK}, {"linespark", PT_SPARK}, {"sparkfan", PT_SPARKFAN},
	{"trianglefan", PT_SPARKFAN}, {"texturedspark", PT_TEXTUREDSPARK}, {"decal", PT_CDECAL},
	{"cdecal", PT_CDECAL}, {"udecal", PT_UDECAL}, {"normal", PT_NORMAL},
};

#define	P_FIND(table, word, i)	\
	for (i = 0 ; i < (int)(sizeof(table) / sizeof(table[0])) && strcmp (word, table[i].name) ; i++)

// what an r_part block's line may set besides its type: the parse's state
typedef struct
{
	const char	*config;
	int			index;			// of the type: the types move
	bool		settype;
	bool		setalphadelta;
	bool		setbeamlen;
} p_parse_t;

/*
=================
P_ParseField

A line of an r_part block
=================
*/
static void P_ParseField (p_parse_t *ps, const p_args_t *args)
{
	p_type_t	*t = &p_types[ps->index];
	const char	*var = P_Arg (args, 0), *value = P_Arg (args, 1);
	float		f, mn, mx;
	int			i, first, last, dims;
	p_ramp_t	*r;

	if (!strcmp (var, "texture") || !strcmp (var, "linear_texture") || !strcmp (var, "nearest_texture")
		|| !strcmp (var, "nearesttexture"))
		Q_strncpyz (t->texname, value, sizeof(t->texname));
	else if (!strcmp (var, "tcoords"))
	{
		f = P_ArgF (args, 5) > 0 ? P_ArgF (args, 5) : 1;
		t->s1 = P_ArgF (args, 1) / f;
		t->t1 = P_ArgF (args, 2) / f;
		t->s2 = P_ArgF (args, 3) / f;
		t->t2 = P_ArgF (args, 4) / f;
		t->randsmax = atoi (P_Arg (args, 6));
		t->texsstride = args->argc > 7 ? P_ArgF (args, 7) : 1 / f;
		if (t->randsmax < 1 || !t->texsstride)
			t->randsmax = 1;
	}
	else if (!strcmp (var, "atlas"))
	{
		// atlas <cells across> <first> [last]
		dims = (int)P_ArgF (args, 1);
		dims = dims < 1 ? 1 : dims;
		first = atoi (P_Arg (args, 2));
		last = atoi (P_Arg (args, 3));
		if (last > (last / dims) * dims + dims - 1)
		{
			last = (last / dims) * dims + dims - 1;
			Con_Printf ("%s.%s: atlas wraps across a row\n", t->config, t->name);
		}
		last = last < first ? first : last;
		t->s1 = 1.0f / dims * (first % dims);
		t->s2 = 1.0f / dims * (1 + first % dims);
		t->t1 = 1.0f / dims * (first / dims);
		t->t2 = 1.0f / dims * (1 + first / dims);
		t->randsmax = last - first + 1;
		t->texsstride = t->s2 - t->s1;
	}
	else if (!strcmp (var, "rotation") || !strcmp (var, "rotationstart"))
	{
		t->rotationstartmin = (float)(P_ArgF (args, 1) * Q_PI / 180);
		t->rotationstartrand = args->argc > 2 ? (float)(P_ArgF (args, 2) * Q_PI / 180) - t->rotationstartmin : 0;
		if (!strcmp (var, "rotation"))
		{
			t->rotationmin = (float)(P_ArgF (args, 3) * Q_PI / 180);
			t->rotationrand = args->argc > 4 ? (float)(P_ArgF (args, 4) * Q_PI / 180) - t->rotationmin : 0;
		}
	}
	else if (!strcmp (var, "rotationspeed"))
	{
		t->rotationmin = (float)(P_ArgF (args, 1) * Q_PI / 180);
		t->rotationrand = args->argc > 2 ? (float)(P_ArgF (args, 2) * Q_PI / 180) - t->rotationmin : 0;
	}
	else if (!strcmp (var, "beamtexstep"))
	{
		t->rotationstartmin = 1 / P_ArgF (args, 1);
		t->rotationstartrand = 0;
		ps->setbeamlen = true;
	}
	else if (!strcmp (var, "beamtexspeed"))
		t->rotationmin = P_ArgF (args, 1);
	else if (!strcmp (var, "scale"))
	{
		t->scale = P_ArgF (args, 1);
		if (args->argc > 2)
			t->scalerand = P_ArgF (args, 2) - t->scale;
	}
	else if (!strcmp (var, "scalerand"))
		t->scalerand = P_ArgF (args, 1);
	else if (!strcmp (var, "scalefactor"))
		t->looks.scalefactor = P_ArgF (args, 1);
	else if (!strcmp (var, "scaledelta"))
		t->scaledelta = P_ArgF (args, 1);
	else if (!strcmp (var, "stretchfactor"))
	{
		t->looks.stretch = P_ArgF (args, 1);
		t->looks.minstretch = P_ArgF (args, 2);
	}
	else if (!strcmp (var, "step") || !strcmp (var, "count"))
	{
		// step: a particle so many units along a trail
		bool	step = !strcmp (var, "step");

		t->countspacing = step ? P_ArgF (args, 1) : 0;
		t->count = step ? 1 / P_ArgF (args, 1) : P_ArgF (args, 1);
		if (args->argc > 2)
			t->countrand = step ? 1 / P_ArgF (args, 2) : P_ArgF (args, 2);
		if (args->argc > 3)
			t->countextra = P_ArgF (args, 3);
	}
	else if (!strcmp (var, "rainfrequency"))
		t->rainfrequency = P_ArgF (args, 1);
	else if (!strcmp (var, "alpha"))
		t->alpha = P_ArgF (args, 1);
	else if (!strcmp (var, "alpharand"))
		t->alpharand = P_ArgF (args, 1);
	else if (!strcmp (var, "alphachange"))		// the old name, scaled by alpha and die
		t->alphachange = P_ArgF (args, 1);
	else if (!strcmp (var, "alphadelta"))
	{
		t->alphachange = P_ArgF (args, 1);
		ps->setalphadelta = true;
	}
	else if (!strcmp (var, "die"))
	{
		mn = mx = P_ArgF (args, 1);
		if (args->argc > 2)
			mx = P_ArgF (args, 2);
		t->die = fmaxf (mn, mx);
		t->randdie = fabsf (mx - mn);
	}
	else if (!strcmp (var, "diesubrand"))
		t->randdie = P_ArgF (args, 1);
	else if (!strcmp (var, "randomvel"))
	{
		// velwrand, and velbias for up and down
		t->velbias[0] = t->velbias[1] = 0;
		t->velwrand[0] = t->velwrand[1] = P_ArgF (args, 1);
		if (args->argc > 3)
		{
			// from one to the other: rand is half the range, bias its middle
			t->velwrand[2] = (P_ArgF (args, 3) - P_ArgF (args, 2)) / 2;
			t->velbias[2] = P_ArgF (args, 2) + t->velwrand[2];
		}
		else
		{
			t->velwrand[2] = args->argc > 2 ? P_ArgF (args, 2) : t->velwrand[0];
			t->velbias[2] = 0;
		}
	}
	else if (!strcmp (var, "veladd"))
	{
		t->veladd = P_ArgF (args, 1);
		t->randomveladd = args->argc > 2 ? P_ArgF (args, 2) - t->veladd : 0;
	}
	else if (!strcmp (var, "orgadd"))
	{
		t->orgadd = P_ArgF (args, 1);
		t->randomorgadd = args->argc > 2 ? P_ArgF (args, 2) - t->orgadd : 0;
	}
	else if (!strcmp (var, "orgbias") || !strcmp (var, "orgwrand") || !strcmp (var, "velbias")
		|| !strcmp (var, "velwrand"))
	{
		float	*v = !strcmp (var, "orgbias") ? t->orgbias : !strcmp (var, "orgwrand") ? t->orgwrand
			: !strcmp (var, "velbias") ? t->velbias : t->velwrand;

		VectorSet (v, P_ArgF (args, 1), P_ArgF (args, 2), P_ArgF (args, 3));
	}
	else if (!strcmp (var, "friction"))
	{
		// all three, or horizontal and up, or each
		t->friction[0] = t->friction[1] = t->friction[2] = P_ArgF (args, 1);
		if (args->argc > 3)
		{
			t->friction[1] = P_ArgF (args, 2);
			t->friction[2] = P_ArgF (args, 3);
		}
		else if (args->argc > 2)
			t->friction[2] = P_ArgF (args, 2);
	}
	else if (!strcmp (var, "gravity"))
		t->gravity = P_ArgF (args, 1);
	else if (!strcmp (var, "flurry"))
		t->flurry = P_ArgF (args, 1);
	else if (!strcmp (var, "assoc") || !strcmp (var, "inwater"))
	{
		i = P_CheckAssociation (ps->config, value, ps->index);
		t = &p_types[ps->index];
		if (!strcmp (var, "assoc"))
			t->assoc = i;
		else
			t->inwater = i;
	}
	else if (!strcmp (var, "underwater"))
		P_ParseFluid (t, args, PT_TRUNDERWATER);
	else if (!strcmp (var, "notunderwater"))
		P_ParseFluid (t, args, PT_TROVERWATER);
	else if (!strcmp (var, "sound"))
		P_ParseSound (t, args);
	else if (!strcmp (var, "colorindex"))
	{
		if (args->argc > 2)
			t->colorrand = (int)strtoul (P_Arg (args, 2), NULL, 0);
		t->colorindex = (int)strtoul (value, NULL, 0);
	}
	else if (!strcmp (var, "colorrand"))
		t->colorrand = atoi (value);
	else if (!strcmp (var, "citracer"))
		t->flags |= PT_CITRACER;
	else if (!strcmp (var, "red") || !strcmp (var, "green") || !strcmp (var, "blue"))
		t->rgb[var[0] == 'r' ? 0 : var[0] == 'g' ? 1 : 2] = P_ArgF (args, 1) / 255;
	else if (!strcmp (var, "reddelta") || !strcmp (var, "greendelta") || !strcmp (var, "bluedelta"))
	{
		t->rgbchange[var[0] == 'r' ? 0 : var[0] == 'g' ? 1 : 2] = P_ArgF (args, 1) / 255;
		if (!t->rgbchangetime)
			t->rgbchangetime = t->die;
	}
	else if (!strcmp (var, "redrand") || !strcmp (var, "greenrand") || !strcmp (var, "bluerand"))
		t->rgbrand[var[0] == 'r' ? 0 : var[0] == 'g' ? 1 : 2] = P_ArgF (args, 1) / 255;
	else if (!strcmp (var, "redrandsync") || !strcmp (var, "greenrandsync") || !strcmp (var, "bluerandsync"))
		t->rgbrandsync[var[0] == 'r' ? 0 : var[0] == 'g' ? 1 : 2] = P_ArgF (args, 1);
	else if (!strcmp (var, "rgb") || !strcmp (var, "rgbf") || !strcmp (var, "rgbdelta") || !strcmp (var, "rgbdeltaf")
		|| !strcmp (var, "rgbrand") || !strcmp (var, "rgbrandf") || !strcmp (var, "rgbrandsync"))
	{
		// one value for all three, or each; bytes, or floats with the f
		float	*v = !strncmp (var, "rgbdelta", 8) ? t->rgbchange : !strcmp (var, "rgbrandsync") ? t->rgbrandsync
			: !strncmp (var, "rgbrand", 7) ? t->rgbrand : t->rgb;
		float	scale = var[strlen (var) - 1] == 'f' || v == t->rgbrandsync ? 1 : 1.0f / 255;

		v[0] = v[1] = v[2] = P_ArgF (args, 1) * scale;
		if (args->argc > 3)
		{
			v[1] = P_ArgF (args, 2) * scale;
			v[2] = P_ArgF (args, 3) * scale;
		}
		if (v == t->rgbchange && !t->rgbchangetime)
			t->rgbchangetime = t->die;
	}
	else if (!strcmp (var, "rgbdeltatime"))
		t->rgbchangetime = P_ArgF (args, 1);
	else if (!strcmp (var, "blend"))
	{
		P_FIND (p_blends, value, i);
		if (i < (int)(sizeof(p_blends) / sizeof(p_blends[0])))
		{
			t->looks.blendmode = p_blends[i].mode;
			t->looks.premul = p_blends[i].premul;
		}
		else
		{
			Con_DPrintf ("%s.%s: unknown blend %s, blendalpha instead\n", t->config, t->name, value);
			t->looks.blendmode = RPB_BLEND;
			t->looks.premul = 0;
		}
	}
	else if (!strcmp (var, "spawnmode"))
	{
		P_FIND (p_spawnmodes, value, i);
		if (i < (int)(sizeof(p_spawnmodes) / sizeof(p_spawnmodes[0])))
			t->spawnmode = p_spawnmodes[i].mode;
		else
		{
			Con_DPrintf ("%s.%s: unknown spawn mode %s, box instead\n", t->config, t->name, value);
			t->spawnmode = SM_BOX;
		}
		if (t->spawnmode == SM_FIELD)
		{
			t->spawnparam1 = 16;
			t->spawnparam2 = 0;
		}
		if (args->argc > 2)
		{
			t->spawnparam1 = P_ArgF (args, 2);
			if (args->argc > 3)
				t->spawnparam2 = P_ArgF (args, 3);
		}
	}
	else if (!strcmp (var, "type") || !strcmp (var, "isbeam") || !strcmp (var, "clippeddecal"))
	{
		if (!strcmp (var, "isbeam"))
			t->looks.type = PT_BEAM;
		else if (!strcmp (var, "clippeddecal"))
			t->looks.type = PT_CDECAL;		// its surface flags aren't kept
		else
		{
			P_FIND (p_shapes, value, i);
			if (i < (int)(sizeof(p_shapes) / sizeof(p_shapes[0])))
				t->looks.type = p_shapes[i].type;
			else
			{
				Con_DPrintf ("%s.%s: unknown type %s, normal instead\n", t->config, t->name, value);
				t->looks.type = PT_NORMAL;
			}
		}
		ps->settype = true;
	}
	else if (!strcmp (var, "spawntime"))
		t->spawntime = P_ArgF (args, 1);
	else if (!strcmp (var, "spawnchance"))
		t->spawnchance = P_ArgF (args, 1);
	else if (!strcmp (var, "cliptype") || !strcmp (var, "emit"))
	{
		i = P_AllocateParticleType (ps->config, value);
		t = &p_types[ps->index];
		if (var[0] == 'c')
			t->cliptype = i;
		else
			t->emit = i;
	}
	else if (!strcmp (var, "clipcount"))
		t->clipcount = P_ArgF (args, 1);
	else if (!strcmp (var, "clipbounce"))
	{
		t->clipbounce = P_ArgF (args, 1);
		if (t->clipbounce < 0 && t->cliptype == P_INVALID)
			t->cliptype = ps->index;
	}
	else if (!strcmp (var, "bounce"))
	{
		t->cliptype = ps->index;
		t->clipbounce = P_ArgF (args, 1);
	}
	else if (!strcmp (var, "emitinterval"))
		t->emittime = P_ArgF (args, 1);
	else if (!strcmp (var, "emitintervalrand"))
		t->emitrand = P_ArgF (args, 1);
	else if (!strcmp (var, "emitstart"))
		t->emitstart = P_ArgF (args, 1);
	else if (!strcmp (var, "spawnorg") || !strcmp (var, "areaspread"))
	{
		t->areaspread = t->areaspreadvert = P_ArgF (args, 1);
		if (args->argc > 2)
			t->areaspreadvert = P_ArgF (args, 2);
	}
	else if (!strcmp (var, "areaspreadvert"))
		t->areaspreadvert = P_ArgF (args, 1);
	else if (!strcmp (var, "spawnvel") || !strcmp (var, "offsetspread"))
	{
		t->spawnvel = t->spawnvelvert = P_ArgF (args, 1);
		if (args->argc > 2)
			t->spawnvelvert = P_ArgF (args, 2);
	}
	else if (!strcmp (var, "offsetspreadvert"))
		t->spawnvelvert = P_ArgF (args, 1);
	else if (!strcmp (var, "spawnparam1"))
		t->spawnparam1 = P_ArgF (args, 1);
	else if (!strcmp (var, "spawnparam2"))
		t->spawnparam2 = P_ArgF (args, 1);
	else if (!strcmp (var, "up"))
		t->orgbias[2] = P_ArgF (args, 1);
	else if (!strcmp (var, "rampmode"))
	{
		if (!strcmp (value, "none"))
			t->rampmode = RAMP_NONE;
		else if (!strcmp (value, "nearest") || !strcmp (value, "absolute"))
			t->rampmode = RAMP_NEAREST;
		else if (!strcmp (value, "lerp"))
			t->rampmode = RAMP_LERP;
		else
		{
			if (strcmp (value, "delta"))
				Con_DPrintf ("%s.%s: unknown ramp mode %s, delta instead\n", t->config, t->name, value);
			t->rampmode = RAMP_DELTA;
		}
	}
	else if (!strcmp (var, "rampindexlist"))
	{
		for (i = 1 ; i < args->argc ; i++)
		{
			r = P_NewRamp (t);
			P_PaletteRamp (r, atoi (args->argv[i]));
			r->scale = t->scale;
		}
	}
	else if (!strcmp (var, "rampindex"))
	{
		// rampindex <index> [alpha] [scale]
		r = P_NewRamp (t);
		P_PaletteRamp (r, atoi (value));
		if (args->argc > 2)
			r->alpha *= P_ArgF (args, 2);
		r->scale = args->argc > 3 ? P_ArgF (args, 3) : t->scale;
	}
	else if (!strcmp (var, "ramp"))
	{
		// ramp <grey> or ramp <r g b> [alpha] [scale]
		r = P_NewRamp (t);
		r->rgb[0] = r->rgb[1] = r->rgb[2] = P_ArgF (args, 1) / 255;
		if (args->argc > 3)
		{
			r->rgb[1] = P_ArgF (args, 2) / 255;
			r->rgb[2] = P_ArgF (args, 3) / 255;
		}
		r->alpha = args->argc > 4 ? P_ArgF (args, 4) : t->alpha;
		r->scale = args->argc > 5 ? P_ArgF (args, 5) : t->scaledelta;
	}
	else if (!strcmp (var, "perframe"))
		t->flags |= PT_INVFRAMETIME;
	else if (!strcmp (var, "averageout"))
		t->flags |= PT_AVERAGETRAIL;
	else if (!strcmp (var, "nostate"))
		t->flags |= PT_NOSTATE;
	else if (!strcmp (var, "nospreadfirst"))
		t->flags |= PT_NOSPREADFIRST;
	else if (!strcmp (var, "nospreadlast"))
		t->flags |= PT_NOSPREADLAST;
	else if (!strcmp (var, "lightradius"))
	{
		t->dl_radius[0] = P_ArgF (args, 1);
		t->dl_radius[1] = (args->argc > 2 ? P_ArgF (args, 2) : t->dl_radius[0]) - t->dl_radius[0];
	}
	else if (!strcmp (var, "lightradiusfade"))
		t->dl_decay = P_ArgF (args, 1);
	else if (!strcmp (var, "lightrgb"))
		VectorSet (t->dl_rgb, P_ArgF (args, 1), P_ArgF (args, 2), P_ArgF (args, 3));
	else if (!strcmp (var, "lighttime"))
		t->dl_time = P_ArgF (args, 1);
	else if (!strcmp (var, "shader") || !strcmp (var, "model") || !strcmp (var, "viewspace")
		|| !strcmp (var, "spawnstain") || !strcmp (var, "stains") || !strcmp (var, "lightrgbfade")
		|| !strcmp (var, "lightcorona") || !strcmp (var, "lightshadows") || !strcmp (var, "lightcubemap")
		|| !strcmp (var, "lightscales"))
		Con_DPrintf ("%s.%s: %s isn't supported\n", t->config, t->name, var);
	else if (args->argc)
		Con_DPrintf ("%s.%s: %s isn't a particle field\n", t->config, t->name, var);
}

// a block of lines between { and }, passed by
static bool P_SkipBlock (char *line, size_t size, const char *data, size_t length, size_t *offset)
{
	const char	*l;
	int			depth = 1;

	while ((l = P_ReadLine (line, size, data, length, offset)))
	{
		if (*l == '{')
			depth++;
		else if (*l == '}' && !--depth)
			return true;
	}
	return false;
}

/*
=================
P_ParseScript

FTE's script: "r_part <name>" and a block of its fields between { and } on
lines of their own; "r_part +<name>" adds to the effect, "r_part namespace
<name> [weak]" names the types after it; r_effect, r_trail and r_partredirect.
A weak script's types don't replace ones a strong script has.
=================
*/
static void P_ParseScript (const char *config, bool weak, const char *data, size_t length)
{
	char		line[512], namespace[MAX_QPATH], newname[MAX_QPATH + 8];
	const char	*l, *name;
	p_args_t	args;
	p_parse_t	ps;
	p_type_t	*t;
	size_t		offset = 0;
	int			i, assoc, parent;

	Q_strncpyz (namespace, config, sizeof(namespace));
	l = P_ReadLine (line, sizeof(line), data, length, &offset);
	while (l)
	{
		P_Tokenize (&args, l);
		l = NULL;
		if (!args.argc)
			;
		else if (!strcmp (args.argv[0], "r_effect") || !strcmp (args.argv[0], "r_trail"))
			P_AssociateEffect (&args);
		else if (!strcmp (args.argv[0], "r_partredirect"))
			P_Redirect (&args);
		else if (strcmp (args.argv[0], "r_part"))
			Con_Printf ("%s: unknown particle command %s\n", config, args.argv[0]);
		else if (args.argc != 2)
		{
			if (!strcmp (P_Arg (&args, 1), "namespace"))
			{
				Q_strncpyz (namespace, P_Arg (&args, 2), sizeof(namespace));
				if (args.argc >= 4)
					weak = atoi (args.argv[3]) != 0;
			}
			else
				Con_Printf ("%s: r_part without a name\n", config);
		}
		else
		{
			name = args.argv[1];
			if (!(l = P_ReadLine (line, sizeof(line), data, length, &offset)))
				return;
			if (*l != '{')
			{
				Con_Printf ("%s: r_part %s without a block\n", config, name);
				continue;	// the line again, as a command
			}
			l = NULL;

			t = P_GetParticleType (namespace, *name == '+' ? name + 1 : name);
			if (weak && t->loaded == 2)
			{
				if (!P_SkipBlock (line, sizeof(line), data, length, &offset))
					return;
				l = P_ReadLine (line, sizeof(line), data, length, &offset);
				continue;
			}
			if (*name == '+' && t->loaded)
			{
				// the end of the effect's chain
				for (i = 0 ; i < 64 ; i++)
				{
					parent = (int)(t - p_types);
					snprintf (newname, sizeof(newname), "+%i%s", i, t->name);
					t = P_GetParticleType (namespace, newname);
					if (!t->loaded)
					{
						if (p_types[parent].assoc != P_INVALID)
							Con_Printf ("%s: %s's assoc overridden\n", config, name + 1);
						p_types[parent].assoc = (int)(t - p_types);
						break;
					}
				}
				if (i == 64)
				{
					Con_Printf ("%s: too many of %s\n", config, name);
					return;
				}
			}
			else if (*name != '+' && t->loaded)
			{
				// what was added to it goes
				for (assoc = t->assoc ; assoc != P_INVALID && assoc < p_numtypes && *p_types[assoc].name == '+' ; )
				{
					p_types[assoc].loaded = 0;
					assoc = p_types[assoc].assoc;
				}
			}

			ps.config = namespace;
			ps.index = (int)(t - p_types);
			ps.settype = ps.setalphadelta = ps.setbeamlen = false;
			P_ResetToDefaults (t);
			for (;;)
			{
				if (!(l = P_ReadLine (line, sizeof(line), data, length, &offset)))
				{
					Con_Printf ("%s: %s ends early\n", config, p_types[ps.index].name);
					return;
				}
				if (*l == '}')
					break;
				P_Tokenize (&args, l);
				P_ParseField (&ps, &args);
			}
			l = NULL;

			t = &p_types[ps.index];
			t->loaded = weak ? 1 : 2;
			t->clipcount = t->clipcount < 1 ? 1 : t->clipcount;
			if (!ps.settype)
			{
				// sparks without images are lines, with a size triangles
				if (t->looks.type == PT_NORMAL && !*t->texname)
					t->looks.type = t->scale ? PT_SPARKFAN : PT_SPARK;
				else if (t->looks.type == PT_SPARK && *t->texname)
					t->looks.type = PT_TEXTUREDSPARK;
				else if (t->looks.type == PT_SPARK && t->scale)
					t->looks.type = PT_SPARKFAN;
			}
			if (!ps.setalphadelta && t->die)	// alphachange's: the alpha gone by die
				t->alphachange = -t->alphachange / t->die * t->alpha;
			P_FinishParticleType (t);
			if (t->looks.type == PT_BEAM && !ps.setbeamlen)
				t->rotationstartmin = 1 / 128.0f;
		}
		if (!l)
			l = P_ReadLine (line, sizeof(line), data, length, &offset);
	}
}

/*
==============================================================================

DARKPLACES' EFFECTINFO

==============================================================================
*/

// the cells of particles/particlefont.tga: s1 s2, then t flipped as
// QuakeSpasm-Spiked flips them; 8x8 unless particlefont.txt says
static void P_LoadParticleFont (float cells[256][4])
{
	char		line[1024];
	const char	*l;
	p_args_t	args;
	byte		*data;
	size_t		offset = 0;
	int			i, length;

	for (i = 0 ; i < 256 ; i++)
	{
		cells[i][0] = 1 / 8.0f * (i & 7);
		cells[i][1] = 1 / 8.0f * (1 + (i & 7));
		cells[i][2] = 1 / 8.0f * (1 + (i >> 3));
		cells[i][3] = 1 / 8.0f * (i >> 3);
	}
	if (!(data = FS_LoadFile ("particles/particlefont.txt", &length)))
		return;
	while ((l = P_ReadLine (line, sizeof(line), (const char *)data, (size_t)length, &offset)))
	{
		P_Tokenize (&args, l);
		i = atoi (P_Arg (&args, 0));
		if (args.argc >= 5 && i >= 0 && i < 256)
		{
			cells[i][0] = P_ArgF (&args, 1);
			cells[i][1] = P_ArgF (&args, 3);
			cells[i][2] = P_ArgF (&args, 4);
			cells[i][3] = P_ArgF (&args, 2);
		}
	}
	Mem_Free (data);
}

// the rest of an effectinfo effect, as FTE's look
static void P_FinishEffectinfo (p_type_t *t, bool blooddecal)
{
	if (t->looks.type == PT_CDECAL)
	{
		if (t->die == 9999)
			t->die = 20;
		t->alphachange = -(t->alpha / t->die);
	}
	else if (t->looks.type == PT_UDECAL)
	{
		// DarkPlaces' decals are sized by their radius, and stretched
		t->looks.stretch *= 1 / 1.414213562373095f;
		t->scale *= t->looks.stretch;
		t->scalerand *= t->looks.stretch;
		t->scaledelta *= t->looks.stretch;
		t->looks.stretch = 1;
	}
	else if (t->looks.type == PT_NORMAL)
	{
		// FTE's textured particles are a quarter of their scale, DarkPlaces'
		// their whole size
		t->scale *= 2 * t->looks.stretch;
		t->scalerand *= 2 * t->looks.stretch;
		t->scaledelta *= 2 * 2 * t->looks.stretch;
		t->looks.stretch = 1;
	}
	if (blooddecal)		// DarkPlaces' blood leaves decals where it lands, and doesn't bounce
		t->clipbounce = -2;
	if (t->looks.type == PT_TEXTUREDSPARK)
	{
		t->looks.stretch *= 0.04f;
		if (t->looks.stretch < 0)
			t->looks.stretch = 0.000001f;
	}
	if (t->die == 9999)		// none given: until it fades
		t->die = t->alphachange ? (t->alpha + t->alpharand) / -t->alphachange : 15;
	t->looks.minstretch = 0.5f;
	P_FinishParticleType (t);
}

// an effectinfo "type"
static const struct
{
	const char		*name;
	p_shape_t		type;
	r_partblend_t	blend;
	int				premul;
} p_effectinfotypes[] =
{
	{"decal", PT_CDECAL, RPB_INVMODC, 2},
	{"cdecal", PT_CDECAL, RPB_INVMODC, 2},
	{"udecal", PT_UDECAL, RPB_INVMODC, 2},
	{"alphastatic", PT_NORMAL, RPB_PREMUL, 1},
	{"static", PT_NORMAL, RPB_PREMUL, 2},
	{"smoke", PT_NORMAL, RPB_PREMUL, 2},
	{"spark", PT_TEXTUREDSPARK, RPB_PREMUL, 2},
	{"bubble", PT_NORMAL, RPB_PREMUL, 2},
	{"blood", PT_NORMAL, RPB_INVMODC, 2},
	{"beam", PT_BEAM, RPB_PREMUL, 2},
	{"snow", PT_NORMAL, RPB_PREMUL, 2},
};

/*
=================
P_ImportEffectInfo

DarkPlaces' effectinfo.txt: "effect <name>" and the lines of its fields; an
effect named again adds a stage to it
=================
*/
static void P_ImportEffectInfo (const char *config, char *data, bool weak)
{
	static float	cells[256][4];
	p_type_t		*t = NULL;
	p_args_t		args;
	const char		*var;
	char			*line = data, *eol, newname[MAX_QPATH];
	bool			blooddecal = false;
	unsigned		rgb1, rgb2;
	float			a1, a2;
	int				i, parent, first, last;

	P_LoadParticleFont (cells);
	while (line && *line)
	{
		while (*line == ' ' || *line == '\t')
			line++;
		if (line[0] == '/' && line[1] == '*')
		{
			eol = strstr (line + 2, "*/");
			line = eol ? eol + 2 : line + strlen (line);
			continue;
		}
		if ((eol = strchr (line, '\n')))
			*eol++ = 0;
		P_Tokenize (&args, line);
		line = eol;
		if (!args.argc)
			continue;

		var = args.argv[0];
		if (!strcmp (var, "effect"))
		{
			if (t)
				P_FinishEffectinfo (t, blooddecal);
			blooddecal = false;

			t = P_GetParticleType (config, P_Arg (&args, 1));
			if (t->loaded)
			{
				for (i = 0 ; i < 64 ; i++)
				{
					parent = (int)(t - p_types);
					snprintf (newname, sizeof(newname), "%i+%s", i, P_Arg (&args, 1));
					t = P_GetParticleType (config, newname);
					if (!t->loaded)
					{
						p_types[parent].assoc = (int)(t - p_types);
						break;
					}
				}
				if (i == 64)
				{
					Con_Printf ("%s: too many of %s\n", config, P_Arg (&args, 1));
					t = NULL;
					break;
				}
			}
			P_ResetToDefaults (t);
			t->loaded = weak ? 1 : 2;
			t->scale = 1;
			t->alpha = 0;
			t->alpharand = 1;
			t->alphachange = -1;
			t->die = 9999;
			Q_strncpyz (t->texname, "particles/particlefont", sizeof(t->texname));
			VectorSet (t->rgb, 1, 1, 1);
			t->looks.scalefactor = 2;
			t->looks.invscalefactor = 0;
			t->looks.type = PT_NORMAL;
			t->looks.blendmode = RPB_PREMUL;
			t->looks.premul = 1;
			t->looks.stretch = 1;
			t->s1 = cells[63][0];		// the default image
			t->s2 = cells[63][1];
			t->t1 = cells[63][2];
			t->t2 = cells[63][3];
			t->texsstride = 0;
			t->randsmax = 1;
		}
		else if (!t)
		{
			Con_Printf ("%s: %s before an effect\n", config, var);
			break;
		}
		else if (!strcmp (var, "countabsolute") && args.argc == 2)
			t->countextra = P_ArgF (&args, 1);
		else if (!strcmp (var, "count") && args.argc == 2)
			t->count = P_ArgF (&args, 1);
		else if (!strcmp (var, "type") && args.argc == 2)
		{
			P_FIND (p_effectinfotypes, args.argv[1], i);
			if (i < (int)(sizeof(p_effectinfotypes) / sizeof(p_effectinfotypes[0])))
			{
				t->looks.type = p_effectinfotypes[i].type;
				t->looks.blendmode = p_effectinfotypes[i].blend;
				t->looks.premul = p_effectinfotypes[i].premul;
				if (!strcmp (args.argv[1], "blood"))
				{
					t->gravity = 800;
					blooddecal = true;
				}
				else if (!strcmp (args.argv[1], "snow"))
					t->flurry = 32;
			}
			else
				Con_Printf ("%s: type %s isn't supported\n", config, args.argv[1]);
		}
		else if (!strcmp (var, "tex") && args.argc == 3)
		{
			first = atoi (args.argv[1]) & 255;
			last = atoi (args.argv[2]);
			t->s1 = cells[first][0];
			t->s2 = cells[first][1];
			t->t1 = cells[first][2];
			t->t2 = cells[first][3];
			t->texsstride = cells[(first + 1) & 255][0] - cells[first][0];
			t->randsmax = last - first < 1 ? 1 : last - first;
		}
		else if (!strcmp (var, "size") && args.argc == 3)
		{
			t->scale = P_ArgF (&args, 1);
			t->scalerand = P_ArgF (&args, 2) - t->scale;
		}
		else if (!strcmp (var, "sizeincrease") && args.argc == 2)
			t->scaledelta = P_ArgF (&args, 1);
		else if (!strcmp (var, "color") && args.argc == 3)
		{
			rgb1 = (unsigned)strtoul (args.argv[1], NULL, 0);
			rgb2 = (unsigned)strtoul (args.argv[2], NULL, 0);
			for (i = 0 ; i < 3 ; i++)
			{
				t->rgb[i] = ((rgb1 >> (16 - i * 8)) & 0xff) / 255.0f;
				t->rgbrand[i] = ((int)((rgb2 >> (16 - i * 8)) & 0xff) - (int)((rgb1 >> (16 - i * 8)) & 0xff)) / 255.0f;
				t->rgbrandsync[i] = 1;
			}
		}
		else if (!strcmp (var, "alpha") && args.argc == 4)
		{
			a1 = P_ArgF (&args, 1);
			a2 = P_ArgF (&args, 2);
			t->alpha = fminf (a1, a2) / 256;
			t->alpharand = fabsf (a2 - a1) / 256;
			t->alphachange = -P_ArgF (&args, 3) / 256;
		}
		else if ((!strcmp (var, "velocityoffset") || !strcmp (var, "velocityjitter") || !strcmp (var, "originoffset")
			|| !strcmp (var, "originjitter")) && args.argc == 4)
		{
			float	*v = !strcmp (var, "velocityoffset") ? t->velbias : !strcmp (var, "velocityjitter") ? t->velwrand
				: !strcmp (var, "originoffset") ? t->orgbias : t->orgwrand;

			VectorSet (v, P_ArgF (&args, 1), P_ArgF (&args, 2), P_ArgF (&args, 3));
		}
		else if (!strcmp (var, "gravity") && args.argc == 2)
			t->gravity = 800 * P_ArgF (&args, 1);
		else if (!strcmp (var, "bounce") && args.argc == 2)
		{
			t->clipbounce = P_ArgF (&args, 1);
			if (t->clipbounce < 0)
				t->cliptype = (int)(t - p_types);
		}
		else if (!strcmp (var, "airfriction") && args.argc == 2)
			t->friction[0] = t->friction[1] = t->friction[2] = P_ArgF (&args, 1);
		else if (!strcmp (var, "underwater") && args.argc == 1)
			t->flags |= PT_TRUNDERWATER;
		else if (!strcmp (var, "notunderwater") && args.argc == 1)
			t->flags |= PT_TROVERWATER;
		else if (!strcmp (var, "velocitymultiplier") && args.argc == 2)
			t->veladd = P_ArgF (&args, 1);
		else if (!strcmp (var, "trailspacing") && args.argc == 2)
		{
			t->countspacing = P_ArgF (&args, 1);
			t->count = 1 / t->countspacing;
		}
		else if (!strcmp (var, "time") && args.argc == 3)
		{
			a1 = P_ArgF (&args, 1);
			a2 = P_ArgF (&args, 2);
			t->die = fmaxf (a1, a2);		// from the shorter to the longer
			t->randdie = fabsf (a2 - a1);
		}
		else if (!strcmp (var, "stretchfactor") && args.argc == 2)
			t->looks.stretch = P_ArgF (&args, 1);
		else if (!strcmp (var, "blend") && args.argc == 2)
		{
			if (!strcmp (args.argv[1], "invmod"))
			{
				t->looks.blendmode = RPB_INVMODC;
				t->looks.premul = 2;
			}
			else if (!strcmp (args.argv[1], "alpha") || !strcmp (args.argv[1], "add"))
			{
				t->looks.blendmode = RPB_PREMUL;
				t->looks.premul = args.argv[1][1] == 'l' ? 1 : 2;
			}
			else
				Con_Printf ("%s: blend %s isn't supported\n", config, args.argv[1]);
		}
		else if (!strcmp (var, "orientation") && args.argc == 2)
		{
			if (!strcmp (args.argv[1], "billboard"))
				t->looks.type = PT_NORMAL;
			else if (!strcmp (args.argv[1], "spark"))
				t->looks.type = PT_TEXTUREDSPARK;
			else if (!strcmp (args.argv[1], "oriented"))
			{
				if (t->looks.type != PT_CDECAL)
					t->looks.type = PT_UDECAL;
			}
			else if (!strcmp (args.argv[1], "beam"))
				t->looks.type = PT_BEAM;
			else
				Con_Printf ("%s: orientation %s isn't supported\n", config, args.argv[1]);
		}
		else if (!strcmp (var, "lightradius") && args.argc == 2)
		{
			t->dl_radius[0] = P_ArgF (&args, 1);
			t->dl_radius[1] = 0;
		}
		else if (!strcmp (var, "lightradiusfade") && args.argc == 2)
			t->dl_decay = P_ArgF (&args, 1);
		else if (!strcmp (var, "lightcolor") && args.argc == 4)
			VectorSet (t->dl_rgb, P_ArgF (&args, 1), P_ArgF (&args, 2), P_ArgF (&args, 3));
		else if (!strcmp (var, "lighttime") && args.argc == 2)
			t->dl_time = P_ArgF (&args, 1);
		else if (!strcmp (var, "rotate") && args.argc == 5)
		{
			t->rotationstartmin = (float)(P_ArgF (&args, 1) * Q_PI / 180 + Q_PI / 4);
			t->rotationstartrand = (float)((P_ArgF (&args, 2) - P_ArgF (&args, 1)) * Q_PI / 180);
			t->rotationmin = (float)(P_ArgF (&args, 3) * Q_PI / 180);
			t->rotationrand = (float)((P_ArgF (&args, 4) - P_ArgF (&args, 3)) * Q_PI / 180);
		}
		else if (!strcmp (var, "liquidfriction") || !strcmp (var, "lightshadow") || !strcmp (var, "lightcubemapnum")
			|| !strcmp (var, "lightcorona") || !strncmp (var, "stain", 5))
			;		// not done here
		else
			Con_Printf ("%s: %s isn't an effect field, or has the wrong arguments\n", config, var);
	}
	if (t)
		P_FinishEffectinfo (t, blooddecal);
	p_looksdirty = true;
}

/*
==============================================================================

LOADING

==============================================================================
*/

/*
=================
P_LoadParticleSet

A script by name, once: particles/<name>.cfg or <name>.cfg, else for
effectinfo or effectinfo_<name>, <name>.txt. "classic" is id's particles,
which need none. False if there was nothing to load.
=================
*/
static bool P_LoadParticleSet (const char *name, bool weak, bool warn)
{
	p_config_t	*cfg;
	byte		*data;
	int			length;

	if (!*name)
		return false;
	for (cfg = p_configs ; cfg ; cfg = cfg->next)
		if (!strcmp (cfg->name, name))
			return false;
	cfg = Mem_Calloc (1, sizeof(*cfg));
	Q_strncpyz (cfg->name, name, sizeof(cfg->name));
	cfg->next = p_configs;
	p_configs = cfg;

	name = cfg->name;
	if (!strcmp (name, "classic"))
		return true;
	if ((data = FS_LoadFile (va ("particles/%s.cfg", name), &length)) || (data = FS_LoadFile (va ("%s.cfg", name), &length)))
	{
		P_ParseScript (name, weak, (const char *)data, (size_t)length);
		Mem_Free (data);
		return true;
	}
	if ((!strcmp (name, "effectinfo") || !strncmp (name, "effectinfo_", 11))
		&& (data = FS_LoadFile (va ("%s.txt", name), &length)))
	{
		P_ImportEffectInfo (name, (char *)data, weak);
		Mem_Free (data);
		return true;
	}
	if (warn)
		Con_Printf ("Couldn't find particle description %s\n", name);
	return false;
}

void P_LoadScriptText (const char *name, const char *text)
{
	p_config_t	*cfg;

	for (cfg = p_configs ; cfg ; cfg = cfg->next)
		if (!strcmp (cfg->name, name))
			return;
	cfg = Mem_Calloc (1, sizeof(*cfg));
	Q_strncpyz (cfg->name, name, sizeof(cfg->name));
	cfg->next = p_configs;
	p_configs = cfg;
	P_ParseScript (cfg->name, true, text, strlen (text));
}

// every type unloaded and every script forgotten, to be loaded again
static void P_UnloadAll (void)
{
	p_config_t	*cfg;
	int			i;

	for (i = 0 ; i < p_numtypes ; i++)
	{
		p_types[i].texname[0] = 0;
		p_types[i].scale = 0;
		p_types[i].loaded = 0;
		Mem_Free (p_types[i].ramp);
		p_types[i].ramp = NULL;
		p_types[i].rampmode = RAMP_NONE;
	}
	while ((cfg = p_configs))
	{
		p_configs = cfg->next;
		Mem_Free (cfg);
	}
}

/*
=================
P_ReloadScripts

r_particledesc's scripts, and the map's own (map_<name>.cfg), loaded again
=================
*/
void P_ReloadScripts (void)
{
	char	*s;

	P_UnloadAll ();
	p_looksdirty = true;
	Q_strncpyz (p_lastdesc, r_particledesc.string, sizeof(p_lastdesc));
	for (s = COM_Parse (r_particledesc.string) ; com_token[0] ; s = COM_Parse (s))
		P_LoadParticleSet (com_token, false, true);
	if (*p_mapname)
		P_LoadParticleSet (va ("map_%s", p_mapname), false, false);
	if (p_host.changed)
		p_host.changed ();
}

static void P_CvarChanged (cvar_t *var)
{
	if (var == &r_particledesc && strcmp (var->string, p_lastdesc))
		P_ReloadScripts ();
}

/*
=================
P_UpdateLooks

After a type changed: id's particles' types found again, and the client
finds its effects again
=================
*/
void P_UpdateLooks (void)
{
	p_looksdirty = false;
	p_skydirty = true;
	pe_default = P_FindParticleType ("PE_DEFAULT");
	pe_size2 = P_FindParticleType ("PE_SIZE2");
	pe_size3 = P_FindParticleType ("PE_SIZE3");
	pe_defaulttrail = P_FindParticleType ("PE_DEFAULTTRAIL");
	if (p_host.changed)
		p_host.changed ();
}

unsigned P_PointContents (const vec3_t p)
{
	switch (p_host.contents ? p_host.contents (p) : CONTENTS_EMPTY)
	{
	case CONTENTS_EMPTY:	return 0;
	case CONTENTS_SOLID:	return P_CONT_SOLID;
	case CONTENTS_SLIME:	return P_CONT_SLIME;
	case CONTENTS_LAVA:		return P_CONT_LAVA;
	case CONTENTS_SKY:		return P_CONT_SKY;
	default:				return P_CONT_WATER;
	}
}

void P_PaletteColor (int index, float *rgb)
{
	int		i;

	for (i = 0 ; i < 3 ; i++)
		rgb[i] = p_host.palette ? p_host.palette[(index & 255) * 3 + i] * (1 / 255.0f) : 1;
}

/*
=================
P_PartInfo_f

The effects running, their particles and decals
=================
*/
static void P_PartInfo_f (void)
{
	const p_type_t		*t;
	const p_particle_t	*p;
	const p_decal_t		*d;
	int					np, nd, total = 0, totald = 0, traced = 0, freep = 0, freed = 0, loaded = 0, i;

	for (i = 0 ; i < p_numtypes ; i++)
		loaded += p_types[i].loaded != 0;
	Con_Printf ("%i particle types, %i loaded\n", p_numtypes, loaded);
	for (t = p_runlist ; t ; t = t->nexttorun)
	{
		for (np = 0, p = t->particles ; p ; p = p->next)
			np++;
		for (nd = 0, d = t->clippeddecals ; d ; d = d->next)
			nd++;
		Con_Printf ("%s.%s: %i particles%s, %i decals\n", t->config, t->name, np,
			t->cliptype >= 0 ? " (traced)" : "", nd);
		total += np;
		totald += nd;
		traced += t->cliptype >= 0 ? np : 0;
	}
	for (p = p_freeparticles ; p ; p = p->next)
		freep++;
	for (d = p_freedecals ; d ; d = d->next)
		freed++;
	Con_Printf ("%i particles, %i free, %i traced\n", total, freep, traced);
	Con_Printf ("%i decals, %i free\n", totald, freed);
}

/*
==============================================================================

THE MODULE

==============================================================================
*/

void P_Init (const p_host_t *host)
{
	static cvar_t	*cvars[] =
	{
		&r_particledesc, &r_part_rain, &r_part_rain_quantity, &r_part_density, &r_part_maxparticles,
		&r_part_maxdecals, &r_part_sparks, &r_part_sparks_trifan, &r_part_sparks_textured, &r_part_beams,
		&r_part_contentswitch, &r_bouncysparks, &r_particle_tracelimit, &r_decal_noperpendicular, &r_lightflicker,
	};
	int		i;

	p_host = *host;
	for (i = 0 ; i < (int)(sizeof(cvars) / sizeof(cvars[0])) ; i++)
		Cvar_RegisterVariable (cvars[i]);
	Cvar_AddChangeHook (P_CvarChanged);
	Cmd_AddCommand ("r_partredirect", P_Redirect_f,
		"Looks an effect up by another name: r_partredirect <from> <to>; with no <to>, says what <from> is.");
	Cmd_AddCommand ("r_partinfo", P_PartInfo_f, "Lists the particle effects running.");
	P_AllocParticles (r_part_maxparticles.value, r_part_maxdecals.value);
	P_ReloadScripts ();
}

void P_Shutdown (void)
{
	p_modeleffect_t	*me;
	p_alias_t		*a;
	int				i;

	P_UnloadAll ();
	for (i = 0 ; i < p_numtypes ; i++)
		Mem_Free (p_types[i].sounds);
	Mem_Free (p_types);
	p_types = NULL;
	p_numtypes = 0;
	p_runlist = NULL;
	while ((me = p_modeleffects))
	{
		p_modeleffects = me->next;
		Mem_Free (me);
	}
	while ((a = p_aliases))
	{
		p_aliases = a->next;
		Mem_Free (a);
	}
	P_FreeParticles ();
}

void P_NewMap (const char *mapname)
{
	Q_strncpyz (p_mapname, mapname, sizeof(p_mapname));
	P_ClearParticles ();
	P_ReloadScripts ();
}
