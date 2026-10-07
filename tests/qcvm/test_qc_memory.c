// test_qc_memory.c -- the VM's memory before anything runs: its layout, where
// addresses land, the host's header in each entity and its field layout, entity
// slots and their reuse, fields added later, the heap, temp and static strings
// and their collection

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float GetFloat (qcvm_t *vm, qc_ent_t e, uint32_t ofs)
{
	uint32_t	w = 0;

	QT_CHECK (QC_GetField (vm, e, ofs, 1, &w));
	return QC_BitsFloat (w);
}

static void SetFloat (qcvm_t *vm, qc_ent_t e, uint32_t ofs, float v)
{
	uint32_t	w = QC_FloatBits (v);

	QT_CHECK (QC_SetField (vm, e, ofs, 1, &w));
}

static uint32_t Field (qcvm_t *vm, const char *name)
{
	uint32_t	ofs = 0;

	QT_CHECK (QC_FindField (vm, name, &ofs, NULL));
	return ofs;
}

/*
==============================================================================

THE HEAP

==============================================================================
*/

static void TestHeap (void)
{
	qc_heap_t	h;
	uint32_t	a = 0, b = 0, c = 0, size = 0;

	QT_CHECK (QC_HeapInit (&h, 1u << 20));
	QT_CHECK (QC_HeapAlloc (&h, 10, &a) && QC_HeapAlloc (&h, 20, &b) && QC_HeapAlloc (&h, 30, &c));
	QT_CHECK (a == 0 && b == 16 && c == 48);
	QT_CHECK (QC_HeapRelease (&h, b));
	QT_CHECK (!QC_HeapRelease (&h, b));
	QT_CHECK (QC_HeapAlloc (&h, 5, &b) && b == 16);
	QT_CHECK (QC_HeapRelease (&h, 16));
	QT_CHECK (QC_HeapRelease (&h, a));
	// a and b coalesced into one 48-byte block
	QT_CHECK (QC_HeapAlloc (&h, 40, &a) && a == 0);
	QT_CHECK (QC_HeapBlockSize (&h, 0, &size) && size == 40);
	QC_HeapFree (&h);

	// blocks come zeroed, and the heap is bounded
	QT_CHECK (QC_HeapInit (&h, 64));
	QT_CHECK (QC_HeapAlloc (&h, 16, &a));
	memset (h.region.base, 0xAA, h.len);
	QT_CHECK (QC_HeapRelease (&h, a));
	QT_CHECK (QC_HeapAlloc (&h, 16, &b) && b == a);
	QT_CHECK (h.region.base[0] == 0 && h.region.base[15] == 0);
	QT_CHECK (!QC_HeapAlloc (&h, 64, &c));
	QT_CHECK (QC_HeapAlloc (&h, 48, &c));
	QC_HeapFree (&h);

	// realloc keeps what the block held
	QT_CHECK (QC_HeapInit (&h, 1u << 16));
	QT_CHECK (QC_HeapAlloc (&h, 4, &a));
	memcpy (h.region.base + a, "\1\2\3\4", 4);
	QT_CHECK (QC_HeapRealloc (&h, true, a, 100, &b));
	QT_CHECK (!memcmp (h.region.base + b, "\1\2\3\4", 4));
	QT_CHECK (!QC_HeapBlockSize (&h, a, &size));
	QC_HeapFree (&h);
}

/*
==============================================================================

STRINGS ON THEIR OWN

==============================================================================
*/

