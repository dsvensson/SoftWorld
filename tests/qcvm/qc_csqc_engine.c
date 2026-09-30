// qc_csqc_engine.c -- a stub engine for KTX's weapon-prediction csprogs.dat
// (qcvm-rs's tests/all/support/csqc_engine.rs)

#include "qc_csqc_engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
==============================================================================

LISTS

==============================================================================
*/

static char *CSE_Dup (const char *s)
{
	size_t	len = strlen (s);
	char	*copy = malloc (len + 1);

	if (!copy)
		abort ();
	memcpy (copy, s, len + 1);
	return copy;
}

static void *CSE_Grow (void *items, uint32_t *size, uint32_t count, size_t item)
{
	uint32_t	n;

	if (count < *size)
		return items;
	n = *size ? *size * 2 : 16;
	items = realloc (items, (size_t)n * item);
	if (!items)
		abort ();
	*size = n;
	return items;
}

void CSE_Push (cse_strings_t *list, const char *text)
{
	list->items = CSE_Grow (list->items, &list->size, list->count, sizeof(*list->items));
	list->items[list->count++] = CSE_Dup (text);
}

void CSE_FreeStrings (cse_strings_t *list)
{
	uint32_t	i;

	for (i = 0 ; i < list->count ; i++)
		free (list->items[i]);
	free (list->items);
	*list = (cse_strings_t){0};
}

bool CSE_Contains (const cse_strings_t *list, const char *text)
{
	uint32_t	i;

	for (i = 0 ; i < list->count ; i++)
		if (!strcmp (list->items[i], text))
			return true;
	return false;
}

// a formatted line for the log
static void CSE_Log (cse_engine_t *e, const char *fmt, ...)
{
	char	text[1024];
	va_list	args;

	va_start (args, fmt);
	vsnprintf (text, sizeof(text), fmt, args);
	va_end (args);
	CSE_Push (&e->log, text);
}

void CSE_SetPair (cse_pairs_t *pairs, const char *key, const char *value)
{
	uint32_t	i;

	for (i = 0 ; i < pairs->count ; i++)
		if (!strcmp (pairs->keys[i], key))
		{
			free (pairs->values[i]);
			pairs->values[i] = CSE_Dup (value);
			return;
		}
	if (pairs->count == pairs->size)
	{
		pairs->size = pairs->size ? pairs->size * 2 : 16;
		pairs->keys = realloc (pairs->keys, pairs->size * sizeof(*pairs->keys));
		pairs->values = realloc (pairs->values, pairs->size * sizeof(*pairs->values));
		if (!pairs->keys || !pairs->values)
			abort ();
	}
	pairs->keys[pairs->count] = CSE_Dup (key);
	pairs->values[pairs->count++] = CSE_Dup (value);
}

const char *CSE_Pair (const cse_pairs_t *pairs, const char *key)
{
	uint32_t	i;

	for (i = 0 ; i < pairs->count ; i++)
		if (!strcmp (pairs->keys[i], key))
			return pairs->values[i];
	return NULL;
}

static void CSE_FreePairs (cse_pairs_t *pairs)
{
	uint32_t	i;

	for (i = 0 ; i < pairs->count ; i++)
	{
		free (pairs->keys[i]);
		free (pairs->values[i]);
	}
	free (pairs->keys);
	free (pairs->values);
	*pairs = (cse_pairs_t){0};
}

static void CSE_ScenePush (cse_scene_t *s, uint32_t e)
{
	s->ents = CSE_Grow (s->ents, &s->size, s->count, sizeof(*s->ents));
	s->ents[s->count++] = e;
}

/*
==============================================================================

THE PROFILE

==============================================================================
*/

typedef struct
{
	char		*key;
	uint64_t	count;
} cse_countslot_t;

// counts by text: open addressing
typedef struct
{
	cse_countslot_t	*slots;
	uint32_t		mask, count;
} cse_counts_t;

struct cse_profile_s
{
	cse_counts_t	ops;
	cse_counts_t	pairs;			// "first second"
	bool			has_last;
	uint32_t		last;
	char			last_op[64];
};

static uint32_t CSE_Hash (const char *s)
{
	uint32_t	h = 2166136261u;

	for ( ; *s ; s++)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h;
}

static cse_countslot_t *CSE_CountSlot (cse_countslot_t *slots, uint32_t mask, const char *key)
{
	uint32_t	i = CSE_Hash (key) & mask;

	while (slots[i].key && strcmp (slots[i].key, key))
		i = (i + 1) & mask;
	return &slots[i];
}

static void CSE_Count (cse_counts_t *c, const char *key)
{
	cse_countslot_t	*old = c->slots, *s;
	uint32_t		oldsize = old ? c->mask + 1 : 0, size, i;

	if ((c->count + 1) * 2 > oldsize)
	{
		size = oldsize ? oldsize * 2 : 256;
		c->slots = calloc (size, sizeof(*c->slots));
		if (!c->slots)
			abort ();
		c->mask = size - 1;
		for (i = 0 ; i < oldsize ; i++)
			if (old[i].key)
				*CSE_CountSlot (c->slots, c->mask, old[i].key) = old[i];
		free (old);
	}
	s = CSE_CountSlot (c->slots, c->mask, key);
	if (!s->key)
	{
		s->key = CSE_Dup (key);
		c->count++;
	}
	s->count++;
}

static void CSE_FreeCounts (cse_counts_t *c)
{
	uint32_t	i;

	for (i = 0 ; c->slots && i <= c->mask ; i++)
		free (c->slots[i].key);
	free (c->slots);
	*c = (cse_counts_t){0};
}

