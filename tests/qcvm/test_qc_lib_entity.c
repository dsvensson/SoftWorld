// test_qc_lib_entity.c -- entity allocation, iteration, search and copying
// (qcvm-rs's tests/all/builtins_misc/entity.rs)

#include "qc_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WORLD	0u

static const char	*named[] = {"findentity", NULL};

static void Setup (qc_asm_t *a, void *ctx)
{
	static const char	*floats[] = {"health", "flags", "solid"};
	static const char	*vectors[] = {"origin", "mins", "maxs"};
	static const char	*ents[] = {"chain", "chain2", "owner"};
	int					i;

	(void)ctx;
	QA_Field (a, "classname", QC_EV_STRING, NULL);
	for (i = 0 ; i < 3 ; i++)
		QA_Field (a, floats[i], QC_EV_FLOAT, NULL);
	for (i = 0 ; i < 3 ; i++)
		QA_Field (a, vectors[i], QC_EV_VECTOR, NULL);
	for (i = 0 ; i < 3 ; i++)
		QA_Field (a, ents[i], QC_EV_ENTITY, NULL);
	QA_Field (a, "count", QC_EV_INTEGER, NULL);
	QH_AddPeek (a);
	QH_Named (a, (void *)named);
}

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, Setup, NULL);
}

static uint32_t Bits (float f)
{
	uint32_t	u;

	memcpy (&u, &f, 4);
	return u;
}

// a field's offset
static uint32_t Fld (qh_t *h, const char *name)
{
	uint32_t	ofs = 0, type;

	if (!QC_FindField (h->vm, name, &ofs, &type))
		printf ("  no field %s\n", name);
	return ofs;
}

static void SetWords (qh_t *h, uint32_t e, const char *name, uint32_t words, const uint32_t *v)
{
	QT_CHECK (QC_SetField (h->vm, e, Fld (h, name), words, v));
}

// a string field, interned text or null
static void SetStr (qh_t *h, uint32_t e, const char *name, const char *text)
{
	uint32_t	r = text ? QC_Intern (h->vm, text, strlen (text)) : 0;

	SetWords (h, e, name, 1, &r);
}

static void SetF (qh_t *h, uint32_t e, const char *name, float x)
{
	uint32_t	u = Bits (x);

	SetWords (h, e, name, 1, &u);
}

static void SetV (qh_t *h, uint32_t e, const char *name, float x, float y, float z)
{
	uint32_t	u[3] = {Bits (x), Bits (y), Bits (z)};

	SetWords (h, e, name, 3, u);
}

static void SetE (qh_t *h, uint32_t e, const char *name, uint32_t x)
{
	SetWords (h, e, name, 1, &x);
}

static uint32_t GetWord (qh_t *h, uint32_t e, const char *name)
{
	uint32_t	u = 0;

	QT_CHECK (QC_GetField (h->vm, e, Fld (h, name), 1, &u));
	return u;
}

static float GetF (qh_t *h, uint32_t e, const char *name)
{
	return QC_BitsFloat (GetWord (h, e, name));
}

static uint32_t Spawn (qh_t *h)
{
	return QH_Word (h, "spawn", NOARGS);
}

static void SpawnN (qh_t *h, uint32_t *es, int n)
{
	int	i;

	for (i = 0 ; i < n ; i++)
		es[i] = Spawn (h);
}

static void Remove (qh_t *h, uint32_t e)
{
	QT_CHECK (QH_Call (h, "remove", ARGS (W (e)), NULL));
}

// a chain as a list, following field name from head; how many
static int Chain (qh_t *h, uint32_t head, const char *name, uint32_t *out)
{
	int			n = 0;
	uint32_t	e = head;

	while (e != WORLD && n < 100)
	{
		out[n++] = e;
		e = GetWord (h, e, name);
	}
	return n;
}

// the zero-terminated entity list a _list builtin returned, read back through QuakeC
static int List (qh_t *h, uint32_t p, uint32_t *out)
{
	int		n = 0;
	int32_t	e;

	while (n < 100 && (e = QH_Peek (h, p, n)))
		out[n++] = (uint32_t)e;
	return n;
}

// whether got[0..n) is want[0..count)
static bool Same (const uint32_t *got, int n, const uint32_t *want, int count)
{
	int	i;

	if (n != count)
		return false;
	for (i = 0 ; i < n ; i++)
		if (got[i] != want[i])
			return false;
	return true;
}

