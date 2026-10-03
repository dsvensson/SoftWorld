// test_qc_vm.c -- calls, returns, builtins, re-entrancy, errors and their
// backtraces, limits, abort, tracing, the animation opcodes, entities and temp
// strings as QuakeC uses them, and a longjmp out of the VM

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// what builtins and warnings saw
typedef struct
{
	char		log[512];
	int			numwarnings;
	char		warnings[16][256];
	qc_warnkind_t	warnkind[16];
	int64_t		warnvalue[16];
	char		warnframe[16][64];
	qc_func_t	callback;
	char		trace[64][96];
	int			numtrace;
	int			spawned[8][2];
	int			numspawned;
	int			removed[8];
	float		removedhealth[8];
	int			numremoved;
	jmp_buf		*escape;
} host_t;

static void OnWarning (void *ctx, const qc_warning_t *w)
{
	host_t	*h = ctx;

	if (h->numwarnings >= 16)
		return;
	QC_WarningText (w, h->warnings[h->numwarnings], sizeof(h->warnings[0]));
	h->warnkind[h->numwarnings] = w->kind;
	h->warnvalue[h->numwarnings] = w->value;
	snprintf (h->warnframe[h->numwarnings], sizeof(h->warnframe[0]), "%s",
		w->backtrace.count ? w->backtrace.frames[0].name : "");
	h->numwarnings++;
}

static void OnTrace (void *ctx, const char *line)
{
	host_t	*h = ctx;

	if (h->numtrace < 64)
		snprintf (h->trace[h->numtrace++], sizeof(h->trace[0]), "%s", line);
}

static const qc_host_t	test_host = {.warning = OnWarning, .trace = OnTrace};

static host_t *Host (qcvm_t *vm)
{
	return QC_HostContext (vm);
}

static qcvm_t *MakeVM (const qc_asm_t *a, qc_builtins_t *b, host_t *h)
{
	return QA_CreateVM (a, NULL, b, &test_host, h);
}

static qc_func_t Func (qcvm_t *vm, const char *name)
{
	qc_func_t	f = QC_FindFunction (vm, name);

	if (!f)
		printf ("no function %s\n", name);
	return f;
}

static bool CallF (qcvm_t *vm, const char *name, int argc, const qc_value_t *args, float *out)
{
	qc_value_t	ret = {{0}};
	bool		ok = QC_Call (vm, Func (vm, name), argc, args, &ret);

	if (out)
		*out = QC_BitsFloat (ret.w[0]);
	return ok;
}

static qc_errkind_t CallError (qcvm_t *vm, const char *name)
{
	return QC_Call (vm, Func (vm, name), 0, NULL, NULL) ? QC_ERR_NONE : QC_LastError (vm)->kind;
}

/*
==============================================================================

TEST BUILTINS

==============================================================================
*/

static bool B_Log (qcvm_t *vm)
{
	host_t	*h = Host (vm);
	char	item[32];
	int		i;

	strncat (h->log, "log(", sizeof(h->log) - strlen (h->log) - 1);
	for (i = 0 ; i < QC_Argc (vm) ; i++)
	{
		snprintf (item, sizeof(item), i ? ",%g" : "%g", (double)QC_ArgFloat (vm, i));
		strncat (h->log, item, sizeof(h->log) - strlen (h->log) - 1);
	}
	strncat (h->log, ") ", sizeof(h->log) - strlen (h->log) - 1);
	QC_ReturnFloat (vm, QC_ArgFloat (vm, 0) * 10);
	return true;
}

static bool B_Callback (qcvm_t *vm)
{
	// read the arguments before re-entering QuakeC: the parameter slots are shared
	float		x = QC_ArgFloat (vm, 0);
	qc_value_t	arg = QC_ValFloat (x + 1), ret;

	if (!QC_Call (vm, Host (vm)->callback, 1, &arg, &ret))
		return false;
	QC_ReturnFloat (vm, QC_BitsFloat (ret.w[0]) * 2);
	return true;
}

static bool B_RunCallback (qcvm_t *vm)
{
	return QC_Call (vm, Host (vm)->callback, 0, NULL, NULL);
}

static bool B_Fail (qcvm_t *vm)
{
	return QC_Error (vm, "boom");
}

static bool B_Spawn (qcvm_t *vm)
{
	qc_ent_t	e;

	if (!QC_Spawn (vm, &e))
		return false;
	QC_ReturnWord (vm, e);
	return true;
}

static bool B_Remove (qcvm_t *vm)
{
	QC_Remove (vm, QC_ArgWord (vm, 0), false);
	return true;
}

static bool B_Concat (qcvm_t *vm)
{
	char	out[256] = "";
	int		i;

	for (i = 0 ; i < QC_Argc (vm) ; i++)
		strncat (out, QC_ArgString (vm, i), sizeof(out) - strlen (out) - 1);
	return QC_ReturnString (vm, out, strlen (out));
}

static bool B_Abort (qcvm_t *vm)
{
	return QC_Abort (vm, QC_ValFloat (QC_ArgFloat (vm, 0)));
}

static bool B_TraceOn (qcvm_t *vm)
{
	QC_SetTrace (vm, true);
	return true;
}

static bool B_TraceOff (qcvm_t *vm)
{
	QC_SetTrace (vm, false);
	return true;
}

static bool B_Escape (qcvm_t *vm)
{
	longjmp (*Host (vm)->escape, 1);
}

static qc_builtins_t *Builtins1 (uint32_t number, const char *name, qc_builtin_t f)
{
	qc_builtins_t	*b = QC_BuiltinsCreate (QC_NUMBERING_NONE);

	QC_BuiltinsSetNumbered (b, number, name, f);
	return b;
}

/*
==============================================================================

CALLS

==============================================================================
*/

static void TestAdd (void)
{
	qc_asm_t	*a = QA_New ();
	uint8_t		sizes[2] = {1, 1};
	qa_func_t	f = QA_Function (a, "add", sizes, 2, 1);
	host_t		h = {0};
	qcvm_t		*vm;
	qc_value_t	args[2] = {QC_ValFloat (2.5f), QC_ValFloat (4)};
	float		r = 0;

	QA_Emit (a, QOP_ADD_F, QA_Local (f, 0), QA_Local (f, 1), QA_Local (f, 2));
	QA_Emit (a, QOP_RETURN, QA_Local (f, 2), 0, 0);
	vm = MakeVM (a, NULL, &h);
	QT_CHECK (CallF (vm, "add", 2, args, &r) && r == 6.5f);
	QC_Destroy (vm);
	QA_Free (a);
}

