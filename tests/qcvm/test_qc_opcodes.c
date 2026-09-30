// test_qc_opcodes.c -- every opcode, against docs/qcvm/vm.md (with the fixes in
// deviations.md), in 16- and 32-bit statements; at the end, every opcode must
// have run at least once

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARK	0xDEADBEEFu

typedef struct
{
	uint32_t	w[3];
} W;

static W F (float x)			{ return (W){{QC_FloatBits (x), 0, 0}}; }
static W V (float x, float y, float z)	{ return (W){{QC_FloatBits (x), QC_FloatBits (y), QC_FloatBits (z)}}; }
static W I (int32_t x)			{ return (W){{(uint32_t)x, 0, 0}}; }
static W U (uint32_t x)			{ return (W){{x, 0, 0}}; }
static W L (int64_t x)			{ return (W){{(uint32_t)(uint64_t)x, (uint32_t)((uint64_t)x >> 32), 0}}; }
static W D (double x)			{ uint64_t b = QC_DoubleBits (x); return (W){{(uint32_t)b, (uint32_t)(b >> 32), 0}}; }
static W Words (uint32_t x, uint32_t y, uint32_t z)	{ return (W){{x, y, z}}; }
static const W	marks = {{MARK, MARK, MARK}}, zero3 = {{0, 0, 0}};

static uint32_t FB (bool b)		{ return b ? QC_FloatBits (1) : 0; }
static uint32_t FL (float x)	{ return QC_FloatBits (x); }

// what running one statement left
typedef struct
{
	W				a, b, c, ret;
	bool			failed;
	qc_errkind_t	err;
	int64_t			value;
	uint32_t		low, high;
} out_t;

static qc_format_t	format;
static qc_config_t	config;
static bool			covered[QOP_NUMREAL];
static const char	*context = "";

static void Cover (uint32_t op)
{
	if (op < QOP_NUMREAL)
		covered[op] = true;
}

static W GetGlobal (qcvm_t *vm, const char *name)
{
	uint32_t	word;
	W			w = {{0, 0, 0}};

	if (QC_FindGlobal (vm, name, &word, NULL))
		memcpy (w.w, &QC_Globals (vm)[word], 12);
	return w;
}

static uint32_t Global (qc_asm_t *a, const char *name, W init)
{
	return QA_Global (a, name, QC_EV_VECTOR, init.w, 3);
}

static out_t Collect (qcvm_t *vm, bool ok, qc_value_t ret)
{
	out_t				out = {0};
	const qc_error_t	*e = QC_LastError (vm);

	out.a = GetGlobal (vm, "A");
	out.b = GetGlobal (vm, "B");
	out.c = GetGlobal (vm, "C");
	if (ok)
		memcpy (out.ret.w, ret.w, 12);
	else
	{
		out.failed = true;
		out.err = e->kind;
		out.value = e->value;
		out.low = e->low;
		out.high = e->high;
	}
	return out;
}

static out_t Exec (const qc_asm_t *a)
{
	qcvm_t		*vm = QA_CreateVMAs (a, format, &config, NULL, NULL, NULL);
	qc_value_t	ret = {{0}};
	bool		ok = QC_Call (vm, QC_FindFunction (vm, "t"), 0, NULL, &ret);
	out_t		out = Collect (vm, ok, ret);

	QC_Destroy (vm);
	return out;
}

// op A B C on globals holding a, b and c; setup may prepare the program first
static out_t RunWith (uint32_t op, W a, W b, W c, void (*setup) (qc_asm_t *))
{
	qc_asm_t	*pa = QA_New ();
	uint32_t	ga, gb, gc;
	out_t		out;

	Cover (op);
	if (setup)
		setup (pa);
	ga = Global (pa, "A", a);
	gb = Global (pa, "B", b);
	gc = Global (pa, "C", c);
	QA_Function (pa, "t", NULL, 0, 0);
	QA_Emit (pa, op, ga, gb, gc);
	QA_Emit (pa, QOP_DONE, 0, 0, 0);
	out = Exec (pa);
	QA_Free (pa);
	return out;
}

static out_t Run (uint32_t op, W a, W b, W c)
{
	return RunWith (op, a, b, c, NULL);
}

static void Report (uint32_t op)
{
	printf ("  %s (%s, %s)\n", qc_opinfo[op].name, format == QC_FORMAT_FTE16 ? "16-bit" : "32-bit", context);
}

// the first words of C after op A B
static void CheckC (uint32_t op, W a, W b, W want, int n)
{
	out_t	out = Run (op, a, b, marks);
	int		k;

	if (!QT_CHECK (!out.failed))
		Report (op);
	for (k = 0 ; k < n ; k++)
		if (!QT_EQ_U (out.c.w[k], want.w[k]))
			Report (op);
}
#define C1(op, a, b, w0)		CheckC ((op), (a), (b), Words ((w0), 0, 0), 1)
#define C2(op, a, b, want)		CheckC ((op), (a), (b), (want), 2)
#define C3(op, a, b, want)		CheckC ((op), (a), (b), (want), 3)

// the first words of B after op A B (stores)
static void CheckB (uint32_t op, W a, W want, int n)
{
	out_t	out = Run (op, a, marks, zero3);
	int		k;

	if (!QT_CHECK (!out.failed))
		Report (op);
	for (k = 0 ; k < n ; k++)
		if (!QT_EQ_U (out.b.w[k], want.w[k]))
			Report (op);
}

// the first word of C, or of B, a run left: taken from a copy, as MSVC doesn't
// index the arrays of a struct a function returns (C4223)
static uint32_t C0 (out_t out)	{ return out.c.w[0]; }
static uint32_t B0 (out_t out)	{ return out.b.w[0]; }

// n words at got are want's
static bool SameWords (const uint32_t *got, W want, int n)
{
	return !memcmp (got, want.w, (size_t)n * sizeof(uint32_t));
}

// op A B C faults
static void Fault (uint32_t op, W a, W b, W c, qc_errkind_t kind, int64_t value)
{
	out_t	out = Run (op, a, b, c);

	if (!QT_CHECK (out.failed && out.err == kind && out.value == value))
		Report (op);
}

/*
==============================================================================

ARITHMETIC, COMPARISONS, LOGIC

==============================================================================
*/

static void Arithmetic (void)
{
	C1 (QOP_MUL_F, F (3), F (4), FL (12));
	C1 (QOP_DIV_F, F (6), F (4), FL (1.5f));
	C1 (QOP_DIV_F, F (1), F (0), FL (INFINITY));
	C1 (QOP_ADD_F, F (1.5f), F (2.25f), FL (3.75f));
	C1 (QOP_SUB_F, F (1.5f), F (2.25f), FL (-0.75f));
	C1 (QOP_MUL_V, V (1, 2, 3), V (4, 5, 6), FL (32));
	C3 (QOP_MUL_FV, F (2), V (1, 2, 3), V (2, 4, 6));
	C3 (QOP_MUL_VF, V (1, 2, 3), F (2), V (2, 4, 6));
	C3 (QOP_DIV_VF, V (2, 4, 6), F (2), V (1, 2, 3));
	C3 (QOP_ADD_V, V (1, 2, 3), V (10, 20, 30), V (11, 22, 33));
	C3 (QOP_SUB_V, V (1, 2, 3), V (10, 20, 30), V (-9, -18, -27));
}

static void Comparisons (void)
{
	C1 (QOP_EQ_F, F (-0.0f), F (0), FB (true));
	C1 (QOP_EQ_F, F (NAN), F (NAN), FB (false));
	C1 (QOP_NE_F, F (NAN), F (1), FB (true));
	C1 (QOP_LE_F, F (1), F (1), FB (true));
	C1 (QOP_GE_F, F (0.5f), F (1), FB (false));
	C1 (QOP_LT_F, F (0.5f), F (1), FB (true));
	C1 (QOP_GT_F, F (0.5f), F (1), FB (false));
	C1 (QOP_EQ_V, V (1, 2, 3), V (1, 2, 3), FB (true));
	C1 (QOP_EQ_V, V (1, 2, 3), V (1, 2, 4), FB (false));
	C1 (QOP_NE_V, V (1, 2, 3), V (1, 2, 4), FB (true));
	C1 (QOP_EQ_E, U (5), U (5), FB (true));
	C1 (QOP_NE_E, U (5), U (6), FB (true));
	C1 (QOP_EQ_FNC, U (0x01000003), U (3), FB (false));
	C1 (QOP_NE_FNC, U (3), U (3), FB (false));
}

