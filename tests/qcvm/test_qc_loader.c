// test_qc_loader.c -- loading programs: every format, the load-time rewrites,
// malformed input; and the opcode table, the number conversions and the hash
// maps the loader stands on

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const qc_format_t	all_formats[6] = {QC_FORMAT_QTEST, QC_FORMAT_V6, QC_FORMAT_FTE16,
	QC_FORMAT_FTE32, QC_FORMAT_KK7, QC_FORMAT_UHEXEN2};

static const char	*format_names[6] = {"QTest", "V6", "Fte16", "Fte32", "Kk7", "UHexen2"};

// A small program with every operand kind: globals, jumps both ways, global
// indices and immediates
static qc_asm_t *Sample (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	one = QA_Float (a, 1), ten = QA_Float (a, 10);
	uint32_t	counter = QA_Global (a, "counter", QC_EV_FLOAT, NULL, 0);
	uint32_t	origin, print, hello, print_g, top, lt, back, arr, idx, skip, end;
	uint32_t	arrinit[4] = {1, 2, 3, 4};
	qa_func_t	main;

	QA_Field (a, "origin", QC_EV_VECTOR, &origin);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	print = QA_Builtin (a, "print", 1, 1);
	hello = QA_StrConst (a, "hello");
	print_g = QA_Global1 (a, "print_ref", QC_EV_FUNCTION, print);

	main = QA_Function (a, "main", NULL, 0, 2);
	top = QA_Here (a);
	QA_Emit (a, QOP_ADD_F, counter, one, counter);
	lt = QA_Local (main, 0);
	QA_Emit (a, QOP_LT_F, counter, ten, lt);
	back = QA_Emit (a, QOP_IF_I, lt, 0, 0);
	QA_PatchJump (a, back, 1, top);
	QA_Emit (a, QOP_STORE_S, hello, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, print_g, 0, 0);
	arr = QA_Alloc (a, 4, arrinit, 4);
	idx = QA_Int (a, 2);
	QA_Emit (a, QOP_LOADA_F, arr, idx, QA_Local (main, 1));
	QA_Emit (a, QOP_BOUNDCHECK, idx, 4, 0);
	skip = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	QA_Emit (a, QOP_STORE_F, origin, QA_OFS_RETURN, 0);
	end = QA_Emit (a, QOP_RETURN, 0, 0, 0);
	QA_PatchJump (a, skip, 0, end);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	return a;
}

// each line of a listing, without QTest's per-statement line annotations
static void ListingLine (void *ctx, const char *text)
{
	qt_text_t	*t = ctx;
	char		line[1100];
	char		*cut;

	snprintf (line, sizeof(line), "%s", text);
	cut = strstr (line, "    ; line");
	if (cut)
		*cut = 0;
	if (t->len)
		QT_TextAppend (t, "\n");
	QT_TextAppend (t, line);
}

// the disassembly of every function (free the text)
static qt_text_t Listing (const qc_progs_t *p)
{
	qt_text_t	t = {0};
	uint32_t	i;

	QT_TextAppend (&t, "");
	for (i = 0 ; i < QC_ProgsNumFunctions (p) ; i++)
		QC_DisassembleFunction (p, i, ListingLine, &t);
	return t;
}

static bool HasNote (const qc_progs_t *p, qc_notekind_t kind, uint32_t index, uint32_t value)
{
	const qc_loadnote_t	*notes;
	uint32_t			count, i;

	notes = QC_ProgsNotes (p, &count);
	for (i = 0 ; i < count ; i++)
		if (notes[i].kind == kind && notes[i].index == index && notes[i].value == value)
			return true;
	return false;
}

static uint32_t CountNotes (const qc_progs_t *p, qc_notekind_t kind)
{
	const qc_loadnote_t	*notes;
	uint32_t			count, i, n = 0;

	notes = QC_ProgsNotes (p, &count);
	for (i = 0 ; i < count ; i++)
		n += notes[i].kind == kind;
	return n;
}

static qc_loaderror_t LoadError (const uint8_t *data, size_t size)
{
	qc_loaderror_t	error;
	qc_progs_t		*p = QC_LoadProgs (data, size, &error);

	QT_CHECK (p == NULL);
	QC_ReleaseProgs (p);
	return error;
}

/*
==============================================================================

THE OPCODE TABLE, NUMBERS AND MAPS

==============================================================================
*/