// counts one trace line ("function: index: OPCODE operands")
static void CSE_ProfileRecord (cse_profile_t *p, const char *line)
{
	const char	*rest = strstr (line, ": "), *stmt;
	char		op[64], pair[140];
	char		*end;
	unsigned long	index;
	size_t		n;

	if (!rest)
		return;
	rest += 2;
	while (*rest == ' ')
		rest++;
	index = strtoul (rest, &end, 10);
	if (end == rest || strncmp (end, ": ", 2))
		return;
	for (stmt = end + 2 ; *stmt == ' ' ; stmt++)
		;
	for (n = 0 ; stmt[n] && stmt[n] != ' ' && n < sizeof(op) - 1 ; n++)
		op[n] = stmt[n];
	op[n] = 0;
	if (!n)
		return;
	CSE_Count (&p->ops, op);
	if (p->has_last && p->last + 1 == index)
	{
		snprintf (pair, sizeof(pair), "%s %s", p->last_op, op);
		CSE_Count (&p->pairs, pair);
	}
	p->has_last = true;
	p->last = (uint32_t)index;
	snprintf (p->last_op, sizeof(p->last_op), "%s", op);
}

void CSE_ProfileStart (cse_engine_t *e)
{
	if (!e->profile && !(e->profile = calloc (1, sizeof(*e->profile))))
		abort ();
}

static int CSE_CompareCounts (const void *a, const void *b)
{
	const cse_countslot_t	*x = a, *y = b;

	return x->count < y->count ? 1 : x->count > y->count ? -1 : strcmp (x->key, y->key);
}

static void CSE_PrintCounts (const cse_counts_t *c, uint32_t top, uint64_t total)
{
	cse_countslot_t	*sorted = malloc (((size_t)c->count + 1) * sizeof(*sorted));
	uint32_t		i, n = 0;

	if (!sorted)
		abort ();
	for (i = 0 ; c->slots && i <= c->mask ; i++)
		if (c->slots[i].key)
			sorted[n++] = c->slots[i];
	qsort (sorted, n, sizeof(*sorted), CSE_CompareCounts);
	for (i = 0 ; i < n && i < top ; i++)
		printf ("%-33s %10llu %5.1f%%\n", sorted[i].key, (unsigned long long)sorted[i].count,
			total ? (double)sorted[i].count * 100.0 / (double)total : 0.0);
	free (sorted);
}

void CSE_ProfileReport (cse_engine_t *e, uint32_t top)
{
	cse_profile_t	*p = e->profile;
	uint64_t		total = 0;
	uint32_t		i;

	if (!p)
		return;
	for (i = 0 ; p->ops.slots && i <= p->ops.mask ; i++)
		total += p->ops.slots[i].count;
	printf ("%llu statements\n", (unsigned long long)total);
	CSE_PrintCounts (&p->ops, top, total);
	printf ("\nstraight-line pairs\n");
	CSE_PrintCounts (&p->pairs, top, total);
}

static void CSE_FreeProfile (cse_engine_t *e)
{
	if (!e->profile)
		return;
	CSE_FreeCounts (&e->profile->ops);
	CSE_FreeCounts (&e->profile->pairs);
	free (e->profile);
	e->profile = NULL;
}

/*
==============================================================================

THE ENGINE

==============================================================================
*/

void CSE_EngineInit (cse_engine_t *e)
{
	static const char	*cvars[][2] = {
		{"r_drawviewmodel", "1"}, {"r_rocketlight", "1"}, {"r_rockettrail", "1"}, {"r_grenadetrail", "1"},
		{"v_viewheight", "0"}};
	uint32_t			i, items = IT_ROCKET_LAUNCHER;

	*e = (cse_engine_t){0};
	for (i = 0 ; i < sizeof(cvars) / sizeof(cvars[0]) ; i++)
		CSE_SetPair (&e->cvars, cvars[i][0], cvars[i][1]);
	CSE_SetPair (&e->serverkeys, "sv_antilag", "1");
	e->stats[STAT_HEALTH] = 100;
	memcpy (&e->stats[STAT_ITEMS], &items, 4);		// an integer stat's bits
	e->stats[STAT_KTX_GRAVITY] = 800;
}

void CSE_EngineFree (cse_engine_t *e)
{
	uint32_t	i;

	CSE_FreeStrings (&e->log);
	QT_TextFree (&e->printed);
	CSE_FreeStrings (&e->warnings);
	CSE_FreePairs (&e->cvars);
	CSE_FreePairs (&e->serverkeys);
	CSE_FreePairs (&e->playerkeys);
	free (e->net);
	free (e->inputs);
	CSE_FreeStrings (&e->models);
	CSE_FreeStrings (&e->sounds_precached);
	CSE_FreeStrings (&e->effects);
	free (e->scene.ents);
	for (i = 0 ; i < e->numsounds ; i++)
		free (e->sounds[i].sample);
	free (e->sounds);
	for (i = 0 ; i < e->numrendered ; i++)
		free (e->rendered[i].ents);
	free (e->rendered);
	CSE_FreeProfile (e);
	*e = (cse_engine_t){0};
}

uint32_t CSE_ModelIndex (const cse_engine_t *e, const char *name)
{
	uint32_t	i;

	for (i = 0 ; i < e->models.count ; i++)
		if (!strcmp (e->models.items[i], name))
			return i + 1;
	return 0;
}

size_t CSE_NetLeft (const cse_engine_t *e)
{
	return e->netlen - e->netpos;
}

// the next n bytes of the message; missing ones read as 0 and are noted
static void CSE_Take (cse_engine_t *e, uint8_t *out, size_t n)
{
	size_t	i;

	for (i = 0 ; i < n ; i++)
	{
		if (e->netpos < e->netlen)
			out[i] = e->net[e->netpos++];
		else
		{
			out[i] = 0;
			e->net_underflow = true;
		}
	}
}

