// test_qc_lib_introspect.c -- checkbuiltin, isfunction, callfunction, the
// extern builtins, abort and the debugging hooks (qcvm-rs's
// tests/all/builtins_misc/introspect.rs)

#include "qc_harness.h"

#include <stdio.h>
#include <string.h>

static void Setup (qc_asm_t *a, void *ctx)
{
	static const uint8_t	two[2] = {1, 1};
	uint32_t				abort_g, gval, missing_name, missing_arg, seven, one, ninety_nine, watched;
	qa_func_t				f;

	(void)ctx;
	// FTE numbers these, though its extension dump doesn't
	QA_Builtin (a, "callfunction", 605, 8);
	QA_Builtin (a, "externrefcall", 205, 8);
	abort_g = QA_Global1 (a, "abort_fn", QC_EV_FUNCTION, QA_Builtin (a, "abort", 211, 1));
	gval = QA_Global1 (a, "gval", QC_EV_FLOAT, QC_FloatBits (5));
	QA_DefGlobal (a, "gval_alias", QC_EV_FLOAT, gval);
	QA_Global (a, "gvec", QC_EV_VECTOR, NULL, 0);
	QA_Global (a, "gptr", QC_EV_POINTER, NULL, 0);
	missing_name = QA_Global (a, "missing_name", QC_EV_STRING, NULL, 0);
	missing_arg = QA_Global (a, "missing_arg", QC_EV_FLOAT, NULL, 0);
	seven = QA_Float (a, 7);
	one = QA_Float (a, 1);
	ninety_nine = QA_Float (a, 99);

	f = QA_Function (a, "add", two, 2, 1);
	QA_Emit (a, QOP_ADD_F, QA_Local (f, 0), QA_Local (f, 1), QA_Local (f, 2));
	QA_Emit (a, QOP_RETURN, QA_Local (f, 2), 0, 0);

	f = QA_Function (a, "other", two, 2, 0);
	QA_Emit (a, QOP_RETURN, QA_Local (f, 1), 0, 0);

	f = QA_Function (a, "MissingFunc", two, 2, 0);
	QA_Emit (a, QOP_STORE_S, QA_Local (f, 0), missing_name, 0);
	QA_Emit (a, QOP_STORE_F, QA_Local (f, 1), missing_arg, 0);
	QA_Emit (a, QOP_RETURN, ninety_nine, 0, 0);

	// abort_test() { abort(7); return 1; }
	QA_Function (a, "abort_test", NULL, 0, 0);
	QA_Emit (a, QOP_STORE_F, seven, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, abort_g, 0, 0);
	QA_Emit (a, QOP_RETURN, one, 0, 0);

	// bump() { watched += 1; watched += 1; }
	QA_Builtin (a, "setwatchpoint", 0, 3);
	watched = QA_Global (a, "watched", QC_EV_FLOAT, NULL, 0);
	QA_Function (a, "bump", NULL, 0, 0);
	QA_Emit (a, QOP_ADD_F, watched, one, watched);
	QA_Emit (a, QOP_ADD_F, watched, one, watched);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
}

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, Setup, NULL);
}

// a main progs global's word
static uint32_t Global (qh_t *h, const char *name)
{
	uint32_t	word = 0, type;

	if (!QC_FindGlobal (h->vm, name, &word, &type))
		printf ("  no global %s\n", name);
	return word;
}

static void TestCheckbuiltinAndIsfunction (void)
{
	qh_t	*h = Harness ();

	QT_EQ_F (QH_Float (h, "checkbuiltin", ARGS (W (QH_Func (h, "print")))), 1);
	QT_EQ_F (QH_Float (h, "checkbuiltin", ARGS (W (QH_Func (h, "add")))), 0);		// QuakeC functions aren't
	// an engine builtin the standard library doesn't provide
	QT_EQ_F (QH_Float (h, "checkbuiltin", ARGS (W (QH_Func (h, "drawpic")))), 0);
	QT_EQ_F (QH_Float (h, "checkbuiltin", ARGS (W (0))), 0);
	QT_EQ_F (QH_Float (h, "isfunction", ARGS (QH_S (h, "add"))), 1);
	QT_EQ_F (QH_Float (h, "isfunction", ARGS (QH_S (h, "print"))), 1);
	QT_EQ_F (QH_Float (h, "isfunction", ARGS (QH_S (h, "nope"))), 0);
	QT_EQ_F (QH_Float (h, "isfunction", ARGS (QH_S (h, "0:add"))), 1);
	QT_EQ_F (QH_Float (h, "isfunction", ARGS (QH_S (h, "1:add"))), 0);		// there's no progs 1
	QH_Free (h);
}