// offsets in a fresh assembler's string table: 0 null, 1 "", then these
static void InternStrings (qc_asm_t *a)
{
	QA_String (a, "abc");
	QA_String (a, "abd");
	QA_String (a, "xabc");
}

static uint32_t S (uint32_t op, uint32_t a, uint32_t b)
{
	return C0 (RunWith (op, U (a), U (b), marks, InternStrings));
}

static void Strings (void)
{
	qc_asm_t	*probe = QA_New ();
	uint32_t	abc, abd, xabc;

	InternStrings (probe);
	abc = QA_String (probe, "abc");
	abd = QA_String (probe, "abd");
	xabc = QA_String (probe, "xabc");
	QA_Free (probe);

	QT_EQ_U (S (QOP_EQ_S, 0, 1), FB (true));		// null == ""
	QT_EQ_U (S (QOP_EQ_S, abc, abc), FB (true));
	QT_EQ_U (S (QOP_EQ_S, abc, abd), FB (false));
	QT_EQ_U (S (QOP_EQ_S, abc, xabc + 1), FB (true));		// the same text elsewhere
	QT_EQ_U (S (QOP_NE_S, abc, abd), FB (true));
	QT_EQ_U (S (QOP_NE_S, 0, 0), FB (false));
	QT_EQ_U (S (QOP_NOT_S, 0, 0), FB (true));
	QT_EQ_U (S (QOP_NOT_S, 1, 0), FB (true));		// "" is 'not'
	QT_EQ_U (S (QOP_NOT_S, abc, 0), FB (false));
	// pointer arithmetic on string references
	QT_EQ_U (S (QOP_ADD_SF, abc, FL (2)), abc + 2);
	QT_EQ_U (S (QOP_SUB_S, abc + 2, abc), 2);
	// LOADP_C reads characters through a string reference
	QT_EQ_U (S (QOP_LOADP_C, abc, FL (1)), FL ('b'));
}

static void Logic (void)
{
	C1 (QOP_NOT_F, F (-0.0f), F (0), FB (true));
	C1 (QOP_NOT_F, U (1), F (0), FB (false));
	C1 (QOP_NOT_V, V (0, -0.0f, 0), F (0), FB (true));
	C1 (QOP_NOT_V, V (0, 0, 1), F (0), FB (false));
	C1 (QOP_NOT_ENT, U (0), U (0), FB (true));
	C1 (QOP_NOT_FNC, U (0x01000000), U (0), FB (true));
	C1 (QOP_NOT_I, U (0), U (0), 1);
	C1 (QOP_NOT_I, U (7), U (0), 0);
	C1 (QOP_AND_F, F (-0.0f), F (1), FB (false));
	C1 (QOP_AND_F, F (2), F (1), FB (true));
	C1 (QOP_OR_F, F (-0.0f), F (0), FB (false));
	C1 (QOP_OR_F, F (-0.0f), U (1), FB (true));
	C1 (QOP_BITAND_F, F (6.9f), F (3.1f), FL (2));
	C1 (QOP_BITOR_F, F (4), F (1), FL (5));
}

/*
==============================================================================

BRANCHES AND STORES

==============================================================================
*/

// op cond -> skip; C = 1; skip: RETURN C. Whether the branch was taken.
static bool Branch (uint32_t op, W cond)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	ga, one, gc, br, end;
	out_t		out;

	Cover (op);
	ga = Global (a, "A", cond);
	one = QA_Float (a, 1);
	Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	QA_Function (a, "t", NULL, 0, 0);
	br = QA_Emit (a, op, ga, 0, 0);
	QA_Emit (a, QOP_STORE_F, one, gc, 0);
	end = QA_Emit (a, QOP_RETURN, gc, 0, 0);
	QA_PatchJump (a, br, 1, end);
	out = Exec (a);
	QA_Free (a);
	return out.c.w[0] == 0;
}

static void Branches (void)
{
	qc_asm_t	*a;
	uint32_t	gc, one, g, end;

	QT_CHECK (Branch (QOP_IF_I, F (-0.0f)));		// integer truth: -0 is true
	QT_CHECK (!Branch (QOP_IF_I, U (0)));
	QT_CHECK (Branch (QOP_IFNOT_I, U (0)));
	QT_CHECK (!Branch (QOP_IF_F, F (-0.0f)));		// IF_F masks the sign
	QT_CHECK (Branch (QOP_IF_F, U (1)));			// denormals are true
	QT_CHECK (Branch (QOP_IFNOT_F, F (-0.0f)));
	QT_CHECK (Branch (QOP_IF_S, U (1)));			// IF_S tests for null only
	QT_CHECK (!Branch (QOP_IF_S, U (0)));
	QT_CHECK (Branch (QOP_IFNOT_S, U (0)));
	QT_CHECK (!Branch (QOP_IFNOT_S, U (1)));

	Cover (QOP_GOTO);
	a = QA_New ();
	Global (a, "A", zero3);
	Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	one = QA_Float (a, 1);
	QA_Function (a, "t", NULL, 0, 0);
	g = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	QA_Emit (a, QOP_STORE_F, one, gc, 0);
	end = QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_PatchJump (a, g, 0, end);
	QT_EQ_U (C0 (Exec (a)), 0);
	QA_Free (a);
}

static void Stores (void)
{
	static const uint32_t	ops[] = {QOP_STORE_F, QOP_STORE_S, QOP_STORE_ENT, QOP_STORE_FLD, QOP_STORE_FNC,
		QOP_STORE_I, QOP_STORE_P};
	size_t					i;

	for (i = 0 ; i < sizeof(ops) / sizeof(ops[0]) ; i++)
		CheckB (ops[i], U (42), Words (42, MARK, MARK), 3);
	CheckB (QOP_STORE_V, V (1, 2, 3), V (1, 2, 3), 3);
	CheckB (QOP_STORE_I64, Words (1, 2, 3), Words (1, 2, MARK), 3);
	CheckB (QOP_STORE_IF, I (-3), Words (FL (-3), 0, 0), 1);
	CheckB (QOP_STORE_FI, F (-3.9f), Words ((uint32_t)-3, 0, 0), 1);
	CheckB (QOP_STORE_FI, F (NAN), Words ((uint32_t)INT32_MIN, 0, 0), 1);
}

/*
==============================================================================

ENTITIES

==============================================================================
*/

typedef void (*entitybody_t) (qc_asm_t *a, const uint32_t g[3], const uint32_t fields[3], uint32_t op);

static const char	*entity_fields[6] = {"fa", "fv_x", "fv_y", "fv_z", "fi", "fi2"};

// Runs code on entity 1 (spawned first) with fields fa (float), fv (vector) and
// fi (two words wide); values gets their words after.
static out_t EntityCase (uint32_t op, W a, W b, W c, entitybody_t body, uint32_t values[6])
{
	qc_asm_t	*pa = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e = 0;
	uint32_t	g[3], fields[3], k, ofs, v;
	qc_value_t	ret = {{0}};
	bool		ok;
	out_t		out;

	Cover (op);
	g[0] = Global (pa, "A", a);
	g[1] = Global (pa, "B", b);
	g[2] = Global (pa, "C", c);
	QA_Field (pa, "fa", QC_EV_FLOAT, &fields[0]);
	QA_Field (pa, "fv", QC_EV_VECTOR, &fields[1]);
	QA_Field (pa, "fi", QC_EV_FLOAT, &fields[2]);
	QA_Field (pa, "fi2", QC_EV_FLOAT, NULL);
	QA_Function (pa, "t", NULL, 0, 0);
	body (pa, g, fields, op);
	QA_Emit (pa, QOP_DONE, 0, 0, 0);
	vm = QA_CreateVMAs (pa, format, &config, NULL, NULL, NULL);
	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	for (k = 0 ; k < 6 ; k++)
	{
		v = 100 + k;
		QT_CHECK (QC_FindField (vm, entity_fields[k], &ofs, NULL) && QC_SetField (vm, e, ofs, 1, &v));
	}
	ok = QC_Call (vm, QC_FindFunction (vm, "t"), 0, NULL, &ret);
	out = Collect (vm, ok, ret);
	for (k = 0 ; values && k < 6 ; k++)
	{
		values[k] = 0;
		QC_FindField (vm, entity_fields[k], &ofs, NULL);
		QC_GetField (vm, e, ofs, 1, &values[k]);
	}
	QC_Destroy (vm);
	QA_Free (pa);
	return out;
}

