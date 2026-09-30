// test_qc_fuzz.c -- random programs never crash the VM
//
// Each case assembles functions from random statements (any opcode, operands
// mostly within the globals so they do something, sometimes anywhere) over
// globals of random words, fields, a spawned entity and builtins that re-enter
// QuakeC, make temp strings and collect garbage. Running them may fail in any
// way the VM reports, but must not crash; run it under ASan and UBSan.
//
// Programs rich in compare-and-branch pairs also check that the interpreter
// behaves exactly as its tracing instance, and that stopping for the budget
// and resuming loses nothing. QC_FUZZ_ITERS sets the cases (default 256),
// QC_FUZZ_SEED the seed.

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t	rng;

/*
==============================================================================

BUILTINS

==============================================================================
*/

static bool B_Spawn (qcvm_t *vm)
{
	qc_ent_t	e;

	if (!QC_Spawn (vm, &e))
		return false;
	QC_ReturnWord (vm, e);
	return true;
}

static bool B_Temp (qcvm_t *vm)
{
	char	text[128];

	snprintf (text, sizeof(text), "%g\"%.64s\"", (double)QC_ArgFloat (vm, 0), QC_ArgString (vm, 1));
	return QC_ReturnString (vm, text, strlen (text));
}

// calls the function in its first argument: re-entering QuakeC from a builtin
static bool B_Call (qcvm_t *vm)
{
	qc_func_t	f = QC_ArgWord (vm, 0);
	qc_value_t	arg = QC_ArgValue (vm, 1), ret;

	if (!QC_Call (vm, f, 1, &arg, &ret))
		return false;
	QC_ReturnValue (vm, ret);
	return true;
}

// collects garbage, which does nothing while QuakeC runs
static bool B_GC (qcvm_t *vm)
{
	QC_CollectGarbage (vm);
	return true;
}

// the standard builtins, and these in place of the first four
static qc_builtins_t *FuzzBuiltins (void)
{
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);

	QC_BuiltinsSetNumbered (b, 1, "spawn", B_Spawn);
	QC_BuiltinsSetNumbered (b, 2, "temp", B_Temp);
	QC_BuiltinsSetNumbered (b, 3, "call", B_Call);
	QC_BuiltinsSetNumbered (b, 4, "gc", B_GC);
	return b;
}

/*
==============================================================================

RANDOM PROGRAMS

==============================================================================
*/

typedef struct
{
	uint32_t	op, a, b, c;
} stmt_t;

typedef struct
{
	stmt_t		stmts[64];
	uint32_t	count;
} func_t;

// usually a global of the program, sometimes a small jump, sometimes anything
static uint32_t Operand (void)
{
	uint32_t	r = QT_RandBelow (&rng, 11);

	if (r < 8)
		return QT_RandBelow (&rng, 96);
	if (r < 10)
		return (uint32_t)((int32_t)QT_RandBelow (&rng, 16) - 8);
	return (uint32_t)QT_Rand (&rng);
}

static uint32_t Opcode (void)
{
	return QT_RandBelow (&rng, 10) < 9 ? QT_RandBelow (&rng, QOP_NUMREAL) : (uint32_t)(uint16_t)QT_Rand (&rng);
}

// small numbers, floats, entity numbers, function references, addresses, bits
static uint32_t Word (void)
{
	static const uint32_t	special[] = {0x80000000u, 0xC0000000u, 0xFFFFFFFFu, 1u << 24, 0x01000003u};

	switch (QT_RandBelow (&rng, 4))
	{
	case 0:		return QT_RandBelow (&rng, 8);
	case 1:		return QC_FloatBits (-4.0f + (float)QT_RandBelow (&rng, 30400) / 100.0f);
	case 2:		return special[QT_RandBelow (&rng, 5)];
	default:	return (uint32_t)QT_Rand (&rng);
	}
}

static void RandomFunction (func_t *f)
{
	uint32_t	i;

	f->count = 1 + QT_RandBelow (&rng, 47);
	for (i = 0 ; i < f->count ; i++)
		f->stmts[i] = (stmt_t){Opcode (), Operand (), Operand (), Operand ()};
}