static void TestStrings (void)
{
	qc_strings_t	s;
	uint32_t		a = 0, b = 0, c = 0, size = 0, x = 0, grown = 0;
	uint8_t			*marks, *data, root[4];

	QC_StringsInit (&s, 1u << 20, 1u << 24);
	QT_CHECK (QC_TempAlloc (&s, "hello", 5, &a));
	QT_CHECK (QC_TempAlloc (&s, "", 0, &b));
	QT_EQ_U (a, QC_TEMP_TAG | 0);
	QT_CHECK (b != 0);		// empty temps are not null
	data = QC_TempData (&s, a & QC_INDEX_MASK, &size);
	QT_CHECK (data && !strcmp ((char *)data, "hello") && size == 8);
	memcpy (root, &a, 4);
	QT_CHECK (QC_GCBegin (&s, &marks));
	QC_GCMark (&s, marks, root, 4);
	QT_EQ_U (QC_GCSweep (&s, marks), 1);
	QT_EQ_U (s.live, 1);
	QT_CHECK (!QC_TempData (&s, b & QC_INDEX_MASK, NULL));
	QT_CHECK (QC_TempData (&s, a & QC_INDEX_MASK, NULL) != NULL);
	QC_StringsFree (&s);

	// pins survive collection
	QC_StringsInit (&s, 1u << 20, 1u << 24);
	QT_CHECK (QC_TempAlloc (&s, "x", 1, &a));
	QC_StringsPin (&s, a);
	QT_CHECK (QC_GCBegin (&s, &marks));
	QT_EQ_U (QC_GCSweep (&s, marks), 0);
	QC_StringsUnpin (&s, a);
	QT_CHECK (QC_GCBegin (&s, &marks));
	QT_EQ_U (QC_GCSweep (&s, marks), 1);
	QC_StringsFree (&s);

	// limits: a count, and bytes
	QC_StringsInit (&s, 2, 1u << 20);
	QT_CHECK (QC_TempAlloc (&s, "a", 1, &a));
	QT_CHECK (QC_TempAlloc (&s, "b", 1, &b));
	QT_CHECK (!QC_TempAlloc (&s, "c", 1, &c));
	QC_StringsFree (&s);
	QC_StringsInit (&s, 100, 16);
	QT_CHECK (QC_TempAlloc (&s, "0123456789", 10, &a));
	QT_CHECK (!QC_TempAlloc (&s, "0123456789", 10, &b));
	QC_StringsFree (&s);

	// interned text dedups; borrowed text is found by its pointer
	QC_StringsInit (&s, 8, 64);
	QT_CHECK (QC_StaticIntern (&s, "model", 5, &a));
	QT_CHECK (QC_StaticIntern (&s, "model", 5, &b) && a == b);
	QT_EQ_U (a, QC_STATIC_TAG | 0);
	QT_EQ_S (QC_StaticText (&s, 0), "model");
	{
		static char	name[16] = "player";

		QT_CHECK (QC_StaticBorrow (&s, name, &c));
		QT_CHECK (QC_StaticBorrow (&s, name, &x) && x == c);
		QT_CHECK (c != a);
		name[0] = 'P';
		QT_EQ_S (QC_StaticText (&s, c & QC_INDEX_MASK), "Player");
	}
	QC_StringsFree (&s);

	// temps grow when written past their end
	QC_StringsInit (&s, 8, 1u << 21);
	QT_CHECK (QC_TempAlloc (&s, "ab", 2, &a));
	QT_CHECK (QC_TempGrow (&s, a & QC_INDEX_MASK, 10) != NULL);
	QC_TempData (&s, a & QC_INDEX_MASK, &grown);
	QT_EQ_U (grown, 12);
	QT_CHECK (QC_TempGrow (&s, a & QC_INDEX_MASK, QC_MAX_TEMP_GROWTH + 1) == NULL);
	QC_StringsFree (&s);
}

/*
==============================================================================

LAYOUT

==============================================================================
*/

static void TestLayout (void)
{
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm;
	qc_word_t	*g;
	uint32_t	hello, ptr, word, type, thisprogs, fast, target;
	qc_loc_t	loc;

	hello = QA_StrConst (a, "hello");
	target = QA_Global1 (a, "target", QC_EV_FLOAT, QC_FloatBits (5));
	ptr = QA_Global1 (a, "ptr", QC_EV_POINTER, 0x80000000u | target * 4);
	thisprogs = QA_Global1 (a, "thisprogs", QC_EV_FLOAT, QC_FloatBits (9));
	fast = QA_Global1 (a, "__ext__fasttrackarrays", QC_EV_FLOAT, 0);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	g = QC_Globals (vm);

	// the strings start at address 0, so string offsets are addresses
	QT_EQ_S (QC_String (vm, g[hello].u), "hello");
	QT_EQ_S (QC_String (vm, 0), "");
	QT_CHECK (QC_FindGlobal (vm, "target", &word, &type) && word == target && type == QC_EV_FLOAT);
	QT_EQ_F (g[target].f, 5);
	// pointer relocations become byte addresses of the globals
	QT_EQ_U (g[ptr].u, vm->progs[0].gbase + target * 4);
	loc = QC_Locate (&vm->mem, g[ptr].u, 4);
	QT_CHECK (loc.type == QC_LOC_S && loc.p == (uint8_t *)&g[target]);
	QT_EQ_F (g[thisprogs].f, 0);
	QT_EQ_F (g[fast].f, 1);

	// the regions below 2^31, entities after S, the heap after the entities
	QT_CHECK (vm->mem.e_base >= vm->mem.s_len);
	QT_CHECK (vm->mem.h_base >= vm->mem.e_base + ((uint64_t)vm->mem.max_edicts << vm->mem.shift));
	QT_CHECK ((uint64_t)vm->mem.h_base + vm->config.limits.heap_bytes <= 0x80000000ull);
	QT_EQ_U (QC_NumEdicts (vm), 1);
	QT_CHECK (!QC_IsFree (vm, 0));
	QT_EQ_U (QC_Serial (vm, 0), 1);

	QC_Destroy (vm);
	QA_Free (a);
}