static void TestOpcodeTable (void)
{
	int		i;

	for (i = 0 ; i < QOP_COUNT ; i++)
		QT_CHECK (qc_opinfo[i].name && qc_opinfo[i].name[0]);
	QT_EQ_S (qc_opinfo[QOP_MUL_F].name, "MUL_F");
	QT_EQ_S (qc_opinfo[QOP_CONV_FU64].name, "CONV_FU64");
	QT_EQ_U (QOP_CONV_FU64, 281);
	QT_EQ_U (qc_opinfo[QOP_IF_I].operands[1], QC_OPND_J);
	QT_EQ_U (qc_opinfo[QOP_LOADA_F].operands[0], QC_OPND_A);
	QT_EQ_U (qc_opinfo[QOP_BOUNDCHECK].operands[2], QC_OPND_I);
	QT_EQ_I (QC_CallArgc (QOP_CALL0), 0);
	QT_EQ_I (QC_CallArgc (QOP_CALL8), 8);
	QT_EQ_I (QC_CallArgc (QOP_CALL1H), 1);
	QT_EQ_I (QC_CallArgc (QOP_CALL8H), 8);
	QT_EQ_I (QC_CallArgc (QOP_RETURN), -1);
}

static void TestNumbers (void)
{
	QT_EQ_I (QC_F2I (3.9f), 3);
	QT_EQ_I (QC_F2I (-3.9f), -3);
	QT_EQ_I (QC_F2I (NAN), INT32_MIN);
	QT_EQ_I (QC_F2I (2147483648.0f), INT32_MIN);
	QT_EQ_I (QC_F2I (-2147483648.0f), INT32_MIN);
	QT_EQ_I (QC_F2I (3e9f), INT32_MIN);
	QT_EQ_I (QC_F2I (INFINITY), INT32_MIN);
	QT_EQ_I (QC_F2I (-2147483500.0f), -2147483520);

	QT_EQ_U (QC_F2U (-1.0f), UINT32_MAX);
	QT_EQ_U (QC_F2U (4294967040.0f), 4294967040u);
	QT_EQ_U (QC_F2U (NAN), 0);
	QT_EQ_U (QC_D2U64 (-1.0), UINT64_MAX);
	QT_EQ_U (QC_D2U64 (1e19), 10000000000000000000ull);
	QT_EQ_U (QC_D2U64 (1e20), 0);
	QT_EQ_U (QC_D2U64 (NAN), 1ull << 63);

	QT_CHECK (!QC_FloatTrue (QC_FloatBits (-0.0f)));
	QT_CHECK (QC_FloatTrue (1));
	QT_CHECK (QC_FloatTrue (QC_FloatBits (1.0f)));
	QT_EQ_U (QC_FBool (true), QC_FloatBits (1.0f));
}

// random inserts and removals against a plain array
static void TestMap (void)
{
	enum { KEYS = 600 };
	static char	names[KEYS][8];
	int32_t		want[KEYS];
	qc_map_t	map = {0};
	uint64_t	rng = 1;
	uint32_t	value;
	int			i, k, live = 0;

	for (i = 0 ; i < KEYS ; i++)
	{
		snprintf (names[i], sizeof(names[i]), "k%d", i);
		want[i] = -1;
	}
	for (i = 0 ; i < 20000 ; i++)
	{
		k = (int)QT_RandBelow (&rng, KEYS);
		switch (QT_RandBelow (&rng, 3))
		{
		case 0:
			QT_CHECK (QC_MapAdd (&map, names[k], strlen (names[k]), (uint32_t)i));
			if (want[k] < 0)
			{
				want[k] = i;
				live++;
			}
			break;
		case 1:
			QT_CHECK (QC_MapSet (&map, names[k], strlen (names[k]), (uint32_t)i));
			live += want[k] < 0;
			want[k] = i;
			break;
		default:
			QT_EQ_U (QC_MapRemove (&map, names[k], strlen (names[k])), want[k] >= 0);
			live -= want[k] >= 0;
			want[k] = -1;
			break;
		}
	}
	QT_EQ_U (map.count, live);
	for (k = 0 ; k < KEYS ; k++)
	{
		if (want[k] < 0)
			QT_CHECK (!QC_MapGet (&map, names[k], strlen (names[k]), &value));
		else if (QT_CHECK (QC_MapGet (&map, names[k], strlen (names[k]), &value)))
			QT_EQ_U (value, want[k]);
	}
	QC_MapClear (&map);
	QT_CHECK (!QC_MapGet (&map, "k1", 2, &value));
}

/*
==============================================================================

LOADING

==============================================================================
*/

