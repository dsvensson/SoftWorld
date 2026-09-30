// qc_lib_entity.c -- entity allocation, iteration and search (docs/spec/builtins.md)
//
// Searches visit entities 1 to num_edicts - 1, skip free slots and never
// return the world except to mean "nothing found". Chains are built by
// prepending: a chain starts at the highest-numbered match, and its last
// entity's chain field is the world.

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

// FL_FINDABLE_NONSOLID: lets findradius find non-solid entities
#define QC_FL_FINDABLE_NONSOLID		16384

// the first entity in use after e (never the world), or num_edicts
static uint32_t QC_NextLive (const qcvm_t *vm, uint32_t e)
{
	uint64_t	i = (uint64_t)e + 1;

	for (i = i < 1 ? 1 : i ; i < vm->mem.num_edicts ; i++)
		if (vm->mem.slots[i].in_use)
			return (uint32_t)i;
	return vm->mem.num_edicts;
}

static bool QC_BadField (qcvm_t *vm, const char *name)
{
	return QC_Error (vm, "%s: bad field reference", name);
}

// the chain field: argument i if passed, else the progs' .chain
static bool QC_ChainField (qcvm_t *vm, int i, const char *name, uint32_t *cf)
{
	int64_t	f = QC_Argc (vm) > i ? (int64_t)QC_ArgWord (vm, i) : QC_LibField (vm, "chain");

	if (f < 0 || !QC_LibFieldOk (vm, (uint32_t)f, 1))
		return QC_BadField (vm, name);
	*cf = (uint32_t)f;
	return true;
}

// a growing list of entity numbers
typedef struct
{
	uint32_t	*e;
	uint32_t	count, size;
	bool		failed;
} qc_entlist_t;

static void QC_ListAdd (qc_entlist_t *l, uint32_t e)
{
	uint32_t	*grown, size;

	if (l->count == l->size)
	{
		size = l->size ? l->size * 2 : 64;
		grown = realloc (l->e, (size_t)size * sizeof(*grown));
		if (!grown)
		{
			l->failed = true;
			return;
		}
		l->e = grown;
		l->size = size;
	}
	l->e[l->count++] = e;
}