// LOAD_*: A the entity, B a field global (holding the offset), C the destination
static void LoadField (qc_asm_t *a, const uint32_t g[3], const uint32_t f[3], uint32_t op)
{
	QA_Emit (a, QOP_STORE_F, op == QOP_LOAD_V ? f[1] : op == QOP_LOAD_I64 ? f[2] : f[0], g[1], 0);
	QA_Emit (a, op, g[0], g[1], g[2]);
}

static uint32_t store_field;

// STOREF_*: A the entity, B the field, C the value
static void StoreField (qc_asm_t *a, const uint32_t g[3], const uint32_t f[3], uint32_t op)
{
	QA_Emit (a, QOP_STORE_F, f[store_field], g[1], 0);
	QA_Emit (a, op, g[0], g[1], g[2]);
}

// ADDRESS then STOREP_*: a pointer to fa (fv for STOREP_V), then a store through it
static void StoreThrough (qc_asm_t *a, const uint32_t g[3], const uint32_t f[3], uint32_t op)
{
	uint32_t	p = QA_Temp (a, 1);

	QA_Emit (a, QOP_ADDRESS, g[1], op == QOP_STOREP_V ? f[1] : f[0], p);
	QA_Emit (a, op, g[0], p, 0);
}

static void Entities (void)
{
	static const uint32_t	loads[] = {QOP_LOAD_F, QOP_LOAD_S, QOP_LOAD_ENT, QOP_LOAD_FLD, QOP_LOAD_FNC,
		QOP_LOAD_I, QOP_LOAD_P};
	static const uint32_t	storeps[] = {QOP_STOREP_F, QOP_STOREP_S, QOP_STOREP_ENT, QOP_STOREP_FLD,
		QOP_STOREP_FNC, QOP_STOREP_I};
	static const struct
	{
		uint32_t	op, field, want[6];
	} storefs[] = {
		{QOP_STOREF_F, 0, {7, 101, 102, 103, 104, 105}},
		{QOP_STOREF_S, 0, {7, 101, 102, 103, 104, 105}},
		{QOP_STOREF_I, 0, {7, 101, 102, 103, 104, 105}},
		{QOP_STOREF_V, 1, {100, 7, 8, 9, 104, 105}},
		{QOP_STOREF_I64, 2, {100, 101, 102, 103, 7, 8}},
	};
	uint32_t	values[6];
	out_t		out;
	size_t		i;

	for (i = 0 ; i < sizeof(loads) / sizeof(loads[0]) ; i++)
		if (!QT_EQ_U (C0 (EntityCase (loads[i], U (1), zero3, marks, LoadField, NULL)), 100))
			Report (loads[i]);
	out = EntityCase (QOP_LOAD_V, U (1), zero3, marks, LoadField, NULL);
	QT_CHECK (out.c.w[0] == 101 && out.c.w[1] == 102 && out.c.w[2] == 103);
	out = EntityCase (QOP_LOAD_I64, U (1), zero3, marks, LoadField, NULL);
	QT_CHECK (out.c.w[0] == 104 && out.c.w[1] == 105 && out.c.w[2] == MARK);
	// a bad entity: LOAD_V zeroes all three words
	out = EntityCase (QOP_LOAD_V, U (77), zero3, marks, LoadField, NULL);
	QT_CHECK (out.c.w[0] == 0 && out.c.w[1] == 0 && out.c.w[2] == 0);

	for (i = 0 ; i < sizeof(storefs) / sizeof(storefs[0]) ; i++)
	{
		store_field = storefs[i].field;
		out = EntityCase (storefs[i].op, U (1), zero3, Words (7, 8, 9), StoreField, values);
		QT_CHECK (!out.failed);
		if (!QT_CHECK (!memcmp (values, storefs[i].want, sizeof(values))))
			Report (storefs[i].op);
	}

	for (i = 0 ; i < sizeof(storeps) / sizeof(storeps[0]) ; i++)
	{
		EntityCase (storeps[i], U (55), U (1), zero3, StoreThrough, values);
		if (!QT_EQ_U (values[0], 55))
			Report (storeps[i]);
	}
	Cover (QOP_ADDRESS);
	EntityCase (QOP_STOREP_V, Words (1, 2, 3), U (1), zero3, StoreThrough, values);
	QT_CHECK (values[1] == 1 && values[2] == 2 && values[3] == 3);
}

/*
==============================================================================

POINTERS

==============================================================================
*/

typedef struct
{
	uint32_t	op, x, idx;
} sized_t;

// sized loads and stores and conversions through a pointer to a global word
static out_t Sized (sized_t s, uint32_t init)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	word, gb, gc, p, zero, xg, ig;
	out_t		out;

	word = QA_Alloc (a, 1, &init, 1);
	Global (a, "A", zero3);
	gb = Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	p = QA_Temp (a, 1);
	zero = QA_Int (a, 0);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, QOP_GLOBALADDRESS, word, zero, p);
	Cover (s.op);
	xg = QA_Alloc (a, 1, &s.x, 1);
	ig = QA_Alloc (a, 1, &s.idx, 1);
	if (s.op == QOP_STOREP_C || s.op == QOP_STOREP_I8 || s.op == QOP_STOREP_I16 || s.op == QOP_STOREP_IF
		|| s.op == QOP_STOREP_FI)
		QA_Emit (a, s.op, xg, p, ig);
	else
		QA_Emit (a, s.op, p, ig, gc);
	QA_Emit (a, QOP_LOADP_I, p, zero, gb);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	return out;
}