// random statements and compare-and-branch pairs as fteqcc emits them: a
// statement writing a truth value to c, then IF or IFNOT on c with a short
// jump; random jumps land in the middle of pairs too
static void BranchyFunction (func_t *f)
{
	static const uint32_t	tests[] = {QOP_LT_F, QOP_LE_F, QOP_GT_F, QOP_GE_F, QOP_EQ_F, QOP_NE_F, QOP_EQ_I,
		QOP_LT_I, QOP_GE_I, QOP_LT_IF, QOP_GE_FI, QOP_EQ_E, QOP_NE_FNC, QOP_NOT_F, QOP_NOT_ENT, QOP_NOT_FNC,
		QOP_NOT_I, QOP_AND_F, QOP_OR_F, QOP_BITAND_F};
	static const uint32_t	branches[] = {QOP_IF_I, QOP_IFNOT_I, QOP_IF_F, QOP_IFNOT_F};
	uint32_t	parts = 1 + QT_RandBelow (&rng, 23), i, c;

	f->count = 0;
	for (i = 0 ; i < parts && f->count + 2 <= 64 ; i++)
	{
		if (QT_RandBelow (&rng, 2))
			f->stmts[f->count++] = (stmt_t){Opcode (), Operand (), Operand (), Operand ()};
		else
		{
			c = QT_RandBelow (&rng, 96);
			f->stmts[f->count++] = (stmt_t){tests[QT_RandBelow (&rng, 20)], QT_RandBelow (&rng, 96),
				QT_RandBelow (&rng, 96), c};
			f->stmts[f->count++] = (stmt_t){branches[QT_RandBelow (&rng, 4)], c,
				(uint32_t)((int32_t)QT_RandBelow (&rng, 16) - 8), 0};
		}
	}
}

static qc_progs_t *Build (const func_t *funcs, uint32_t numfuncs, const uint32_t *words, uint32_t numwords,
	qc_format_t format)
{
	static const char	*names[] = {"spawn", "temp", "call", "gc", "sleep", "fork"};
	static const uint32_t	numbers[] = {1, 2, 3, 4, 212, 210};
	static const int32_t	parms[] = {0, 2, 2, 0, 1, 0};
	qc_asm_t	*a = QA_New ();
	uint8_t		sizes[2] = {1, 3};
	uint32_t	i, k, b;
	char		name[32];
	qa_func_t	f;
	uint8_t		*data;
	size_t		size;
	qc_progs_t	*p;

	QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Global (a, "time", QC_EV_FLOAT, NULL, 0);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	QA_Field (a, "think", QC_EV_FUNCTION, NULL);
	for (i = 0 ; i < 6 ; i++)
	{
		b = QA_Builtin (a, names[i], numbers[i], parms[i]);
		snprintf (name, sizeof(name), "builtin%u", i);
		QA_Global1 (a, name, QC_EV_FUNCTION, b);
	}
	QA_String (a, "some text");
	QA_Alloc (a, numwords, words, numwords);
	for (i = 0 ; i < numfuncs ; i++)
	{
		snprintf (name, sizeof(name), "f%u", i);
		f = QA_Function (a, name, sizes, 2, 4);
		snprintf (name, sizeof(name), "f%u_g", i);
		QA_Global1 (a, name, QC_EV_FUNCTION, f.index);
		for (k = 0 ; k < funcs[i].count ; k++)
			QA_Emit (a, funcs[i].stmts[k].op, funcs[i].stmts[k].a, funcs[i].stmts[k].b, funcs[i].stmts[k].c);
		QA_Emit (a, QOP_DONE, 0, 0, 0);
	}
	data = QA_Build (a, format, &size);
	p = QC_LoadProgs (data, size, NULL);
	free (data);
	QA_Free (a);
	return p;
}

static void FuzzConfig (qc_config_t *config, uint32_t runaway)
{
	QC_DefaultConfig (config, QC_CSQC);
	config->limits.runaway = runaway;
	config->limits.call_depth = 64;
	config->limits.local_stack_words = 4096;
	config->limits.reentry = 8;
}

// runs every function once
static void RunAll (qcvm_t *vm, uint32_t numfuncs, uint32_t arg)
{
	qc_value_t	args[2] = {QC_ValWord (arg), QC_ValVector (1, 2, 3)};
	char		name[16];
	uint32_t	i;
	qc_ent_t	e;

	QC_Spawn (vm, &e);
	for (i = 0 ; i < numfuncs ; i++)
	{
		snprintf (name, sizeof(name), "f%u", i);
		QC_Call (vm, QC_FindFunction (vm, name), 2, args, NULL);
		QC_RunThreads (vm, NULL);
	}
}