/*
==============================================================================

ENTITIES

==============================================================================
*/

typedef struct
{
	int		spawned[8];
	int		numspawned;
	int		removed[8];
	int		numremoved;
	bool	refuse;
	int		warnings;
} hooks_t;

static void OnSpawn (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	hooks_t	*h = ctx;

	(void)vm;
	if (h->numspawned < 8)
		h->spawned[h->numspawned++] = (int)e;
}

static bool OnRemove (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	hooks_t	*h = ctx;

	(void)vm;
	if (h->refuse)
		return false;
	if (h->numremoved < 8)
		h->removed[h->numremoved++] = (int)e;
	return true;
}

static void OnWarning (void *ctx, const qc_warning_t *w)
{
	hooks_t	*h = ctx;
	char	text[256];

	(void)w;
	QC_WarningText (w, text, sizeof(text));
	h->warnings++;
}

static void TestEntities (void)
{
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e, f, reused;
	uint32_t	health, origin, v[3] = {1, 2, 3}, out[3];

	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	health = Field (vm, "health");
	origin = Field (vm, "origin");

	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	SetFloat (vm, e, health, 200);
	QT_CHECK (QC_SetField (vm, e, origin, 3, v));
	QT_CHECK (QC_GetField (vm, e, origin, 3, out) && out[0] == 1 && out[2] == 3);
	QT_CHECK (!QC_IsFree (vm, e));
	QT_CHECK (!QC_GetField (vm, 99, health, 1, out));
	QT_CHECK (!QC_GetField (vm, e, 1000, 1, out));
	QT_CHECK (QC_Spawn (vm, &f) && f == 2);

	// removing zeroes only the configured fields; the slot is reused after half a second
	QC_SetTime (vm, 10.0);
	QT_CHECK (QC_Remove (vm, e, false));
	QT_CHECK (QC_IsFree (vm, e));
	QT_EQ_F (GetFloat (vm, e, health), 200);		// freed entities stay readable
	QC_SetTime (vm, 10.2);
	QT_CHECK (QC_Spawn (vm, &reused) && reused == 3);
	QC_SetTime (vm, 10.6);
	QT_CHECK (QC_Spawn (vm, &reused) && reused == e);
	QT_EQ_F (GetFloat (vm, reused, health), 0);	// spawn zeroes fields

	// refused removals
	QT_CHECK (!QC_Remove (vm, 0, false));
	QT_CHECK (!QC_Remove (vm, 1000, false));
	QT_CHECK (QC_Remove (vm, f, true));
	QT_CHECK (!QC_Remove (vm, f, true));
	QT_CHECK (!QC_SetProtected (vm, e, true));
	QT_CHECK (QC_IsProtected (vm, e));
	QT_CHECK (!QC_Remove (vm, e, true));
	QT_CHECK (QC_SetProtected (vm, e, false));
	QT_EQ_U (vm->numwarnings, 4);

	QC_Destroy (vm);
	QA_Free (a);
}

static qcvm_t *LimitedVM (const qc_asm_t *a, uint32_t max_edicts, uint32_t first_spawnable)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_SSQC);
	config.limits.max_edicts = max_edicts;
	config.first_spawnable = first_spawnable;
	return QA_CreateVM (a, &config, NULL, NULL, NULL);
}