static void Pointers (void)
{
	static const uint32_t	pairs[][2] = {{QOP_LOADP_S, QOP_STOREP_S}, {QOP_LOADP_ENT, QOP_STOREP_ENT},
		{QOP_LOADP_FLD, QOP_STOREP_FLD}, {QOP_LOADP_FNC, QOP_STOREP_FNC}, {QOP_LOADP_I, QOP_STOREP_F},
		{QOP_LOADP_I64, QOP_STOREP_I64}};
	uint32_t	arr, ga, gb, gc, two, one, p, q, zero, src, dst, cell, words;
	qc_asm_t	*a;
	out_t		out;
	size_t		i;

	// GLOBALADDRESS: &global[a + B] as a byte address; LOADP and STOREP through it
	a = QA_New ();
	Cover (QOP_GLOBALADDRESS);
	Cover (QOP_LOADP_F);
	Cover (QOP_STOREP_I);
	Cover (QOP_ADD_PIW);
	arr = QA_Alloc (a, 4, (const uint32_t[]){10, 20, 30, 40}, 4);
	ga = Global (a, "A", zero3);
	gb = Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	two = QA_Int (a, 2);
	one = QA_Int (a, 1);
	p = QA_Temp (a, 1);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, QOP_GLOBALADDRESS, arr, two, p);		// p = &arr[2]
	QA_Emit (a, QOP_LOADP_F, p, 0, ga);				// A = arr[2]
	QA_Emit (a, QOP_LOADP_F, p, one, gb);				// B = arr[3]
	QA_Emit (a, QOP_STOREP_I, two, p, one);			// arr[3] = 2
	QA_Emit (a, QOP_ADD_PIW, p, one, p);				// p = &arr[3]
	QA_Emit (a, QOP_LOADP_F, p, 0, gc);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	QT_CHECK (!out.failed && out.a.w[0] == 30 && out.b.w[0] == 40 && out.c.w[0] == 2);

	QT_EQ_U (C0 (Sized ((sized_t){QOP_LOADP_U8, 0, 1}, 0x88776655u)), 0x66);
	QT_EQ_U (C0 (Sized ((sized_t){QOP_LOADP_I8, 0, 3}, 0x88776655u)), 0xFFFFFF88u);
	QT_EQ_U (C0 (Sized ((sized_t){QOP_LOADP_U16, 0, 1}, 0x88776655u)), 0x8877);
	QT_EQ_U (C0 (Sized ((sized_t){QOP_LOADP_I16, 0, 1}, 0x88776655u)), 0xFFFF8877u);
	QT_EQ_U (B0 (Sized ((sized_t){QOP_STOREP_I8, 0xAB, 2}, 0x11111111u)), 0x11AB1111u);
	QT_EQ_U (B0 (Sized ((sized_t){QOP_STOREP_I16, 0xBEEF, 1}, 0x11111111u)), 0xBEEF1111u);
	QT_EQ_U (B0 (Sized ((sized_t){QOP_STOREP_C, FL (65), 0}, 0x11111111u)), 0x11111141u);
	QT_EQ_U (B0 (Sized ((sized_t){QOP_STOREP_IF, 3, 0}, 0)), FL (3));
	QT_EQ_U (B0 (Sized ((sized_t){QOP_STOREP_FI, FL (-2.5f), 0}, 0)), (uint32_t)-2);
	QT_EQ_U (C0 (Sized ((sized_t){QOP_LOADP_ITOF, 0, 99}, 7)), FL (7));
	QT_EQ_U (C0 (Sized ((sized_t){QOP_LOADP_FTOI, 0, 99}, FL (7.9f))), 7);

	// vector, 64-bit and the other word forms through pointers
	a = QA_New ();
	src = QA_Alloc (a, 3, (const uint32_t[]){1, 2, 3}, 3);
	dst = QA_Alloc (a, 3, NULL, 0);
	Global (a, "A", zero3);
	gb = Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	p = QA_Temp (a, 1);
	q = QA_Temp (a, 1);
	zero = QA_Int (a, 0);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, QOP_GLOBALADDRESS, src, zero, p);
	QA_Emit (a, QOP_GLOBALADDRESS, dst, zero, q);
	QA_Emit (a, QOP_LOADP_V, p, zero, gc);
	QA_Emit (a, QOP_STOREP_V, gc, q, zero);
	QA_Emit (a, QOP_LOADP_V, q, zero, gb);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	Cover (QOP_LOADP_V);
	Cover (QOP_STOREP_V);
	out = Exec (a);
	QA_Free (a);
	QT_CHECK (out.b.w[0] == 1 && out.b.w[1] == 2 && out.b.w[2] == 3 && out.c.w[2] == 3);

	for (i = 0 ; i < sizeof(pairs) / sizeof(pairs[0]) ; i++)
	{
		Cover (pairs[i][0]);
		Cover (pairs[i][1]);
		a = QA_New ();
		cell = QA_Alloc (a, 2, NULL, 0);
		Global (a, "A", zero3);
		gb = Global (a, "B", zero3);
		gc = Global (a, "C", Words (77, 88, 0));
		p = QA_Temp (a, 1);
		zero = QA_Int (a, 0);
		QA_Function (a, "t", NULL, 0, 0);
		QA_Emit (a, QOP_GLOBALADDRESS, cell, zero, p);
		QA_Emit (a, pairs[i][1], gc, p, zero);
		QA_Emit (a, pairs[i][0], p, zero, gb);
		QA_Emit (a, QOP_DONE, 0, 0, 0);
		out = Exec (a);
		QA_Free (a);
		words = pairs[i][0] == QOP_LOADP_I64 ? 2 : 1;
		if (!QT_CHECK (out.b.w[0] == 77 && (words == 1 || out.b.w[1] == 88)))
			Report (pairs[i][0]);
	}

	// faults
	Fault (QOP_LOADP_F, U (0x7FFF0000u), U (0), U (0), QC_ERR_BAD_POINTER_READ, 0x7FFF0000);
	Fault (QOP_STOREP_F, U (1), U (0), U (0), QC_ERR_NULL_POINTER_WRITE, 0);
	Fault (QOP_STOREP_F, U (1), U (0x7FFF0000u), U (0), QC_ERR_BAD_POINTER_WRITE, 0x7FFF0000);
	// the 0xFFFFFFFF sentinel reads zero and swallows writes
	out = Run (QOP_LOADP_F, U (UINT32_MAX), U (0), marks);
	QT_CHECK (!out.failed && out.c.w[0] == 0);
	out = Run (QOP_STOREP_F, U (5), U (UINT32_MAX), U (0));
	QT_CHECK (!out.failed);
}

/*
==============================================================================

HEXEN 2

==============================================================================
*/

// B a pointer to a cell holding cell; C gets the new value (not for BITSET/BITCLR); A the cell after
static out_t CompoundThrough (uint32_t op, W a, W cell)
{
	qc_asm_t	*pa = QA_New ();
	uint32_t	target, ga, gb, gc, zero;
	out_t		out;

	Cover (op);
	target = QA_Alloc (pa, 3, cell.w, 3);
	ga = Global (pa, "A", a);
	gb = Global (pa, "B", zero3);
	gc = Global (pa, "C", marks);
	zero = QA_Int (pa, 0);
	QA_Function (pa, "t", NULL, 0, 0);
	QA_Emit (pa, QOP_GLOBALADDRESS, target, zero, gb);
	QA_Emit (pa, op, ga, gb, gc);
	QA_Emit (pa, QOP_LOADP_V, gb, zero, ga);		// the cell back into A
	QA_Emit (pa, QOP_DONE, 0, 0, 0);
	out = Exec (pa);
	QA_Free (pa);
	return out;
}

static void Compound (void)
{
	qc_asm_t	*pa;
	uint32_t	target, ga, gb, zero;
	out_t		out;

	// the global forms update B and leave C alone
	out = Run (QOP_MULSTORE_F, F (3), F (4), marks);
	QT_CHECK (out.b.w[0] == FL (12) && out.c.w[0] == MARK);
	QT_EQ_U (B0 (Run (QOP_DIVSTORE_F, F (4), F (2), marks)), FL (0.5f));
	QT_EQ_U (B0 (Run (QOP_ADDSTORE_F, F (4), F (2), marks)), FL (6));
	QT_EQ_U (B0 (Run (QOP_SUBSTORE_F, F (4), F (2), marks)), FL (-2));
	out = Run (QOP_MULSTORE_VF, F (2), V (1, 2, 3), marks);
	QT_CHECK (SameWords (out.b.w, V (2, 4, 6), 3));
	out = Run (QOP_ADDSTORE_V, V (1, 1, 1), V (1, 2, 3), marks);
	QT_CHECK (SameWords (out.b.w, V (2, 3, 4), 3));
	out = Run (QOP_SUBSTORE_V, V (1, 1, 1), V (1, 2, 3), marks);
	QT_CHECK (SameWords (out.b.w, V (0, 1, 2), 3));
	QT_EQ_U (B0 (Run (QOP_BITSETSTORE_F, F (1), F (4), marks)), FL (5));
	QT_EQ_U (B0 (Run (QOP_BITCLRSTORE_F, F (1), F (5), marks)), FL (4));

	// through pointers
	out = CompoundThrough (QOP_MULSTOREP_F, F (3), F (4));
	QT_CHECK (out.a.w[0] == FL (12) && out.c.w[0] == FL (12));
	QT_EQ_U (C0 (CompoundThrough (QOP_DIVSTOREP_F, F (2), F (4))), FL (2));
	QT_EQ_U (C0 (CompoundThrough (QOP_ADDSTOREP_F, F (2), F (4))), FL (6));
	QT_EQ_U (C0 (CompoundThrough (QOP_SUBSTOREP_F, F (2), F (4))), FL (2));
	out = CompoundThrough (QOP_MULSTOREP_VF, F (2), V (1, 2, 3));
	QT_CHECK (SameWords (out.a.w, V (2, 4, 6), 3) && SameWords (out.c.w, V (2, 4, 6), 3));
	out = CompoundThrough (QOP_ADDSTOREP_V, V (1, 1, 1), V (1, 2, 3));
	QT_CHECK (SameWords (out.c.w, V (2, 3, 4), 3));
	out = CompoundThrough (QOP_SUBSTOREP_V, V (1, 1, 1), V (1, 2, 3));
	QT_CHECK (SameWords (out.c.w, V (0, 1, 2), 3));
	out = CompoundThrough (QOP_BITSETSTOREP_F, F (1), F (4));
	QT_CHECK (out.a.w[0] == FL (5) && out.c.w[0] == MARK);
	out = CompoundThrough (QOP_BITCLRSTOREP_F, F (1), F (5));
	QT_CHECK (out.a.w[0] == FL (4) && out.c.w[0] == MARK);

	// interleaved when C is A, as fteqcc emits it: C[k] is written before A[k+1] is read
	pa = QA_New ();
	target = QA_Alloc (pa, 3, (const uint32_t[]){FL (1), FL (2), FL (3)}, 3);
	ga = Global (pa, "A", V (10, 10, 10));
	gb = Global (pa, "B", zero3);
	Global (pa, "C", zero3);
	zero = QA_Int (pa, 0);
	QA_Function (pa, "t", NULL, 0, 0);
	QA_Emit (pa, QOP_GLOBALADDRESS, target, zero, gb);
	QA_Emit (pa, QOP_ADDSTOREP_V, ga, gb, ga);
	QA_Emit (pa, QOP_DONE, 0, 0, 0);
	out = Exec (pa);
	QT_CHECK (SameWords (out.a.w, V (11, 12, 13), 3));
	QA_Free (pa);
	// an invalid pointer is fatal
	Fault (QOP_ADDSTOREP_F, F (1), U (0x7FFF0000u), U (0), QC_ERR_BAD_POINTER_WRITE, 0x7FFF0000);
}