static void TestRandomStatements (uint64_t cases)
{
	func_t			funcs[3];
	uint32_t		words[40], numfuncs, numwords, i;
	qc_builtins_t	*b = FuzzBuiltins ();
	qc_config_t		config;
	qc_progs_t		*p;
	qcvm_t			*vm;
	uint64_t		n;

	FuzzConfig (&config, 20000);
	for (n = 0 ; n < cases ; n++)
	{
		numfuncs = 1 + QT_RandBelow (&rng, 3);
		for (i = 0 ; i < numfuncs ; i++)
			RandomFunction (&funcs[i]);
		numwords = 8 + QT_RandBelow (&rng, 32);
		for (i = 0 ; i < numwords ; i++)
			words[i] = Word ();
		p = Build (funcs, numfuncs, words, numwords, QT_RandBelow (&rng, 2) ? QC_FORMAT_FTE32 : QC_FORMAT_FTE16);
		if (!p)
			continue;
		vm = QC_Create (p, b, &config, NULL, NULL, NULL);
		QC_ReleaseProgs (p);
		if (!QT_CHECK (vm != NULL))
			continue;
		RunAll (vm, numfuncs, Word ());
		QC_CollectGarbage (vm);
		QT_CHECK (QC_Reset (vm));
		QC_Destroy (vm);
	}
	QC_BuiltinsFree (b);
}

/*
==============================================================================

THE INTERPRETER AGAINST ITS TRACING INSTANCE

==============================================================================
*/

typedef struct
{
	qt_text_t	log;
} recorder_t;

static void RecordWarning (void *ctx, const qc_warning_t *w)
{
	recorder_t	*r = ctx;
	char		text[512];

	QT_TextAppend (&r->log, QC_WarningText (w, text, sizeof(text)));
	QT_TextAppend (&r->log, "\n");
}

// what running every function did: results (errors with their backtraces),
// warnings, and memory from the strings through the globals into the local stack
static qt_text_t Outcome (qc_progs_t *p, const qc_builtins_t *b, uint32_t numfuncs, uint32_t arg, bool trace,
	const qc_config_t *config)
{
	qc_host_t	host = {.warning = RecordWarning};
	recorder_t	r = {{0}};
	qcvm_t		*vm = QC_Create (p, b, config, &host, &r, NULL);
	qc_value_t	args[2] = {QC_ValWord (arg), QC_ValVector (1, 2, 3)}, ret;
	char		name[16], text[2048];
	uint32_t	i, self, end, at, ran;
	uint8_t		byte;
	qc_ent_t	e;
	bool		ok;

	QT_TextAppend (&r.log, "");
	if (!vm)
		return r.log;
	QC_SetTrace (vm, trace);
	QC_Spawn (vm, &e);
	for (i = 0 ; i < numfuncs ; i++)
	{
		snprintf (name, sizeof(name), "f%u", i);
		ret = (qc_value_t){{0}};
		ok = QC_Call (vm, QC_FindFunction (vm, name), 2, args, &ret);
		if (ok)
			snprintf (text, sizeof(text), "ok %08x %08x %08x\n", ret.w[0], ret.w[1], ret.w[2]);
		else
		{
			QC_ErrorText (QC_LastError (vm), text, sizeof(text));
			QT_TextAppend (&r.log, text);
			QC_BacktraceText (&QC_LastError (vm)->backtrace, text, sizeof(text));
		}
		QT_TextAppend (&r.log, text);
		if (QC_RunThreads (vm, &ran))
			snprintf (text, sizeof(text), "threads ok %u\n", ran);
		else
		{
			QC_ErrorText (QC_LastError (vm), text, sizeof(text));
			QT_TextAppend (&r.log, text);
			QC_BacktraceText (&QC_LastError (vm)->backtrace, text, sizeof(text));
		}
		QT_TextAppend (&r.log, text);
	}
	QC_FindGlobal (vm, "self", &self, NULL);
	end = vm->progs[0].gbase + self * 4 + 4096;
	for (at = 0 ; at < end ; at++)
	{
		byte = 0;
		snprintf (text, sizeof(text), QC_ReadMemory (vm, at, &byte, 1) ? "%02x" : "--", byte);
		QT_TextAppend (&r.log, text);
	}
	QC_Destroy (vm);
	return r.log;
}