static bool NoFree (qcvm_t *vm)
{
	qc_ent_t	e;

	return !QC_Spawn (vm, &e) && QC_LastError (vm)->kind == QC_ERR_NO_FREE_EDICTS;
}

// max_edicts counts the world, and every slot up to it can be spawned;
// first_spawnable reserves the slots below it, whether the table grows or is reused
static void TestEntityLimits (void)
{
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e;
	uint32_t	i;

	QA_Field (a, "health", QC_EV_FLOAT, NULL);

	vm = LimitedVM (a, 1, 0);		// the world alone
	QT_CHECK (NoFree (vm));
	QC_Destroy (vm);
	vm = LimitedVM (a, 2, 0);		// the world and one
	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	QT_CHECK (NoFree (vm));
	QC_Destroy (vm);
	vm = LimitedVM (a, 64, 0);		// every slot below the limit
	for (i = 1 ; i < 64 ; i++)
		QT_CHECK (QC_Spawn (vm, &e) && e == i);
	QT_CHECK (NoFree (vm));
	QC_Destroy (vm);

	// slots 1-4 reserved (SSQC's clients): a fresh table grows past them
	vm = LimitedVM (a, 7, 5);
	QT_CHECK (QC_Spawn (vm, &e) && e == 5);
	QT_CHECK (QC_Spawn (vm, &e) && e == 6);
	QT_CHECK (NoFree (vm));
	QT_CHECK (QC_IsFree (vm, 1));
	// reuse honours the reservation too
	QC_SetTime (vm, 10);
	QT_CHECK (QC_Remove (vm, 5, false));
	QC_SetTime (vm, 11);
	QT_CHECK (QC_Spawn (vm, &e) && e == 5);
	// the host claims the reserved slots itself
	QT_CHECK (QC_ClaimEdict (vm, 2));
	QT_CHECK (!QC_IsFree (vm, 2));
	QT_EQ_U (QC_Serial (vm, 2), 1);
	QT_CHECK (QC_ClaimEdict (vm, 2));
	QT_EQ_U (QC_Serial (vm, 2), 1);
	QT_CHECK (!QC_ClaimEdict (vm, 7));
	QC_Destroy (vm);
	// a reservation at or past the limit leaves nothing to spawn
	vm = LimitedVM (a, 4, 4);
	QT_CHECK (NoFree (vm));
	QC_Destroy (vm);
	QA_Free (a);
}

// serials tell reused slots apart
static void TestSerials (void)
{
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e, again;
	uint32_t	first;

	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	QT_CHECK (QC_Spawn (vm, &e));
	first = QC_Serial (vm, e);
	QC_SetTime (vm, 10);
	QT_CHECK (QC_Remove (vm, e, true));
	QT_EQ_U (QC_Serial (vm, e), first);		// a free slot keeps its serial
	QT_CHECK (QC_Spawn (vm, &again) && again == e);
	QT_EQ_U (QC_Serial (vm, again), first + 1);
	QT_EQ_U (QC_Serial (vm, 1000000), 0);
	QC_Destroy (vm);
	QA_Free (a);
}

// fields added later hold values of their type, and are found again by it
static void TestEnsureField (void)
{
	static const uint32_t	types[] = {QC_EV_FLOAT, QC_EV_INTEGER, QC_EV_UINT, QC_EV_VECTOR, QC_EV_ENTITY,
		QC_EV_STRING, QC_EV_FUNCTION, QC_EV_POINTER, QC_EV_FIELD, QC_EV_INT64, QC_EV_UINT64, QC_EV_DOUBLE};
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e;
	uint32_t	i, ofs, again, found, type, in[3] = {11, 22, 33}, out[3];
	char		name[32];
	int			words;

	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	for (i = 0 ; i < sizeof(types) / sizeof(types[0]) ; i++)
	{
		snprintf (name, sizeof(name), "added_%u", i);
		words = QC_TypeWords (types[i]);
		QT_CHECK (QC_EnsureField (vm, name, types[i], &ofs));
		QT_CHECK (QC_FindField (vm, name, &found, &type) && found == ofs && type == types[i]);
		QT_CHECK (QC_EnsureField (vm, name, types[i], &again) && again == ofs);
		QT_CHECK (QC_Spawn (vm, &e));
		QT_CHECK (QC_SetField (vm, e, ofs, (uint32_t)words, in));
		memset (out, 0, sizeof(out));
		QT_CHECK (QC_GetField (vm, e, ofs, (uint32_t)words, out) && !memcmp (in, out, (size_t)words * 4));
	}
	// a field of another type under the name is refused
	QT_CHECK (!QC_EnsureField (vm, "added_6", QC_EV_FLOAT, &ofs));
	// an integer reads any word
	QT_CHECK (QC_EnsureField (vm, "added_6", QC_EV_INTEGER, &ofs));
	// until the reserve is spent
	for (i = 0 ; i < 10000 ; i++)
	{
		snprintf (name, sizeof(name), "more_%u", i);
		if (!QC_EnsureField (vm, name, QC_EV_VECTOR, &ofs))
			break;
	}
	QT_CHECK (i < 10000);
	QT_CHECK ((uint64_t)QC_FieldWords (vm) * 4 <= vm->mem.field_capacity);
	QC_Destroy (vm);
	QA_Free (a);
}