#define WANT(...)	(int)(sizeof ((uint32_t[]){__VA_ARGS__}) / sizeof (uint32_t)), (uint32_t[]){__VA_ARGS__}

static bool CheckList (const char *what, const uint32_t *got, int n, int count, const uint32_t *want)
{
	int	i;

	if (Same (got, n, want, count))
		return QT_CHECK (true);
	printf ("  %s: got [", what);
	for (i = 0 ; i < n ; i++)
		printf (i ? ", %u" : "%u", got[i]);
	printf ("], want [");
	for (i = 0 ; i < count ; i++)
		printf (i ? ", %u" : "%u", want[i]);
	printf ("]\n");
	return QT_CHECK (false);
}

// a chain from head through field name must be the entities listed
static void CheckChain (qh_t *h, uint32_t head, const char *name, int count, const uint32_t *want)
{
	uint32_t	got[100];
	int			n = Chain (h, head, name, got);

	CheckList (name, got, n, count, want);
}

// the list at p must be the entities listed
static void CheckListAt (qh_t *h, uint32_t p, int count, const uint32_t *want)
{
	uint32_t	got[100];
	int			n = List (h, p, got);

	CheckList ("list", got, n, count, want);
}

static void CheckEmptyList (qh_t *h, uint32_t p)
{
	uint32_t	got[100];

	QT_EQ_I (List (h, p, got), 0);
}

// spawn zeroes every field; remove frees
static void TestSpawnAndRemove (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b, c;

	a = Spawn (h);
	b = Spawn (h);
	QT_EQ_U (a, 1);
	QT_EQ_U (b, 2);
	SetF (h, a, "health", 5);
	SetF (h, a, "solid", 2);
	QT_EQ_F (QH_Float (h, "wasfreed", ARGS (W (a))), 0);
	Remove (h, a);
	QT_EQ_F (QH_Float (h, "wasfreed", ARGS (W (a))), 1);
	// CSQC's remove clears a few engine fields; the rest stay readable until reuse
	QT_EQ_F (GetF (h, a, "solid"), 0);
	QT_EQ_F (GetF (h, a, "health"), 5);
	// within the first two seconds freed slots are reused at once, zeroed
	c = Spawn (h);
	QT_EQ_U (c, a);
	QT_EQ_F (GetF (h, c, "health"), 0);
	QT_EQ_I (QH_NumWarnings (h), 0);
	QH_Free (h);
}

// remove refuses the world, free entities and protected ones
static void TestRemoveRefuses (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b;

	a = Spawn (h);
	b = Spawn (h);
	Remove (h, WORLD);
	QT_CHECK (!QC_IsFree (h->vm, WORLD));
	Remove (h, a);
	Remove (h, a);
	QC_SetProtected (h->vm, b, true);
	Remove (h, b);
	QT_CHECK (!QC_IsFree (h->vm, b));
	if (QT_EQ_I (QH_NumWarnings (h), 3))
	{
		QT_CHECK (QT_Contains (QH_WarningText (h, 0), "Unable to remove the world"));
		QT_CHECK (QT_Contains (QH_WarningText (h, 1), "already free"));
		QT_CHECK (QT_Contains (QH_WarningText (h, 2), "protected"));
	}
	QH_Free (h);
}

// removeinstant makes the slot reusable at once
static void TestRemoveinstant (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b;

	a = Spawn (h);
	b = Spawn (h);
	QC_SetTime (h->vm, 10.0);
	Remove (h, a);
	QC_SetTime (h->vm, 10.1);
	// a slot freed less than half a second ago isn't reused
	QT_EQ_U (Spawn (h), 3);
	QT_CHECK (QH_Call (h, "removeinstant", ARGS (W (b)), NULL));
	QT_EQ_U (Spawn (h), b);
	QC_SetTime (h->vm, 10.7);
	QT_EQ_U (Spawn (h), a);
	QH_Free (h);
}

// a host whose spawn hook sets health to 100
typedef struct
{
	qcvm_t		*vm;
	uint32_t	spawned[8];
	int			numspawned;
} spawnhost_t;