/*
------------------------------------------------------------------------------
host callbacks
------------------------------------------------------------------------------
*/

static void CSE_Warning (void *ctx, const qc_warning_t *w)
{
	cse_engine_t	*e = ctx;
	char			text[1024];

	CSE_Push (&e->warnings, QC_WarningText (w, text, sizeof(text)));
}

static void CSE_Print (void *ctx, const char *text)
{
	cse_engine_t	*e = ctx;

	QT_TextAppend (&e->printed, text);
	CSE_Log (e, "print \"%s\"", text);
}

static float CSE_CvarFloat (void *ctx, const char *name)
{
	const char	*value = CSE_Pair (&((cse_engine_t *)ctx)->cvars, name);
	char		*end;
	float		f;

	if (!value || !*value)
		return 0;
	f = strtof (value, &end);
	return *end ? 0 : f;
}

static const char *CSE_CvarString (void *ctx, const char *name)
{
	return CSE_Pair (&((cse_engine_t *)ctx)->cvars, name);
}

static float CSE_IsDemo (void *ctx)
{
	(void)ctx;
	return 0;
}

static void CSE_Trace (void *ctx, const char *line)
{
	cse_engine_t	*e = ctx;

	if (e->profile)
		CSE_ProfileRecord (e->profile, line);
}

const qc_host_t	cse_host = {
	.warning = CSE_Warning,
	.print = CSE_Print,
	.cvar_float = CSE_CvarFloat,
	.cvar_string = CSE_CvarString,
	.is_demo = CSE_IsDemo,
	.trace = CSE_Trace,
};

/*
------------------------------------------------------------------------------
typed access
------------------------------------------------------------------------------
*/

float CSE_Float (uint32_t bits)
{
	float	f;

	memcpy (&f, &bits, 4);
	return f;
}

static uint32_t CSE_Bits (float f)
{
	uint32_t	u;

	memcpy (&u, &f, 4);
	return u;
}

bool CSE_Global (qcvm_t *vm, const char *name, uint32_t type, uint32_t *word)
{
	uint32_t	t;

	return QC_FindGlobal (vm, name, word, &t) && t == type;
}

bool CSE_Field (const qcvm_t *vm, const char *name, uint32_t type, uint32_t *ofs)
{
	uint32_t	t;

	return QC_FindField (vm, name, ofs, &t) && t == type;
}

void CSE_SetGlobalF (qcvm_t *vm, const char *name, float v)
{
	uint32_t	w;

	if (CSE_Global (vm, name, QC_EV_FLOAT, &w))
		QC_Globals (vm)[w].f = v;
}

void CSE_SetGlobalV (qcvm_t *vm, const char *name, float x, float y, float z)
{
	uint32_t	w;
	qc_word_t	*g;

	if (!CSE_Global (vm, name, QC_EV_VECTOR, &w))
		return;
	g = QC_Globals (vm);
	g[w].f = x;
	g[w + 1].f = y;
	g[w + 2].f = z;
}

void CSE_SetGlobalE (qcvm_t *vm, const char *name, qc_ent_t e)
{
	uint32_t	w;

	if (CSE_Global (vm, name, QC_EV_ENTITY, &w))
		QC_Globals (vm)[w].u = e;
}

float CSE_FieldF (const qcvm_t *vm, qc_ent_t e, const char *name)
{
	uint32_t	ofs, v;

	if (!CSE_Field (vm, name, QC_EV_FLOAT, &ofs) || !QC_GetField (vm, e, ofs, 1, &v))
		return 0;
	return CSE_Float (v);
}

void CSE_FieldV (const qcvm_t *vm, qc_ent_t e, const char *name, float out[3])
{
	uint32_t	ofs, v[3];

	out[0] = out[1] = out[2] = 0;
	if (!CSE_Field (vm, name, QC_EV_VECTOR, &ofs) || !QC_GetField (vm, e, ofs, 3, v))
		return;
	out[0] = CSE_Float (v[0]);
	out[1] = CSE_Float (v[1]);
	out[2] = CSE_Float (v[2]);
}

void CSE_SetFieldF (qcvm_t *vm, qc_ent_t e, const char *name, float v)
{
	uint32_t	ofs, w = CSE_Bits (v);

	if (CSE_Field (vm, name, QC_EV_FLOAT, &ofs))
		QC_SetField (vm, e, ofs, 1, &w);
}

static void CSE_SetFieldV (qcvm_t *vm, qc_ent_t e, const char *name, const float v[3])
{
	uint32_t	ofs, w[3] = {CSE_Bits (v[0]), CSE_Bits (v[1]), CSE_Bits (v[2])};

	if (CSE_Field (vm, name, QC_EV_VECTOR, &ofs))
		QC_SetField (vm, e, ofs, 3, w);
}

// Rust's float to integer casts: toward zero, saturating, NaN 0
static int32_t CSE_I32 (float f)
{
	if (f != f)
		return 0;
	if (f <= -2147483648.0f)
		return INT32_MIN;
	if (f >= 2147483648.0f)
		return INT32_MAX;
	return (int32_t)f;
}

static uint32_t CSE_U32 (float f)
{
	if (!(f > 0))
		return 0;
	if (f >= 4294967296.0f)
		return UINT32_MAX;
	return (uint32_t)f;
}

static void CSE_Vec (const qcvm_t *vm, int i, float out[3])
{
	QC_ArgVector (vm, i, out);
}

/*
------------------------------------------------------------------------------
engine builtins
------------------------------------------------------------------------------
*/