// the host's header in each entity block: QuakeC can't address it, spawning and
// removing leave it alone, and a slot's block never moves
static void TestHostHeader (void)
{
	qc_asm_t	*a = QA_New ();
	qc_config_t	config;
	qcvm_t		*vm;
	qc_ent_t	e;
	uint32_t	health, i;
	uint8_t		*block, *edicts;
	qc_loc_t	loc;
	uint32_t	addr;

	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	QC_DefaultConfig (&config, QC_SSQC);
	config.entity_header_bytes = 100;
	config.limits.max_edicts = 64;
	vm = QA_CreateVM (a, &config, NULL, NULL, NULL);
	health = Field (vm, "health");
	edicts = QC_Edicts (vm);
	QT_CHECK ((1u << QC_EdictShift (vm)) >= 100 + 16 + 4);

	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	block = edicts + ((size_t)e << QC_EdictShift (vm));
	memset (block, 0x5A, 100);
	SetFloat (vm, e, health, 7);
	QT_EQ_F (QC_BitsFloat (*(uint32_t *)(block + 100 + health * 4)), 7);

	// QuakeC's address of the field lands in the fields, after the header
	addr = vm->mem.e_base + (e << vm->mem.shift) + health * 4;
	loc = QC_Locate (&vm->mem, addr, 4);
	QT_CHECK (loc.type == QC_LOC_E && loc.p == block + 100 + health * 4 && loc.ent == e);
	// and nothing past the fields
	loc = QC_Locate (&vm->mem, vm->mem.e_base + (e << vm->mem.shift) + vm->mem.field_bytes, 1);
	QT_CHECK (loc.type == QC_LOC_NONE);

	QC_SetTime (vm, 10);
	QT_CHECK (QC_Remove (vm, e, true));
	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	for (i = 0 ; i < 100 && block[i] == 0x5A ; i++)
		;
	QT_EQ_U (i, 100);
	QT_EQ_F (GetFloat (vm, e, health), 0);
	for (i = 2 ; i < 64 ; i++)
		QT_CHECK (QC_Spawn (vm, &e));
	QT_CHECK (QC_Edicts (vm) == edicts);

	// a header that isn't a multiple of four is refused
	config.entity_header_bytes = 6;
	{
		qc_progs_t	*p = QA_Load (a, QC_FORMAT_FTE16);
		qc_error_t	error;

		QT_CHECK (!QC_Create (p, NULL, &config, NULL, NULL, &error));
		QT_EQ_U (error.kind, QC_ERR_HOST);
		QC_FreeError (&error);
		QC_ReleaseProgs (p);
	}
	QC_Destroy (vm);
	QA_Free (a);
}