static void OnSpawn (void *ctx, qcvm_t *vm, qc_ent_t e)
{
	spawnhost_t	*host = ctx;
	uint32_t	ofs, hundred = Bits (100);

	if (QC_FindField (vm, "health", &ofs, NULL))
		QC_SetField (vm, e, ofs, 1, &hundred);
	if (host->numspawned < 8)
		host->spawned[host->numspawned++] = e;
}

// spawn and copyentity run the spawn hook
static void TestSpawnHook (void)
{
	static const qc_host_t	hooks = {.on_spawn = OnSpawn};
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	spawnhost_t		host = {0};
	qcvm_t			*vm;
	qc_value_t		ret, arg;
	uint32_t		e, c, ofs, health = 0;

	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Builtin (a, "spawn", 14, 0);
	QA_Builtin (a, "copyentity", 400, 2);
	vm = QA_CreateVM (a, NULL, b, &hooks, &host);
	QA_Free (a);
	QT_CHECK (QC_Call (vm, QC_FindFunction (vm, "spawn"), 0, NULL, &ret));
	e = ret.w[0];
	QT_CHECK (QC_FindField (vm, "health", &ofs, NULL));
	QT_CHECK (QC_GetField (vm, e, ofs, 1, &health));
	QT_EQ_F (QC_BitsFloat (health), 100);
	arg = QC_ValWord (e);
	QT_CHECK (QC_Call (vm, QC_FindFunction (vm, "copyentity"), 1, &arg, &ret));
	c = ret.w[0];
	CheckList ("spawned", host.spawned, host.numspawned, WANT (e, c));
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
}

// nextent iterates the entities in use
static void TestNextent (void)
{
	qh_t		*h = Harness ();
	uint32_t	es[4], seen[8], e = WORLD;
	int			n = 0;

	SpawnN (h, es, 4);
	Remove (h, es[1]);
	for ( ; n < 8 ; )
	{
		e = QH_Word (h, "nextent", ARGS (W (e)));
		if (e == WORLD)
			break;
		seen[n++] = e;
	}
	CheckList ("nextent", seen, n, WANT (1, 3, 4));
	QT_EQ_U (QH_Word (h, "nextent", ARGS (W (999))), WORLD);
	QH_Free (h);
}

// entities 1-5 with classnames "foo", "bar", "foo", null, ""
static void Classnames (qh_t *h, uint32_t es[5])
{
	static const char	*names[5] = {"foo", "bar", "foo", NULL, ""};
	int					i;

	SpawnN (h, es, 5);
	for (i = 0 ; i < 5 ; i++)
		SetStr (h, es[i], "classname", names[i]);
}

static uint32_t Find (qh_t *h, uint32_t start, uint32_t field, const char *text)
{
	return QH_Word (h, "find", ARGS (W (start), W (field), QH_S (h, text)));
}

// find matches string fields
static void TestFind (void)
{
	qh_t		*h = Harness ();
	uint32_t	es[5], cn;

	Classnames (h, es);
	cn = Fld (h, "classname");
	QT_EQ_U (Find (h, WORLD, cn, "foo"), 1);
	QT_EQ_U (Find (h, es[0], cn, "foo"), 3);
	QT_EQ_U (Find (h, es[2], cn, "foo"), 0);
	QT_EQ_U (Find (h, WORLD, cn, "baz"), 0);
	// searching for "" matches null and empty fields
	QT_EQ_U (Find (h, WORLD, cn, ""), 4);
	QT_EQ_U (Find (h, es[3], cn, ""), 5);
	QT_EQ_U (QH_Word (h, "find", ARGS (W (WORLD), W (cn), W (0))), 4);
	// free entities are skipped
	Remove (h, es[0]);
	QT_EQ_U (Find (h, WORLD, cn, "foo"), 3);
	QT_EQ_I (QH_NumWarnings (h), 0);
	// developer mode warns about searching for ""
	QC_SetDeveloper (h->vm, true);
	Find (h, WORLD, cn, "");
	if (QT_EQ_I (QH_NumWarnings (h), 1))
		QT_EQ_S (QH_WarningText (h, 0), "find: empty string");
	// a field outside the entity is a builtin error
	QC_SetDeveloper (h->vm, false);
	QT_EQ_I (QH_Fails (h, "find", ARGS (W (WORLD), W (9999), QH_S (h, "foo"))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "bad field"));
	QH_Free (h);
}