#define ENGINE(vm)	((cse_engine_t *)QC_HostContext (vm))

static bool B_Setorigin (qcvm_t *vm)
{
	float	org[3];

	CSE_Vec (vm, 1, org);
	CSE_SetFieldV (vm, QC_ArgWord (vm, 0), "origin", org);
	return true;
}

static bool B_Setmodel (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	qc_ent_t		e = QC_ArgWord (vm, 0);
	char			*name = CSE_Dup (QC_ArgString (vm, 1));
	size_t			len = strlen (name);
	uint32_t		index = CSE_ModelIndex (h, name), ofs, s;
	float			flags = 0;

	CSE_Log (h, "setmodel #%u %s", e, name);
	if (len >= 11 && !strcmp (name + len - 11, "missile.mdl"))
		flags = 1;		// MF_ROCKET
	else if (len >= 11 && !strcmp (name + len - 11, "grenade.mdl"))
		flags = 2;		// MF_GRENADE
	CSE_SetFieldF (vm, e, "modelindex", (float)index);
	CSE_SetFieldF (vm, e, "modelflags", flags);
	s = QC_ArgWord (vm, 1);
	if (CSE_Field (vm, "model", QC_EV_STRING, &ofs))
		QC_SetField (vm, e, ofs, 1, &s);
	free (name);
	return true;
}

static bool B_Sound (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	qc_ent_t		e = QC_ArgWord (vm, 0);
	float			chan = QC_ArgFloat (vm, 1);
	const char		*sample = QC_ArgString (vm, 2);

	CSE_Log (h, "sound #%u %g %s", e, (double)chan, sample);
	h->sounds = CSE_Grow (h->sounds, &h->soundsize, h->numsounds, sizeof(*h->sounds));
	h->sounds[h->numsounds++] = (cse_sound_t){e, chan, CSE_Dup (sample)};
	return true;
}

// traces against a floor plane at z = 0
static bool B_Traceline (qcvm_t *vm)
{
	float	a[3], b[3], fraction = 1, normal[3] = {0, 0, 0}, end[3];
	int		k;

	CSE_Vec (vm, 0, a);
	CSE_Vec (vm, 1, b);
	if (a[2] >= 0 && b[2] < 0)
	{
		fraction = a[2] / (a[2] - b[2]);
		normal[2] = 1;
	}
	for (k = 0 ; k < 3 ; k++)
		end[k] = a[k] + (b[k] - a[k]) * fraction;
	CSE_SetGlobalF (vm, "trace_fraction", fraction);
	CSE_SetGlobalV (vm, "trace_endpos", end[0], end[1], end[2]);
	CSE_SetGlobalV (vm, "trace_plane_normal", normal[0], normal[1], normal[2]);
	CSE_SetGlobalF (vm, "trace_plane_dist", 0);
	CSE_SetGlobalE (vm, "trace_ent", 0);
	CSE_SetGlobalF (vm, "trace_allsolid", 0);
	CSE_SetGlobalF (vm, "trace_startsolid", 0);
	return true;
}

static bool B_PrecacheSound (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	const char		*name = QC_ArgString (vm, 0);

	if (!CSE_Contains (&h->sounds_precached, name))
		CSE_Push (&h->sounds_precached, name);
	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	return true;
}

static bool B_PrecacheModel (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	const char		*name = QC_ArgString (vm, 0);

	if (!CSE_ModelIndex (h, name))
		CSE_Push (&h->models, name);
	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	return true;
}

static bool B_Pointcontents (qcvm_t *vm)
{
	QC_ReturnFloat (vm, -1);		// CONTENT_EMPTY
	return true;
}

static bool B_Getmodelindex (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)CSE_ModelIndex (ENGINE (vm), QC_ArgString (vm, 0)));
	return true;
}

static bool B_Clearscene (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);

	CSE_Log (h, "clearscene");
	h->scene.count = 0;
	return true;
}

// FTE's addentities walk over QuakeC entities: every one in use but the world,
// in slot order, whose drawmask shares a bit with mask. One with a predraw is
// skipped if it returns non-zero or removes the entity. Entities spawned
// during the walk are not visited.
static bool CSE_AddEntities (qcvm_t *vm, cse_engine_t *h, int32_t mask)
{
	uint32_t	n = QC_NumEdicts (vm), e, w, f;
	qc_value_t	ret;

	for (e = 1 ; e < n ; e++)
	{
		if (QC_IsFree (vm, e))
			continue;
		if (!QC_GetField (vm, e, h->drawmask_f, 1, &w) || !(QC_FloatToInt (CSE_Float (w)) & mask))
			continue;
		if (h->has_predraw && QC_GetField (vm, e, h->predraw_f, 1, &f) && QC_FUNC_INDEX (f))
		{
			ret = (qc_value_t){{0}};
			if (!QC_CallAs (vm, e, f, 0, NULL, &ret))
				return false;
			if (CSE_Float (ret.w[0]) != 0 || QC_IsFree (vm, e))
				continue;
		}
		CSE_ScenePush (&h->scene, e);
	}
	return true;
}

static bool B_Addentities (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	int32_t			mask = CSE_I32 (QC_ArgFloat (vm, 0));

	CSE_Log (h, "addentities %d", mask);
	if (!h->has_handles)
		return true;
	return CSE_AddEntities (vm, h, mask);
}

static bool B_Addentity (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	qc_ent_t		e = QC_ArgWord (vm, 0);

	CSE_Log (h, "addentity #%u", e);
	CSE_ScenePush (&h->scene, e);
	return true;
}