// a progs with fields mana, health and origin (with a global for origin_y), and
// functions reading self.health and self.origin_y and writing self.mana; health
// of the type given
static qc_asm_t *HostFieldsProgs (uint32_t health_type)
{
	static const uint8_t	one_parm[] = {1};
	qc_asm_t	*a = QA_New ();
	uint32_t	self_g, mana, health, origin, origin_y, ptr;
	qa_func_t	f;

	self_g = QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Field (a, "mana", QC_EV_FLOAT, &mana);
	QA_Field (a, "health", health_type, &health);
	origin = QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	origin_y = QA_Global1 (a, "origin_y", QC_EV_FIELD, origin + 1);
	f = QA_Function (a, "get_health", NULL, 0, 0);
	QA_Emit (a, QOP_LOAD_F, self_g, health, QA_OFS_RETURN);
	QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
	f = QA_Function (a, "get_origin_y", NULL, 0, 0);
	QA_Emit (a, QOP_LOAD_F, self_g, origin_y, QA_OFS_RETURN);
	QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
	f = QA_Function (a, "set_mana", one_parm, 1, 1);
	ptr = QA_Local (f, 1);
	QA_Emit (a, QOP_ADDRESS, self_g, mana, ptr);
	QA_Emit (a, QOP_STOREP_F, QA_Local (f, 0), ptr, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	return a;
}

static float CallFloat (qcvm_t *vm, const char *name, uint32_t argc, float arg)
{
	qc_value_t	in = QC_ValFloat (arg), out = {0};

	QT_CHECK (QC_Call (vm, QC_FindFunction (vm, name), argc, &in, &out));
	return QC_BitsFloat (out.w[0]);
}

// The host lays out the fields: the progs' move to the host's words by name
// (its field globals with them), the rest go after, the host's own exist
// without the progs, and a field of another size under a host's name fails.
static void TestHostFields (void)
{
	qc_hostfield_t	host[] = {
		{"origin", QC_EV_VECTOR, 0},
		{"health", QC_EV_FLOAT, 3},
		{"lastruntime", QC_EV_FLOAT, 4},
	};
	qc_asm_t	*a = HostFieldsProgs (QC_EV_FLOAT);
	qc_config_t	config;
	qcvm_t		*vm;
	qc_ent_t	e;
	uint32_t	in[3] = {QC_FloatBits (1), QC_FloatBits (2), QC_FloatBits (3)}, type, self_word;

	QC_DefaultConfig (&config, QC_SSQC);
	config.host_fields = host;
	config.num_host_fields = 3;
	vm = QA_CreateVM (a, &config, NULL, NULL, NULL);
	host[1].ofs = 99;		// the VM holds its own copy

	QT_EQ_U (Field (vm, "origin"), 0);
	QT_EQ_U (Field (vm, "origin_y"), 1);
	QT_EQ_U (Field (vm, "health"), 3);
	QT_CHECK (QC_FindField (vm, "lastruntime", NULL, &type) && type == QC_EV_FLOAT);
	QT_EQ_U (Field (vm, "lastruntime"), 4);
	QT_EQ_U (Field (vm, "mana"), 5);
	QT_CHECK (QC_FieldWords (vm) >= 6);

	// QuakeC reads and writes where the host does
	QT_CHECK (QC_Spawn (vm, &e));
	QT_CHECK (QC_FindGlobal (vm, "self", &self_word, NULL));
	QC_Globals (vm)[self_word].u = e;
	SetFloat (vm, e, 3, 42);
	QT_CHECK (QC_SetField (vm, e, 0, 3, in));
	QT_EQ_F (CallFloat (vm, "get_health", 0, 0), 42);
	QT_EQ_F (CallFloat (vm, "get_origin_y", 0, 0), 2);
	CallFloat (vm, "set_mana", 1, 7);
	QT_EQ_F (GetFloat (vm, e, 5), 7);

	// a reset lays them out the same
	QT_CHECK (QC_Reset (vm));
	QT_EQ_U (Field (vm, "health"), 3);
	QT_EQ_U (Field (vm, "mana"), 5);
	QC_Destroy (vm);
	QA_Free (a);

	// health as a vector doesn't fit the host's float
	host[1].ofs = 3;
	a = HostFieldsProgs (QC_EV_VECTOR);
	{
		qc_progs_t	*p = QA_Load (a, QC_FORMAT_FTE16);
		qc_error_t	error;

		QT_CHECK (!QC_Create (p, NULL, &config, NULL, NULL, &error));
		QT_EQ_U (error.kind, QC_ERR_HOST);
		QC_FreeError (&error);
		QC_ReleaseProgs (p);
	}
	QA_Free (a);
}

// spawn defaults, remove's cleared fields, and the hooks
static void TestSpawnRemoveHooks (void)
{
	static const char *const	clears[] = {"origin", "health", NULL};
	qc_asm_t	*a = QA_New ();
	qc_config_t	config;
	qc_host_t	host = {.on_spawn = OnSpawn, .on_remove = OnRemove, .warning = OnWarning};
	hooks_t		h = {0};
	qcvm_t		*vm;
	qc_ent_t	e;
	uint32_t	solid, hit, dflt, origin, health, v[3] = {1, 2, 3}, out[3];

	QA_Field (a, "dimension_solid", QC_EV_FLOAT, NULL);
	QA_Field (a, "dimension_hit", QC_EV_FLOAT, NULL);
	QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);

	// CSQC's: 255 in both, the world included
	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	solid = Field (vm, "dimension_solid");
	hit = Field (vm, "dimension_hit");
	QT_EQ_F (GetFloat (vm, 0, solid), 255);
	QT_CHECK (QC_Spawn (vm, &e));
	QT_EQ_F (GetFloat (vm, e, solid), 255);
	QT_EQ_F (GetFloat (vm, e, hit), 255);
	QC_Destroy (vm);

	// with a dimension_default global, its value when spawned
	dflt = QA_Global1 (a, "dimension_default", QC_EV_FLOAT, QC_FloatBits (7));
	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	QT_CHECK (QC_Spawn (vm, &e));
	QT_EQ_F (GetFloat (vm, e, solid), 7);
	QC_Globals (vm)[dflt].f = 3;
	QT_CHECK (QC_Spawn (vm, &e));
	QT_EQ_F (GetFloat (vm, e, solid), 3);
	QC_Destroy (vm);

	// other kinds leave them zero; the hooks see spawns and removals, and
	// remove clears all of a vector
	QC_DefaultConfig (&config, QC_SSQC);
	config.remove_clears = clears;
	vm = QA_CreateVM (a, &config, NULL, &host, &h);
	origin = Field (vm, "origin");
	health = Field (vm, "health");
	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	QT_EQ_F (GetFloat (vm, e, solid), 0);
	QT_CHECK (QC_SetField (vm, e, origin, 3, v));
	SetFloat (vm, e, health, 5);
	h.refuse = true;
	QT_CHECK (!QC_Remove (vm, e, false));
	QT_CHECK (!QC_IsFree (vm, e));
	h.refuse = false;
	QT_CHECK (QC_Remove (vm, e, false));
	QT_CHECK (QC_GetField (vm, e, origin, 3, out) && !out[0] && !out[1] && !out[2]);
	QT_EQ_F (GetFloat (vm, e, health), 0);
	QT_CHECK (!QC_Remove (vm, e, false));		// already free: refused, no hook
	QT_CHECK (h.numspawned == 1 && h.spawned[0] == 1);
	QT_CHECK (h.numremoved == 1 && h.removed[0] == 1);
	QC_FlushWarnings (vm);
	QT_EQ_I (h.warnings, 1);
	QC_Destroy (vm);
	QA_Free (a);
}