static uint32_t Findfloat (qh_t *h, uint32_t start, uint32_t field, float x)
{
	return QH_Word (h, "findfloat", ARGS (W (start), W (field), F (x)));
}

// findfloat compares raw bits
static void TestFindfloat (void)
{
	static const float	values[4] = {7, -0.0f, NAN, 7};
	qh_t		*h = Harness ();
	uint32_t	es[4], hp, owner;
	int			i;

	SpawnN (h, es, 4);
	hp = Fld (h, "health");
	for (i = 0 ; i < 4 ; i++)
		SetF (h, es[i], "health", values[i]);
	QT_EQ_U (Findfloat (h, WORLD, hp, 7), 1);
	QT_EQ_U (Findfloat (h, es[0], hp, 7), 4);
	QT_EQ_U (Findfloat (h, WORLD, hp, -0.0f), 2);		// -0 matches only -0
	QT_EQ_U (Findfloat (h, WORLD, hp, 0), 0);
	QT_EQ_U (Findfloat (h, WORLD, hp, NAN), 3);			// a NaN matches its own bits
	// entity fields through the findentity alias
	SetE (h, es[3], "owner", es[1]);
	owner = Fld (h, "owner");
	QT_EQ_U (QH_Word (h, "findentity", ARGS (W (WORLD), W (owner), W (es[1]))), es[3]);
	// FTE insists on exactly three arguments
	QT_EQ_I (QH_Fails (h, "findfloat", ARGS (W (WORLD), W (hp))), QC_ERR_BUILTIN);
	QH_Free (h);
}

static uint32_t Findflags (qh_t *h, uint32_t start, uint32_t field, float x)
{
	return QH_Word (h, "findflags", ARGS (W (start), W (field), F (x)));
}

// findflags tests bits
static void TestFindflags (void)
{
	static const float	values[3] = {1, 3, 4.5f};
	qh_t		*h = Harness ();
	uint32_t	es[3], fl;
	int			i;

	SpawnN (h, es, 3);
	for (i = 0 ; i < 3 ; i++)
		SetF (h, es[i], "flags", values[i]);
	fl = Fld (h, "flags");
	QT_EQ_U (Findflags (h, WORLD, fl, 2), 2);
	QT_EQ_U (Findflags (h, es[1], fl, 2), 0);
	QT_EQ_U (Findflags (h, WORLD, fl, 4), 3);
	QT_EQ_U (Findflags (h, es[0], fl, 1), 2);
	QT_EQ_U (Findflags (h, WORLD, fl, 8), 0);
	QH_Free (h);
}

// findchain links the matches through .chain
static void TestFindchain (void)
{
	qh_t		*h = Harness ();
	uint32_t	es[5], cn, c2, head;

	Classnames (h, es);
	cn = Fld (h, "classname");
	SetE (h, es[1], "chain", es[4]);
	head = QH_Word (h, "findchain", ARGS (W (cn), QH_S (h, "foo")));
	CheckChain (h, head, "chain", WANT (3, 1));
	QT_EQ_U (GetWord (h, es[0], "chain"), WORLD);
	QT_EQ_U (GetWord (h, es[1], "chain"), es[4]);		// non-matching entities are untouched
	// unlike find, "" doesn't match null fields
	head = QH_Word (h, "findchain", ARGS (W (cn), QH_S (h, "")));
	CheckChain (h, head, "chain", WANT (5));
	// another chain field
	c2 = Fld (h, "chain2");
	head = QH_Word (h, "findchain", ARGS (W (cn), QH_S (h, "bar"), W (c2)));
	CheckChain (h, head, "chain2", WANT (2));
	QT_EQ_U (GetWord (h, es[1], "chain"), es[4]);
	QT_EQ_U (QH_Word (h, "findchain", ARGS (W (cn), QH_S (h, "nope"))), WORLD);
	QH_Free (h);
}

// findchain needs a chain field
static void TestFindchainNeedsChain (void)
{
	qh_t	*h = QH_Csqc ();

	QT_EQ_I (QH_Fails (h, "findchain", ARGS (W (0), QH_S (h, "foo"))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "bad field"));
	QH_Free (h);
}