static out_t FetchGlobal (uint32_t op, uint32_t stride, float index)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	nine[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9}, base, gb, gc, two = 2;
	out_t		out;

	Cover (op);
	QA_Alloc (a, 1, &two, 1);			// the prefix: count - 1 (3 elements)
	base = QA_Alloc (a, 3 * stride, nine, 9);
	Global (a, "A", zero3);
	gb = Global (a, "B", F (index));
	gc = Global (a, "C", marks);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, op, base, gb, gc);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	return out;
}

static void Hexen2Arrays (void)
{
	static const uint32_t	ops[] = {QOP_FETCH_GBL_F, QOP_FETCH_GBL_S, QOP_FETCH_GBL_E, QOP_FETCH_GBL_FNC,
		QOP_FETCH_GBL_V};
	uint32_t	stride;
	out_t		out;
	size_t		i;

	for (i = 0 ; i < sizeof(ops) / sizeof(ops[0]) ; i++)
	{
		stride = ops[i] == QOP_FETCH_GBL_V ? 3 : 1;
		out = FetchGlobal (ops[i], stride, 2);
		QT_CHECK (!out.failed);
		if (stride == 3)
			QT_CHECK (out.c.w[0] == 7 && out.c.w[1] == 8 && out.c.w[2] == 9);
		else
			QT_EQ_U (out.c.w[0], 3);
		out = FetchGlobal (ops[i], stride, 3);
		QT_CHECK (out.failed && out.err == QC_ERR_ARRAY_INDEX && out.value == 3);
		out = FetchGlobal (ops[i], stride, -1);
		QT_CHECK (out.failed && out.err == QC_ERR_ARRAY_INDEX && out.value == -1);
	}
}

typedef struct
{
	float		frame, weaponframe, nextthink;
	uint32_t	think;
	float		wrapped;
	uint32_t	t;
} anim_t;

static anim_t Animate (uint32_t op, W a, W b, float start)
{
	qc_asm_t	*pa = QA_New ();
	qcvm_t		*vm;
	qc_ent_t	e = 0;
	uint32_t	ga, gb, ofs, v, word;
	qa_func_t	tf;
	anim_t		r = {0};

	Cover (op);
	ga = Global (pa, "A", a);
	gb = Global (pa, "B", b);
	Global (pa, "C", zero3);
	QA_Global1 (pa, "self", QC_EV_ENTITY, 1);
	QA_Global1 (pa, "time", QC_EV_FLOAT, FL (10));
	QA_Global1 (pa, "cycle_wrapped", QC_EV_FLOAT, 0);
	QA_Field (pa, "frame", QC_EV_FLOAT, NULL);
	QA_Field (pa, "weaponframe", QC_EV_FLOAT, NULL);
	QA_Field (pa, "think", QC_EV_FUNCTION, NULL);
	QA_Field (pa, "nextthink", QC_EV_FLOAT, NULL);
	tf = QA_Function (pa, "t", NULL, 0, 0);
	QA_Emit (pa, op, ga, gb, 0);
	QA_Emit (pa, QOP_DONE, 0, 0, 0);
	vm = QA_CreateVMAs (pa, format, &config, NULL, NULL, NULL);
	QT_CHECK (QC_Spawn (vm, &e));
	v = FL (start);
	QC_FindField (vm, "frame", &ofs, NULL);
	QC_SetField (vm, e, ofs, 1, &v);
	QC_FindField (vm, "weaponframe", &ofs, NULL);
	QC_SetField (vm, e, ofs, 1, &v);
	QT_CHECK (QC_Call (vm, QC_FindFunction (vm, "t"), 0, NULL, NULL));
	QC_FindField (vm, "frame", &ofs, NULL);
	QC_GetField (vm, e, ofs, 1, &v);
	r.frame = QC_BitsFloat (v);
	QC_FindField (vm, "weaponframe", &ofs, NULL);
	QC_GetField (vm, e, ofs, 1, &v);
	r.weaponframe = QC_BitsFloat (v);
	QC_FindField (vm, "nextthink", &ofs, NULL);
	QC_GetField (vm, e, ofs, 1, &v);
	r.nextthink = QC_BitsFloat (v);
	QC_FindField (vm, "think", &ofs, NULL);
	QC_GetField (vm, e, ofs, 1, &r.think);
	QC_FindGlobal (vm, "cycle_wrapped", &word, NULL);
	r.wrapped = QC_Globals (vm)[word].f;
	r.t = tf.index;
	QC_Destroy (vm);
	QA_Free (pa);
	return r;
}

static void Animation (void)
{
	anim_t	r;

	r = Animate (QOP_STATE, F (4), U (9), 0);
	QT_CHECK (r.frame == 4 && r.nextthink == 10.1f && r.think == 9);
	// CSTATE cycles 2..5
	r = Animate (QOP_CSTATE, F (2), F (5), 3);
	QT_CHECK (r.frame == 4 && r.wrapped == 0 && r.think == r.t);
	r = Animate (QOP_CSTATE, F (2), F (5), 5);
	QT_CHECK (r.frame == 2 && r.wrapped == 1);
	QT_EQ_F (Animate (QOP_CSTATE, F (2), F (5), 9).frame, 2);		// outside the range: first
	QT_EQ_F (Animate (QOP_CSTATE, F (5), F (2), 4).frame, 3);		// first > last runs backwards
	QT_EQ_F (Animate (QOP_CWSTATE, F (2), F (5), 3).weaponframe, 4);
	QT_EQ_F (Animate (QOP_THINKTIME, U (1), F (2.5f), 0).nextthink, 12.5f);
}

static bool Within (uint32_t w, float lo, float hi, bool inclusive)
{
	float	f = QC_BitsFloat (w);

	return f >= lo && (inclusive ? f <= hi : f < hi);
}

static void Random (void)
{
	out_t	out;
	int		i, k;

	for (i = 0 ; i < 50 ; i++)
	{
		QT_CHECK (Within (C0 (Run (QOP_RAND0, zero3, zero3, zero3)), 0, 1, false));
		QT_CHECK (Within (C0 (Run (QOP_RAND1, F (10), zero3, zero3)), 0, 10, false));
		QT_CHECK (Within (C0 (Run (QOP_RAND2, F (5), F (7), zero3)), 5, 7, false));
		out = Run (QOP_RANDV0, zero3, zero3, zero3);
		for (k = 0 ; k < 3 ; k++)
			QT_CHECK (Within (out.c.w[k], 0, 1, true));
		out = Run (QOP_RANDV1, V (2, 4, 8), zero3, zero3);
		QT_CHECK (Within (out.c.w[0], 0, 2, true) && Within (out.c.w[1], 0, 4, true) && Within (out.c.w[2], 0, 8, true));
		out = Run (QOP_RANDV2, V (1, 1, 1), V (2, 2, 2), zero3);
		for (k = 0 ; k < 3 ; k++)
			QT_CHECK (Within (out.c.w[k], 1, 2, true));
	}
}

/*
==============================================================================

SWITCHES AND CALLS

==============================================================================
*/