/*
==============================================================================

STRINGS IN A VM

==============================================================================
*/

static void TestVMStrings (void)
{
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e;
	uint32_t	message, text, n, kept, pinned, heapofs, lost;
	qc_str_t	s, t, host;
	static char	name[32] = "unnamed";

	QA_Field (a, "message", QC_EV_STRING, NULL);
	text = QA_Global1 (a, "text", QC_EV_STRING, 0);
	vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	message = Field (vm, "message");

	s = QC_TempString (vm, "kept", 4);
	QT_CHECK (QC_IsTempString (s));
	QT_EQ_S (QC_String (vm, s), "kept");
	QC_Globals (vm)[text].u = s;
	t = QC_TempString (vm, "in a field", 10);
	QT_CHECK (QC_Spawn (vm, &e));
	QT_CHECK (QC_SetField (vm, e, message, 1, &t));
	pinned = QC_TempString (vm, "pinned", 6);
	QC_Pin (vm, pinned);
	QT_CHECK (QC_HeapAlloc (&vm->mem.heap, 4, &heapofs));
	kept = QC_TempString (vm, "on the heap", 11);
	memcpy (vm->mem.heap.region.base + heapofs, &kept, 4);
	lost = QC_TempString (vm, "lost", 4);
	QT_EQ_U (QC_NumTempStrings (vm), 5);
	n = QC_CollectGarbage (vm);
	QT_EQ_U (n, 1);
	QT_EQ_S (QC_String (vm, s), "kept");
	QT_EQ_S (QC_String (vm, t), "in a field");
	QT_EQ_S (QC_String (vm, pinned), "pinned");
	QT_EQ_S (QC_String (vm, kept), "on the heap");
	QT_CHECK (!QC_IsValidString (vm, lost));
	QT_EQ_S (QC_String (vm, lost), "");
	QC_Unpin (vm, pinned);
	QT_EQ_U (QC_CollectGarbage (vm), 1);

	// static strings: interned copies, and the host's own text by reference
	s = QC_Intern (vm, "model", 5);
	QT_CHECK (s == QC_Intern (vm, "model", 5) && (s & QC_TAG_MASK) == QC_STATIC_TAG);
	host = QC_HostString (vm, name);
	QT_EQ_S (QC_String (vm, host), "unnamed");
	strcpy (name, "renamed");
	QT_EQ_S (QC_String (vm, host), "renamed");
	QT_CHECK (host == QC_HostString (vm, name));

	// references into memory that isn't there, and unterminated text at the end of a region
	QT_CHECK (!QC_IsValidString (vm, 0x7FFFFFF0u));
	QT_CHECK (!QC_IsValidString (vm, QC_STATIC_TAG | 999));
	memset (vm->mem.s.base + vm->mem.s_len - 4, 'x', 4);
	QT_EQ_S (QC_String (vm, vm->mem.s_len - 4), "xxxx");

	// reset: back to the loaded state
	QC_Globals (vm)[text].f = 42;
	QT_CHECK (QC_Reset (vm));
	QT_EQ_U (QC_Globals (vm)[text].u, 0);
	QT_EQ_U (QC_NumEdicts (vm), 1);
	QT_EQ_U (QC_NumTempStrings (vm), 0);
	QC_Destroy (vm);
	QA_Free (a);
}