static void TestEveryFormatRoundTrips (void)
{
	qc_asm_t		*a = Sample ();
	qc_progs_t		*ref = QA_Load (a, QC_FORMAT_V6), *p;
	qt_text_t		expected = Listing (ref), got, uhexen2 = {0};
	const qc_loadnote_t	*notes;
	qc_definfo_t	def;
	qc_funcinfo_t	fn;
	uint32_t		count, i, n, index;
	int				f;
	const char		*at, *from = "CALL1          print_ref", *to = "CALL1H         print_ref, g0, g0";

	QT_CHECK (QT_Contains (expected.text, "IF_I"));

	// uHexen2 always uses the Hexen 2 calling convention
	at = strstr (expected.text, from);
	if (QT_CHECK (at != NULL))
	{
		char	*head = malloc ((size_t)(at - expected.text) + 1);

		memcpy (head, expected.text, (size_t)(at - expected.text));
		head[at - expected.text] = 0;
		QT_TextAppend (&uhexen2, head);
		QT_TextAppend (&uhexen2, to);
		QT_TextAppend (&uhexen2, at + strlen (from));
		free (head);
	}

	for (f = 0 ; f < 6 ; f++)
	{
		p = QA_Load (a, all_formats[f]);
		if (!QT_CHECK (QC_ProgsFormat (p) == all_formats[f]))
			printf ("  format %s\n", format_names[f]);
		notes = QC_ProgsNotes (p, &count);
		for (i = n = 0 ; i < count ; i++)
			n += notes[i].kind != QC_NOTE_HEXEN2_CALLS;
		if (!QT_EQ_U (n, 0))
			printf ("  format %s\n", format_names[f]);
		got = Listing (p);
		if (!QT_EQ_S (got.text, all_formats[f] == QC_FORMAT_UHEXEN2 ? uhexen2.text : expected.text))
			printf ("  format %s\n", format_names[f]);
		QT_TextFree (&got);
		QT_EQ_U (QC_ProgsNumGlobals (p), QA_NumGlobals (a));
		QT_CHECK (QC_ProgsGlobalDef (p, "counter", &def) && def.type == QC_EV_FLOAT);
		QT_CHECK (QC_ProgsFieldDef (p, "origin", &def) && def.type == QC_EV_VECTOR);
		QT_CHECK (QC_ProgsFieldDef (p, "origin_y", &def) && def.ofs == 1);
		QT_EQ_U (QC_ProgsEntityFields (p), 4);
		QT_CHECK (QC_ProgsFunctionIndex (p, "main", &index) && QC_ProgsFunction (p, index, &fn)
			&& fn.kind == QC_FUNC_QUAKEC);
		QT_CHECK (QC_ProgsFunctionIndex (p, "print", &index) && QC_ProgsFunction (p, index, &fn)
			&& fn.kind == QC_FUNC_BUILTIN && fn.number == 1);
		QC_ReleaseProgs (p);
	}
	QT_TextFree (&expected);
	QT_TextFree (&uhexen2);
	QC_ReleaseProgs (ref);
	QA_Free (a);
}

