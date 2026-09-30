// test_qc_multiprogs.c -- several progs in one VM: linking, calls between
// progs, shared globals, fields unified by name, relocated strings, the limits,
// and addprogs (qcvm-rs's tests/all/multiprogs.rs)

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// what the host saw
typedef struct
{
	int			numwarnings;
	char		warnings[8][256];
	qc_progs_t	*addons[4];			// what load_progs finds by name
	const char	*names[4];
} host_t;

static void OnWarning (void *ctx, const qc_warning_t *w)
{
	host_t	*h = ctx;

	if (h->numwarnings < 8)
		QC_WarningText (w, h->warnings[h->numwarnings++], sizeof(h->warnings[0]));
}

// the named add-on, with a reference for the VM
static qc_progs_t *LoadProgs (void *ctx, const char *name)
{
	host_t	*h = ctx;
	int		i;

	for (i = 0 ; i < 4 ; i++)
		if (h->names[i] && !strcmp (h->names[i], name))
		{
			QC_RetainProgs (h->addons[i]);
			return h->addons[i];
		}
	return NULL;
}

static const qc_host_t	test_host = {.warning = OnWarning, .load_progs = LoadProgs};

// the main progs: self, fields health and origin, an extern addon_twice(x) and
// run(x) = addon_twice(x) + 1
static qc_asm_t *MainProgs (void)
{
	static const uint8_t	one_parm[] = {1};
	qc_asm_t	*a = QA_New ();
	uint32_t	ext, one;
	qa_func_t	f;

	QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	ext = QA_Global1 (a, "addon_twice", QC_EV_FUNCTION, 0);
	QA_Bodyless (a, "addon_twice");
	one = QA_Float (a, 1);
	f = QA_Function (a, "run", one_parm, 1, 0);
	QA_Emit (a, QOP_STORE_F, QA_Local (f, 0), QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, ext, 0, 0);
	QA_Emit (a, QOP_ADD_F, QA_OFS_RETURN, one, QA_OFS_RETURN);
	QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
	return a;
}