static bool B_Setproperty (qcvm_t *vm)
{
	CSE_Log (ENGINE (vm), "setproperty %g %g", (double)QC_ArgFloat (vm, 0), (double)QC_ArgFloat (vm, 1));
	QC_ReturnFloat (vm, 1);
	return true;
}

static bool B_Renderscene (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	cse_scene_t		copy = {0};
	char			text[1024];
	size_t			len;
	uint32_t		i;

	len = (size_t)snprintf (text, sizeof(text), "renderscene [");
	for (i = 0 ; i < h->scene.count && len < sizeof(text) ; i++)
		len += (size_t)snprintf (text + len, sizeof(text) - len, "%s%u", i ? ", " : "", h->scene.ents[i]);
	if (len < sizeof(text))
		snprintf (text + len, sizeof(text) - len, "]");
	CSE_Push (&h->log, text);
	for (i = 0 ; i < h->scene.count ; i++)
		CSE_ScenePush (&copy, h->scene.ents[i]);
	h->rendered = CSE_Grow (h->rendered, &h->renderedsize, h->numrendered, sizeof(*h->rendered));
	h->rendered[h->numrendered++] = copy;
	return true;
}

static bool B_DynamiclightAdd (qcvm_t *vm)
{
	float	org[3];

	CSE_Vec (vm, 0, org);
	CSE_Log (ENGINE (vm), "dynamiclight_add [%g, %g, %g] %g", (double)org[0], (double)org[1], (double)org[2],
		(double)QC_ArgFloat (vm, 1));
	QC_ReturnFloat (vm, 0);
	return true;
}

static bool B_Getproperty (qcvm_t *vm)
{
	static const float	zero[3];

	QC_ReturnVector (vm, QC_ArgFloat (vm, 0) == VF_ORIGIN ? ENGINE (vm)->view_origin : zero);
	return true;
}

// getstatf(stat), or with first, count the bits of an integer stat (getstatbits)
static bool B_Getstatf (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	int32_t			stat = CSE_I32 (QC_ArgFloat (vm, 0));
	float			v = stat >= 0 && stat < CSE_MAX_STATS ? h->stats[stat] : 0;
	uint32_t		first, count, bits;

	if (QC_Argc (vm) > 1)
	{
		first = CSE_U32 (QC_ArgFloat (vm, 1));
		count = CSE_U32 (QC_ArgFloat (vm, 2));
		bits = first < 32 ? CSE_Bits (v) >> first : 0;
		bits &= (1u << (count < 31 ? count : 31)) - 1;
		QC_ReturnFloat (vm, (float)bits);
	}
	else
		QC_ReturnFloat (vm, v);
	return true;
}

static bool B_Modelnameforindex (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	uint32_t		index = CSE_U32 (QC_ArgFloat (vm, 0));
	const char		*name = index >= 1 && index <= h->models.count ? h->models.items[index - 1] : "";

	QC_ReturnWord (vm, QC_Intern (vm, name, strlen (name)));
	return true;
}

static bool B_Particleeffectnum (qcvm_t *vm)
{
	cse_engine_t	*h = ENGINE (vm);
	const char		*name = QC_ArgString (vm, 0);
	uint32_t		i;

	for (i = 0 ; i < h->effects.count ; i++)
		if (!strcmp (h->effects.items[i], name))
			break;
	if (i == h->effects.count)
		CSE_Push (&h->effects, name);
	QC_ReturnFloat (vm, (float)(i + 1));
	return true;
}

static bool B_Trailparticles (qcvm_t *vm)
{
	CSE_Log (ENGINE (vm), "trailparticles %g #%u", (double)QC_ArgFloat (vm, 0), QC_ArgWord (vm, 1));
	return true;
}

static bool B_Getinputstate (qcvm_t *vm)
{
	cse_engine_t		*h = ENGINE (vm);
	int32_t				frame = CSE_I32 (QC_ArgFloat (vm, 0));
	const cse_input_t	*in;

	if (frame < 0 || (uint32_t)frame >= h->numinputs || !h->inputs[frame].present)
	{
		QC_ReturnFloat (vm, 0);
		return true;
	}
	in = &h->inputs[frame];
	CSE_SetGlobalF (vm, "input_timelength", in->timelength);
	CSE_SetGlobalV (vm, "input_angles", in->angles[0], in->angles[1], in->angles[2]);
	CSE_SetGlobalV (vm, "input_movevalues", 0, 0, 0);
	CSE_SetGlobalF (vm, "input_buttons", in->buttons);
	CSE_SetGlobalF (vm, "input_impulse", in->impulse);
	QC_ReturnFloat (vm, 1);
	return true;
}

static bool B_Getplayerkeyfloat (qcvm_t *vm)
{
	const char	*v = CSE_Pair (&ENGINE (vm)->playerkeys, QC_ArgString (vm, 1));

	QC_ReturnFloat (vm, v ? strtof (v, NULL) : 0);
	return true;
}

static bool B_Serverkey (qcvm_t *vm)
{
	const char	*v = CSE_Pair (&ENGINE (vm)->serverkeys, QC_ArgString (vm, 0));

	if (!v)
		v = "";
	return QC_ReturnString (vm, v, strlen (v));
}

static bool B_ReadByte (qcvm_t *vm)
{
	uint8_t	b;

	CSE_Take (ENGINE (vm), &b, 1);
	QC_ReturnFloat (vm, b);
	return true;
}

static int16_t CSE_TakeShort (cse_engine_t *h)
{
	uint8_t	b[2];

	CSE_Take (h, b, 2);
	return (int16_t)(b[0] | b[1] << 8);
}

static bool B_ReadShort (qcvm_t *vm)
{
	QC_ReturnFloat (vm, CSE_TakeShort (ENGINE (vm)));
	return true;
}