static void TestTracedAgrees (uint64_t cases)
{
	func_t			funcs[3];
	uint32_t		words[40], numfuncs, numwords, i, arg;
	qc_builtins_t	*b = FuzzBuiltins ();
	qc_config_t		config, chunked;
	qc_progs_t		*p;
	qt_text_t		fast, traced;
	uint64_t		n;

	for (n = 0 ; n < cases ; n++)
	{
		numfuncs = 1 + QT_RandBelow (&rng, 3);
		for (i = 0 ; i < numfuncs ; i++)
			BranchyFunction (&funcs[i]);
		numwords = 8 + QT_RandBelow (&rng, 32);
		for (i = 0 ; i < numwords ; i++)
			words[i] = Word ();
		arg = Word ();
		p = Build (funcs, numfuncs, words, numwords, QT_RandBelow (&rng, 2) ? QC_FORMAT_FTE32 : QC_FORMAT_FTE16);
		if (!p)
			continue;

		FuzzConfig (&config, 3000);
		fast = Outcome (p, b, numfuncs, arg, false, &config);
		traced = Outcome (p, b, numfuncs, arg, true, &config);
		if (!QT_CHECK (!strcmp (fast.text, traced.text)))
			printf ("  case %llu: plain and traced differ\n", (unsigned long long)n);
		QT_TextFree (&fast);
		QT_TextFree (&traced);

		// a budget handed out in chunks stops the interpreter mid-run: the outcome
		// must not change
		FuzzConfig (&config, 200000);
		chunked = config;
		chunked.limits.deadline = 3600;
		fast = Outcome (p, b, numfuncs, arg, false, &config);
		traced = Outcome (p, b, numfuncs, arg, false, &chunked);
		if (!QT_CHECK (!strcmp (fast.text, traced.text)))
			printf ("  case %llu: whole and chunked budgets differ\n", (unsigned long long)n);
		QT_TextFree (&fast);
		QT_TextFree (&traced);
		QC_ReleaseProgs (p);
	}
	QC_BuiltinsFree (b);
}

/*
==============================================================================

THE STANDARD BUILTINS WITH HOSTILE ARGUMENTS

==============================================================================
*/

// a progs declaring every builtin of a registry (by number where it's bound to
// one, else by name), with the globals and fields the standard builtins look up
static qc_progs_t *BuiltinProgs (const qc_builtins_t *b)
{
	static const struct
	{
		const char	*name;
		uint32_t	type;
	} fields[] = {{"origin", QC_EV_VECTOR}, {"mins", QC_EV_VECTOR}, {"maxs", QC_EV_VECTOR},
		{"angles", QC_EV_VECTOR}, {"gravitydir", QC_EV_VECTOR}, {"solid", QC_EV_FLOAT}, {"flags", QC_EV_FLOAT},
		{"ideal_yaw", QC_EV_FLOAT}, {"yaw_speed", QC_EV_FLOAT}, {"idealpitch", QC_EV_FLOAT},
		{"pitch_speed", QC_EV_FLOAT}, {"health", QC_EV_FLOAT}, {"chain", QC_EV_ENTITY},
		{"classname", QC_EV_STRING}, {"think", QC_EV_FUNCTION}};
	static const char	*strings[] = {"", "hello world", "a\\b\\c", "{\"k\": [1, 2.5, \"x\"]}", "%s %d %v %c", "MD5"};
	static const char	*views[3] = {"v_forward", "v_right", "v_up"};
	static const uint8_t	sizes[2] = {1, 1};
	qc_asm_t			*a = QA_New ();
	const char			*name;
	char				ref[256];
	uint32_t			i, number, f;
	bool				numbered;
	qa_func_t			cb;
	qc_progs_t			*p;

	QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Global (a, "other", QC_EV_ENTITY, NULL, 0);
	QA_Global (a, "time", QC_EV_FLOAT, NULL, 0);
	for (i = 0 ; i < 3 ; i++)
		QA_Global (a, views[i], QC_EV_VECTOR, NULL, 0);
	for (i = 0 ; i < sizeof(fields) / sizeof(fields[0]) ; i++)
		QA_Field (a, fields[i].name, fields[i].type, NULL);
	for (i = 0 ; i < sizeof(strings) / sizeof(strings[0]) ; i++)
		QA_String (a, strings[i]);
	for (i = 0 ; QC_BuiltinsAt (b, i, &name, &number, &numbered) ; i++)
	{
		f = QA_Builtin (a, name, numbered ? number : 0, -1);
		snprintf (ref, sizeof(ref), "%s_ref", name);
		QA_Global1 (a, ref, QC_EV_FUNCTION, f);
	}
	cb = QA_Function (a, "callback", sizes, 2, 1);
	QA_Emit (a, QOP_ADD_F, QA_Local (cb, 0), QA_Local (cb, 1), QA_Local (cb, 2));
	QA_Emit (a, QOP_RETURN, QA_Local (cb, 2), 0, 0);
	p = QA_Load (a, QC_FORMAT_FTE16);
	QA_Free (a);
	return p;
}