// an add-on: addon_twice(x) = x * 2 + self.health + self.mana, a new field
// mana, a string constant, thisprogs, and an init that records its argument
static qc_asm_t *AddonProgs (void)
{
	static const uint8_t	one_parm[] = {1};
	qc_asm_t	*a = QA_New ();
	uint32_t	self_g, mana, health, greeting, init_arg, two, x, t;
	qa_func_t	f, init;

	QA_String (a, "padding so offsets differ from the main progs");
	self_g = QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Field (a, "mana", QC_EV_FLOAT, &mana);
	QA_Field (a, "health", QC_EV_FLOAT, &health);
	QA_Global (a, "thisprogs", QC_EV_FLOAT, NULL, 0);
	greeting = QA_String (a, "hello from the addon");
	QA_Global1 (a, "greeting", QC_EV_STRING, greeting);
	init_arg = QA_Global (a, "init_arg", QC_EV_FLOAT, NULL, 0);
	two = QA_Float (a, 2);
	f = QA_Function (a, "addon_twice", one_parm, 1, 2);
	x = QA_Local (f, 0);
	t = QA_Local (f, 1);
	QA_Emit (a, QOP_MUL_F, x, two, t);
	QA_Emit (a, QOP_LOAD_F, self_g, health, QA_Local (f, 2));
	QA_Emit (a, QOP_ADD_F, t, QA_Local (f, 2), t);
	QA_Emit (a, QOP_LOAD_F, self_g, mana, QA_Local (f, 2));
	QA_Emit (a, QOP_ADD_F, t, QA_Local (f, 2), t);
	QA_Emit (a, QOP_RETURN, t, 0, 0);
	init = QA_Function (a, "init", one_parm, 1, 0);
	QA_Emit (a, QOP_STORE_F, QA_Local (init, 0), init_arg, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	return a;
}

// built, loaded and freed
static qc_progs_t *Load (qc_asm_t *a)
{
	qc_progs_t	*p = QA_Load (a, QC_FORMAT_FTE16);

	QA_Free (a);
	return p;
}

static qcvm_t *MakeVM (qc_asm_t *a, const qc_config_t *config, const qc_builtins_t *b, host_t *h)
{
	qcvm_t	*vm = QA_CreateVM (a, config, b, &test_host, h);

	QA_Free (a);
	return vm;
}

// adds a progs (releasing the caller's reference); its number, or -1
static int64_t Add (qcvm_t *vm, qc_progs_t *p)
{
	uint32_t	pr = 0;
	bool		ok = QC_AddProgs (vm, p, &pr);

	QC_ReleaseProgs (p);
	return ok ? (int64_t)pr : -1;
}

// a global of progs pr
static qc_word_t *Global (qcvm_t *vm, uint32_t pr, const char *name)
{
	static qc_word_t	none;
	uint32_t			word;

	if (!QT_CHECK (QC_FindGlobalIn (vm, pr, name, &word, NULL)))
		return &none;
	return &QC_GlobalsIn (vm, pr)[word];
}

static uint32_t Field (qcvm_t *vm, const char *name)
{
	uint32_t	ofs = 0;

	QT_CHECK (QC_FindField (vm, name, &ofs, NULL));
	return ofs;
}

static void SetFloatField (qcvm_t *vm, qc_ent_t e, const char *name, float v)
{
	uint32_t	w = QC_FloatBits (v);

	QT_CHECK (QC_SetField (vm, e, Field (vm, name), 1, &w));
}

static qc_ent_t Spawn (qcvm_t *vm)
{
	qc_ent_t	e = 0;

	QT_CHECK (QC_Spawn (vm, &e));
	return e;
}

// calls f, which must succeed
static qc_value_t Call (qcvm_t *vm, qc_func_t f, int argc, const qc_value_t *args)
{
	qc_value_t	ret = {{0}};
	char		text[512];

	if (!QT_CHECK (QC_Call (vm, f, argc, args, &ret)))
		printf ("  %s\n", QC_ErrorText (QC_LastError (vm), text, sizeof(text)));
	return ret;
}

static float Ret (qc_value_t v)
{
	return QC_BitsFloat (v.w[0]);
}

static qc_value_t Str (qcvm_t *vm, const char *s)
{
	return QC_ValWord (QC_TempString (vm, s, strlen (s)));
}

// Host lookups see what QuakeC has done to function globals: redirected entry
// points, cleared ones, globals without a function of their own, and
// references into other progs.
static void TestFunctionLookupReadsLiveFunctionGlobals (void)
{
	qc_asm_t	*a = QA_New ();
	uint32_t	one = QA_Float (a, 1), two = QA_Float (a, 2), index = 0;
	qa_func_t	first, second;
	qc_progs_t	*program;
	qc_func_t	second_f, f;
	host_t		h = {0};
	qcvm_t		*vm;
	int64_t		pr;

	first = QA_Function (a, "first", NULL, 0, 0);
	QA_Emit (a, QOP_RETURN, one, 0, 0);
	second = QA_Function (a, "second", NULL, 0, 0);
	QA_Emit (a, QOP_RETURN, two, 0, 0);
	QA_Function (a, "gone", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_Global1 (a, "hook", QC_EV_FUNCTION, second.index);
	program = Load (a);
	vm = QC_Create (program, NULL, NULL, &test_host, &h, NULL);
	if (!QT_CHECK (vm != NULL))
		return;

	QT_EQ_F (Ret (Call (vm, QC_FindFunction (vm, "first"), 0, NULL)), 1);
	// a function global without a function of its own
	QT_EQ_F (Ret (Call (vm, QC_FindFunction (vm, "hook"), 0, NULL)), 2);

	// first redirected to second, and gone cleared
	second_f = QC_FindFunction (vm, "second");
	Global (vm, 0, "first")->u = second_f;
	QT_EQ_F (Ret (Call (vm, QC_FindFunction (vm, "first"), 0, NULL)), 2);
	Global (vm, 0, "gone")->u = 0;
	QT_EQ_U (QC_FindFunction (vm, "gone"), 0);
	// the program itself still describes the initial state
	QT_CHECK (QC_ProgsFunctionIndex (program, "first", &index));
	QT_EQ_U (index, first.index);

	// a global of an add-on pointing into the main progs keeps its progs tag
	pr = Add (vm, Load (AddonProgs ()));
	QT_EQ_I (pr, 1);
	Global (vm, (uint32_t)pr, "addon_twice")->u = second_f;
	f = QC_FindFunctionIn (vm, (uint32_t)pr, "addon_twice");
	QT_EQ_U (QC_FUNC_PROGS (f), 0);
	QT_EQ_F (Ret (Call (vm, f, 0, NULL)), 2);

	QC_Destroy (vm);
	QC_ReleaseProgs (program);
}

static void TestCrossProgsCallsShareFieldsAndGlobals (void)
{
	host_t		h = {0};
	qcvm_t		*vm = MakeVM (MainProgs (), NULL, NULL, &h);
	qc_value_t	arg = QC_ValFloat (1), ret;
	qc_func_t	run = QC_FindFunction (vm, "run"), twice;
	uint32_t	health, mana;
	qc_ent_t	e;
	int64_t		pr;

	// an unlinked extern: calling it fails like a null function
	QT_CHECK (!QC_Call (vm, run, 1, &arg, &ret));
	QT_EQ_I (QC_LastError (vm)->kind, QC_ERR_NULL_FUNCTION);

	pr = Add (vm, Load (AddonProgs ()));
	QT_EQ_I (pr, 1);
	QT_EQ_U (QC_NumProgs (vm), 2);

	// fields: health is shared by name, mana was added
	health = Field (vm, "health");
	mana = Field (vm, "mana");
	QT_CHECK (health != mana);
	e = Spawn (vm);
	SetFloatField (vm, e, "health", 10);
	SetFloatField (vm, e, "mana", 100);
	Global (vm, 0, "self")->u = e;

	// run(3) = addon_twice(3) + 1 = (6 + 10 + 100) + 1; self travels to the add-on
	arg = QC_ValFloat (3);
	QT_EQ_F (Ret (Call (vm, run, 1, &arg)), 117);

	// calling into the add-on directly from the host
	twice = QC_FindFunctionIn (vm, (uint32_t)pr, "addon_twice");
	QT_EQ_U (QC_FUNC_PROGS (twice), pr);
	arg = QC_ValFloat (1);
	QT_EQ_F (Ret (Call (vm, twice, 1, &arg)), 112);

	// the add-on's globals, relocated
	QT_EQ_S (QC_String (vm, Global (vm, (uint32_t)pr, "greeting")->u), "hello from the addon");
	QT_EQ_F (Global (vm, (uint32_t)pr, "thisprogs")->f, 1);
	// init(prevprogs) receives the previous progs' number
	QT_EQ_F (Global (vm, (uint32_t)pr, "init_arg")->f, 0);

	// QC_Reset drops the add-on again
	QT_CHECK (QC_Reset (vm));
	QT_EQ_U (QC_NumProgs (vm), 1);
	QT_CHECK (QC_LoadedProgs (vm, 1) == NULL);
	QC_Destroy (vm);
}

// A progs claiming billions of field words but defining none is refused as
// soon as the reserved room runs out, and leaves the field layout as it was.
static void TestHugeFieldCountsAreRefusedQuickly (void)
{
	host_t		h = {0};
	qcvm_t		*vm = MakeVM (MainProgs (), NULL, NULL, &h);
	qc_asm_t	*tiny = QA_New ();
	qc_progs_t	*program;
	uint32_t	before = Field (vm, "origin"), pr;
	clock_t		started;
	bool		ok;

	QA_HeaderOverride (tiny, 14, UINT32_MAX);		// entity_fields
	program = Load (tiny);
	QT_EQ_U (QC_ProgsEntityFields (program), UINT32_MAX);
	started = clock ();
	ok = QC_AddProgs (vm, program, &pr);
	QT_CHECK ((double)(clock () - started) / CLOCKS_PER_SEC < 1.0);
	QT_CHECK (!ok);
	QT_EQ_I (QC_LastError (vm)->kind, QC_ERR_OUT_OF_MEMORY);
	QC_ReleaseProgs (program);
	QT_EQ_U (QC_NumProgs (vm), 1);
	QT_EQ_U (Field (vm, "origin"), before);
	// the layout still has its room: a well-formed add-on fits afterwards
	QT_EQ_I (Add (vm, Load (AddonProgs ())), 1);
	QC_Destroy (vm);
}

static void TestFieldReserveIsEnforced (void)
{
	host_t		h = {0};
	qc_config_t	config;
	qc_asm_t	*greedy = QA_New ();
	qc_progs_t	*program;
	qcvm_t		*vm;
	char		name[32];
	uint32_t	pr;
	int			i;

	QC_DefaultConfig (&config, QC_CSQC);
	config.field_reserve_bytes = 16;
	vm = MakeVM (MainProgs (), &config, NULL, &h);
	for (i = 0 ; i < 200 ; i++)
	{
		snprintf (name, sizeof(name), "extra%d", i);
		QA_Field (greedy, name, QC_EV_VECTOR, NULL);
	}
	program = Load (greedy);
	QT_CHECK (!QC_AddProgs (vm, program, &pr));
	QT_EQ_I (QC_LastError (vm)->kind, QC_ERR_OUT_OF_MEMORY);
	QC_ReleaseProgs (program);
	QT_EQ_U (QC_NumProgs (vm), 1);
	// a failed QC_AddProgs leaves the fields untouched
	QT_CHECK (!QC_FindField (vm, "extra0", NULL, NULL));
	QC_Destroy (vm);
}

// the main progs of the builtin test: self and the fields changeyaw uses, and
// with_view the view vectors too
static qc_asm_t *ViewMain (bool with_view)
{
	qc_asm_t	*a = QA_New ();

	QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Field (a, "angles", QC_EV_VECTOR, NULL);
	QA_Field (a, "ideal_yaw", QC_EV_FLOAT, NULL);
	QA_Field (a, "yaw_speed", QC_EV_FLOAT, NULL);
	if (with_view)
	{
		QA_Global (a, "v_forward", QC_EV_VECTOR, NULL, 0);
		QA_Global (a, "v_right", QC_EV_VECTOR, NULL, 0);
		QA_Global (a, "v_up", QC_EV_VECTOR, NULL, 0);
	}
	return a;
}

// Builtins use the globals of the progs that calls them: an add-on's
// makevectors writes the add-on's view vectors (which the main progs need not
// even define), and changeyaw turns the add-on's self.
static void TestBuiltinsUseTheCallingProgsGlobals (void)
{
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	qc_asm_t		*addon;
	uint32_t		self_g, target, mv, mv_g, cy, cy_g, yaw90, angles[3], word;
	qc_word_t		*forward;
	host_t			h;
	qcvm_t			*vm;
	qc_ent_t		e;
	int64_t			pr;
	int				with_view;

	for (with_view = 0 ; with_view < 2 ; with_view++)
	{
		addon = QA_New ();
		self_g = QA_Global (addon, "self", QC_EV_ENTITY, NULL, 0);
		QA_Field (addon, "angles", QC_EV_VECTOR, NULL);
		QA_Global (addon, "v_forward", QC_EV_VECTOR, NULL, 0);
		QA_Global (addon, "v_right", QC_EV_VECTOR, NULL, 0);
		QA_Global (addon, "v_up", QC_EV_VECTOR, NULL, 0);
		target = QA_Global (addon, "target", QC_EV_ENTITY, NULL, 0);
		mv = QA_Builtin (addon, "makevectors", 1, 1);
		mv_g = QA_Global1 (addon, "makevectors", QC_EV_FUNCTION, mv);
		cy = QA_Builtin (addon, "changeyaw", 49, 0);
		cy_g = QA_Global1 (addon, "changeyaw", QC_EV_FUNCTION, cy);
		yaw90 = QA_Vector (addon, 0, 90, 0);
		QA_Function (addon, "look", NULL, 0, 0);
		QA_Emit (addon, QOP_STORE_V, yaw90, QA_PARM (0), 0);
		QA_Emit (addon, QOP_CALL1, mv_g, 0, 0);
		QA_Emit (addon, QOP_DONE, 0, 0, 0);
		QA_Function (addon, "turn", NULL, 0, 0);
		QA_Emit (addon, QOP_STORE_ENT, target, self_g, 0);
		QA_Emit (addon, QOP_CALL0, cy_g, 0, 0);
		QA_Emit (addon, QOP_DONE, 0, 0, 0);

		h = (host_t){0};
		vm = MakeVM (ViewMain (with_view), NULL, b, &h);
		pr = Add (vm, Load (addon));
		if (!QT_CHECK (pr == 1))
		{
			QC_Destroy (vm);
			continue;
		}
		Call (vm, QC_FindFunctionIn (vm, 1, "look"), 0, NULL);
		forward = Global (vm, 1, "v_forward");
		if (!QT_CHECK (fabsf (forward[1].f - 1) < 1e-6f && fabsf (forward[0].f) < 1e-6f))
			printf ("  v_forward %g %g %g\n", forward[0].f, forward[1].f, forward[2].f);
		if (QC_FindGlobal (vm, "v_forward", &word, NULL))
		{
			// the main progs' vectors are untouched
			forward = &QC_Globals (vm)[word];
			QT_CHECK (forward[0].f == 0 && forward[1].f == 0 && forward[2].f == 0);
		}
		else
			QT_CHECK (!with_view);

		e = Spawn (vm);
		SetFloatField (vm, e, "ideal_yaw", 90);
		SetFloatField (vm, e, "yaw_speed", 45);
		Global (vm, 1, "target")->u = e;
		Call (vm, QC_FindFunctionIn (vm, 1, "turn"), 0, NULL);
		QT_CHECK (QC_GetField (vm, e, Field (vm, "angles"), 3, angles));
		if (!QT_CHECK (fabsf (QC_BitsFloat (angles[1]) - 45) < 0.01f))
			printf ("  angles_y %g\n", QC_BitsFloat (angles[1]));
		QC_Destroy (vm);
	}
	QC_BuiltinsFree (b);
}

static void AsmBuiltin (qc_asm_t *a, const char *name, uint32_t number)
{
	uint32_t	f = QA_Builtin (a, name, number, -1);

	QA_Global1 (a, name, QC_EV_FUNCTION, f);
}

// calls a function of the main progs by name, which must succeed
static qc_value_t CallNamed (qcvm_t *vm, const char *name, int argc, const qc_value_t *args)
{
	return Call (vm, QC_FindFunction (vm, name), argc, args);
}

// The introspection builtins reach into other progs: externcall, externvalue,
// externset, isfunction and callfunction.
static void TestExternBuiltinsReachOtherProgs (void)
{
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	qc_asm_t		*a = MainProgs ();
	qc_value_t		args[3], r;
	host_t			h = {0};
	qcvm_t			*vm;
	qc_ent_t		e;
	int64_t			pr;
	float			v = 0;

	AsmBuiltin (a, "externcall", 201);
	AsmBuiltin (a, "externvalue", 203);
	AsmBuiltin (a, "externset", 204);
	AsmBuiltin (a, "isfunction", 607);
	AsmBuiltin (a, "callfunction", 605);
	vm = MakeVM (a, NULL, b, &h);
	pr = Add (vm, Load (AddonProgs ()));
	QT_EQ_I (pr, 1);
	e = Spawn (vm);
	SetFloatField (vm, e, "health", 10);
	SetFloatField (vm, e, "mana", 100);
	Global (vm, 0, "self")->u = e;

	// externcall by progs number, and in whichever progs has the function
	args[0] = QC_ValFloat (1);
	args[1] = Str (vm, "addon_twice");
	args[2] = QC_ValFloat (3);
	QT_EQ_F (Ret (CallNamed (vm, "externcall", 3, args)), 116);
	args[0] = QC_ValFloat (-2);
	args[1] = Str (vm, "addon_twice");
	args[2] = QC_ValFloat (1);
	QT_EQ_F (Ret (CallNamed (vm, "externcall", 3, args)), 112);
	// an N: prefix selects the progs
	args[0] = QC_ValFloat (1);
	args[1] = Str (vm, "1:addon_twice");
	args[2] = QC_ValFloat (0);
	QT_EQ_F (Ret (CallNamed (vm, "externcall", 3, args)), 110);

	// externvalue reads another progs' globals (relocated strings included)
	// or their addresses
	args[0] = QC_ValFloat (1);
	args[1] = Str (vm, "greeting");
	r = CallNamed (vm, "externvalue", 2, args);
	QT_EQ_S (QC_String (vm, r.w[0]), "hello from the addon");
	args[1] = Str (vm, "thisprogs");
	QT_EQ_F (Ret (CallNamed (vm, "externvalue", 2, args)), 1);
	// the main progs has no such global
	args[0] = QC_ValFloat (0);
	args[1] = Str (vm, "greeting");
	r = CallNamed (vm, "externvalue", 2, args);
	QT_EQ_U (r.w[0], 0);
	// a function by name
	args[0] = QC_ValFloat (1);
	args[1] = Str (vm, "addon_twice");
	r = CallNamed (vm, "externvalue", 2, args);
	QT_EQ_U (r.w[0], QC_FindFunctionIn (vm, 1, "addon_twice"));

	// externset writes them; &name gives an address QuakeC pointers can use
	args[0] = QC_ValFloat (1);
	args[1] = QC_ValFloat (42);
	args[2] = Str (vm, "init_arg");
	CallNamed (vm, "externset", 3, args);
	QT_EQ_F (Global (vm, 1, "init_arg")->f, 42);
	args[0] = QC_ValFloat (1);
	args[1] = Str (vm, "&init_arg");
	r = CallNamed (vm, "externvalue", 2, args);
	QT_CHECK (QC_ReadMemory (vm, r.w[0], &v, 4));
	QT_EQ_F (v, 42);

	// isfunction and callfunction search every progs
	args[0] = Str (vm, "addon_twice");
	QT_EQ_F (Ret (CallNamed (vm, "isfunction", 1, args)), 1);
	args[0] = Str (vm, "nonexistent");
	QT_EQ_F (Ret (CallNamed (vm, "isfunction", 1, args)), 0);
	args[0] = QC_ValFloat (4);
	args[1] = Str (vm, "addon_twice");
	QT_EQ_F (Ret (CallNamed (vm, "callfunction", 2, args)), 118);

	QC_Destroy (vm);
	QC_BuiltinsFree (b);
}

// an add-on whose init calls a null function
static qc_asm_t *BadInitProgs (void)
{
	static const uint8_t	one_parm[] = {1};
	qc_asm_t	*a = QA_New ();
	uint32_t	nothing = QA_Global1 (a, "nothing", QC_EV_FUNCTION, 0);

	QA_Function (a, "init", one_parm, 1, 0);
	QA_Emit (a, QOP_CALL0, nothing, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	return a;
}

// load(name) = addprogs(name), from QuakeC
static float LoadFromQC (qcvm_t *vm, const char *name, bool *ok)
{
	qc_value_t	arg = Str (vm, name), ret = {{0}};

	*ok = QC_Call (vm, QC_FindFunction (vm, "load"), 1, &arg, &ret);
	return Ret (ret);
}

// addprogs: progs from the host's load_progs, -1 for those it lacks or that
// don't fit (with a warning), and init's errors fail the calling QuakeC
static void TestAddprogsBuiltin (void)
{
	static const uint8_t	one_parm[] = {1};
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	qc_asm_t		*a = MainProgs ();
	qc_config_t		config;
	host_t			h = {0};
	uint32_t		addprogs;
	qa_func_t		f;
	qcvm_t			*vm;
	bool			ok;
	int				i;

	addprogs = QA_Builtin (a, "addprogs", 202, 1);
	addprogs = QA_Global1 (a, "addprogs_g", QC_EV_FUNCTION, addprogs);
	f = QA_Function (a, "load", one_parm, 1, 0);
	QA_Emit (a, QOP_STORE_S, QA_Local (f, 0), QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, addprogs, 0, 0);
	QA_Emit (a, QOP_RETURN, QA_OFS_RETURN, 0, 0);
	h.names[0] = "addon";
	h.addons[0] = Load (AddonProgs ());
	h.names[1] = "badinit";
	h.addons[1] = Load (BadInitProgs ());
	h.names[2] = "another";
	h.addons[2] = Load (AddonProgs ());
	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.progs = 3;
	vm = MakeVM (a, &config, b, &h);

	// a progs the host has
	QT_EQ_F (LoadFromQC (vm, "addon", &ok), 1);
	QT_CHECK (ok);
	QT_EQ_U (QC_NumProgs (vm), 2);
	QT_CHECK (QC_LoadedProgs (vm, 1) == h.addons[0]);
	QT_EQ_F (Global (vm, 1, "thisprogs")->f, 1);
	QT_EQ_I (h.numwarnings, 0);

	// one it lacks
	QT_EQ_F (LoadFromQC (vm, "missing", &ok), -1);
	QT_CHECK (ok);
	QT_EQ_U (QC_NumProgs (vm), 2);

	// init's error fails the QuakeC that called addprogs; the progs stays loaded
	LoadFromQC (vm, "badinit", &ok);
	QT_CHECK (!ok);
	QT_EQ_I (QC_LastError (vm)->kind, QC_ERR_NULL_FUNCTION);
	QT_EQ_U (QC_NumProgs (vm), 3);

	// past the progs limit: -1 and a warning
	QT_EQ_F (LoadFromQC (vm, "another", &ok), -1);
	QT_CHECK (ok);
	QT_EQ_U (QC_NumProgs (vm), 3);
	if (QT_EQ_I (h.numwarnings, 1) && !QT_CHECK (QT_Contains (h.warnings[0], "addprogs another: ")))
		printf ("  %s\n", h.warnings[0]);

	// the calls work across them
	QT_EQ_F (Ret (Call (vm, QC_FindFunctionIn (vm, 1, "addon_twice"), 1, (qc_value_t[]){QC_ValFloat (2)})), 4);

	QC_Destroy (vm);
	for (i = 0 ; i < 3 ; i++)
		QC_ReleaseProgs (h.addons[i]);
	QC_BuiltinsFree (b);
}

int main (void)
{
	TestFunctionLookupReadsLiveFunctionGlobals ();
	TestCrossProgsCallsShareFieldsAndGlobals ();
	TestHugeFieldCountsAreRefusedQuickly ();
	TestFieldReserveIsEnforced ();
	TestBuiltinsUseTheCallingProgsGlobals ();
	TestExternBuiltinsReachOtherProgs ();
	TestAddprogsBuiltin ();
	return QT_Finish ("multiprogs", "progs are added, linked, share fields and globals, and are limited");
}