// callfunction calls by name with the other arguments
static void TestCallfunction (void)
{
	qh_t		*h = Harness ();
	uint32_t	target;

	QT_EQ_F (QH_Float (h, "callfunction", ARGS (F (2), F (3), QH_S (h, "add"))), 5);
	// missing functions are ignored
	QT_CHECK (QH_Call (h, "callfunction", ARGS (F (2), QH_S (h, "nope")), NULL));
	QT_EQ_I (QH_Fails (h, "callfunction", NOARGS), QC_ERR_BUILTIN);
	// the lookup follows a function global's current value (QuakeC may redirect it)
	target = Global (h, "add");
	QC_Globals (h->vm)[target].u = QH_Func (h, "other");
	QT_EQ_F (QH_Float (h, "callfunction", ARGS (F (2), F (3), QH_S (h, "add"))), 3);
	QC_Globals (h->vm)[target].u = 0;
	QT_EQ_F (QH_Float (h, "isfunction", ARGS (QH_S (h, "add"))), 0);
	QH_Free (h);
}

// externcall shifts the arguments
static void TestExterncall (void)
{
	static const float	prnums[3] = {0, -1, -2};
	qh_t				*h = Harness ();
	qc_func_t			add;
	int					i;

	for (i = 0 ; i < 3 ; i++)
		QT_EQ_F (QH_Float (h, "externcall", ARGS (F (prnums[i]), QH_S (h, "add"), F (2), F (3))), 5);
	// a missing function calls MissingFunc(name, args...) instead
	QT_EQ_F (QH_Float (h, "externcall", ARGS (F (0), QH_S (h, "nope"), F (4))), 99);
	QT_EQ_S (QH_Text (h, QC_Globals (h->vm)[Global (h, "missing_name")].u), "nope");
	QT_EQ_F (QC_Globals (h->vm)[Global (h, "missing_arg")].f, 4);
	// progs 1 doesn't exist (and so has no MissingFunc either)
	QT_EQ_I (QH_Fails (h, "externcall", ARGS (F (1), QH_S (h, "add"), F (2), F (3))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "Couldn't find function add"));
	// externrefcall takes a function reference
	add = QH_Func (h, "add");
	QT_EQ_F (QH_Float (h, "externrefcall", ARGS (F (0), W (add), F (4), F (5))), 9);
	QT_EQ_I (QH_Fails (h, "externrefcall", ARGS (F (0), W (0))), QC_ERR_NULL_FUNCTION);
	QH_Free (h);
}

// externvalue and externset reach globals
static void TestExternvalueAndExternset (void)
{
	qh_t		*h = Harness ();
	qc_value_t	r;
	uint32_t	gval = Global (h, "gval"), gvec;
	float		v[3];

	QT_EQ_F (QH_Float (h, "externvalue", ARGS (F (0), QH_S (h, "gval"))), 5);
	QT_EQ_F (QH_Float (h, "externvalue", ARGS (F (-1), QH_S (h, "gv"), QH_S (h, "al"))), 5);	// concatenated
	QT_EQ_F (QH_Float (h, "externvalue", ARGS (F (0), QH_S (h, "gval_alias"))), 5);
	r = QH_Raw (h, "externvalue", ARGS (F (0), QH_S (h, "&gval")));
	QT_EQ_U (r.w[0], h->vm->progs[0].gbase + gval * 4);
	QT_CHECK (r.w[1] == 0 && r.w[2] == 0);
	r = QH_Raw (h, "externvalue", ARGS (F (0), QH_S (h, "&nope")));
	QT_CHECK (r.w[0] == 0 && r.w[1] == 0 && r.w[2] == 0);
	r = QH_Raw (h, "externvalue", ARGS (F (0), QH_S (h, "nope")));
	QT_CHECK (r.w[0] == 0 && r.w[1] == 0 && r.w[2] == 0);
	r = QH_Raw (h, "externvalue", ARGS (F (1), QH_S (h, "gval")));
	QT_CHECK (r.w[0] == 0 && r.w[1] == 0 && r.w[2] == 0);
	// no global of that name: the function of that name
	r = QH_Raw (h, "externvalue", ARGS (F (0), QH_S (h, "0:add")));
	QT_EQ_U (r.w[0], QH_Func (h, "add"));
	QT_CHECK (r.w[1] == 0 && r.w[2] == 0);

	QT_CHECK (QH_Call (h, "externset", ARGS (F (0), F (7), QH_S (h, "gval")), NULL));
	QT_EQ_F (QC_Globals (h->vm)[gval].f, 7);
	QT_CHECK (QH_Call (h, "externset", ARGS (F (-2), V (1, 2, 3), QH_S (h, "g"), QH_S (h, "vec")), NULL));
	gvec = Global (h, "gvec");
	QT_CHECK (QC_Globals (h->vm)[gvec].f == 1 && QC_Globals (h->vm)[gvec + 1].f == 2
		&& QC_Globals (h->vm)[gvec + 2].f == 3);
	QH_Vector (h, v, "externvalue", ARGS (F (0), QH_S (h, "gvec")));
	QT_CHECK (v[0] == 1 && v[1] == 2 && v[2] == 3);
	// pointers are one word; unknown names are ignored
	QT_CHECK (QH_Call (h, "externset", ARGS (F (0), W (0x1234), QH_S (h, "gptr")), NULL));
	QT_EQ_U (QC_Globals (h->vm)[Global (h, "gptr")].u, 0x1234);
	QT_CHECK (QH_Call (h, "externset", ARGS (F (0), F (1), QH_S (h, "nope")), NULL));
	QH_Free (h);
}