static void TestQTestLines (void)
{
	qc_asm_t	*a = Sample ();
	qc_progs_t	*p = QA_Load (a, QC_FORMAT_QTEST);
	uint32_t	line;

	QT_CHECK (QC_ProgsSourceLine (p, 0, &line) && line == 1);
	QT_CHECK (QC_ProgsSourceLine (p, 3, &line) && line == 4);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestHexen2Calls (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	qt_text_t	text;
	uint32_t	f, fg, x;

	f = QA_Builtin (a, "f", 1, 2);
	fg = QA_Global1 (a, "fg", QC_EV_FUNCTION, f);
	x = QA_Float (a, 1);
	QA_Function (a, "main", NULL, 0, 0);
	QA_Emit (a, QOP_CALL2, fg, x, x);
	QA_Emit (a, QOP_CALL0, fg, 0, 0);
	QA_Emit (a, QOP_RAND0, 0, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	p = QA_Load (a, QC_FORMAT_V6);
	QT_CHECK (HasNote (p, QC_NOTE_HEXEN2_CALLS, 0, 0));
	text = Listing (p);
	QT_CHECK (QT_Contains (text.text, "CALL2H"));
	QT_CHECK (QT_Contains (text.text, "CALL0 "));
	// RAND0 with c 0 gets the return slot
	QT_CHECK (QT_Contains (text.text, "RAND0          g1"));
	QT_TextFree (&text);
	QC_ReleaseProgs (p);
	QA_Free (a);

	// without an operand b, calls are left alone
	a = QA_New ();
	f = QA_Builtin (a, "f", 1, 1);
	fg = QA_Global1 (a, "fg", QC_EV_FUNCTION, f);
	QA_Function (a, "main", NULL, 0, 0);
	QA_Emit (a, QOP_CALL1, fg, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	p = QA_Load (a, QC_FORMAT_V6);
	QT_CHECK (!HasNote (p, QC_NOTE_HEXEN2_CALLS, 0, 0));
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestPoisonedStatements (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	qt_text_t	text;
	uint32_t	n, bad, unknown, edge, limit, over;

	QA_Function (a, "main", NULL, 0, 0);
	n = QA_NumGlobals (a);
	bad = QA_Emit (a, QOP_ADD_F, 1, 2, n + 100);
	unknown = QA_Emit (a, 500, 0, 0, 0);
	// operands may reach three words past the globals (FTE's zero tail) ...
	edge = QA_Emit (a, QOP_STORE_F, 1, 2, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	limit = QA_NumGlobals (a) + 3;
	QA_Patch (a, edge, 1, limit - 1);
	// ... but not further
	over = QA_Emit (a, QOP_STORE_F, 1, limit, 0);
	p = QA_Load (a, QC_FORMAT_FTE16);
	QT_CHECK (HasNote (p, QC_NOTE_POISONED_STATEMENT, bad, QOP_ADD_F));
	QT_CHECK (HasNote (p, QC_NOTE_POISONED_STATEMENT, unknown, 500));
	QT_CHECK (HasNote (p, QC_NOTE_POISONED_STATEMENT, over, QOP_STORE_F));
	QT_CHECK (!HasNote (p, QC_NOTE_POISONED_STATEMENT, edge, QOP_STORE_F));
	text = Listing (p);
	QT_CHECK (QT_Contains (text.text, "BAD"));
	QT_TextFree (&text);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

// FTE's sanitizer treats SWITCH_*.b as a global, and would poison a long switch
static void TestSwitchJumps (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	uint32_t	s, i, target, count;

	QA_Function (a, "main", NULL, 0, 1);
	s = QA_Emit (a, QOP_SWITCH_F, 1, 0, 0);
	for (i = 0, count = QA_NumGlobals (a) + 10 ; i < count ; i++)
		QA_Emit (a, QOP_DONE, 0, 0, 0);
	target = QA_Here (a);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_PatchJump (a, s, 1, target);
	p = QA_Load (a, QC_FORMAT_FTE16);
	QC_ProgsNotes (p, &count);
	QT_EQ_U (count, 0);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestWildJumps (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	qt_text_t	text;
	uint32_t	j, back;
	char		want[64];

	QA_Function (a, "main", NULL, 0, 0);
	j = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	QA_Patch (a, j, 0, 5000);
	back = QA_Emit (a, QOP_IF_I, 1, 0, 0);
	QA_Patch (a, back, 1, (uint32_t)-100);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	p = QA_Load (a, QC_FORMAT_V6);
	QT_CHECK (HasNote (p, QC_NOTE_JUMP_OUT_OF_RANGE, j, 0));
	QT_CHECK (HasNote (p, QC_NOTE_JUMP_OUT_OF_RANGE, back, 0));
	snprintf (want, sizeof(want), "GOTO           -> %u", QC_ProgsNumStatements (p));
	text = Listing (p);
	QT_CHECK (QT_Contains (text.text, want));
	QT_TextFree (&text);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestMalformedFunctions (void)
{
	qc_asm_t		*a = QA_New ();
	qc_progs_t		*p;
	qa_func_t		entry, locals, params;
	uint8_t			three = 3;
	uint32_t		named, n;
	qc_funcinfo_t	fn;

	entry = QA_Function (a, "entry_oob", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	locals = QA_Function (a, "locals_oob", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	params = QA_Function (a, "params_oob", &three, 1, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	named = QA_Builtin (a, "named", 0, 0);
	QA_PatchFunction (a, entry.index, 99999, entry.parm_start, 0);
	QA_PatchFunction (a, locals.index, 3, 60000, 100);
	n = QA_NumGlobals (a);
	QA_PatchFunction (a, params.index, 3, n + 2, 1);
	p = QA_Load (a, QC_FORMAT_FTE16);
	QT_CHECK (QC_ProgsFunction (p, entry.index, &fn) && fn.kind == QC_FUNC_INVALID && fn.invalid == QC_INVALID_ENTRY);
	QT_CHECK (QC_ProgsFunction (p, locals.index, &fn) && fn.kind == QC_FUNC_INVALID && fn.invalid == QC_INVALID_LOCALS);
	QT_CHECK (QC_ProgsFunction (p, params.index, &fn) && fn.kind == QC_FUNC_INVALID && fn.invalid == QC_INVALID_LOCALS);
	QT_CHECK (QC_ProgsFunction (p, named, &fn) && fn.kind == QC_FUNC_NAMED_BUILTIN);
	QT_CHECK (QC_ProgsFunction (p, 0, &fn) && fn.kind == QC_FUNC_NULL);
	QT_EQ_U (CountNotes (p, QC_NOTE_INVALID_FUNCTION), 3);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestBreakpointBit (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	qt_text_t	text;
	uint32_t	count;

	QA_Function (a, "main", NULL, 0, 0);
	QA_Emit (a, 0x8000 | QOP_ADD_F, 1, 2, 3);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	p = QA_Load (a, QC_FORMAT_V6);
	QC_ProgsNotes (p, &count);
	QT_EQ_U (count, 0);
	text = Listing (p);
	QT_CHECK (QT_Contains (text.text, "ADD_F"));
	QT_TextFree (&text);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestHeaderErrors (void)
{
	static const uint8_t	short_header[4] = {6, 0, 0, 0};
	qc_loaderror_t			e;
	qc_asm_t				*a;
	uint8_t					*data;
	size_t					size;

	e = LoadError (NULL, 0);
	QT_EQ_U (e.kind, QC_LOAD_TRUNCATED);
	e = LoadError (short_header, sizeof(short_header));
	QT_EQ_U (e.kind, QC_LOAD_TRUNCATED);

	a = Sample ();
	QA_HeaderOverride (a, 0, 5);
	data = QA_Build (a, QC_FORMAT_V6, &size);
	e = LoadError (data, size);
	QT_CHECK (e.kind == QC_LOAD_UNSUPPORTED_VERSION && e.value == 5);
	free (data);
	QA_Free (a);

	// a version-7 header needs the extended fields
	a = Sample ();
	data = QA_Build (a, QC_FORMAT_V6, &size);
	data[0] = 7;
	e = LoadError (data, 60);
	QT_EQ_U (e.kind, QC_LOAD_TRUNCATED);
	free (data);
	QA_Free (a);

	a = Sample ();
	QA_HeaderOverride (a, 21, 3);		// blockscompressed
	data = QA_Build (a, QC_FORMAT_FTE16, &size);
	e = LoadError (data, size);
	QT_CHECK (e.kind == QC_LOAD_COMPRESSED && e.value == 3);
	free (data);
	QA_Free (a);

	a = Sample ();
	QA_HeaderOverride (a, 3, 1000000);	// num_statements
	data = QA_Build (a, QC_FORMAT_V6, &size);
	e = LoadError (data, size);
	QT_CHECK (e.kind == QC_LOAD_SECTION_OUT_OF_BOUNDS && e.what && !strcmp (e.what, "statement"));
	free (data);
	QA_Free (a);
}

static void TestUnknownSecondaryVersion (void)
{
	qc_asm_t			*a = Sample ();
	qc_progs_t			*p;
	const qc_loadnote_t	*notes;
	uint32_t			count;

	QA_SetKK7Magic (a, 0xDEADBEEF);
	p = QA_Load (a, QC_FORMAT_KK7);
	QT_EQ_U (QC_ProgsFormat (p), QC_FORMAT_KK7);
	notes = QC_ProgsNotes (p, &count);
	QT_CHECK (count == 1 && notes[0].kind == QC_NOTE_ASSUMED_KK7 && notes[0].value == 0xDEADBEEF);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestBodyless (void)
{
	qc_asm_t	*a = Sample ();
	qc_progs_t	*p;

	QA_Bodyless (a, "ext_one");
	QA_Bodyless (a, "ext_two");
	p = QA_Load (a, QC_FORMAT_FTE16);
	QT_EQ_U (QC_ProgsNumBodyless (p), 2);
	QT_EQ_S (QC_ProgsBodyless (p, 0), "ext_one");
	QT_EQ_S (QC_ProgsBodyless (p, 1), "ext_two");
	QT_CHECK (QC_ProgsBodyless (p, 2) == NULL);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

static void TestFunctionLookup (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	qa_func_t	fa, fb, fe;
	uint32_t	index;

	fa = QA_Function (a, "a", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	fb = QA_Function (a, "b", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	// a "var" entry point named "entry" whose value is function b
	QA_Global1 (a, "entry", QC_EV_FUNCTION, fb.index);
	fe = QA_Function (a, "entry", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	p = QA_Load (a, QC_FORMAT_V6);
	QT_CHECK (QC_ProgsFunctionIndex (p, "a", &index) && index == fa.index);
	QT_CHECK (QC_ProgsFunctionIndex (p, "entry", &index) && index == fb.index);
	QT_CHECK (fe.index != fb.index);
	QT_CHECK (!QC_ProgsFunctionIndex (p, "missing", &index));
	QC_ReleaseProgs (p);
	QA_Free (a);
}

// a .lno file with counts and lines line numbers from 100
static uint8_t *LnoFile (const uint32_t counts[4], uint32_t lines, size_t *size)
{
	uint8_t		*d = malloc (24 + (size_t)lines * 4);
	uint32_t	words[6] = {0x464F4E4Cu, 1, counts[0], counts[1], counts[2], counts[3]};
	uint32_t	i, v;

	for (i = 0 ; i < 6 + lines ; i++)
	{
		v = i < 6 ? words[i] : 100 + i - 6;
		d[i * 4] = (uint8_t)v;
		d[i * 4 + 1] = (uint8_t)(v >> 8);
		d[i * 4 + 2] = (uint8_t)(v >> 16);
		d[i * 4 + 3] = (uint8_t)(v >> 24);
	}
	*size = 24 + (size_t)lines * 4;
	return d;
}

static void DisassemblyText (void *ctx, const char *text)
{
	QT_TextAppend (ctx, text);
	QT_TextAppend (ctx, "\n");
}

static void TestLineNumberFiles (void)
{
	qc_asm_t		*a = Sample ();
	qc_progs_t		*p = QA_Load (a, QC_FORMAT_FTE16);
	qc_loaderror_t	e;
	qt_text_t		text = {0};
	uint32_t		counts[4], wrong[4], line;
	uint8_t			*lno;
	size_t			size;

	counts[0] = QC_ProgsNumGlobalDefs (p);
	counts[1] = QC_ProgsNumGlobals (p);
	counts[2] = QC_ProgsNumFieldDefs (p);
	counts[3] = QC_ProgsNumStatements (p);
	memcpy (wrong, counts, sizeof(wrong));
	wrong[1]++;

	lno = LnoFile (wrong, counts[3], &size);
	QT_CHECK (!QC_AttachLineNumbers (p, lno, size, &e) && e.kind == QC_LOAD_LINE_NUMBERS);
	free (lno);
	lno = LnoFile (counts, 1, &size);
	QT_CHECK (!QC_AttachLineNumbers (p, lno, size, &e));
	free (lno);
	QT_CHECK (!QC_AttachLineNumbers (p, "nope", 4, &e));
	QT_CHECK (!QC_ProgsSourceLine (p, 2, &line));

	lno = LnoFile (counts, counts[3], &size);
	QT_CHECK (QC_AttachLineNumbers (p, lno, size, &e));
	free (lno);
	QT_CHECK (QC_ProgsSourceLine (p, 2, &line) && line == 102);
	QC_DisassembleFunction (p, 2, DisassemblyText, &text);
	QT_CHECK (QT_Contains (text.text, "; line"));
	QT_TextFree (&text);
	QC_ReleaseProgs (p);
	QA_Free (a);
}

// the builtins the code calls, not merely those it declares; calls through
// variables that only get a builtin at run time are not seen
static void TestCalledBuiltins (void)
{
	qc_asm_t	*a = QA_New ();
	qc_progs_t	*p;
	uint32_t	used, also, used_g, also_g, var_g, qc_g, out[8], n;
	qa_func_t	qc;
	int			f;

	used = QA_Builtin (a, "used", 1, 0);
	also = QA_Builtin (a, "also", 0, 0);		// bound by name
	QA_Builtin (a, "unused", 3, 0);
	QA_Builtin (a, "late", 4, 0);
	used_g = QA_Global1 (a, "used_g", QC_EV_FUNCTION, used);
	also_g = QA_Global1 (a, "also_g", QC_EV_FUNCTION, also);
	var_g = QA_Global1 (a, "var_g", QC_EV_FUNCTION, 0);
	qc = QA_Function (a, "qc", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	qc_g = QA_Global1 (a, "qc_g", QC_EV_FUNCTION, qc.index);
	QA_Function (a, "main", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, used_g, 0, 0);
	QA_Emit (a, QOP_CALL1H, also_g, 0, 0);
	QA_Emit (a, QOP_CALL0, used_g, 0, 0);
	QA_Emit (a, QOP_CALL0, qc_g, 0, 0);
	QA_Emit (a, QOP_CALL0, var_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	for (f = 0 ; f < 6 ; f++)
	{
		p = QA_Load (a, all_formats[f]);
		n = QC_ProgsCalledBuiltins (p, out, 8);
		if (!QT_CHECK (n == 2 && out[0] == used && out[1] == also))
			printf ("  format %s\n", format_names[f]);
		QC_ReleaseProgs (p);
	}
	QA_Free (a);
}

// Names may overlap in the string table, so their total size is not bounded
// by the file size: every suffix of a long string is another name. The loader
// rejects programs whose names add up to more than it is willing to index,
// without scanning them all.
static void TestOverlappingNames (void)
{
	qc_asm_t		*a = QA_New ();
	qc_progs_t		*p;
	char			*s = malloc (20001);
	uint32_t		k, str;
	uint8_t			*data;
	size_t			size;
	qc_loaderror_t	e;

	memset (s, 'a', 20000);
	s[20000] = 0;
	str = QA_String (a, s);
	for (k = 0 ; k < 20000 ; k++)
		QA_DefGlobalAt (a, str + k, QC_EV_FLOAT, 0);
	data = QA_Build (a, QC_FORMAT_FTE32, &size);
	QT_CHECK (size < 400000);
	e = LoadError (data, size);
	QT_EQ_U (e.kind, QC_LOAD_NAMES_TOO_LARGE);
	free (data);
	QA_Free (a);

	// a few hundred are fine
	a = QA_New ();
	memset (s, 'b', 300);
	s[300] = 0;
	str = QA_String (a, s);
	for (k = 0 ; k < 300 ; k++)
		QA_DefGlobalAt (a, str + k, QC_EV_FLOAT, 0);
	p = QA_Load (a, QC_FORMAT_FTE32);
	QT_CHECK (QC_ProgsGlobalNameAt (p, 0) == NULL);
	QC_ReleaseProgs (p);
	QA_Free (a);
	free (s);
}

static void IgnoreLine (void *ctx, const char *text)
{
	(void)ctx;
	(void)text;
}

// corrupted programs never crash the loader or the disassembler
static void TestCorruption (void)
{
	uint64_t		rng = QT_EnvNumber ("QC_FUZZ_SEED", 0x5EED);
	uint64_t		iters = QT_EnvNumber ("QC_FUZZ_ITERS", 2048);
	qc_asm_t		*a = Sample ();
	qc_progs_t		*p;
	qc_definfo_t	def;
	uint8_t			*data;
	size_t			size;
	uint32_t		flips, i, n, out[16];
	uint64_t		it;
	char			buf[256];

	for (it = 0 ; it < iters ; it++)
	{
		data = QA_Build (a, all_formats[QT_RandBelow (&rng, 6)], &size);
		flips = 1 + QT_RandBelow (&rng, 23);
		for (i = 0 ; i < flips ; i++)
			data[QT_RandBelow (&rng, (uint32_t)size)] = (uint8_t)QT_Rand (&rng);
		if (QT_RandBelow (&rng, 2))
			size = QT_RandBelow (&rng, (uint32_t)size);
		p = QC_LoadProgs (data, size, NULL);
		if (p)
		{
			n = QC_ProgsNumFunctions (p);
			for (i = 0 ; i < n && i < 64 ; i++)
				QC_DisassembleFunction (p, i, IgnoreLine, NULL);
			for (i = 0 ; i <= QC_ProgsNumStatements (p) + 1 ; i++)
				QC_DisassembleStatement (p, i, buf, sizeof(buf));
			for (i = 0 ; i < QC_ProgsNumGlobalDefs (p) ; i++)
				QC_ProgsGlobalDefAt (p, i, &def);
			QC_ProgsCalledBuiltins (p, out, 16);
			QC_ReleaseProgs (p);
		}
		free (data);
	}
	QA_Free (a);
}

// the server's own game: qw-qc's qwprogs.dat
static void TestQWProgs (void)
{
	uint8_t				*data;
	size_t				size;
	qc_progs_t			*p;
	uint32_t			count, i, index;
	qc_funcinfo_t		fn;
	qt_text_t			text = {0};

	data = QT_LoadFile (QT_SOURCE_DIR "/qw-qc/qwprogs.dat", &size);
	if (!QT_CHECK (data != NULL))
		return;
	p = QC_LoadProgs (data, size, NULL);
	free (data);
	if (!QT_CHECK (p != NULL))
		return;
	QT_EQ_U (QC_ProgsFormat (p), QC_FORMAT_V6);
	QT_EQ_U (QC_ProgsCRC (p), 54730);
	QC_ProgsNotes (p, &count);
	QT_EQ_U (count, 0);
	for (i = 0 ; i < QC_ProgsNumFunctions (p) ; i++)
		QC_DisassembleFunction (p, i, DisassemblyText, &text);
	QT_CHECK (!QT_Contains (text.text, "BAD"));
	QT_CHECK (QT_Contains (text.text, "; function"));
	QT_CHECK (QC_ProgsFunctionIndex (p, "PlayerPreThink", &index) && QC_ProgsFunction (p, index, &fn)
		&& fn.kind == QC_FUNC_QUAKEC);
	QT_TextFree (&text);
	QC_ReleaseProgs (p);
}

// KTX's csprogs.dat, when QCVM_CSPROGS names one
static void TestKtxCsprogs (void)
{
	const char		*path = getenv ("QCVM_CSPROGS");
	uint8_t			*data, *lno;
	size_t			size, lnosize;
	qc_progs_t		*p;
	qc_funcinfo_t	fn;
	qc_definfo_t	def;
	qt_text_t		text = {0};
	uint32_t		count, i, index, line, qc = 0;
	bool			autocvar = false;
	char			lnopath[1024];

	if (!path || !*path)
	{
		printf ("loader: skipping the KTX csprogs test (QCVM_CSPROGS is not set)\n");
		return;
	}
	data = QT_LoadFile (path, &size);
	if (!QT_CHECK (data != NULL))
		return;
	p = QC_LoadProgs (data, size, NULL);
	free (data);
	if (!QT_CHECK (p != NULL))
		return;
	QT_EQ_U (QC_ProgsFormat (p), QC_FORMAT_FTE16);
	// the CRC covers the engine's system definitions, so it is stable across
	// rebuilds of the mod; the sizes are sanity bounds, as the mod keeps changing
	QT_EQ_U (QC_ProgsCRC (p), 22390);
	QT_CHECK (QC_ProgsNumGlobals (p) > 1000 && QC_ProgsNumFunctions (p) > 400 && QC_ProgsNumStatements (p) > 1000);
	QT_CHECK (QC_ProgsEntityFields (p) > 100);
	QC_ProgsNotes (p, &count);
	QT_EQ_U (count, 0);
	for (i = 0 ; i < QC_ProgsNumFunctions (p) ; i++)
	{
		QC_ProgsFunction (p, i, &fn);
		if (fn.kind != QC_FUNC_QUAKEC)
			continue;
		qc++;
		QC_DisassembleFunction (p, i, DisassemblyText, &text);
	}
	QT_CHECK (qc > 50);
	QT_CHECK (!QT_Contains (text.text, "BAD"));
	QT_TextFree (&text);
	QT_CHECK (QC_ProgsFunctionIndex (p, "CSQC_UpdateView", &index));
	for (i = 0 ; QC_ProgsGlobalDefAt (p, i, &def) ; i++)
		autocvar |= !strncmp (def.name, "autocvar_cl_", 12);
	QT_CHECK (autocvar);

	snprintf (lnopath, sizeof(lnopath), "%s", path);
	if (strlen (lnopath) > 4)
	{
		strcpy (lnopath + strlen (lnopath) - 4, ".lno");
		lno = QT_LoadFile (lnopath, &lnosize);
		if (lno)
		{
			QT_CHECK (QC_AttachLineNumbers (p, lno, lnosize, NULL));
			QT_CHECK (QC_ProgsFunctionIndex (p, "CSQC_Init", &index) && QC_ProgsFunction (p, index, &fn)
				&& fn.kind == QC_FUNC_QUAKEC);
			QT_CHECK (QC_ProgsSourceLine (p, fn.entry, &line) && line > 0);
			free (lno);
		}
	}
	QC_ReleaseProgs (p);
}

int main (void)
{
	TestOpcodeTable ();
	TestNumbers ();
	TestMap ();
	TestEveryFormatRoundTrips ();
	TestQTestLines ();
	TestHexen2Calls ();
	TestPoisonedStatements ();
	TestSwitchJumps ();
	TestWildJumps ();
	TestMalformedFunctions ();
	TestBreakpointBit ();
	TestHeaderErrors ();
	TestUnknownSecondaryVersion ();
	TestBodyless ();
	TestFunctionLookup ();
	TestLineNumberFiles ();
	TestCalledBuiltins ();
	TestOverlappingNames ();
	TestCorruption ();
	TestQWProgs ();
	TestKtxCsprogs ();
	return QT_Finish ("loader", "every format loads, malformed input is refused or poisoned");
}