// switch (A) { case X: C = 1; case lo..hi: C = 2; default: C = 3 }, laid out as
// fteqcc does: SWITCH jumps to the chain of tests after the bodies
static uint32_t SwitchCase (uint32_t sw, W value, W casev, W lo, W hi)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	ga, gc, one, two, three, gcase, glo, ghi, s, j1, j2, j3, body1, body2, body3;
	uint32_t	chain, c1, c2, dflt, end;
	out_t		out;

	Cover (sw);
	Cover (QOP_CASE);
	Cover (QOP_CASERANGE);
	// interned first, so string cases use the offsets a fresh assembler gives
	QA_String (a, "abc");
	QA_String (a, "xabc");
	ga = Global (a, "A", value);
	Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	one = QA_Float (a, 1);
	two = QA_Float (a, 2);
	three = QA_Float (a, 3);
	gcase = QA_VectorRaw (a, casev.w[0], casev.w[1], casev.w[2]);
	glo = QA_VectorRaw (a, lo.w[0], lo.w[1], lo.w[2]);
	ghi = QA_VectorRaw (a, hi.w[0], hi.w[1], hi.w[2]);
	QA_Function (a, "t", NULL, 0, 0);
	s = QA_Emit (a, sw, ga, 0, 0);
	body1 = QA_Emit (a, QOP_STORE_F, one, gc, 0);
	j1 = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	body2 = QA_Emit (a, QOP_STORE_F, two, gc, 0);
	j2 = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	body3 = QA_Emit (a, QOP_STORE_F, three, gc, 0);
	j3 = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	chain = QA_Here (a);
	c1 = QA_Emit (a, QOP_CASE, gcase, 0, 0);
	c2 = sw == QOP_SWITCH_S ? QA_Emit (a, QOP_GOTO, 0, 0, 0) : QA_Emit (a, QOP_CASERANGE, glo, ghi, 0);
	dflt = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	end = QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_PatchJump (a, s, 1, chain);
	QA_PatchJump (a, c1, 1, body1);
	if (sw == QOP_SWITCH_S)
		QA_PatchJump (a, c2, 0, dflt);
	else
		QA_PatchJump (a, c2, 2, body2);
	QA_PatchJump (a, dflt, 0, body3);
	QA_PatchJump (a, j1, 0, end);
	QA_PatchJump (a, j2, 0, end);
	QA_PatchJump (a, j3, 0, end);
	out = Exec (a);
	QA_Free (a);
	if (!QT_CHECK (!out.failed))
		Report (sw);
	return (uint32_t)QC_BitsFloat (out.c.w[0]);
}

static void Switches (void)
{
	static const uint32_t	ints[] = {QOP_SWITCH_E, QOP_SWITCH_FNC, QOP_SWITCH_I};
	qc_asm_t	*probe;
	uint32_t	abc, xabc;
	size_t		i;

	QT_EQ_U (SwitchCase (QOP_SWITCH_F, F (-0.0f), F (0), F (10), F (20)), 1);		// -0 == 0
	QT_EQ_U (SwitchCase (QOP_SWITCH_F, F (15), F (0), F (10), F (20)), 2);
	QT_EQ_U (SwitchCase (QOP_SWITCH_F, F (20), F (0), F (10), F (20)), 2);		// inclusive
	QT_EQ_U (SwitchCase (QOP_SWITCH_F, F (21), F (0), F (10), F (20)), 3);
	QT_EQ_U (SwitchCase (QOP_SWITCH_V, V (1, 2, 3), V (1, 2, 3), V (0, 0, 0), V (1, 1, 1)), 1);
	QT_EQ_U (SwitchCase (QOP_SWITCH_V, V (0.5f, 0.5f, 1), V (1, 2, 3), V (0, 0, 0), V (1, 1, 1)), 2);
	QT_EQ_U (SwitchCase (QOP_SWITCH_V, V (0.5f, 2, 1), V (1, 2, 3), V (0, 0, 0), V (1, 1, 1)), 3);
	for (i = 0 ; i < sizeof(ints) / sizeof(ints[0]) ; i++)
	{
		QT_EQ_U (SwitchCase (ints[i], U (5), U (5), I (-3), I (3)), 1);
		QT_EQ_U (SwitchCase (ints[i], I (-2), U (5), I (-3), I (3)), 2);		// a signed range
		QT_EQ_U (SwitchCase (ints[i], U (9), U (5), I (-3), I (3)), 3);
	}
	// a string switch compares contents: "abc" (in "xabc") matches the literal "abc"
	probe = QA_New ();
	abc = QA_String (probe, "abc");
	xabc = QA_String (probe, "xabc");
	QA_Free (probe);
	QT_EQ_U (SwitchCase (QOP_SWITCH_S, U (xabc + 1), U (abc), zero3, zero3), 1);
	QT_EQ_U (SwitchCase (QOP_SWITCH_S, U (0), U (1), zero3, zero3), 1);		// null matches ""
	QT_EQ_U (SwitchCase (QOP_SWITCH_S, U (abc), U (1), zero3, zero3), 3);
}

// every CALLn and CALLnH passes its arguments; the callee returns the sum of its
// parameters (vectors, to see all three words move)
static void Calls (void)
{
	uint32_t	n, k, op, acc, callee_g, args[8], first, b, c;
	uint8_t		sizes[8];
	qc_asm_t	*a;
	qa_func_t	callee;
	out_t		out;
	float		sum;
	int			h2;

	for (n = 0 ; n <= 8 ; n++)
		for (h2 = 0 ; h2 < 2 ; h2++)
		{
			if (h2 && !n)
				continue;
			op = h2 ? QOP_CALL1H + n - 1 : QOP_CALL0 + n;
			Cover (op);
			a = QA_New ();
			Global (a, "A", zero3);
			Global (a, "B", zero3);
			Global (a, "C", zero3);
			for (k = 0 ; k < n ; k++)
				sizes[k] = 3;
			callee = QA_Function (a, "sum", sizes, (int)n, 3);
			acc = QA_Local (callee, 3 * n);
			for (k = 0 ; k < n ; k++)
				QA_Emit (a, QOP_ADD_V, acc, QA_Local (callee, 3 * k), acc);
			QA_Emit (a, QOP_RETURN, acc, 0, 0);
			callee_g = QA_Global1 (a, "sum_g", QC_EV_FUNCTION, callee.index);
			for (k = 0 ; k < n ; k++)
				args[k] = QA_Vector (a, (float)k + 1, 10, 100);
			QA_Function (a, "t", NULL, 0, 0);
			first = h2 ? (n < 2 ? n : 2) : 0;
			for (k = first ; k < n ; k++)
				QA_Emit (a, QOP_STORE_V, args[k], QA_PARM (k), 0);
			b = h2 ? args[0] : 0;
			c = h2 && n >= 2 ? args[1] : 0;
			QA_Emit (a, op, callee_g, b, c);
			QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
			out = Exec (a);
			QA_Free (a);
			if (!QT_CHECK (!out.failed))
				Report (op);
			for (k = 1, sum = 0 ; k <= n ; k++)
				sum += (float)k;
			if (!QT_CHECK (SameWords (out.ret.w, V (sum, 10.0f * (float)n, 100.0f * (float)n), 3)))
				Report (op);
		}
	Cover (QOP_RETURN);
	Cover (QOP_DONE);
}

/*
==============================================================================

INTEGERS, MIXED, INDEXED GLOBALS

==============================================================================
*/

static void Integers (void)
{
	C1 (QOP_ADD_I, I (INT32_MAX), I (1), (uint32_t)INT32_MIN);
	C1 (QOP_SUB_I, I (1), I (3), (uint32_t)-2);
	C1 (QOP_MUL_I, I (-4), I (5), (uint32_t)-20);
	C1 (QOP_DIV_I, I (-7), I (2), (uint32_t)-3);
	C1 (QOP_DIV_I, I (7), I (0), 0);
	C1 (QOP_DIV_I, I (INT32_MIN), I (-1), (uint32_t)INT32_MAX);
	C1 (QOP_BITAND_I, U (0xC), U (0xA), 0x8);
	C1 (QOP_BITOR_I, U (0xC), U (0xA), 0xE);
	C1 (QOP_BITXOR_I, U (0xC), U (0xA), 0x6);
	C1 (QOP_RSHIFT_I, I (-16), I (2), (uint32_t)-4);
	C1 (QOP_LSHIFT_I, I (3), I (4), 48);
	C1 (QOP_LSHIFT_I, I (1), I (33), 2);
	C1 (QOP_EQ_I, I (3), I (3), 1);
	C1 (QOP_NE_I, I (3), I (3), 0);
	C1 (QOP_LE_I, I (-1), I (0), 1);
	C1 (QOP_GE_I, I (-1), I (0), 0);
	C1 (QOP_LT_I, I (-1), I (0), 1);
	C1 (QOP_GT_I, I (-1), I (0), 0);
	C1 (QOP_AND_I, I (2), I (0), 0);
	C1 (QOP_OR_I, I (2), I (0), 1);
	C1 (QOP_CONV_ITOF, I (-3), zero3, FL (-3));
	C1 (QOP_CONV_FTOI, F (-3.7f), zero3, (uint32_t)-3);
	C1 (QOP_CONV_FTOI, F (3e9f), zero3, (uint32_t)INT32_MIN);
}