// abort unwinds to the engine
static void TestAbort (void)
{
	qh_t		*h = Harness ();
	qc_value_t	r;

	QT_EQ_F (QH_Float (h, "abort_test", NOARGS), 7);
	// called from the engine directly
	QT_EQ_F (QH_Float (h, "abort", ARGS (F (3))), 3);
	r = QH_Raw (h, "abort", NOARGS);
	QT_CHECK (r.w[0] == 0 && r.w[1] == 0 && r.w[2] == 0);
	// through callfunction, abort only unwinds to callfunction's own call
	QT_EQ_F (QH_Float (h, "callfunction", ARGS (QH_S (h, "abort_test"))), 7);
	QH_Free (h);
}

static void TestDebuggingHooks (void)
{
	qh_t	*h = Harness (), *menu;

	QT_CHECK (QH_Call (h, "traceon", NOARGS, NULL));
	QT_CHECK (QH_Call (h, "traceoff", NOARGS, NULL));
	QT_CHECK (QH_Call (h, "breakpoint", NOARGS, NULL));
	if (QT_EQ_I (QH_NumWarnings (h), 1))
		QT_EQ_S (QH_WarningText (h, 0), "break statement");
	QH_Free (h);

	menu = QH_New (QC_NUMBERING_MENU, NULL, NULL, NULL);
	QT_CHECK (QH_Call (menu, "stackdump", NOARGS, NULL));
	if (QT_EQ_I (menu->host.numdumps, 1))
		QT_EQ_I (menu->host.dumps[0].kind, QC_DUMP_TRACE);
	QT_EQ_I (QH_Fails (menu, "crash", NOARGS), QC_ERR_QC);
	QT_EQ_S (QH_ErrorMessage (menu), "crash called");
	QH_Free (menu);
}

// setwatchpoint: a warning at each change of the value, QuakeC's or the
// host's, seen at the statement after it; a null pointer stops it
static void TestWatchpoint (void)
{
	qh_t		*h = Harness ();
	uint32_t	word = Global (h, "watched");
	uint32_t	ptr = (uint32_t)((uint8_t *)&QC_Globals (h->vm)[word] - h->vm->mem.s.base);

	QT_CHECK (QH_Call (h, "setwatchpoint", ARGS (QH_S (h, "w"), F (QC_EV_FLOAT), W (ptr)), NULL));
	QH_ClearWarnings (h);
	QT_CHECK (QH_Call (h, "bump", NOARGS, NULL));
	if (QT_EQ_I (QH_NumWarnings (h), 2))
	{
		QT_EQ_S (QH_WarningText (h, 0), "watch point \"w\" changed from 0 to 1");
		QT_EQ_S (QH_WarningText (h, 1), "watch point \"w\" changed from 1 to 2");
	}
	QC_Globals (h->vm)[word].f = 5;
	QH_ClearWarnings (h);
	QT_CHECK (QH_Call (h, "bump", NOARGS, NULL));
	if (QT_EQ_I (QH_NumWarnings (h), 3))
		QT_EQ_S (QH_WarningText (h, 0), "watch point \"w\" changed from 2 to 5");

	QT_CHECK (QH_Call (h, "setwatchpoint", ARGS (QH_S (h, ""), F (0), W (0)), NULL));
	QH_ClearWarnings (h);
	QT_CHECK (QH_Call (h, "bump", NOARGS, NULL));
	QT_EQ_I (QH_NumWarnings (h), 0);
	QT_EQ_F (QC_Globals (h->vm)[word].f, 9);
	QT_CHECK (QH_Call (h, "setwatchpoint", ARGS (QH_S (h, "x"), F (QC_EV_FLOAT), W (0x7FFFFFF0)), NULL));
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Free (h);
}

int main (void)
{
	TestCheckbuiltinAndIsfunction ();
	TestCallfunction ();
	TestExterncall ();
	TestExternvalueAndExternset ();
	TestAbort ();
	TestDebuggingHooks ();
	TestWatchpoint ();
	return QT_Finish ("lib_introspect", "the VM as QuakeC sees it through its builtins");
}