// an argument word: floats ordinary and extreme, string references (the
// program's, temps, statics, invalid ones), entities, pointers, any bits
static uint32_t ArgWord (void)
{
	static const float		specials[8] = {NAN, INFINITY, -INFINITY, 3.0e9f, -3.0e9f, 1.0e20f, 1.17549435e-38f, -0.0f};
	static const uint32_t	words[4] = {0xFFFFFFFF, 0x7FFFFFFF, 0x40000000, 1u << 24};

	switch (QT_RandBelow (&rng, 7))
	{
	case 0:		return QC_FloatBits (-10.0f + 310.0f * (float)QT_RandBelow (&rng, 1u << 24) / (float)(1u << 24));
	case 1:		return QC_FloatBits (specials[QT_RandBelow (&rng, 8)]);
	case 2:		return QT_RandBelow (&rng, 64);
	case 3:		return 0x80000000u | QT_RandBelow (&rng, 8);
	case 4:		return 0xC0000000u | QT_RandBelow (&rng, 4);
	case 5:		return words[QT_RandBelow (&rng, 4)];
	default:	return (uint32_t)QT_Rand (&rng);
	}
}

// random calls of every standard builtin with hostile arguments must not harm
// the VM or the host
static void TestStandardBuiltins (qc_numbering_t numbering, uint64_t cases)
{
	static const char	*temps[4] = {"temp one", "", "t\xC3\xA9mp", "^1red ^7white"};
	qc_builtins_t		*b = QC_BuiltinsStandard (numbering);
	qc_progs_t			*p = BuiltinProgs (b);
	uint32_t			count = QC_BuiltinsCount (b), calls, i, k, pick, number;
	qc_config_t			config;
	qc_value_t			args[8];
	const char			*name;
	bool				numbered;
	qcvm_t				*vm;
	qc_ent_t			e;
	uint64_t			n;
	int					argc;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.runaway = 100000;
	config.limits.heap_bytes = 1u << 22;
	config.developer = true;
	for (n = 0 ; n < cases ; n++)
	{
		vm = QC_Create (p, b, &config, NULL, NULL, NULL);
		if (!QT_CHECK (vm != NULL))
			break;
		for (i = 0 ; i < 3 ; i++)
			QC_Spawn (vm, &e);
		for (i = 0 ; i < 4 ; i++)
			QC_TempString (vm, temps[i], strlen (temps[i]));
		QC_Intern (vm, "interned", 8);
		calls = 1 + QT_RandBelow (&rng, 23);
		for (i = 0 ; i < calls ; i++)
		{
			pick = QT_RandBelow (&rng, count);
			argc = (int)QT_RandBelow (&rng, 9);
			for (k = 0 ; k < 8 ; k++)
				args[k] = (qc_value_t){{ArgWord (), ArgWord (), ArgWord ()}};
			if (!QC_BuiltinsAt (b, pick, &name, &number, &numbered))
				continue;
			QC_Call (vm, QC_FindFunction (vm, name), argc, args, NULL);
			QC_RunThreads (vm, NULL);
		}
		QC_CollectGarbage (vm);
		QC_Destroy (vm);
	}
	QC_ReleaseProgs (p);
	QC_BuiltinsFree (b);
}

int main (void)
{
	uint64_t	cases = QT_EnvNumber ("QC_FUZZ_ITERS", 256);

	rng = QT_EnvNumber ("QC_FUZZ_SEED", 0xF022);
	TestRandomStatements (cases);
	TestTracedAgrees (cases);
	// each case makes a VM with every builtin declared: a quarter as many
	TestStandardBuiltins (QC_NUMBERING_CSQC, (cases + 3) / 4);
	TestStandardBuiltins (QC_NUMBERING_SSQC, (cases + 3) / 4);
	TestStandardBuiltins (QC_NUMBERING_MENU, (cases + 3) / 4);
	return QT_Finish ("fuzz", "random programs and hostile builtin calls run without harm, traced or not");
}