static void Mixed (void)
{
	static const struct
	{
		uint32_t	op;
		W			a, b;
		uint32_t	want;
	} cmps[] = {
		{QOP_LE_IF, {{1, 0, 0}}, {{0x3F800000, 0, 0}}, 1},
		{QOP_GE_IF, {{1, 0, 0}}, {{0x3FC00000, 0, 0}}, 0},
		{QOP_LT_IF, {{1, 0, 0}}, {{0x3FC00000, 0, 0}}, 1},
		{QOP_GT_IF, {{2, 0, 0}}, {{0x3FC00000, 0, 0}}, 1},
		{QOP_EQ_IF, {{2, 0, 0}}, {{0x40000000, 0, 0}}, 1},
		{QOP_NE_IF, {{2, 0, 0}}, {{0x40000000, 0, 0}}, 0},
		{QOP_LE_FI, {{0x3F800000, 0, 0}}, {{1, 0, 0}}, 1},
		{QOP_GE_FI, {{0x3F000000, 0, 0}}, {{1, 0, 0}}, 0},
		{QOP_LT_FI, {{0x3F000000, 0, 0}}, {{1, 0, 0}}, 1},
		{QOP_GT_FI, {{0x3FC00000, 0, 0}}, {{1, 0, 0}}, 1},
		{QOP_EQ_FI, {{0x40000000, 0, 0}}, {{2, 0, 0}}, 1},
		{QOP_NE_FI, {{0x40000000, 0, 0}}, {{2, 0, 0}}, 0},
		{QOP_AND_IF, {{1, 0, 0}}, {{0x80000000, 0, 0}}, 0},
		{QOP_OR_IF, {{0, 0, 0}}, {{0x7FC00000, 0, 0}}, 1},
		{QOP_AND_FI, {{0x3F000000, 0, 0}}, {{3, 0, 0}}, 1},
		{QOP_OR_FI, {{0, 0, 0}}, {{0, 0, 0}}, 0},
	};
	size_t	i;

	C1 (QOP_ADD_FI, F (1.5f), I (2), FL (3.5f));
	C1 (QOP_ADD_IF, I (2), F (1.5f), FL (3.5f));
	C1 (QOP_SUB_FI, F (1.5f), I (2), FL (-0.5f));
	C1 (QOP_SUB_IF, I (2), F (1.5f), FL (0.5f));
	C1 (QOP_MUL_IF, I (2), F (1.5f), FL (3));
	C1 (QOP_MUL_FI, F (1.5f), I (2), FL (3));
	C1 (QOP_DIV_IF, I (3), F (2), FL (1.5f));
	C1 (QOP_DIV_FI, F (3), I (2), FL (1.5f));
	C3 (QOP_MUL_VI, V (1, 2, 3), I (2), V (2, 4, 6));
	C3 (QOP_MUL_IV, I (2), V (1, 2, 3), V (2, 4, 6));
	for (i = 0 ; i < sizeof(cmps) / sizeof(cmps[0]) ; i++)
		C1 (cmps[i].op, cmps[i].a, cmps[i].b, cmps[i].want);
	C1 (QOP_BITAND_IF, I (7), F (2.9f), 2);
	C1 (QOP_BITOR_IF, I (4), F (1.9f), 5);
	C1 (QOP_BITAND_FI, F (7.9f), I (2), 2);
	C1 (QOP_BITOR_FI, F (4.2f), I (1), 5);
}

static out_t ArrayCase (uint32_t op, int32_t index, uint32_t *arr)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	gb, gc;
	out_t		out;

	Cover (op);
	*arr = QA_Alloc (a, 6, (const uint32_t[]){11, 22, 33, 44, 55, 66}, 6);
	Global (a, "A", zero3);
	gb = Global (a, "B", I (index));
	gc = Global (a, "C", marks);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, op, *arr, gb, gc);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	return out;
}

static bool IsGStore (uint32_t op)
{
	return op >= QOP_GSTOREP_I && op <= QOP_GSTOREP_V;
}

// GLOAD and GSTOREP address globals by a runtime index
static out_t GCase (uint32_t op)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	target, ga, gb, gc, val;
	out_t		out;

	Cover (op);
	target = QA_Alloc (a, 3, (const uint32_t[]){7, 8, 9}, 3);
	ga = Global (a, "A", U (target));
	gb = Global (a, "B", U (target));
	gc = Global (a, "C", marks);
	QA_Function (a, "t", NULL, 0, 0);
	if (IsGStore (op))
	{
		val = QA_VectorRaw (a, 5, 6, 4);
		QA_Emit (a, op, val, gb, 0);
		QA_Emit (a, QOP_GLOAD_V, ga, 0, gc);
	}
	else
		QA_Emit (a, op, ga, 0, gc);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	return out;
}

static out_t Bound (int32_t value)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	ga;
	out_t		out;

	Cover (QOP_BOUNDCHECK);
	ga = Global (a, "A", I (value));
	Global (a, "B", zero3);
	Global (a, "C", zero3);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, QOP_BOUNDCHECK, ga, 10, 2);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	return out;
}

static void GlobalsIndexed (void)
{
	static const uint32_t	loads[] = {QOP_LOADA_F, QOP_LOADA_S, QOP_LOADA_ENT, QOP_LOADA_FLD, QOP_LOADA_FNC,
		QOP_LOADA_I};
	static const uint32_t	gloads[] = {QOP_GLOAD_I, QOP_GLOAD_F, QOP_GLOAD_FLD, QOP_GLOAD_ENT, QOP_GLOAD_S,
		QOP_GLOAD_FNC};
	static const uint32_t	gstores[] = {QOP_GSTOREP_I, QOP_GSTOREP_F, QOP_GSTOREP_ENT, QOP_GSTOREP_FLD,
		QOP_GSTOREP_S, QOP_GSTOREP_FNC};
	uint32_t	arr;
	out_t		out;
	size_t		i;

	for (i = 0 ; i < sizeof(loads) / sizeof(loads[0]) ; i++)
		if (!QT_EQ_U (C0 (ArrayCase (loads[i], 2, &arr)), 33))
			Report (loads[i]);
	out = ArrayCase (QOP_LOADA_V, 1, &arr);
	QT_CHECK (out.c.w[0] == 22 && out.c.w[1] == 33 && out.c.w[2] == 44);
	out = ArrayCase (QOP_LOADA_I64, 4, &arr);
	QT_CHECK (out.c.w[0] == 55 && out.c.w[1] == 66);
	out = ArrayCase (QOP_LOADA_F, -100, &arr);
	QT_CHECK (out.failed && out.err == QC_ERR_ARRAY_INDEX && out.value == (int64_t)arr - 100);

	for (i = 0 ; i < sizeof(gloads) / sizeof(gloads[0]) ; i++)
		if (!QT_EQ_U (C0 (GCase (gloads[i])), 7))
			Report (gloads[i]);
	out = GCase (QOP_GLOAD_V);
	QT_CHECK (out.c.w[0] == 7 && out.c.w[1] == 8 && out.c.w[2] == 9);
	for (i = 0 ; i < sizeof(gstores) / sizeof(gstores[0]) ; i++)
	{
		out = GCase (gstores[i]);
		if (!QT_CHECK (out.c.w[0] == 5 && out.c.w[1] == 8 && out.c.w[2] == 9))
			Report (gstores[i]);
	}
	out = GCase (QOP_GSTOREP_V);
	QT_CHECK (out.c.w[0] == 5 && out.c.w[1] == 6 && out.c.w[2] == 4);
	Fault (QOP_GLOAD_F, I (-1), zero3, zero3, QC_ERR_ARRAY_INDEX, -1);
	Fault (QOP_GSTOREP_F, zero3, I (1000000), zero3, QC_ERR_ARRAY_INDEX, 1000000);

	// BOUNDCHECK: c <= A < b, unsigned
	QT_CHECK (!Bound (2).failed);
	QT_CHECK (!Bound (9).failed);
	out = Bound (10);
	QT_CHECK (out.failed && out.err == QC_ERR_BOUND_CHECK && out.value == 10 && out.low == 2 && out.high == 10);
	out = Bound (1);
	QT_CHECK (out.failed && out.err == QC_ERR_BOUND_CHECK && out.value == 1);
	out = Bound (-1);
	QT_CHECK (out.failed && out.err == QC_ERR_BOUND_CHECK && out.value == -1);
}