static bool B_ReadCoord (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)CSE_TakeShort (ENGINE (vm)) / 8.0f);
	return true;
}

static bool B_ReadAngle (qcvm_t *vm)
{
	uint8_t	b;

	CSE_Take (ENGINE (vm), &b, 1);
	QC_ReturnFloat (vm, (float)b * 360.0f / 256.0f);
	return true;
}

static bool B_ReadFloat (qcvm_t *vm)
{
	uint8_t		b[4];
	uint32_t	u;

	CSE_Take (ENGINE (vm), b, 4);
	u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	QC_ReturnFloat (vm, CSE_Float (u));
	return true;
}

static bool B_TeLightning2 (qcvm_t *vm)
{
	float	a[3], b[3];

	CSE_Vec (vm, 1, a);
	CSE_Vec (vm, 2, b);
	CSE_Log (ENGINE (vm), "te_lightning2 #%u [%g, %g, %g] [%g, %g, %g]", QC_ArgWord (vm, 0), (double)a[0],
		(double)a[1], (double)a[2], (double)b[0], (double)b[1], (double)b[2]);
	return true;
}

qc_builtins_t *CSE_Builtins (void)
{
	static const struct
	{
		uint32_t		number;
		const char		*name;
		qc_builtin_t	func;
	} engine[] = {
		{2, "setorigin", B_Setorigin},
		{3, "setmodel", B_Setmodel},
		{8, "sound", B_Sound},
		{16, "traceline", B_Traceline},
		{19, "precache_sound", B_PrecacheSound},
		{20, "precache_model", B_PrecacheModel},
		{41, "pointcontents", B_Pointcontents},
		{200, "getmodelindex", B_Getmodelindex},
		{300, "clearscene", B_Clearscene},
		{301, "addentities", B_Addentities},
		{302, "addentity", B_Addentity},
		{303, "setproperty", B_Setproperty},
		{304, "renderscene", B_Renderscene},
		{305, "dynamiclight_add", B_DynamiclightAdd},
		{309, "getproperty", B_Getproperty},
		{331, "getstatf", B_Getstatf},
		{334, "modelnameforindex", B_Modelnameforindex},
		{335, "particleeffectnum", B_Particleeffectnum},
		{336, "trailparticles", B_Trailparticles},
		{345, "getinputstate", B_Getinputstate},
		{354, "serverkey", B_Serverkey},
		{360, "ReadByte", B_ReadByte},
		{362, "ReadShort", B_ReadShort},
		{364, "ReadCoord", B_ReadCoord},
		{365, "ReadAngle", B_ReadAngle},
		{367, "ReadFloat", B_ReadFloat},
		{429, "te_lightning2", B_TeLightning2},
	};
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	size_t			i;

	if (!b)
		return NULL;
	for (i = 0 ; i < sizeof(engine) / sizeof(engine[0]) ; i++)
		if (!QC_BuiltinsSetNumbered (b, engine[i].number, engine[i].name, engine[i].func))
		{
			QC_BuiltinsFree (b);
			return NULL;
		}
	if (!QC_BuiltinsSet (b, "getplayerkeyfloat", B_Getplayerkeyfloat))
	{
		QC_BuiltinsFree (b);
		return NULL;
	}
	return b;
}

qc_progs_t *CSE_LoadProgram (const char *path)
{
	uint8_t			*data, *lno;
	size_t			size, lnosize, len = strlen (path);
	char			*lnopath;
	const char		*dot, *slash;
	qc_progs_t		*p;
	qc_loaderror_t	err;
	char			text[256];

	data = QT_LoadFile (path, &size);
	if (!data)
		return NULL;
	p = QC_LoadProgs (data, size, &err);
	free (data);
	if (!p)
	{
		printf ("%s: %s\n", path, QC_LoadErrorText (&err, text, sizeof(text)));
		return NULL;
	}
	// the .lno beside it: the path with its extension replaced
	dot = strrchr (path, '.');
	slash = strrchr (path, '/');
	if (!dot || (slash && dot < slash))
		dot = path + len;
	lnopath = malloc ((size_t)(dot - path) + 5);
	if (!lnopath)
		abort ();
	memcpy (lnopath, path, (size_t)(dot - path));
	memcpy (lnopath + (dot - path), ".lno", 5);
	lno = QT_LoadFile (lnopath, &lnosize);
	if (lno && !QT_CHECK (QC_AttachLineNumbers (p, lno, lnosize, &err)))
		printf ("%s: %s\n", lnopath, QC_LoadErrorText (&err, text, sizeof(text)));
	free (lno);
	free (lnopath);
	return p;
}

/*
==============================================================================

NETWORK MESSAGES

==============================================================================
*/

static void CSE_MsgPut (cse_msg_t *m, const void *bytes, size_t n)
{
	if (m->len + n > m->size)
	{
		m->size = (m->len + n) * 2 + 16;
		m->data = realloc (m->data, m->size);
		if (!m->data)
			abort ();
	}
	memcpy (m->data + m->len, bytes, n);
	m->len += n;
}

cse_msg_t *CSE_MsgByte (cse_msg_t *m, uint8_t v)
{
	CSE_MsgPut (m, &v, 1);
	return m;
}

cse_msg_t *CSE_MsgShort (cse_msg_t *m, int16_t v)
{
	uint8_t	b[2] = {(uint8_t)v, (uint8_t)((uint16_t)v >> 8)};

	CSE_MsgPut (m, b, 2);
	return m;
}

cse_msg_t *CSE_MsgFloat (cse_msg_t *m, float v)
{
	uint32_t	u = CSE_Bits (v);
	uint8_t		b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};

	CSE_MsgPut (m, b, 4);
	return m;
}