// fib (n) = n < 2 ? n : fib (n-1) + fib (n-2): locals across recursion
static void TestRecursion (void)
{
	static const float	cases[][2] = {{0, 0}, {1, 1}, {2, 1}, {10, 55}, {20, 6765}};
	qc_asm_t	*a = QA_New ();
	uint32_t	two = QA_Float (a, 2), one = QA_Float (a, 1), fib_g, n, t, acc, br, rec;
	uint8_t		size = 1;
	qa_func_t	f;
	host_t		h = {0};
	qcvm_t		*vm;
	qc_value_t	arg;
	float		r;
	size_t		i;

	fib_g = QA_Global1 (a, "fib_ref", QC_EV_FUNCTION, 0);
	f = QA_Function (a, "fib", &size, 1, 3);
	n = QA_Local (f, 0);
	t = QA_Local (f, 1);
	acc = QA_Local (f, 2);
	QA_Emit (a, QOP_LT_F, n, two, t);
	br = QA_Emit (a, QOP_IFNOT_I, t, 0, 0);
	QA_Emit (a, QOP_RETURN, n, 0, 0);
	rec = QA_Here (a);
	QA_PatchJump (a, br, 1, rec);
	QA_Emit (a, QOP_SUB_F, n, one, QA_PARM (0));
	QA_Emit (a, QOP_CALL1, fib_g, 0, 0);
	QA_Emit (a, QOP_STORE_F, QA_OFS_RETURN, acc, 0);
	QA_Emit (a, QOP_SUB_F, n, two, QA_PARM (0));
	QA_Emit (a, QOP_CALL1, fib_g, 0, 0);
	QA_Emit (a, QOP_ADD_F, acc, QA_OFS_RETURN, acc);
	QA_Emit (a, QOP_RETURN, acc, 0, 0);
	QA_SetGlobal (a, fib_g, f.index);
	vm = MakeVM (a, NULL, &h);
	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
	{
		arg = QC_ValFloat (cases[i][0]);
		r = -1;
		if (!QT_CHECK (CallF (vm, "fib", 1, &arg, &r) && r == cases[i][1]))
			printf ("  fib(%g) = %g\n", (double)cases[i][0], (double)r);
	}
	QC_Destroy (vm);
	QA_Free (a);
}