static void PushAndFaults (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	gb, gc, four, zero, v, p, q;
	out_t		out;

	// PUSH reserves local stack words; the pointer is good until the function returns
	Cover (QOP_PUSH);
	Global (a, "A", zero3);
	gb = Global (a, "B", zero3);
	gc = Global (a, "C", zero3);
	four = QA_Int (a, 4);
	zero = QA_Int (a, 0);
	v = QA_Int (a, 1234);
	p = QA_Temp (a, 1);
	q = QA_Temp (a, 1);
	QA_Function (a, "t", NULL, 0, 0);
	QA_Emit (a, QOP_PUSH, four, 0, p);
	QA_Emit (a, QOP_PUSH, four, 0, q);
	QA_Emit (a, QOP_STOREP_I, v, p, zero);
	QA_Emit (a, QOP_LOADP_I, p, zero, gc);
	QA_Emit (a, QOP_SUB_I, q, p, gb);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	out = Exec (a);
	QA_Free (a);
	QT_CHECK (!out.failed && out.c.w[0] == 1234 && out.b.w[0] == 16);
	Fault (QOP_PUSH, I (INT32_MAX), zero3, zero3, QC_ERR_PUSHED_TOO_MUCH, 0);

	Fault (QOP_GADDRESS, zero3, zero3, zero3, QC_ERR_GADDRESS, 0);
	Fault (QOP_UNUSED, zero3, zero3, zero3, QC_ERR_BAD_OPCODE, QOP_UNUSED);
	Fault (QOP_POP, zero3, zero3, zero3, QC_ERR_BAD_OPCODE, QOP_POP);
}

static void UnsignedAndWide (void)
{
	C1 (QOP_LE_U, U (1), U (UINT32_MAX), 1);
	C1 (QOP_LT_U, U (UINT32_MAX), U (1), 0);
	C1 (QOP_DIV_U, U (UINT32_MAX), U (2), UINT32_MAX / 2);
	C1 (QOP_DIV_U, U (1), U (0), 0);
	C1 (QOP_RSHIFT_U, U (0x80000000u), U (31), 1);
	C1 (QOP_CONV_UF, U (UINT32_MAX), zero3, FL (4294967296.0f));
	C1 (QOP_CONV_FU, F (-1), zero3, UINT32_MAX);

	C2 (QOP_ADD_I64, L (INT64_MAX), L (1), L (INT64_MIN));
	C2 (QOP_SUB_I64, L (1), L (3), L (-2));
	C2 (QOP_MUL_I64, L (1ll << 40), L (3), L (3ll << 40));
	C2 (QOP_DIV_I64, L (-9), L (2), L (-4));
	C2 (QOP_DIV_I64, L (9), L (0), L (0));
	C2 (QOP_DIV_I64, L (INT64_MIN), L (-1), L (INT64_MIN));
	C2 (QOP_BITAND_I64, L (0xF0F0), L (0xFF00), L (0xF000));
	C2 (QOP_BITOR_I64, L (0xF0F0), L (0xFF00), L (0xFFF0));
	C2 (QOP_BITXOR_I64, L (0xF0F0), L (0xFF00), L (0x0FF0));
	C2 (QOP_LSHIFT_I64I, L (1), I (40), L (1ll << 40));
	C2 (QOP_RSHIFT_I64I, L (-(1ll << 40)), I (8), L (-(1ll << 32)));
	C2 (QOP_RSHIFT_U64I, L (-1), I (60), L (15));
	C1 (QOP_LE_I64, L (-1), L (0), 1);
	C1 (QOP_LT_I64, L (0), L (-1), 0);
	C1 (QOP_EQ_I64, L (1ll << 33), L (1ll << 33), 1);
	C1 (QOP_NE_I64, L (1ll << 33), L (1ll << 32), 1);
	C1 (QOP_LE_U64, L (-1), L (0), 0);
	C1 (QOP_LT_U64, L (0), L (-1), 1);
	C2 (QOP_DIV_U64, L (-1), L (2), L (INT64_MAX));
	C2 (QOP_DIV_U64, L (5), L (0), L (0));
	C2 (QOP_CONV_UI64, U (UINT32_MAX), zero3, L (UINT32_MAX));
	C2 (QOP_CONV_II64, I (-5), zero3, L (-5));
	C1 (QOP_CONV_I64I, L (0x100000007ll), zero3, 7);
	C1 (QOP_CONV_I64F, L (-3), zero3, FL (-3));
	C1 (QOP_CONV_U64F, L (-1), zero3, FL (18446744073709551616.0f));
	C2 (QOP_CONV_FI64, F (-2.5f), zero3, L (-2));
	C2 (QOP_CONV_FU64, F (-1), zero3, L (-1));

	C2 (QOP_ADD_D, D (0.1), D (0.2), D (0.1 + 0.2));
	C2 (QOP_SUB_D, D (1), D (0.25), D (0.75));
	C2 (QOP_MUL_D, D (1.5), D (4), D (6));
	C2 (QOP_DIV_D, D (1), D (3), D (1.0 / 3.0));
	C1 (QOP_LE_D, D (1), D (1), 1);
	C1 (QOP_LT_D, D (1), D (1), 0);
	C1 (QOP_EQ_D, D (0.5), D (0.5), 1);
	C1 (QOP_NE_D, D (0.5), D (0.5), 0);
	C2 (QOP_CONV_FD, F (0.1f), zero3, D ((double)0.1f));
	C1 (QOP_CONV_DF, D (0.1), zero3, FL (0.1f));
	C2 (QOP_CONV_I64D, L (-7), zero3, D (-7));
	C2 (QOP_CONV_U64D, L (-1), zero3, D (18446744073709551616.0));
	C2 (QOP_CONV_DI64, D (-7.9), zero3, L (-7));
	C2 (QOP_CONV_DU64, D (1e19), zero3, L ((int64_t)10000000000000000000ull));
}

static void Bitfields (void)
{
	W	desc = U (4 | (8 << 8));		// a field of width 4 at bit 8

	C1 (QOP_BITEXTEND_I, U (0x00000F00), desc, UINT32_MAX);
	C1 (QOP_BITEXTEND_I, U (0x00000700), desc, 7);
	C1 (QOP_BITEXTEND_U, U (0x00000F00), desc, 15);
	C1 (QOP_BITEXTEND_U, U (UINT32_MAX), U (0), 0);
	QT_EQ_U (C0 (Run (QOP_BITCOPY_I, U (0x5), desc, U (UINT32_MAX))), 0xFFFFF5FFu);
}

// every opcode test, in one format; small shrinks the local stack, which moves
// what follows the globals
static void CheckAll (qc_format_t f, bool small)
{
	int	op, missing = 0;

	format = f;
	QC_DefaultConfig (&config, QC_CSQC);
	if (small)
		config.limits.local_stack_words = 1u << 12;
	context = small ? "small local stack" : "default";
	memset (covered, 0, sizeof(covered));
	Arithmetic ();
	Comparisons ();
	Strings ();
	Logic ();
	Branches ();
	Stores ();
	Entities ();
	Pointers ();
	Compound ();
	Hexen2Arrays ();
	Animation ();
	Random ();
	Switches ();
	Calls ();
	Integers ();
	Mixed ();
	GlobalsIndexed ();
	PushAndFaults ();
	UnsignedAndWide ();
	Bitfields ();
	for (op = 0 ; op < QOP_NUMREAL ; op++)
		if (!covered[op])
		{
			printf ("never executed: %s\n", qc_opinfo[op].name);
			missing++;
		}
	QT_EQ_I (missing, 0);
}

int main (void)
{
	CheckAll (QC_FORMAT_FTE16, false);
	CheckAll (QC_FORMAT_FTE32, false);
	CheckAll (QC_FORMAT_FTE16, true);
	CheckAll (QC_FORMAT_FTE32, true);
	return QT_Finish ("opcodes", "every opcode behaves, in 16- and 32-bit statements");
}