cse_msg_t *CSE_MsgCoords (cse_msg_t *m, const float v[3])
{
	float	r;
	int		k;

	for (k = 0 ; k < 3 ; k++)
	{
		// (c * 8.0).round() as i16: half away from zero, saturating
		r = roundf (v[k] * 8.0f);
		CSE_MsgShort (m, r != r ? 0 : r <= -32768.0f ? INT16_MIN : r >= 32767.0f ? INT16_MAX : (int16_t)r);
	}
	return m;
}

void CSE_SnapshotMsg (const cse_snapshot_t *s, cse_msg_t *m)
{
	CSE_MsgByte (m, EZCSQC_WEAPONINFO);
	CSE_MsgByte (m, 0xFF);
	CSE_MsgByte (m, 0);			// impulse
	CSE_MsgByte (m, (uint8_t)(s->generation << 4 | s->weapon));
	CSE_MsgByte (m, 25);			// shells
	CSE_MsgByte (m, 100);			// nails
	CSE_MsgByte (m, s->rockets);
	CSE_MsgByte (m, 50);			// cells
	CSE_MsgFloat (m, s->attack_finished);
	CSE_MsgFloat (m, 0);			// client_nextthink
	CSE_MsgByte (m, 0);			// client_thinkindex
	CSE_MsgFloat (m, s->client_time);
	CSE_MsgByte (m, 0);			// frame
	CSE_MsgByte (m, s->predflags);
	CSE_MsgByte (m, s->ping_ms);
}

void CSE_ProjectileMsg (cse_msg_t *m, const float origin[3], const float velocity[3], int16_t model,
	int16_t owner)
{
	CSE_MsgByte (m, EZCSQC_PROJECTILE);
	CSE_MsgByte (m, 0x1F);
	CSE_MsgCoords (m, origin);
	CSE_MsgCoords (m, velocity);
	CSE_MsgFloat (m, 0);
	CSE_MsgShort (m, model);
	CSE_MsgShort (m, 0);			// effects
	CSE_MsgByte (m, 0);
	CSE_MsgByte (m, 0);
	CSE_MsgByte (m, 0);			// angles
	CSE_MsgShort (m, owner);
	CSE_MsgCoords (m, origin);		// spawn origin
}

/*
==============================================================================

THE CLIENT

==============================================================================
*/

bool CSE_ClientStart (cse_client_t *c, qc_progs_t *progs, const qc_builtins_t *builtins)
{
	qc_config_t	config;
	qc_error_t	err;
	char		text[1024];

	QC_DefaultConfig (&config, QC_CSQC);
	c->vm = QC_Create (progs, builtins, &config, &cse_host, &c->host, &err);
	if (!c->vm)
	{
		printf ("QC_Create: %s\n", QC_ErrorText (&err, text, sizeof(text)));
		QC_FreeError (&err);
		return false;
	}
	CSE_ClientInit (c);
	return true;
}

void CSE_ClientInit (cse_client_t *c)
{
	qcvm_t		*vm = c->vm;
	qc_value_t	args[3];
	bool		ok;

	c->host.has_handles = CSE_Field (vm, "drawmask", QC_EV_FLOAT, &c->host.drawmask_f);
	c->host.has_predraw = CSE_Field (vm, "predraw", QC_EV_FUNCTION, &c->host.predraw_f);
	c->numents = 0;
	c->frame = 0;
	c->server = (cse_server_t){.acked = -1};
	QT_CHECK (QC_SyncAutocvars (vm));
	CSE_SetGlobalF (vm, "player_localentnum", PLAYER_ENT);
	CSE_SetGlobalF (vm, "player_localnum", 0);
	args[0] = QC_ValFloat (0);
	args[1] = QC_ValWord (QC_TempString (vm, "FTE", 3));
	args[2] = QC_ValFloat (5000);
	ok = CSE_Call (c, "CSQC_Init", 3, args, NULL);
	CSE_OK (c, ok);
	QC_SetProtected (vm, 0, true);
}

void CSE_ClientFree (cse_client_t *c)
{
	QC_Destroy (c->vm);
	CSE_EngineFree (&c->host);
	free (c->ents);
	*c = (cse_client_t){0};
}

bool CSE_Report (const cse_client_t *c, bool ok, const char *what, const char *file, int line)
{
	const qc_error_t	*e;
	char				*text;

	if (ok)
		return true;
	QT_Check (false, what, file, line);
	if (!c->vm || !(text = malloc (16384)))
		return false;
	e = QC_LastError (c->vm);
	printf ("  %s\n", QC_ErrorText (e, text, 16384));
	printf ("%s", QC_BacktraceText (&e->backtrace, text, 16384));
	free (text);
	return false;
}

qc_func_t CSE_Func (cse_client_t *c, const char *name)
{
	qc_func_t	f = QC_FindFunction (c->vm, name);

	if (!f)
	{
		QT_Check (false, "the progs has the function", __FILE__, __LINE__);
		printf ("  %s missing\n", name);
	}
	return f;
}

bool CSE_Call (cse_client_t *c, const char *name, int argc, const qc_value_t *args, qc_value_t *ret)
{
	return QC_Call (c->vm, CSE_Func (c, name), argc, args, ret);
}

float CSE_Time (const cse_client_t *c)
{
	return (float)c->frame / FPS;
}

qc_ent_t CSE_Ent (const cse_client_t *c, uint16_t entnum)
{
	uint32_t	i;

	for (i = 0 ; i < c->numents ; i++)
		if (c->ents[i].num == entnum)
			return c->ents[i].e;
	return 0;
}