// links the matches (ascending) through the chain field, and returns the head
static bool QC_ReturnChain (qcvm_t *vm, qc_entlist_t *l, uint32_t cf)
{
	uint32_t	chain = 0, i;

	if (l->failed)
	{
		free (l->e);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	for (i = 0 ; i < l->count ; i++)
	{
		QC_LibSetWord (vm, l->e[i], cf, chain);
		chain = l->e[i];
	}
	free (l->e);
	QC_ReturnWord (vm, chain);
	return true;
}

// the matches as a zero-terminated temp int array, their count in __out
// parameter count_arg
static bool QC_ReturnList (qcvm_t *vm, qc_entlist_t *l, int count_arg)
{
	uint8_t		*bytes;
	uint32_t	i, ref;

	if (l->failed || !(bytes = malloc (((size_t)l->count + 1) * 4)))
	{
		free (l->e);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	for (i = 0 ; i < l->count ; i++)
		memcpy (bytes + (size_t)i * 4, &l->e[i], 4);
	memset (bytes + (size_t)l->count * 4, 0, 4);
	ref = QC_NewTemp (vm, (const char *)bytes, ((size_t)l->count + 1) * 4);
	free (bytes);
	free (l->e);
	if (!ref)
		return false;
	QC_LibSetArgWord (vm, count_arg, l->count);
	QC_ReturnWord (vm, ref);
	return true;
}

/*
==============================================================================

ALLOCATION

==============================================================================
*/

// entity spawn(): every field zeroed, FTE's slot reuse
static bool QC_SpawnBuiltin (qcvm_t *vm)
{
	qc_ent_t	e;

	if (!QC_Spawn (vm, &e))
		return false;
	QC_ReturnWord (vm, e);
	return true;
}

// void remove(entity e): the world, free and protected entities are refused
// with a warning; the slot isn't reused for half a second
static bool QC_RemoveBuiltin (qcvm_t *vm)
{
	QC_Remove (vm, QC_LibEntArg (vm, 0), false);
	return true;
}

// void removeinstant(entity e): the slot may be reused at once
static bool QC_Removeinstant (qcvm_t *vm)
{
	QC_Remove (vm, QC_LibEntArg (vm, 0), true);
	return true;
}

/*
==============================================================================

ITERATION AND SEARCH

==============================================================================
*/

// entity nextent(entity e)
static bool QC_Nextent (qcvm_t *vm)
{
	uint32_t	e = QC_NextLive (vm, QC_ArgWord (vm, 0));

	QC_ReturnWord (vm, e < vm->mem.num_edicts ? e : 0);
	return true;
}

// entity find(entity start, .string fld, string match): searching for "" finds
// null and empty fields; otherwise a null field never matches
static bool QC_FindBuiltin (qcvm_t *vm)
{
	uint32_t	start = QC_ArgWord (vm, 0), f = QC_ArgWord (vm, 1), e, t;
	const char	*want, *text;
	char		*copy;

	if (!QC_LibFieldOk (vm, f, 1))
		return QC_BadField (vm, "find");
	want = QC_ArgString (vm, 2);
	if (!*want && vm->config.developer)
		QC_Warning (vm, "find: empty string");
	copy = malloc (strlen (want) + 1);
	if (!copy)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	strcpy (copy, want);
	for (e = QC_NextLive (vm, start) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
	{
		t = QC_LibWord (vm, e, f);
		text = QC_Str (vm, t);
		if (!text)
			text = "";
		if (*copy ? t && !strcmp (text, copy) : !t || !*text)
			break;
	}
	free (copy);
	QC_ReturnWord (vm, e < vm->mem.num_edicts ? e : 0);
	return true;
}

// entity findfloat(entity start, .__variant fld, __variant match) (findentity):
// the field's bits equal match's (-0 isn't 0, a NaN matches itself)
static bool QC_Findfloat (qcvm_t *vm)
{
	uint32_t	start = QC_ArgWord (vm, 0), f = QC_ArgWord (vm, 1), want = QC_ArgWord (vm, 2), e;

	if (QC_Argc (vm) != 3)
		return QC_Error (vm, "findfloat: exactly 3 arguments are required");
	if (!QC_LibFieldOk (vm, f, 1))
		return QC_BadField (vm, "findfloat");
	for (e = QC_NextLive (vm, start) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
		if (QC_LibWord (vm, e, f) == want)
			break;
	QC_ReturnWord (vm, e < vm->mem.num_edicts ? e : 0);
	return true;
}

// entity findflags(entity start, .float fld, float flags): a bit in common
static bool QC_Findflags (qcvm_t *vm)
{
	uint32_t	start = QC_ArgWord (vm, 0), f = QC_ArgWord (vm, 1), e;
	int32_t		flags;

	if (!QC_LibFieldOk (vm, f, 1))
		return QC_BadField (vm, "findflags");
	flags = QC_LibArgInt (vm, 2);
	for (e = QC_NextLive (vm, start) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
		if (QC_FloatToInt (QC_LibFloat (vm, e, f)) & flags)
			break;
	QC_ReturnWord (vm, e < vm->mem.num_edicts ? e : 0);
	return true;
}

// entity findchain(.string fld, string match, optional .entity chainfld = chain):
// null fields never match, even for ""
static bool QC_Findchain (qcvm_t *vm)
{
	uint32_t		f = QC_ArgWord (vm, 0), cf, e, t;
	qc_entlist_t	l = {0};
	const char		*text;
	char			*want;

	if (!QC_ChainField (vm, 2, "findchain", &cf))
		return false;
	if (!QC_LibFieldOk (vm, f, 1))
		return QC_BadField (vm, "findchain");
	text = QC_ArgString (vm, 1);
	want = malloc (strlen (text) + 1);
	if (!want)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	strcpy (want, text);
	for (e = QC_NextLive (vm, 0) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
	{
		t = QC_LibWord (vm, e, f);
		text = t ? QC_Str (vm, t) : NULL;
		if (t && !strcmp (text ? text : "", want))
			QC_ListAdd (&l, e);
	}
	free (want);
	return QC_ReturnChain (vm, &l, cf);
}

// entity findchainfloat(.float fld, float match, optional .entity chainfld):
// equal as floats (-0 == 0, a NaN never matches)
static bool QC_Findchainfloat (qcvm_t *vm)
{
	uint32_t		f = QC_ArgWord (vm, 0), cf, e;
	qc_entlist_t	l = {0};
	float			want;

	if (!QC_ChainField (vm, 2, "findchainfloat", &cf))
		return false;
	if (!QC_LibFieldOk (vm, f, 1))
		return QC_BadField (vm, "findchainfloat");
	want = QC_ArgFloat (vm, 1);
	for (e = QC_NextLive (vm, 0) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
		if (QC_LibFloat (vm, e, f) == want)
			QC_ListAdd (&l, e);
	return QC_ReturnChain (vm, &l, cf);
}

// entity findchainflags(.float fld, float flags, optional .entity chainfld)
static bool QC_Findchainflags (qcvm_t *vm)
{
	uint32_t		f = QC_ArgWord (vm, 0), cf, e;
	qc_entlist_t	l = {0};
	int32_t			flags;

	if (!QC_ChainField (vm, 2, "findchainflags", &cf))
		return false;
	if (!QC_LibFieldOk (vm, f, 1))
		return QC_BadField (vm, "findchainflags");
	flags = QC_LibArgInt (vm, 1);
	for (e = QC_NextLive (vm, 0) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
		if (QC_FloatToInt (QC_LibFloat (vm, e, f)) & flags)
			QC_ListAdd (&l, e);
	return QC_ReturnChain (vm, &l, cf);
}

// the entities whose bounding box centre is within rad of org, the non-solid
// ones only if flagged FL_FINDABLE_NONSOLID; ascending
static void QC_RadiusMatches (const qcvm_t *vm, const float org[3], float rad, qc_entlist_t *l)
{
	float		rad2 = rad * rad, origin[3], mins[3], maxs[3], d[3];
	uint32_t	e;
	int			j;

	for (e = QC_NextLive (vm, 0) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
	{
		if (QC_LibNamedFloat (vm, e, "solid") == 0
			&& !(QC_FloatToInt (QC_LibNamedFloat (vm, e, "flags")) & QC_FL_FINDABLE_NONSOLID))
			continue;
		QC_LibNamedVector (vm, e, "origin", origin);
		QC_LibNamedVector (vm, e, "mins", mins);
		QC_LibNamedVector (vm, e, "maxs", maxs);
		// FTE mixes precisions here: the half-size sum is scaled in double
		for (j = 0 ; j < 3 ; j++)
			d[j] = (float)((double)org[j] - ((double)origin[j] + (double)(mins[j] + maxs[j]) * 0.5));
		if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= rad2)
			QC_ListAdd (l, e);
	}
}

// entity findradius(vector org, float rad, optional .entity chainfld = chain)
static bool QC_Findradius (qcvm_t *vm)
{
	float			org[3], rad;
	uint32_t		cf;
	qc_entlist_t	l = {0};

	QC_ArgVector (vm, 0, org);
	rad = QC_ArgFloat (vm, 1);
	if (!QC_ChainField (vm, 2, "findradius", &cf))
		return false;
	QC_RadiusMatches (vm, org, rad, &l);
	return QC_ReturnChain (vm, &l, cf);
}

// entity *findradius_list(vector org, float rad, __out int count, int sort = 0):
// findradius's matches as a zero-terminated temp array (ascending, whatever sort)
static bool QC_FindradiusList (qcvm_t *vm)
{
	float			org[3];
	qc_entlist_t	l = {0};

	QC_ArgVector (vm, 0, org);
	QC_RadiusMatches (vm, org, QC_ArgFloat (vm, 1), &l);
	return QC_ReturnList (vm, &l, 2);
}

// the words of a value of a type find_list accepts, or 0
static uint32_t QC_ListTypeWords (int32_t type)
{
	if ((type >= 0 && type <= 2) || (type >= 4 && type <= 9))
		return 1;
	if (type == 3)
		return 3;
	if (type >= 10 && type <= 12)
		return 2;
	return 0;
}

// entity *find_list(.__variant fld, __variant match, int type = EV_STRING, __out
// int count): strings by content (null fields never match; read-only entities
// are skipped), floats and doubles by value, vectors and 64-bit integers per
// component, the rest by their bits
static bool QC_FindList (qcvm_t *vm)
{
	uint32_t		f = QC_ArgWord (vm, 0), want[3], words, e, w[3], k;
	int32_t			type = QC_Argc (vm) > 2 ? QC_ArgInt (vm, 2) : 1;
	static const uint32_t	zero[3] = {0, 0, 0};
	qc_entlist_t	l = {0};
	const char		*text;
	char			*wanttext;
	bool			match;

	words = QC_ListTypeWords (type);
	if (!words || !QC_LibFieldOk (vm, f, words))
	{
		QC_LibSetArgWord (vm, 3, 0);
		QC_ReturnRaw (vm, zero);
		return true;
	}
	QC_ArgRaw (vm, 1, want);
	text = QC_ArgString (vm, 1);
	wanttext = malloc (strlen (text) + 1);
	if (!wanttext)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	strcpy (wanttext, text);
	for (e = QC_NextLive (vm, 0) ; e < vm->mem.num_edicts ; e = QC_NextLive (vm, e))
	{
		for (k = 0 ; k < 3 ; k++)
			w[k] = QC_LibWord (vm, e, f + k);
		switch (type)
		{
		case 1:
			text = w[0] ? QC_Str (vm, w[0]) : NULL;
			match = !QC_Protected (&vm->mem, e) && w[0] && !strcmp (text ? text : "", wanttext);
			break;
		case 2:
			match = QC_BitsFloat (w[0]) == QC_BitsFloat (want[0]);
			break;
		case 3:
			match = QC_BitsFloat (w[0]) == QC_BitsFloat (want[0]) && QC_BitsFloat (w[1]) == QC_BitsFloat (want[1])
				&& QC_BitsFloat (w[2]) == QC_BitsFloat (want[2]);
			break;
		case 12:
			match = QC_BitsDouble (w[0] | ((uint64_t)w[1] << 32)) == QC_BitsDouble (want[0] | ((uint64_t)want[1] << 32));
			break;
		case 10:
		case 11:
			match = w[0] == want[0] && w[1] == want[1];
			break;
		default:
			match = w[0] == want[0];
			break;
		}
		if (match)
			QC_ListAdd (&l, e);
	}
	free (wanttext);
	return QC_ReturnList (vm, &l, 3);
}

/*
==============================================================================

NUMBERS AND REFERENCES

==============================================================================
*/

// entity edict_num(float n): even a free one; the world out of range
static bool QC_EdictNum (qcvm_t *vm)
{
	uint32_t	n = QC_F2U (QC_ArgFloat (vm, 0));

	QC_ReturnWord (vm, n < vm->mem.num_edicts ? n : 0);
	return true;
}

static bool QC_NumForEdict (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)QC_LibEntArg (vm, 0));
	return true;
}

// float wasfreed(entity e): whether its slot isn't in use
static bool QC_Wasfreed (qcvm_t *vm)
{
	QC_LibReturnBool (vm, !QC_InUse (&vm->mem, QC_LibEntArg (vm, 0)));
	return true;
}

// float etof(entity e) (menu)
static bool QC_Etof (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)QC_ArgWord (vm, 0));
	return true;
}

// entity ftoe(float n) (menu): the world out of range
static bool QC_Ftoe (qcvm_t *vm)
{
	int32_t	n = QC_LibArgInt (vm, 0);

	QC_ReturnWord (vm, n >= 0 && (uint32_t)n < vm->mem.num_edicts ? (uint32_t)n : 0);
	return true;
}

// entity copyentity(entity from, optional entity to): every field of from into
// to (a new entity without one); from or to free, or to read-only, is an error
static bool QC_Copyentity (qcvm_t *vm)
{
	uint32_t	from = QC_LibEntArg (vm, 0), to, words = vm->mem.field_bytes / 4;
	qc_ent_t	e;
	uint8_t		*src, *dst;

	if (QC_Argc (vm) <= 1)
	{
		if (!QC_Spawn (vm, &e))
			return false;
		to = e;
	}
	else
		to = QC_LibEntArg (vm, 1);
	if (!QC_InUse (&vm->mem, from) && !QC_LibSoftError (vm, "copyentity: source is free"))
		return false;
	if (!QC_InUse (&vm->mem, to) && !QC_LibSoftError (vm, "copyentity: destination is free"))
		return false;
	if (QC_Protected (&vm->mem, to) && !QC_LibSoftError (vm, "copyentity: destination is read-only"))
		return false;
	src = words ? QC_FieldPtr (&vm->mem, from, 0, words) : NULL;
	dst = words ? QC_FieldPtr (&vm->mem, to, 0, words) : NULL;
	if (src && dst)
		memmove (dst, src, vm->mem.field_bytes);
	QC_ReturnWord (vm, to);
	return true;
}

// float entityprotection(entity e, float readonly): protects the entity from
// QuakeC's writes (1) or not (0), returning what it was before; other values
// change nothing. FTE returns the new setting instead.
static bool QC_Entityprotection (qcvm_t *vm)
{
	uint32_t	e = QC_LibEntArg (vm, 0);
	int32_t		prot = QC_LibArgInt (vm, 1);
	bool		previous;

	if (!QC_InUse (&vm->mem, e) && !QC_LibSoftError (vm, "entityprotection: entity is free"))
		return false;
	previous = QC_Protected (&vm->mem, e);
	if (prot == 0 || prot == 1)
		QC_SetProtected (vm, e, prot == 1);
	QC_LibReturnBool (vm, previous);
	return true;
}

static const qc_libentry_t	qc_entity[] = {
	{"spawn", QC_SpawnBuiltin, NULL, 0},
	{"remove", QC_RemoveBuiltin, NULL, 0},
	{"removeinstant", QC_Removeinstant, NULL, 0},
	{"nextent", QC_Nextent, NULL, 0},
	{"find", QC_FindBuiltin, NULL, 0},
	{"findfloat", QC_Findfloat, NULL, 0},
	{"findentity", NULL, "findfloat", 0},
	{"findflags", QC_Findflags, NULL, 0},
	{"findchain", QC_Findchain, NULL, 0},
	{"findchainfloat", QC_Findchainfloat, NULL, 0},
	{"findchainflags", QC_Findchainflags, NULL, 0},
	{"findradius", QC_Findradius, NULL, 0},
	{"find_list", QC_FindList, NULL, 0},
	{"findradius_list", QC_FindradiusList, NULL, 0},
	{"edict_num", QC_EdictNum, NULL, 0},
	{"num_for_edict", QC_NumForEdict, NULL, 0},
	{"wasfreed", QC_Wasfreed, NULL, 0},
	{"copyentity", QC_Copyentity, NULL, 0},
	{"entityprotection", QC_Entityprotection, NULL, 0},
	{"etof", QC_Etof, NULL, 0},
	{"ftoe", QC_Ftoe, NULL, 0},
};

bool QC_RegisterEntity (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_entity, sizeof(qc_entity) / sizeof(qc_entity[0]));
}