// findchainfloat compares values, and findchainflags bits
static void TestFindchainfloatAndFlags (void)
{
	static const float	healths[4] = {7, 7, NAN, -0.0f}, flags[4] = {1, 2, 3, 0};
	qh_t		*h = Harness ();
	uint32_t	es[4], hp, fl, c2, head;
	int			i;

	SpawnN (h, es, 4);
	for (i = 0 ; i < 4 ; i++)
		SetF (h, es[i], "health", healths[i]);
	hp = Fld (h, "health");
	head = QH_Word (h, "findchainfloat", ARGS (W (hp), F (7)));
	CheckChain (h, head, "chain", WANT (2, 1));
	QT_EQ_U (QH_Word (h, "findchainfloat", ARGS (W (hp), F (NAN))), WORLD);
	// -0 equals 0 by value
	head = QH_Word (h, "findchainfloat", ARGS (W (hp), F (0)));
	CheckChain (h, head, "chain", WANT (4));

	for (i = 0 ; i < 4 ; i++)
		SetF (h, es[i], "flags", flags[i]);
	fl = Fld (h, "flags");
	c2 = Fld (h, "chain2");
	head = QH_Word (h, "findchainflags", ARGS (W (fl), F (2), W (c2)));
	CheckChain (h, head, "chain2", WANT (3, 2));
	QH_Free (h);
}

// entities for the radius searches (see the test for the layout)
static void RadiusScene (qh_t *h, uint32_t es[5])
{
	static const float	scene[5][5] = {
		{10, 0, 0, 2, 0},
		{100, 0, 0, 2, 0},
		{20, 0, 0, 0, 0},
		{0, 30, 0, 0, 16384},
		{60, 0, 0, 3, 0},
	};
	int					i;

	SpawnN (h, es, 5);
	for (i = 0 ; i < 5 ; i++)
	{
		SetV (h, es[i], "origin", scene[i][0], scene[i][1], scene[i][2]);
		SetF (h, es[i], "solid", scene[i][3]);
		SetF (h, es[i], "flags", scene[i][4]);
	}
	// entity 5's box is centred 15 units behind its origin
	SetV (h, es[4], "mins", -20, -5, -5);
	SetV (h, es[4], "maxs", -10, 5, 5);
}

// findradius takes the boxes' centres and skips the non-solid
static void TestFindradius (void)
{
	qh_t		*h = Harness ();
	uint32_t	es[5], c2, head;

	RadiusScene (h, es);
	// 1 at 10 (solid), 2 too far, 3 non-solid, 4 non-solid but findable, 5 centred at 45
	head = QH_Word (h, "findradius", ARGS (V (0, 0, 0), F (50)));
	CheckChain (h, head, "chain", WANT (5, 4, 1));
	// the radius is inclusive
	head = QH_Word (h, "findradius", ARGS (V (0, 0, 0), F (10)));
	CheckChain (h, head, "chain", WANT (1));
	c2 = Fld (h, "chain2");
	head = QH_Word (h, "findradius", ARGS (V (100, 0, 0), F (1), W (c2)));
	CheckChain (h, head, "chain2", WANT (2));
	QH_Free (h);
}

// findradius_list returns a temp array
static void TestFindradiusList (void)
{
	qh_t		*h = Harness ();
	uint32_t	es[5], r;

	RadiusScene (h, es);
	r = QH_Word (h, "findradius_list", ARGS (V (0, 0, 0), F (50), I (0), I (1)));
	QT_EQ_U (QH_ParmWord (h, 2), 3);
	QT_CHECK (QC_IsTempString (r));
	CheckListAt (h, r, WANT (1, 4, 5));
	r = QH_Word (h, "findradius_list", ARGS (V (500, 0, 0), F (1), I (99)));
	QT_EQ_U (QH_ParmWord (h, 2), 0);
	CheckEmptyList (h, r);
	QH_Free (h);
}