bool CSE_EntUpdate (cse_client_t *c, uint16_t entnum, cse_msg_t *msg)
{
	qc_ent_t	e = CSE_Ent (c, entnum);
	bool		isnew = !e, ok;
	qc_value_t	arg;

	if (isnew)
	{
		QT_CHECK (QC_Spawn (c->vm, &e));
		CSE_SetFieldF (c->vm, e, "entnum", (float)entnum);
		c->ents = CSE_Grow (c->ents, &c->entsize, c->numents, sizeof(*c->ents));
		c->ents[c->numents++] = (cse_entmap_t){entnum, e};
	}
	free (c->host.net);
	c->host.net = msg->data;
	c->host.netlen = msg->len;
	c->host.netpos = 0;
	*msg = (cse_msg_t){0};
	arg = QC_ValFloat (isnew ? 1 : 0);
	ok = QC_CallAs (c->vm, e, CSE_Func (c, "CSQC_Ent_Update"), 1, &arg, NULL);
	if (ok)
	{
		if (!QT_CHECK (!c->host.net_underflow))
			printf ("  CSQC_Ent_Update read past the message\n");
		if (!QT_EQ_U (CSE_NetLeft (&c->host), 0))
			printf ("  CSQC_Ent_Update left bytes\n");
	}
	return ok;
}

void CSE_EntRemove (cse_client_t *c, uint16_t entnum)
{
	qc_ent_t	e = 0;
	uint32_t	i;
	bool		ok;

	for (i = 0 ; i < c->numents ; i++)
		if (c->ents[i].num == entnum)
		{
			e = c->ents[i].e;
			c->ents[i] = c->ents[--c->numents];
			break;
		}
	if (!QT_CHECK (e != 0))
		return;
	ok = QC_CallAs (c->vm, e, CSE_Func (c, "CSQC_Ent_Remove"), 0, NULL, NULL);
	CSE_OK (c, ok);
	QT_CHECK (QC_IsFree (c->vm, e));
}

void CSE_BeginFrame (cse_client_t *c, bool attack)
{
	cse_engine_t	*h = &c->host;
	float			now = CSE_Time (c);
	uint32_t		n;

	if ((uint32_t)c->frame >= h->numinputs)
	{
		n = h->numinputs ? h->numinputs * 2 : 1024;
		while (n <= (uint32_t)c->frame)
			n *= 2;
		h->inputs = realloc (h->inputs, n * sizeof(*h->inputs));
		if (!h->inputs)
			abort ();
		memset (h->inputs + h->numinputs, 0, (n - h->numinputs) * sizeof(*h->inputs));
		h->numinputs = n;
	}
	h->inputs[c->frame] = (cse_input_t){true, 1.0f / FPS, {0, 90, 0}, attack ? 1.0f : 0.0f, 0};
	QC_SetTime (c->vm, (double)now);
	CSE_SetGlobalF (c->vm, "time", now);
	CSE_SetGlobalF (c->vm, "cltime", now);
	CSE_SetGlobalF (c->vm, "frametime", 1.0f / FPS);
	CSE_SetGlobalF (c->vm, "clientcommandframe", (float)(c->frame + 1));
	CSE_SetGlobalF (c->vm, "servercommandframe", (float)(c->server.acked > 0 ? c->server.acked : 0));
	CSE_SetGlobalV (c->vm, "pmove_org", 0, 0, 24);
	CSE_SetGlobalV (c->vm, "view_angles", 0, 90, 0);
	h->view_origin[0] = 0;
	h->view_origin[1] = 0;
	h->view_origin[2] = 46;
}

void CSE_Draw (cse_client_t *c)
{
	qc_value_t	args[3] = {QC_ValFloat (640), QC_ValFloat (480), QC_ValFloat (1)};
	bool		ok = CSE_Call (c, "CSQC_UpdateView", 3, args, NULL);

	CSE_OK (c, ok);
	c->frame++;
}

void CSE_Render (cse_client_t *c, bool attack)
{
	CSE_BeginFrame (c, attack);
	CSE_Draw (c);
}

void CSE_Step (cse_client_t *c, bool attack)
{
	cse_server_t	*s = &c->server;
	cse_engine_t	*h = &c->host;
	int32_t			target = c->frame - LATENCY;
	cse_snapshot_t	snap;
	cse_msg_t		msg = {0};
	bool			held, ok;

	while (s->acked < target)
	{
		s->acked++;
		s->clock += 1.0f / FPS;
		held = (uint32_t)s->acked < h->numinputs && h->inputs[s->acked].present && h->inputs[s->acked].buttons != 0;
		if (held && s->clock >= s->attack_finished)
		{
			s->attack_finished = s->clock + 0.8f;
			s->shots++;
		}
	}
	CSE_BeginFrame (c, attack);
	if (c->frame % 6 == 0 && s->acked >= 0)
	{
		snap = (cse_snapshot_t){ROCKET_LAUNCHER, 1, (uint8_t)(s->shots < 100 ? 100 - s->shots : 0),
			s->attack_finished, s->clock, 50, 0};
		CSE_SnapshotMsg (&snap, &msg);
		ok = CSE_EntUpdate (c, 500, &msg);
		CSE_OK (c, ok);
	}
	CSE_Draw (c);
}

qc_ent_t CSE_LocalProjectile (const cse_client_t *c)
{
	uint32_t	n = QC_NumEdicts (c->vm), e, ofs, v;

	if (!CSE_Field (c->vm, "is_local", QC_EV_FLOAT, &ofs))
		return 0;
	for (e = 0 ; e < n ; e++)
		if (!QC_IsFree (c->vm, e) && QC_GetField (c->vm, e, ofs, 1, &v) && CSE_Float (v) == 1.0f)
			return e;
	return 0;
}