static void TestBuiltinBinding (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = QC_BuiltinsCreate (QC_NUMBERING_NONE);
	uint32_t		log, named, missing, log_g, named_g, missing_g, one, two;
	qc_unbound_t	unbound[4];
	host_t			h = {0};
	qcvm_t			*vm;
	float			r = 0;

	log = QA_Builtin (a, "log", 7, 2);
	named = QA_Builtin (a, "named_log", 0, 1);
	missing = QA_Builtin (a, "missing", 99, 0);
	log_g = QA_Global1 (a, "log_g", QC_EV_FUNCTION, log);
	named_g = QA_Global1 (a, "named_g", QC_EV_FUNCTION, named);
	missing_g = QA_Global1 (a, "missing_g", QC_EV_FUNCTION, missing);
	one = QA_Float (a, 1);
	two = QA_Float (a, 2);
	QA_Function (a, "main", NULL, 0, 0);
	QA_Emit (a, QOP_STORE_F, one, QA_PARM (0), 0);
	QA_Emit (a, QOP_STORE_F, two, QA_PARM (1), 0);
	QA_Emit (a, QOP_CALL2, log_g, 0, 0);
	QA_Emit (a, QOP_STORE_F, QA_OFS_RETURN, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, named_g, 0, 0);
	QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
	QA_Function (a, "calls_missing", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, missing_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);

	QC_BuiltinsSetNumbered (b, 7, "log", B_Log);
	QC_BuiltinsSet (b, "named_log", B_Log);
	vm = MakeVM (a, b, &h);
	QT_EQ_U (QC_UnboundBuiltins (vm, false, unbound, 4), 1);
	QT_EQ_S (unbound[0].name, "missing");
	QT_EQ_U (unbound[0].number, 99);

	QT_CHECK (CallF (vm, "main", 0, NULL, &r) && r == 100);
	QT_EQ_S (h.log, "log(1,2) log(10) ");

	QT_EQ_U (CallError (vm, "calls_missing"), QC_ERR_BUILTIN_NOT_IMPLEMENTED);
	QT_EQ_I (QC_LastError (vm)->value, 99);
	QT_EQ_S (QC_LastError (vm)->message, "missing");
	QT_CHECK (QC_LastError (vm)->backtrace.count && !strcmp (QC_LastError (vm)->backtrace.frames[0].name, "calls_missing"));
	// still usable
	QT_CHECK (CallF (vm, "main", 0, NULL, &r) && r == 100);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static void TestCallback (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (1, "callback", B_Callback);
	uint32_t		cb, cb_g, three, y, z;
	uint8_t			one = 1;
	qa_func_t		inner, outer;
	host_t			h = {0};
	qcvm_t			*vm;
	qc_value_t		arg = QC_ValFloat (4);
	float			r = 0;

	cb = QA_Builtin (a, "callback", 1, 1);
	cb_g = QA_Global1 (a, "cb_g", QC_EV_FUNCTION, cb);
	three = QA_Float (a, 3);
	// inner (x) = x * x
	inner = QA_Function (a, "inner", &one, 1, 1);
	QA_Emit (a, QOP_MUL_F, QA_Local (inner, 0), QA_Local (inner, 0), QA_Local (inner, 1));
	QA_Emit (a, QOP_RETURN, QA_Local (inner, 1), 0, 0);
	// outer (y) { local z = y + 3; r = callback (y); return r + z; }: z survives the callback
	outer = QA_Function (a, "outer", &one, 1, 2);
	y = QA_Local (outer, 0);
	z = QA_Local (outer, 1);
	QA_Emit (a, QOP_ADD_F, y, three, z);
	QA_Emit (a, QOP_STORE_F, y, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, cb_g, 0, 0);
	QA_Emit (a, QOP_ADD_F, QA_OFS_RETURN, z, QA_Local (outer, 2));
	QA_Emit (a, QOP_RETURN, QA_Local (outer, 2), 0, 0);
	vm = MakeVM (a, b, &h);
	h.callback = Func (vm, "inner");
	// outer (4): z = 7; callback (4) = inner (5) * 2 = 50; 50 + 7 = 57
	QT_CHECK (CallF (vm, "outer", 1, &arg, &r) && r == 57);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static void TestErrors (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (1, "fail", B_Fail);
	uint32_t		fail, fail_g, seven, inner_g, v;
	qa_func_t		inner;
	host_t			h = {0};
	qcvm_t			*vm;
	const qc_error_t	*e;
	int				i;
	bool			found = false;

	fail = QA_Builtin (a, "fail", 1, 0);
	fail_g = QA_Global1 (a, "fail_g", QC_EV_FUNCTION, fail);
	seven = QA_Float (a, 7);
	inner = QA_Function (a, "inner", NULL, 0, 1);
	QA_Emit (a, QOP_STORE_F, seven, QA_Local (inner, 0), 0);
	QA_Emit (a, QOP_CALL0, fail_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	inner_g = QA_Global1 (a, "inner_g", QC_EV_FUNCTION, inner.index);
	QA_Function (a, "outer", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, inner_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = MakeVM (a, b, &h);

	QT_EQ_U (CallError (vm, "outer"), QC_ERR_BUILTIN);
	e = QC_LastError (vm);
	QT_EQ_S (e->message, "boom");
	QT_CHECK (e->backtrace.count == 2 && !strcmp (e->backtrace.frames[0].name, "inner")
		&& !strcmp (e->backtrace.frames[1].name, "outer"));
	// locals were restored: inner's local holds its value from before the call
	v = QC_Globals (vm)[QA_Local (inner, 0)].u;
	QT_EQ_U (v, 0);

	// in developer mode the builtin error is a warning, and the call completes
	QC_SetDeveloper (vm, true);
	QT_CHECK (QC_Call (vm, Func (vm, "outer"), 0, NULL, NULL));
	for (i = 0 ; i < h.numwarnings ; i++)
		found |= QT_Contains (h.warnings[i], "boom");
	QT_CHECK (found);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static void TestRunawayAndDepth (void)
{
	qc_asm_t	*a = QA_New ();
	qc_config_t	config;
	uint32_t	l, deep_g;
	qa_func_t	deep;
	qc_progs_t	*p;
	host_t		h = {0};
	qcvm_t		*vm;

	QA_Function (a, "spin", NULL, 0, 0);
	l = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	QA_PatchJump (a, l, 0, l);
	deep_g = QA_Global1 (a, "deep_g", QC_EV_FUNCTION, 0);
	deep = QA_Function (a, "deep", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, deep_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_SetGlobal (a, deep_g, deep.index);

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.runaway = 5000;
	p = QA_Load (a, QC_FORMAT_V6);
	vm = QC_Create (p, NULL, &config, &test_host, &h, NULL);
	QC_ReleaseProgs (p);
	QT_EQ_U (CallError (vm, "spin"), QC_ERR_RUNAWAY);
	QT_EQ_U (CallError (vm, "deep"), QC_ERR_CALL_DEPTH);
	QT_EQ_U (QC_LastError (vm)->backtrace.count, 1024);
	// still usable after unwinding 1024 frames
	QT_EQ_U (CallError (vm, "spin"), QC_ERR_RUNAWAY);
	QT_EQ_U (vm->numframes, 0);
	QC_Destroy (vm);
	QA_Free (a);
}

static void TestNullAndInvalid (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	bogus = QA_Int (a, 0x00012345);
	host_t		h = {0};
	qcvm_t		*vm;

	QA_Function (a, "null_call", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, 0, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_Function (a, "bad_call", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, bogus, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = MakeVM (a, NULL, &h);
	QT_EQ_U (CallError (vm, "null_call"), QC_ERR_NULL_FUNCTION);
	QT_EQ_U (CallError (vm, "bad_call"), QC_ERR_INVALID_FUNCTION);
	QT_EQ_I (QC_LastError (vm)->value, 0x00012345);
	QT_CHECK (!QC_Call (vm, 0x05000001, 0, NULL, NULL) && QC_LastError (vm)->kind == QC_ERR_INVALID_FUNCTION);
	QC_Destroy (vm);
	QA_Free (a);
}

/*
==============================================================================

ENTITIES AND STRINGS AS QUAKEC SEES THEM

==============================================================================
*/

static void TestEntitiesAndFields (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (14, "spawn", B_Spawn);
	uint32_t		spawn, spawn_g, health_g, origin_g, hundred, v, e, p, out, health, origin, w[3];
	qa_func_t		f;
	host_t			h = {0};
	qcvm_t			*vm;
	qc_value_t		ret;
	qc_ent_t		reused;

	spawn = QA_Builtin (a, "spawn", 14, 0);
	spawn_g = QA_Global1 (a, "spawn_g", QC_EV_FUNCTION, spawn);
	QA_Field (a, "health", QC_EV_FLOAT, &health_g);
	QA_Field (a, "origin", QC_EV_VECTOR, &origin_g);
	hundred = QA_Float (a, 100);
	v = QA_Vector (a, 1, 2, 3);
	f = QA_Function (a, "make", NULL, 0, 3);
	e = QA_Local (f, 0);
	p = QA_Local (f, 1);
	out = QA_Local (f, 2);
	QA_Emit (a, QOP_CALL0, spawn_g, 0, 0);
	QA_Emit (a, QOP_STORE_ENT, QA_OFS_RETURN, e, 0);
	QA_Emit (a, QOP_ADDRESS, e, health_g, p);
	QA_Emit (a, QOP_STOREP_F, hundred, p, 0);
	QA_Emit (a, QOP_STOREF_V, e, origin_g, v);
	QA_Emit (a, QOP_LOAD_F, e, health_g, out);
	QA_Emit (a, QOP_ADD_F, out, out, out);
	QA_Emit (a, QOP_ADDRESS, e, health_g, p);
	QA_Emit (a, QOP_STOREP_F, out, p, 0);
	QA_Emit (a, QOP_RETURN, e, 0, 0);
	vm = MakeVM (a, b, &h);

	QT_CHECK (QC_Call (vm, Func (vm, "make"), 0, NULL, &ret) && ret.w[0] == 1);
	QC_FindField (vm, "health", &health, NULL);
	QC_FindField (vm, "origin", &origin, NULL);
	QT_CHECK (QC_GetField (vm, 1, health, 1, w) && QC_BitsFloat (w[0]) == 200);
	QT_CHECK (QC_GetField (vm, 1, origin, 3, w) && QC_BitsFloat (w[0]) == 1 && QC_BitsFloat (w[2]) == 3);

	// a protected entity keeps its fields (make works on a new one)
	QC_SetProtected (vm, 1, true);
	QT_CHECK (QC_Call (vm, Func (vm, "make"), 0, NULL, NULL));
	QT_CHECK (QC_GetField (vm, 1, health, 1, w) && QC_BitsFloat (w[0]) == 200);
	QC_SetProtected (vm, 1, false);

	// the slot is reused half a second after it was freed
	QC_SetTime (vm, 10);
	QT_CHECK (QC_Remove (vm, 1, false));
	QC_SetTime (vm, 10.2);
	QT_CHECK (QC_Spawn (vm, &reused) && reused == 3);
	QC_SetTime (vm, 10.6);
	QT_CHECK (QC_Spawn (vm, &reused) && reused == 1);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// hosts may pass eight arguments at most; QC_CallAs sets and restores self
static void TestArgumentsAndSelf (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	self_g;
	host_t		h = {0};
	qcvm_t		*vm;
	qc_value_t	nine[9] = {{{0}}}, ret;
	qc_ent_t	e;

	self_g = QA_Global1 (a, "self", QC_EV_ENTITY, 0);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Function (a, "whoami", NULL, 0, 0);
	QA_Emit (a, QOP_RETURN, self_g, 0, 0);
	vm = MakeVM (a, NULL, &h);
	QT_CHECK (!QC_Call (vm, Func (vm, "whoami"), 9, nine, NULL));
	QT_CHECK (QC_LastError (vm)->kind == QC_ERR_TOO_MANY_ARGUMENTS && QC_LastError (vm)->value == 9);
	QT_CHECK (QC_Call (vm, Func (vm, "whoami"), 8, nine, NULL));
	QT_CHECK (QC_Spawn (vm, &e));
	QT_CHECK (QC_CallAs (vm, e, Func (vm, "whoami"), 0, NULL, &ret) && ret.w[0] == e);
	QT_EQ_U (QC_Globals (vm)[self_g].u, 0);		// restored
	QC_Destroy (vm);
	QA_Free (a);
}

static void TestReachableUnbound (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (8, "bound", B_Log);
	uint32_t		called, bound, called_g, bound_g;
	qc_unbound_t	u[4];
	host_t			h = {0};
	qcvm_t			*vm;

	called = QA_Builtin (a, "called", 7, 0);
	bound = QA_Builtin (a, "bound", 8, 0);
	QA_Builtin (a, "declared", 9, 0);
	called_g = QA_Global1 (a, "called_g", QC_EV_FUNCTION, called);
	bound_g = QA_Global1 (a, "bound_g", QC_EV_FUNCTION, bound);
	QA_Function (a, "main", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, called_g, 0, 0);
	QA_Emit (a, QOP_CALL0, bound_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = MakeVM (a, b, &h);
	QT_EQ_U (QC_UnboundBuiltins (vm, false, u, 4), 2);
	QT_CHECK (!strcmp (u[0].name, "called") && !strcmp (u[1].name, "declared"));
	QT_EQ_U (QC_UnboundBuiltins (vm, true, u, 4), 1);
	QT_EQ_S (u[0].name, "called");
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static void TestBadEntityAccess (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	health_g, big;
	qa_func_t	f;
	host_t		h = {0};
	qcvm_t		*vm;
	qc_value_t	ret;

	QA_Field (a, "health", QC_EV_FLOAT, &health_g);
	big = QA_Int (a, 9999);
	f = QA_Function (a, "peek", NULL, 0, 1);
	QA_Emit (a, QOP_LOAD_F, big, health_g, QA_Local (f, 0));
	QA_Emit (a, QOP_RETURN, QA_Local (f, 0), 0, 0);
	vm = MakeVM (a, NULL, &h);
	QT_CHECK (QC_Call (vm, Func (vm, "peek"), 0, NULL, &ret) && ret.w[0] == 0);
	QT_EQ_I (h.numwarnings, 1);
	QT_CHECK (h.warnkind[0] == QC_WARN_BAD_ENTITY && h.warnvalue[0] == 9999);
	QT_EQ_S (h.warnframe[0], "peek");
	QC_Destroy (vm);
	QA_Free (a);
}

static void TestTempStrings (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (115, "strcat", B_Concat);
	uint32_t		cat, cat_g, hello, world, hw, kept;
	qa_func_t		f;
	host_t			h = {0};
	qcvm_t			*vm;
	float			r = 0;
	int				i;
	qc_str_t		k;

	cat = QA_Builtin (a, "strcat", 115, 8);
	cat_g = QA_Global1 (a, "cat_g", QC_EV_FUNCTION, cat);
	hello = QA_StrConst (a, "hello");
	world = QA_StrConst (a, " world");
	hw = QA_StrConst (a, "hello world");
	kept = QA_Global1 (a, "kept", QC_EV_STRING, 0);
	f = QA_Function (a, "go", NULL, 0, 1);
	QA_Emit (a, QOP_STORE_S, hello, QA_PARM (0), 0);
	QA_Emit (a, QOP_STORE_S, world, QA_PARM (1), 0);
	QA_Emit (a, QOP_CALL2, cat_g, 0, 0);
	QA_Emit (a, QOP_STORE_S, QA_OFS_RETURN, kept, 0);
	QA_Emit (a, QOP_EQ_S, kept, hw, QA_Local (f, 0));
	QA_Emit (a, QOP_RETURN, QA_Local (f, 0), 0, 0);
	// a temp string nothing keeps
	QA_Function (a, "garbage", NULL, 0, 0);
	QA_Emit (a, QOP_STORE_S, hello, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, cat_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = MakeVM (a, b, &h);

	QT_CHECK (CallF (vm, "go", 0, NULL, &r) && r == 1);
	k = QC_Globals (vm)[kept].u;
	QT_CHECK (QC_IsTempString (k));
	QT_EQ_S (QC_String (vm, k), "hello world");
	for (i = 0 ; i < 3000 ; i++)
		QT_CHECK (QC_Call (vm, Func (vm, "garbage"), 0, NULL, NULL));
	// collections ran along the way; the referenced string survived
	QT_CHECK (QC_NumTempStrings (vm) < 3000);
	QC_CollectGarbage (vm);
	QT_EQ_U (QC_NumTempStrings (vm), 1);
	QT_EQ_S (QC_String (vm, k), "hello world");
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

/*
==============================================================================

ANIMATION, RE-ENTRY, ABORT, TRACING

==============================================================================
*/

typedef struct
{
	host_t	h;
	int		calls;
	bool	fail;
} stateop_host_t;

static bool StateOpOverride (void *ctx, qcvm_t *vm, const qc_stateop_t *op, bool *handled)
{
	stateop_host_t	*s = ctx;

	(void)op;
	s->calls++;
	if (s->fail)
		return QC_HostError (vm, "no animation here");
	*handled = true;
	return true;
}

static void TestStateOpcode (void)
{
	qc_asm_t		*a = QA_New ();
	uint32_t		think_g, five, ofs, w;
	qa_func_t		think_fn;
	stateop_host_t	s = {0};
	qc_host_t		host = test_host;
	qcvm_t			*vm;
	qc_ent_t		e;
	uint32_t		self_w, time_w;

	QA_Global1 (a, "self", QC_EV_ENTITY, 0);
	QA_Global1 (a, "time", QC_EV_FLOAT, 0);
	QA_Field (a, "frame", QC_EV_FLOAT, NULL);
	QA_Field (a, "think", QC_EV_FUNCTION, NULL);
	QA_Field (a, "nextthink", QC_EV_FLOAT, NULL);
	five = QA_Float (a, 5);
	think_fn = QA_Function (a, "thinker", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	think_g = QA_Global1 (a, "thinker_g", QC_EV_FUNCTION, think_fn.index);
	QA_Function (a, "animate", NULL, 0, 0);
	QA_Emit (a, QOP_STATE, five, think_g, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);

	// FTE's default
	vm = MakeVM (a, NULL, &s.h);
	QT_CHECK (QC_Spawn (vm, &e));
	QC_FindGlobal (vm, "self", &self_w, NULL);
	QC_FindGlobal (vm, "time", &time_w, NULL);
	QC_Globals (vm)[self_w].u = e;
	QC_Globals (vm)[time_w].f = 10;
	QT_CHECK (QC_Call (vm, Func (vm, "animate"), 0, NULL, NULL));
	QC_FindField (vm, "frame", &ofs, NULL);
	QT_CHECK (QC_GetField (vm, e, ofs, 1, &w) && QC_BitsFloat (w) == 5);
	QC_FindField (vm, "nextthink", &ofs, NULL);
	QT_CHECK (QC_GetField (vm, e, ofs, 1, &w) && QC_BitsFloat (w) == 10.1f);
	QC_FindField (vm, "think", &ofs, NULL);
	QT_CHECK (QC_GetField (vm, e, ofs, 1, &w) && w == think_fn.index);
	QC_Destroy (vm);

	// the host's own, and its errors
	host.state_op = StateOpOverride;
	vm = QA_CreateVM (a, NULL, NULL, &host, &s);
	QT_CHECK (QC_Spawn (vm, &e));
	QC_FindGlobal (vm, "self", &self_w, NULL);
	QC_Globals (vm)[self_w].u = e;
	QT_CHECK (QC_Call (vm, Func (vm, "animate"), 0, NULL, NULL));
	QC_FindField (vm, "frame", &ofs, NULL);
	QT_CHECK (QC_GetField (vm, e, ofs, 1, &w) && w == 0);
	s.fail = true;
	QT_CHECK (!QC_Call (vm, Func (vm, "animate"), 0, NULL, NULL));
	QT_CHECK (QC_LastError (vm)->kind == QC_ERR_HOST && !strcmp (QC_LastError (vm)->message, "no animation here"));
	QT_EQ_I (s.calls, 2);
	QC_Destroy (vm);
	QA_Free (a);
}

static void TestReentrancyLimit (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (1, "callback", B_Callback);
	uint32_t		cb, cb_g;
	uint8_t			one = 1;
	qa_func_t		f;
	host_t			h = {0};
	qcvm_t			*vm;
	qc_value_t		arg = QC_ValFloat (0);

	cb = QA_Builtin (a, "callback", 1, 1);
	cb_g = QA_Global1 (a, "cb_g", QC_EV_FUNCTION, cb);
	f = QA_Function (a, "recurse", &one, 1, 0);
	QA_Emit (a, QOP_STORE_F, QA_Local (f, 0), QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, cb_g, 0, 0);
	QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
	vm = MakeVM (a, b, &h);
	h.callback = Func (vm, "recurse");
	QT_CHECK (!QC_Call (vm, Func (vm, "recurse"), 1, &arg, NULL) && QC_LastError (vm)->kind == QC_ERR_REENTRANCY);
	// fully unwound: it happens again the same way
	QT_CHECK (!QC_Call (vm, Func (vm, "recurse"), 1, &arg, NULL) && QC_LastError (vm)->kind == QC_ERR_REENTRANCY);
	QT_CHECK (vm->numframes == 0 && vm->nesting == 0);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static void TestAbort (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (211, "abort", B_Abort);
	uint32_t		ab, ab_g, forty2, seven, inner_g;
	qa_func_t		inner;
	host_t			h = {0};
	qcvm_t			*vm;
	float			r = 0;

	ab = QA_Builtin (a, "abort", 211, 1);
	ab_g = QA_Global1 (a, "ab_g", QC_EV_FUNCTION, ab);
	forty2 = QA_Float (a, 42);
	seven = QA_Float (a, 7);
	inner = QA_Function (a, "inner", NULL, 0, 1);
	QA_Emit (a, QOP_STORE_F, seven, QA_Local (inner, 0), 0);
	QA_Emit (a, QOP_STORE_F, forty2, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, ab_g, 0, 0);
	QA_Emit (a, QOP_RETURN, seven, 0, 0);
	inner_g = QA_Global1 (a, "inner_g", QC_EV_FUNCTION, inner.index);
	QA_Function (a, "outer", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, inner_g, 0, 0);
	QA_Emit (a, QOP_RETURN, seven, 0, 0);
	vm = MakeVM (a, b, &h);
	QT_CHECK (CallF (vm, "outer", 0, NULL, &r) && r == 42);
	QT_EQ_U (vm->numframes, 0);
	QT_EQ_U (QC_Globals (vm)[QA_Local (inner, 0)].u, 0);		// the local was restored
	QT_CHECK (CallF (vm, "outer", 0, NULL, &r) && r == 42);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// traceon and traceoff report every statement between, also of called functions
static void TestTrace (void)
{
	static const char	*want[5] = {"main ADD_F", "main CALL0", "helper MUL_F", "helper RETURN", "main CALL0"};
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = QC_BuiltinsCreate (QC_NUMBERING_NONE);
	uint32_t		on, off, on_g, off_g, one, two, helper_g;
	qa_func_t		helper, f;
	host_t			h = {0};
	qcvm_t			*vm;
	char			got[96], func[64], op[32];
	int				i;

	QC_BuiltinsSetNumbered (b, 29, "traceon", B_TraceOn);
	QC_BuiltinsSetNumbered (b, 30, "traceoff", B_TraceOff);
	on = QA_Builtin (a, "traceon", 29, 0);
	off = QA_Builtin (a, "traceoff", 30, 0);
	on_g = QA_Global1 (a, "on_g", QC_EV_FUNCTION, on);
	off_g = QA_Global1 (a, "off_g", QC_EV_FUNCTION, off);
	one = QA_Float (a, 1);
	two = QA_Float (a, 2);
	helper = QA_Function (a, "helper", NULL, 0, 1);
	QA_Emit (a, QOP_MUL_F, two, two, QA_Local (helper, 0));
	QA_Emit (a, QOP_RETURN, QA_Local (helper, 0), 0, 0);
	helper_g = QA_Global1 (a, "helper_g", QC_EV_FUNCTION, helper.index);
	f = QA_Function (a, "main", NULL, 0, 1);
	QA_Emit (a, QOP_ADD_F, one, one, QA_Local (f, 0));
	QA_Emit (a, QOP_CALL0, on_g, 0, 0);
	QA_Emit (a, QOP_ADD_F, one, two, QA_Local (f, 0));
	QA_Emit (a, QOP_CALL0, helper_g, 0, 0);
	QA_Emit (a, QOP_CALL0, off_g, 0, 0);
	QA_Emit (a, QOP_SUB_F, one, two, QA_Local (f, 0));
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = MakeVM (a, b, &h);
	QT_CHECK (QC_Call (vm, Func (vm, "main"), 0, NULL, NULL));
	QT_EQ_I (h.numtrace, 5);
	for (i = 0 ; i < 5 && i < h.numtrace ; i++)
	{
		// "func:  index: OP operands"
		func[0] = op[0] = 0;
		sscanf (h.trace[i], "%63[^:]: %*u: %31s", func, op);
		snprintf (got, sizeof(got), "%s %s", func, op);
		if (!QT_EQ_S (got, want[i]))
			printf ("  %s\n", h.trace[i]);
	}
	QT_CHECK (!QC_IsTracing (vm));
	QC_SetTrace (vm, true);
	QT_CHECK (QC_Call (vm, Func (vm, "main"), 0, NULL, NULL));
	QT_CHECK (h.numtrace > 5 && !strncmp (h.trace[5], "main:", 5));
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// a builtin that longjmps out (as a host's error handling might): the host
// abandons the call, and the VM is as if nothing ran
static void TestAbandon (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (1, "escape", B_Escape);
	uint32_t		esc, esc_g, seven, inner_g;
	qa_func_t		inner;
	host_t			h = {0};
	qcvm_t			*vm;
	jmp_buf			jb;
	volatile bool	jumped = false;
	float			r = 0;

	esc = QA_Builtin (a, "escape", 1, 0);
	esc_g = QA_Global1 (a, "esc_g", QC_EV_FUNCTION, esc);
	seven = QA_Float (a, 7);
	inner = QA_Function (a, "inner", NULL, 0, 1);
	QA_Emit (a, QOP_STORE_F, seven, QA_Local (inner, 0), 0);
	QA_Emit (a, QOP_CALL0, esc_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	inner_g = QA_Global1 (a, "inner_g", QC_EV_FUNCTION, inner.index);
	QA_Function (a, "outer", NULL, 0, 0);
	QA_Emit (a, QOP_CALL0, inner_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_Function (a, "fine", NULL, 0, 0);
	QA_Emit (a, QOP_RETURN, seven, 0, 0);
	vm = MakeVM (a, b, &h);
	h.escape = &jb;
	if (!setjmp (jb))
		QC_Call (vm, Func (vm, "outer"), 0, NULL, NULL);
	else
		jumped = true;
	QT_CHECK (jumped);
	QT_CHECK (vm->numframes == 2 && vm->nesting == 1);
	QC_Abandon (vm);
	QT_CHECK (vm->numframes == 0 && vm->nesting == 0 && vm->x.func == QC_NO_FUNCTION);
	QT_EQ_U (QC_Globals (vm)[QA_Local (inner, 0)].u, 0);
	QT_CHECK (CallF (vm, "fine", 0, NULL, &r) && r == 7);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// vector copies through overlapping pointers behave as if through a temporary
static void TestOverlappingVectors (void)
{
	static const struct
	{
		const char	*name;
		qc_format_t	format;
	} cases[2] = {{"store", QC_FORMAT_FTE16}, {"load", QC_FORMAT_FTE32}};
	qc_asm_t	*a = QA_New ();
	uint32_t	block, zero, one, w[4], word;
	qa_func_t	f, g;
	host_t		h = {0};
	qcvm_t		*vm;
	int			i, k;

	block = QA_Alloc (a, 4, (const uint32_t[]){QC_FloatBits (1), QC_FloatBits (2), QC_FloatBits (3),
		QC_FloatBits (4)}, 4);
	zero = QA_Int (a, 0);
	one = QA_Int (a, 1);
	// STOREP_V: the first three words onto the last three
	f = QA_Function (a, "store", NULL, 0, 1);
	QA_Emit (a, QOP_GLOBALADDRESS, block, one, QA_Local (f, 0));
	QA_Emit (a, QOP_STOREP_V, block, QA_Local (f, 0), 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	// LOADP_V: through a pointer to the first word, onto the second
	g = QA_Function (a, "load", NULL, 0, 1);
	QA_Emit (a, QOP_GLOBALADDRESS, block, zero, QA_Local (g, 0));
	QA_Emit (a, QOP_LOADP_V, QA_Local (g, 0), zero, block + 1);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_DefGlobal (a, "block", QC_EV_FLOAT, block);
	for (i = 0 ; i < 2 ; i++)
	{
		vm = QA_CreateVMAs (a, cases[i].format, NULL, NULL, &test_host, &h);
		QT_CHECK (QC_Call (vm, Func (vm, cases[i].name), 0, NULL, NULL));
		QC_FindGlobal (vm, "block", &word, NULL);
		for (k = 0 ; k < 4 ; k++)
			QT_CHECK (QC_ReadMemory (vm, vm->progs[0].gbase + (word + (uint32_t)k) * 4, &w[k], 4));
		if (!QT_CHECK (QC_BitsFloat (w[0]) == 1 && QC_BitsFloat (w[1]) == 1 && QC_BitsFloat (w[2]) == 2
			&& QC_BitsFloat (w[3]) == 3))
			printf ("  %s\n", cases[i].name);
		QC_Destroy (vm);
	}
	QA_Free (a);
}

/*
==============================================================================

BUDGETS

==============================================================================
*/

// gotos jumps to the next statement and a DONE: gotos + 1 counted instructions
static void StraightLine (qc_asm_t *a, const char *name, uint32_t gotos)
{
	uint32_t	i, at;

	QA_Function (a, name, NULL, 0, 0);
	for (i = 0 ; i < gotos ; i++)
	{
		at = QA_Emit (a, QOP_GOTO, 0, 0, 0);
		QA_PatchJump (a, at, 0, at + 1);
	}
	QA_Emit (a, QOP_DONE, 0, 0, 0);
}

// the runaway budget is exact across the chunks the interpreter runs in, and
// calls builtins make back into QuakeC share their host call's
static void TestRunawayBudget (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (1, "run_callback", B_RunCallback);
	qc_config_t		config;
	uint32_t		rb, rb_g, i;
	qc_progs_t		*p;
	host_t			h = {0};
	qcvm_t			*vm;

	StraightLine (a, "fits", 69998);
	StraightLine (a, "too_long", 69999);
	StraightLine (a, "work", 999);
	rb = QA_Builtin (a, "run_callback", 1, 0);
	rb_g = QA_Global1 (a, "run_callback_g", QC_EV_FUNCTION, rb);
	QA_Function (a, "many", NULL, 0, 0);
	for (i = 0 ; i < 10 ; i++)
		QA_Emit (a, QOP_CALL0, rb_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	p = QA_Load (a, QC_FORMAT_FTE32);
	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.runaway = 70000;

	// 69,999 counted instructions fit a budget of 70,000; the 70,000th doesn't
	vm = QC_Create (p, b, &config, &test_host, &h, NULL);
	QT_EQ_U (CallError (vm, "fits"), QC_ERR_NONE);
	QT_EQ_U (CallError (vm, "too_long"), QC_ERR_RUNAWAY);
	// ten nested runs of 1,000 fit
	h.callback = Func (vm, "work");
	QT_EQ_U (CallError (vm, "many"), QC_ERR_NONE);
	QC_Destroy (vm);

	// with a deadline the budget comes in chunks of 65,536, and stays exact
	config.limits.deadline = 3600;
	vm = QC_Create (p, b, &config, &test_host, &h, NULL);
	QT_EQ_U (CallError (vm, "fits"), QC_ERR_NONE);
	QT_EQ_U (CallError (vm, "too_long"), QC_ERR_RUNAWAY);
	QC_Destroy (vm);

	// ten nested runs of 1,000 exceed 5,000 between them
	config.limits.deadline = 0;
	config.limits.runaway = 5000;
	vm = QC_Create (p, b, &config, &test_host, &h, NULL);
	h.callback = Func (vm, "work");
	QT_EQ_U (CallError (vm, "many"), QC_ERR_RUNAWAY);
	// the next host call starts with a full budget again
	QT_EQ_U (CallError (vm, "work"), QC_ERR_NONE);
	QC_Destroy (vm);
	QC_ReleaseProgs (p);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static double Seconds (void)
{
	struct timespec	ts;

	timespec_get (&ts, TIME_UTC);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void TestDeadline (void)
{
	qc_asm_t	*a = QA_New ();
	qc_config_t	config;
	uint32_t	l;
	host_t		h = {0};
	qcvm_t		*vm;
	double		start, took;

	QA_Function (a, "spin", NULL, 0, 0);
	l = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	QA_PatchJump (a, l, 0, l);
	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.runaway = UINT32_MAX;
	config.limits.deadline = 0.05;
	vm = QA_CreateVM (a, &config, NULL, &test_host, &h);
	start = Seconds ();
	QT_EQ_U (CallError (vm, "spin"), QC_ERR_DEADLINE);
	took = Seconds () - start;
	// a millisecond less: a clock of whole milliseconds (the browser's) ends the
	// wait in the deadline's, which the seconds since 1970 may round below it
	QT_CHECK (took >= 0.049 && took < 5);
	QC_Destroy (vm);
	QA_Free (a);
}

/*
==============================================================================

HOOKS AND BIG PROGRAMS

==============================================================================
*/

static void HookSpawn (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	host_t	*h = ctx;

	if (h->numspawned < 8)
	{
		h->spawned[h->numspawned][0] = (int)e;
		h->spawned[h->numspawned][1] = (int)QC_Serial (vm, e);
		h->numspawned++;
	}
}

static bool HookRemove (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	host_t		*h = ctx;
	uint32_t	ofs, w = 0;

	QC_FindField (vm, "health", &ofs, NULL);
	QC_GetField (vm, e, ofs, 1, &w);
	if (h->numremoved < 8)
	{
		h->removed[h->numremoved] = (int)e;
		h->removedhealth[h->numremoved] = QC_BitsFloat (w);
		h->numremoved++;
	}
	return true;
}

// the hooks see QuakeC's spawns and removals (with the fields intact), and not
// the removals refused
static void TestHooks (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = QC_BuiltinsCreate (QC_NUMBERING_NONE);
	qc_host_t		host = {.on_spawn = HookSpawn, .on_remove = HookRemove};
	uint32_t		health_g, spawn, remove, spawn_g, remove_g, seven, e, p;
	qa_func_t		f;
	host_t			h = {0};
	qcvm_t			*vm;
	qc_ent_t		own;

	QC_BuiltinsSetNumbered (b, 14, "spawn", B_Spawn);
	QC_BuiltinsSetNumbered (b, 15, "remove", B_Remove);
	QA_Field (a, "health", QC_EV_FLOAT, &health_g);
	spawn = QA_Builtin (a, "spawn", 14, 0);
	remove = QA_Builtin (a, "remove", 15, 1);
	spawn_g = QA_Global1 (a, "spawn_g", QC_EV_FUNCTION, spawn);
	remove_g = QA_Global1 (a, "remove_g", QC_EV_FUNCTION, remove);
	seven = QA_Float (a, 7);
	f = QA_Function (a, "churn", NULL, 0, 2);
	e = QA_Local (f, 0);
	p = QA_Local (f, 1);
	QA_Emit (a, QOP_CALL0, spawn_g, 0, 0);
	QA_Emit (a, QOP_STORE_ENT, QA_OFS_RETURN, e, 0);
	QA_Emit (a, QOP_ADDRESS, e, health_g, p);
	QA_Emit (a, QOP_STOREP_F, seven, p, 0);
	QA_Emit (a, QOP_STORE_ENT, e, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, remove_g, 0, 0);
	QA_Emit (a, QOP_STORE_ENT, e, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, remove_g, 0, 0);		// already free: refused
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = QA_CreateVM (a, NULL, b, &host, &h);
	QT_CHECK (QC_Spawn (vm, &own) && own == 1);
	QT_CHECK (QC_Call (vm, Func (vm, "churn"), 0, NULL, NULL));
	QT_CHECK (QC_Remove (vm, own, true));
	// the host's own spawns and removals go through the hooks too
	QT_EQ_I (h.numspawned, 2);
	QT_CHECK (h.spawned[0][0] == 1 && h.spawned[1][0] == 2 && h.spawned[1][1] == 1);
	QT_EQ_I (h.numremoved, 2);
	QT_CHECK (h.removed[0] == 2 && h.removedhealth[0] == 7 && h.removed[1] == 1);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// a program with more statements than 16 bits of jump reach, and one with
// globals past 4 MiB
static void TestBigPrograms (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	one = QA_Float (a, 1), two = QA_Float (a, 2), t, skip, end, i;
	qa_func_t	f;
	host_t		h = {0};
	qcvm_t		*vm;
	float		r = 0;

	f = QA_Function (a, "far_code", NULL, 0, 1);
	t = QA_Local (f, 0);
	skip = QA_Emit (a, QOP_GOTO, 0, 0, 0);
	for (i = 0 ; i < 70000 ; i++)
		QA_Emit (a, QOP_ADD_F, t, one, t);
	end = QA_Here (a);
	QA_PatchJump (a, skip, 0, end);
	QA_Emit (a, QOP_ADD_F, one, two, t);
	QA_Emit (a, QOP_RETURN, t, 0, 0);
	vm = QA_CreateVMAs (a, QC_FORMAT_FTE32, NULL, NULL, &test_host, &h);
	QT_CHECK (CallF (vm, "far_code", 0, NULL, &r) && r == 3);
	QC_Destroy (vm);
	QA_Free (a);

	a = QA_New ();
	QA_Alloc (a, 1100000, NULL, 0);
	one = QA_Float (a, 1);
	two = QA_Float (a, 2);
	f = QA_Function (a, "far_globals", NULL, 0, 1);
	QA_Emit (a, QOP_ADD_F, one, two, QA_Local (f, 0));
	QA_Emit (a, QOP_RETURN, QA_Local (f, 0), 0, 0);
	vm = QA_CreateVMAs (a, QC_FORMAT_FTE32, NULL, NULL, &test_host, &h);
	QT_CHECK (CallF (vm, "far_globals", 0, NULL, &r) && r == 3);
	QC_Destroy (vm);
	QA_Free (a);
}

// the QuakeC function a builtin was called from
static bool B_Caller (qcvm_t *vm)
{
	host_t	*h = Host (vm);

	snprintf (h->log, sizeof(h->log), "%s", QC_CallerName (vm));
	return true;
}

// the statements each function ran, counted while profiling (tracing or not),
// and the caller's name as a builtin sees it
static void TestProfileAndCaller (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = Builtins1 (1, "caller", B_Caller);
	uint32_t		one, caller_g, helper_g, count = 0, mainf, helperf;
	qa_func_t		helper, f;
	host_t			h = {0};
	qcvm_t			*vm;
	const uint64_t	*counts;
	int				pass;

	one = QA_Float (a, 1);
	caller_g = QA_Global1 (a, "caller_g", QC_EV_FUNCTION, QA_Builtin (a, "caller", 1, 0));
	helper = QA_Function (a, "helper", NULL, 0, 1);
	QA_Emit (a, QOP_ADD_F, one, one, QA_Local (helper, 0));
	QA_Emit (a, QOP_CALL0, caller_g, 0, 0);
	QA_Emit (a, QOP_RETURN, QA_Local (helper, 0), 0, 0);
	helper_g = QA_Global1 (a, "helper_g", QC_EV_FUNCTION, helper.index);
	f = QA_Function (a, "main", NULL, 0, 1);
	QA_Emit (a, QOP_ADD_F, one, one, QA_Local (f, 0));
	QA_Emit (a, QOP_CALL0, helper_g, 0, 0);
	QA_Emit (a, QOP_CALL0, helper_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	vm = MakeVM (a, b, &h);
	mainf = QC_FUNC_INDEX (Func (vm, "main"));
	helperf = QC_FUNC_INDEX (Func (vm, "helper"));

	QT_EQ_S (QC_CallerName (vm), "");
	QT_CHECK (QC_Call (vm, Func (vm, "main"), 0, NULL, NULL));
	QT_EQ_S (h.log, "helper");
	QT_CHECK (QC_Profile (vm, 0, &count) == NULL);		// never counted
	QT_CHECK (QC_Profile (vm, 1, &count) == NULL);		// no such progs

	// main runs 4 statements, helper 3 each time; tracing changes nothing
	QC_SetProfiling (vm, true);
	for (pass = 0 ; pass < 2 ; pass++)
	{
		QC_SetTrace (vm, pass == 1);
		QT_CHECK (QC_Call (vm, Func (vm, "main"), 0, NULL, NULL));
		counts = QC_Profile (vm, 0, &count);
		if (QT_CHECK (counts != NULL))
		{
			QT_EQ_U (count, QC_ProgsNumFunctions (QC_MainProgs (vm)));
			QT_EQ_U (counts[mainf], 4);
			QT_EQ_U (counts[helperf], 6);
			QT_EQ_U (counts[0], 0);
		}
		QC_ClearProfile (vm);
		QT_EQ_U (counts ? counts[mainf] + counts[helperf] : 1, 0);
	}
	QC_SetTrace (vm, false);
	QT_EQ_S (h.log, "helper");

	// off: the counts stay as they are
	QC_SetProfiling (vm, false);
	QT_CHECK (QC_Call (vm, Func (vm, "main"), 0, NULL, NULL));
	counts = QC_Profile (vm, 0, &count);
	QT_CHECK (counts && counts[mainf] == 0 && counts[helperf] == 0);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// a host that reaches blocks it hasn't spawned into commits them first
static void TestCommitEdicts (void)
{
	qc_asm_t	*a = QA_New ();
	host_t		h = {0};
	qcvm_t		*vm;
	uint8_t		*last;
	uint32_t	max;
	qc_ent_t	e;

	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	vm = MakeVM (a, NULL, &h);
	max = QC_MaxEdicts (vm);
	QT_CHECK (QC_CommitEdicts (vm, max + 5));		// as many as there can be
	last = QC_Edicts (vm) + ((size_t)(max - 1) << QC_EdictShift (vm));
	QT_EQ_U (last[0], 0);
	last[0] = 1;
	QT_EQ_U (QC_NumEdicts (vm), 1);					// nothing spawned
	QT_CHECK (QC_Spawn (vm, &e) && e == 1);
	QC_Destroy (vm);
	QA_Free (a);
}

// float and double to int as x86 converts them
static void TestConversions (void)
{
	QT_EQ_I (QC_FloatToInt (2.9f), 2);
	QT_EQ_I (QC_FloatToInt (-2.9f), -2);
	QT_EQ_I (QC_FloatToInt (-2147483648.0f), INT32_MIN);
	QT_EQ_I (QC_FloatToInt (2147483520.0f), 2147483520);
	QT_EQ_I (QC_FloatToInt (2147483648.0f), INT32_MIN);
	QT_EQ_I (QC_FloatToInt (-2147483904.0f), INT32_MIN);
	QT_EQ_I (QC_FloatToInt (QC_BitsFloat (0x7FC00000)), INT32_MIN);		// NaN
	QT_EQ_I (QC_FloatToInt (QC_BitsFloat (0xFF800000)), INT32_MIN);		// -inf
	QT_EQ_I (QC_DoubleToInt (0.9999999), 0);
	QT_EQ_I (QC_DoubleToInt (2147483647.9), INT32_MAX);
	QT_EQ_I (QC_DoubleToInt (-2147483648.9), INT32_MIN);
	QT_EQ_I (QC_DoubleToInt (2147483648.0), INT32_MIN);
	QT_EQ_I (QC_DoubleToInt (-2147483649.0), INT32_MIN);
	QT_EQ_I (QC_DoubleToInt (QC_BitsDouble (0x7FF8000000000000ull)), INT32_MIN);
}

int main (void)
{
	TestAdd ();
	TestRecursion ();
	TestBuiltinBinding ();
	TestCallback ();
	TestErrors ();
	TestRunawayAndDepth ();
	TestNullAndInvalid ();
	TestEntitiesAndFields ();
	TestArgumentsAndSelf ();
	TestReachableUnbound ();
	TestBadEntityAccess ();
	TestTempStrings ();
	TestStateOpcode ();
	TestReentrancyLimit ();
	TestAbort ();
	TestTrace ();
	TestAbandon ();
	TestOverlappingVectors ();
	TestRunawayBudget ();
	TestDeadline ();
	TestHooks ();
	TestBigPrograms ();
	TestProfileAndCaller ();
	TestCommitEdicts ();
	TestConversions ();
	return QT_Finish ("vm", "calls, builtins, errors, limits and the rest behave");
}