// find_list searches by type
static void TestFindList (void)
{
	static const float	healths[5] = {1, -0.0f, 1, 0, 2};
	qh_t		*h = Harness ();
	uint32_t	es[5], cn, hp, org, count, r, fortytwo = 42;
	qc_value_t	ret;
	int			i;

	Classnames (h, es);
	cn = Fld (h, "classname");
	r = QH_Word (h, "find_list", ARGS (W (cn), QH_S (h, "foo"), I (1), I (0)));
	QT_EQ_U (QH_ParmWord (h, 3), 2);
	CheckListAt (h, r, WANT (1, 3));
	// string searches skip protected entities; null fields never match, even ""
	QC_SetProtected (h->vm, es[2], true);
	r = QH_Word (h, "find_list", ARGS (W (cn), QH_S (h, "foo"), I (1), I (0)));
	CheckListAt (h, r, WANT (1));
	r = QH_Word (h, "find_list", ARGS (W (cn), QH_S (h, ""), I (1), I (0)));
	CheckListAt (h, r, WANT (5));
	// the type defaults to EV_STRING
	r = QH_Word (h, "find_list", ARGS (W (cn), QH_S (h, "bar")));
	CheckListAt (h, r, WANT (2));

	for (i = 0 ; i < 5 ; i++)
		SetF (h, es[i], "health", healths[i]);
	hp = Fld (h, "health");
	// floats compare by value
	r = QH_Word (h, "find_list", ARGS (W (hp), F (0), I (2), I (0)));
	CheckListAt (h, r, WANT (2, 4));
	// only string searches skip protected entities (entity 3 still is)
	r = QH_Word (h, "find_list", ARGS (W (hp), F (1), I (2), I (0)));
	CheckListAt (h, r, WANT (1, 3));
	SetV (h, es[4], "origin", 1, 2, 3);
	org = Fld (h, "origin");
	r = QH_Word (h, "find_list", ARGS (W (org), V (1, 2, 3), I (3), I (0)));
	CheckListAt (h, r, WANT (5));
	count = Fld (h, "count");
	QT_CHECK (QC_SetField (h->vm, es[1], count, 1, &fortytwo));
	r = QH_Word (h, "find_list", ARGS (W (count), I (42), I (8), I (0)));
	CheckListAt (h, r, WANT (2));
	// an unknown type or a field outside the entity: null and a zero count
	ret = QH_Raw (h, "find_list", ARGS (W (hp), F (0), I (13), I (7)));
	QT_EQ_U (ret.w[0], 0);
	QT_EQ_U (QH_ParmWord (h, 3), 0);
	ret = QH_Raw (h, "find_list", ARGS (W (9999), F (0), I (2), I (7)));
	QT_EQ_U (ret.w[0], 0);
	QT_EQ_U (QH_ParmWord (h, 3), 0);
	QH_Free (h);
}

// edict_num, num_for_edict and wasfreed
static void TestEdictNum (void)
{
	qh_t		*h = Harness ();
	uint32_t	es[3];

	SpawnN (h, es, 3);
	Remove (h, es[1]);
	QT_EQ_U (QH_Word (h, "edict_num", ARGS (F (2))), es[1]);		// free entities too
	QT_EQ_U (QH_Word (h, "edict_num", ARGS (F (3.9f))), es[2]);
	QT_EQ_U (QH_Word (h, "edict_num", ARGS (F (4))), WORLD);
	QT_EQ_U (QH_Word (h, "edict_num", ARGS (F (-1))), WORLD);
	QT_EQ_F (QH_Float (h, "num_for_edict", ARGS (W (es[2]))), 3);
	QT_EQ_I (QH_NumWarnings (h), 0);
	// a reference past the entities warns and reads as the world
	QT_EQ_F (QH_Float (h, "num_for_edict", ARGS (W (999))), 0);
	QT_EQ_F (QH_Float (h, "wasfreed", ARGS (W (999))), 0);
	if (QT_EQ_I (QH_NumWarnings (h), 2))
	{
		QT_EQ_S (QH_WarningText (h, 0), "bad entity index 999");
		QT_EQ_S (QH_WarningText (h, 1), "bad entity index 999");
	}
	QH_Free (h);
}

// menu's etof and ftoe
static void TestEtofFtoe (void)
{
	qh_t		*h = QH_New (QC_NUMBERING_MENU, NULL, NULL, NULL);
	uint32_t	e = Spawn (h);

	QT_EQ_F (QH_Float (h, "etof", ARGS (W (e))), 1);
	QT_EQ_U (QH_Word (h, "ftoe", ARGS (F (1))), e);
	QT_EQ_U (QH_Word (h, "ftoe", ARGS (F (7))), WORLD);
	QT_EQ_U (QH_Word (h, "ftoe", ARGS (F (-1))), WORLD);
	QH_Free (h);
}