// the host's blocks of the QuakeC heap: memalloc's, zeroed, freed once
static void TestHostAlloc (void)
{
	qc_asm_t	*a = QA_New ();
	qcvm_t		*vm = QA_CreateVM (a, NULL, NULL, NULL, NULL);
	qc_ptr_t	p, q;
	uint8_t		bytes[16], zero[16] = {0};

	p = QC_Alloc (vm, 16);
	QT_CHECK (p != 0);
	QT_CHECK (QC_ReadMemory (vm, p, bytes, 16) && !memcmp (bytes, zero, 16));
	memset (bytes, 7, 16);
	QT_CHECK (QC_WriteMemory (vm, p, bytes, 16));
	q = QC_Alloc (vm, 0);		// a byte
	QT_CHECK (q != 0 && q != p);
	QT_EQ_U (QC_Alloc (vm, 0x01000001), 0);		// past memalloc's 16 MiB
	QT_CHECK (QC_Free (vm, p));
	QT_CHECK (!QC_Free (vm, p));
	QT_CHECK (QC_Free (vm, q));
	QT_CHECK (!QC_Free (vm, 4));
	QC_Destroy (vm);
	QA_Free (a);
}

// the temp string limits, through the VM
static void TestTempLimits (void)
{
	qc_asm_t	*a = QA_New ();
	qc_config_t	config;
	qcvm_t		*vm;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.temp_strings = 3;
	vm = QA_CreateVM (a, &config, NULL, NULL, NULL);
	QT_CHECK (QC_TempString (vm, "a", 1) && QC_TempString (vm, "b", 1) && QC_TempString (vm, "c", 1));
	QT_EQ_U (QC_TempString (vm, "d", 1), 0);
	QT_CHECK (QC_LastError (vm)->kind == QC_ERR_OUT_OF_MEMORY && QC_LastError (vm)->value == QC_RES_TEMP_STRINGS);
	QT_EQ_U (QC_CollectGarbage (vm), 3);
	QT_CHECK (QC_TempString (vm, "d", 1) != 0);
	QC_Destroy (vm);
	QA_Free (a);
}

int main (void)
{
	TestHeap ();
	TestStrings ();
	TestLayout ();
	TestEntities ();
	TestEntityLimits ();
	TestSerials ();
	TestEnsureField ();
	TestHostHeader ();
	TestHostFields ();
	TestSpawnRemoveHooks ();
	TestVMStrings ();
	TestHostAlloc ();
	TestTempLimits ();
	return QT_Finish ("memory", "regions, entities, the heap and strings hold");
}