// copyentity copies every field
static void TestCopyentity (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b, c, origin[3] = {0, 0, 0};

	a = Spawn (h);
	b = Spawn (h);
	SetStr (h, a, "classname", "monster");
	SetV (h, a, "origin", 1, 2, 3);
	SetF (h, a, "health", 50);
	QT_EQ_U (QH_Word (h, "copyentity", ARGS (W (a), W (b))), b);
	QT_CHECK (QC_GetField (h->vm, b, Fld (h, "origin"), 3, origin));
	QT_EQ_F (QC_BitsFloat (origin[0]), 1);
	QT_EQ_F (QC_BitsFloat (origin[1]), 2);
	QT_EQ_F (QC_BitsFloat (origin[2]), 3);
	QT_EQ_F (GetF (h, b, "health"), 50);
	// without a destination a new entity is spawned
	c = QH_Word (h, "copyentity", ARGS (W (a)));
	QT_EQ_U (c, 3);
	QT_EQ_S (QH_Text (h, GetWord (h, c, "classname")), "monster");
	QH_Free (h);
}

// copyentity refuses free and protected entities
static void TestCopyentityRefuses (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b, c;

	a = Spawn (h);
	b = Spawn (h);
	c = Spawn (h);
	SetF (h, a, "health", 9);
	Remove (h, c);
	QT_EQ_I (QH_Fails (h, "copyentity", ARGS (W (c), W (a))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "source is free"));
	QT_EQ_I (QH_Fails (h, "copyentity", ARGS (W (a), W (c))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "destination is free"));
	QC_SetProtected (h->vm, b, true);
	QT_EQ_I (QH_Fails (h, "copyentity", ARGS (W (a), W (b))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "read-only"));
	// developer mode: a warning, and FTE copies anyway
	QC_SetDeveloper (h->vm, true);
	QT_EQ_U (QH_Word (h, "copyentity", ARGS (W (a), W (b))), b);
	QT_EQ_F (GetF (h, b, "health"), 9);
	if (QT_EQ_I (QH_NumWarnings (h), 1))
		QT_EQ_S (QH_WarningText (h, 0), "copyentity: destination is read-only");
	QH_Free (h);
}

// entityprotection returns the previous setting
static void TestEntityprotection (void)
{
	qh_t		*h = Harness ();
	uint32_t	e = Spawn (h);

	// FTE returns the new value; this returns the previous one, as documented
	QT_EQ_F (QH_Float (h, "entityprotection", ARGS (W (e), F (1))), 0);
	QT_CHECK (QC_IsProtected (h->vm, e));
	QT_EQ_F (QH_Float (h, "entityprotection", ARGS (W (e), F (1))), 1);
	QT_EQ_F (QH_Float (h, "entityprotection", ARGS (W (e), F (0))), 1);
	QT_CHECK (!QC_IsProtected (h->vm, e));
	// values other than 0 and 1 change nothing
	QT_EQ_F (QH_Float (h, "entityprotection", ARGS (W (e), F (2))), 0);
	QT_CHECK (!QC_IsProtected (h->vm, e));
	// the world can be unprotected too
	QC_SetProtected (h->vm, WORLD, true);
	QT_EQ_F (QH_Float (h, "entityprotection", ARGS (W (WORLD), F (0))), 1);
	// a free entity is a builtin error
	Remove (h, e);
	QT_EQ_I (QH_Fails (h, "entityprotection", ARGS (W (e), F (1))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "free"));
	QH_Free (h);
}

int main (void)
{
	TestSpawnAndRemove ();
	TestRemoveRefuses ();
	TestRemoveinstant ();
	TestSpawnHook ();
	TestNextent ();
	TestFind ();
	TestFindfloat ();
	TestFindflags ();
	TestFindchain ();
	TestFindchainNeedsChain ();
	TestFindchainfloatAndFlags ();
	TestFindradius ();
	TestFindradiusList ();
	TestFindList ();
	TestEdictNum ();
	TestEtofFtoe ();
	TestCopyentity ();
	TestCopyentityRefuses ();
	TestEntityprotection ();
	return QT_Finish ("lib_entity", "the entity builtins allocate, search and copy as FTE does");
}
