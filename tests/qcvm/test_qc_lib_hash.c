// test_qc_lib_hash.c -- hash tables (qcvm-rs's tests/all/builtins_misc/hash.rs)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EV_STRING		1.0f
#define EV_FLOAT		2.0f
#define EV_VECTOR		3.0f
#define HASH_REPLACE	256.0f
#define HASH_ADD		512.0f

// adds cb(string key, vector value), which prints key and a newline, counts its
// calls in cb_count and keeps the last value in cb_value
static void Setup (qc_asm_t *a, void *ctx)
{
	static const uint8_t	sizes[2] = {1, 3};
	uint32_t				print, print_g, count, value, one, newline;
	qa_func_t				cb;

	(void)ctx;
	// declared by number: FTE numbers hash_getcb #293, though its extension dump doesn't
	QA_Builtin (a, "hash_getcb", 293, 3);
	print = QA_Builtin (a, "print", 339, 8);
	print_g = QA_Global1 (a, "print_fn", QC_EV_FUNCTION, print);
	count = QA_Global (a, "cb_count", QC_EV_FLOAT, NULL, 0);
	value = QA_Global (a, "cb_value", QC_EV_VECTOR, NULL, 0);
	one = QA_Float (a, 1);
	newline = QA_StrConst (a, "\n");
	cb = QA_Function (a, "cb", sizes, 2, 0);
	QA_Emit (a, QOP_ADD_F, count, one, count);
	QA_Emit (a, QOP_STORE_V, QA_Local (cb, 1), value, 0);
	QA_Emit (a, QOP_STORE_S, QA_Local (cb, 0), QA_PARM (0), 0);
	QA_Emit (a, QOP_STORE_S, newline, QA_PARM (1), 0);
	QA_Emit (a, QOP_CALL2, print_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
}

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, Setup, NULL);
}

// a call that must succeed
static void Call (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	char	text[1024];

	if (!QT_CHECK (QH_Call (h, name, argc, args, NULL)))
		printf ("  %s: %s\n", name, QC_ErrorText (QC_LastError (h->vm), text, sizeof(text)));
}

static bool VecEq (const float v[3], float x, float y, float z)
{
	return v[0] == x && v[1] == y && v[2] == z;
}

// a vector result, checked
static void CheckVec (qh_t *h, const char *name, int argc, const qc_value_t *args, float x, float y, float z)
{
	float	v[3];

	QH_Vector (h, v, name, argc, args);
	if (!QT_CHECK (VecEq (v, x, y, z)))
		printf ("  %s: '%g %g %g', want '%g %g %g'\n", name, (double)v[0], (double)v[1], (double)v[2], (double)x,
			(double)y, (double)z);
}

// a global's word
static qc_word_t *Global (qh_t *h, const char *name)
{
	uint32_t	word = 0, type;

	if (!QC_FindGlobal (h->vm, name, &word, &type))
		printf ("  no global %s\n", name);
	return &QC_Globals (h->vm)[word];
}

// tables are numbered from 1, and the numbers of destroyed ones are reused
static void TestTablesAreNumberedFromOneAndReused (void)
{
	qh_t	*h = Harness ();

	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (16))), 1);
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (0))), 2);
	Call (h, "hash_destroytab", ARGS (F (1)));
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (1e9f))), 1);
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (4))), 3);
	QH_Free (h);
}

static void TestTheTableLimitIsEnforced (void)
{
	qc_config_t	config;
	qh_t		*h;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.hash_tables = 2;
	h = QH_New (QC_NUMBERING_CSQC, &config, Setup, NULL);
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (8))), 1);
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (8))), 2);
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (8))), 0);
	Call (h, "hash_destroytab", ARGS (F (2)));
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (8))), 2);
	QH_Free (h);
}

static void TestValuesDefaultToVectors (void)
{
	qh_t	*h = Harness ();
	float	t = QH_Float (h, "hash_createtab", ARGS (F (8)));

	Call (h, "hash_add", ARGS (F (t), QH_S (h, "a"), V (1, 2, 3)));
	CheckVec (h, "hash_get", ARGS (F (t), QH_S (h, "a")), 1, 2, 3);
	// keys are case-sensitive
	CheckVec (h, "hash_get", ARGS (F (t), QH_S (h, "A")), 0, 0, 0);
	CheckVec (h, "hash_get", ARGS (F (t), QH_S (h, "b"), V (7, 8, 9)), 7, 8, 9);
	// an explicit type 0 means EV_VECTOR too
	t = QH_Float (h, "hash_createtab", ARGS (F (8), F (0)));
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "a"), V (4, 5, 6)));
	CheckVec (h, "hash_get", ARGS (F (t), QH_S (h, "a"), V (0, 0, 0), F (EV_VECTOR)), 4, 5, 6);
	QH_Free (h);
}

static void TestStringValuesAreCopied (void)
{
	qh_t		*h = Harness ();
	float		t = QH_Float (h, "hash_createtab", ARGS (F (8), F (EV_STRING)));
	qc_value_t	r;
	qc_str_t	def;

	Call (h, "hash_add", ARGS (F (t), QH_S (h, "greeting"), QH_S (h, "hello")));
	// the temp string passed in is gone after a collection; the table kept a copy
	QC_CollectGarbage (h->vm);
	r = QH_Raw (h, "hash_get", ARGS (F (t), QH_S (h, "greeting")));
	QT_EQ_S (QH_Text (h, r.w[0]), "hello");
	QT_CHECK (QC_IsTempString (r.w[0]));
	QT_CHECK (r.w[1] == 0 && r.w[2] == 0);
	// a string default comes back as it was passed
	def = QC_Intern (h->vm, "none", 4);
	r = QH_Raw (h, "hash_get", ARGS (F (t), QH_S (h, "missing"), W (def)));
	QT_EQ_U (r.w[0], def);
	QH_Free (h);
}

static void TestTypesCanBeRequired (void)
{
	qh_t	*h = Harness ();
	float	t = QH_Float (h, "hash_createtab", ARGS (F (8)));

	Call (h, "hash_add", ARGS (F (t), QH_S (h, "k"), F (5), F (HASH_ADD + EV_FLOAT)));
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "k"), QH_S (h, "text"), F (HASH_ADD + EV_STRING)));
	QT_EQ_F (QH_Float (h, "hash_get", ARGS (F (t), QH_S (h, "k"), F (-1), F (EV_FLOAT))), 5);
	QT_EQ_S (QH_String (h, "hash_get", ARGS (F (t), QH_S (h, "k"), F (-1), F (EV_STRING))), "text");
	QT_EQ_F (QH_Float (h, "hash_get", ARGS (F (t), QH_S (h, "k"), F (-1), F (EV_VECTOR))), -1);
	// without a required type the newest entry wins
	QT_EQ_S (QH_String (h, "hash_get", ARGS (F (t), QH_S (h, "k"))), "text");
	QH_Free (h);
}

// the index-th entry of key y
static float GetY (qh_t *h, float t, float index)
{
	return QH_Float (h, "hash_get", ARGS (F (t), QH_S (h, "y"), F (-1), F (0), F (index)));
}

static void TestReplaceRemovesOnlyTheNewestEntry (void)
{
	qh_t	*h = Harness ();
	float	t = QH_Float (h, "hash_createtab", ARGS (F (8), F (EV_FLOAT)));

	// plain adds replace
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "x"), F (1)));
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "x"), F (2)));
	QT_EQ_F (QH_Float (h, "hash_get", ARGS (F (t), QH_S (h, "x"))), 2);
	QT_EQ_F (QH_Float (h, "hash_get", ARGS (F (t), QH_S (h, "x"), F (-1), F (0), F (1))), -1);
	// HASH_ADD keeps duplicates, reachable by index (the newest first)
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "y"), F (1), F (HASH_ADD)));
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "y"), F (2), F (HASH_ADD)));
	QT_EQ_F (GetY (h, t, 0), 2);
	QT_EQ_F (GetY (h, t, 1), 1);
	QT_EQ_F (GetY (h, t, 2), -1);
	// HASH_REPLACE wins over HASH_ADD, and removes just the newest duplicate
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "y"), F (3), F (HASH_ADD + HASH_REPLACE)));
	QT_EQ_F (GetY (h, t, 0), 3);
	QT_EQ_F (GetY (h, t, 1), 1);
	QT_EQ_F (GetY (h, t, 2), -1);
	// empty keys are ignored
	Call (h, "hash_add", ARGS (F (t), QH_S (h, ""), F (9)));
	QT_EQ_F (QH_Float (h, "hash_get", ARGS (F (t), QH_S (h, ""), F (-1))), -1);
	QH_Free (h);
}

static void TestDeleteReturnsTheNewestValue (void)
{
	qh_t		*h = Harness ();
	float		t = QH_Float (h, "hash_createtab", ARGS (F (8)));
	qc_value_t	r;

	Call (h, "hash_add", ARGS (F (t), QH_S (h, "k"), V (1, 1, 1), F (HASH_ADD)));
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "k"), QH_S (h, "two"), F (HASH_ADD + EV_STRING)));
	QT_EQ_S (QH_String (h, "hash_delete", ARGS (F (t), QH_S (h, "k"))), "two");
	CheckVec (h, "hash_delete", ARGS (F (t), QH_S (h, "k")), 1, 1, 1);
	r = QH_Raw (h, "hash_delete", ARGS (F (t), QH_S (h, "k")));
	QT_CHECK (r.w[0] == 0 && r.w[1] == 0 && r.w[2] == 0);
	CheckVec (h, "hash_get", ARGS (F (t), QH_S (h, "k"), V (4, 4, 4)), 4, 4, 4);
	QH_Free (h);
}

static int CompareStrings (const void *a, const void *b)
{
	return strcmp (*(char *const *)a, *(char *const *)b);
}

static void TestGetkeyEnumeratesEveryEntry (void)
{
	static const char	*keys[5] = {"alpha", "beta", "gamma", "delta", "epsilon"};
	static const char	*sorted[5] = {"alpha", "beta", "delta", "epsilon", "gamma"};
	qh_t				*h = Harness ();
	float				t = QH_Float (h, "hash_createtab", ARGS (F (4)));
	char				*got[5];
	int					i;

	for (i = 0 ; i < 5 ; i++)
		Call (h, "hash_add", ARGS (F (t), QH_S (h, keys[i]), F (1)));
	for (i = 0 ; i < 5 ; i++)
		got[i] = QC_LibDup (QH_String (h, "hash_getkey", ARGS (F (t), F ((float)i))));
	qsort (got, 5, sizeof(got[0]), CompareStrings);
	for (i = 0 ; i < 5 ; i++)
	{
		QT_EQ_S (got[i], sorted[i]);
		free (got[i]);
	}
	QT_EQ_U (QH_Word (h, "hash_getkey", ARGS (F (t), F (5))), 0);
	QT_EQ_U (QH_Word (h, "hash_getkey", ARGS (F (t), F (-1))), 0);
	QH_Free (h);
}

static void TestDestroyedAndInvalidTablesAreBuiltinErrors (void)
{
	qh_t	*h = Harness ();
	float	t = QH_Float (h, "hash_createtab", ARGS (F (8)));

	Call (h, "hash_destroytab", ARGS (F (t)));
	QT_EQ_I (QH_Fails (h, "hash_get", ARGS (F (t), QH_S (h, "k"))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "invalid"));
	QT_EQ_I (QH_Fails (h, "hash_add", ARGS (F (t), QH_S (h, "k"), F (1))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "invalid"));
	QT_EQ_I (QH_Fails (h, "hash_delete", ARGS (F (5), QH_S (h, "k"))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "invalid"));
	QT_EQ_I (QH_Fails (h, "hash_getkey", ARGS (F (-1), F (0))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "invalid"));
	QT_EQ_I (QH_Fails (h, "hash_destroytab", ARGS (F (9))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "invalid"));
	// in developer mode FTE warns and carries on: hash_get returns its default
	QC_SetDeveloper (h->vm, true);
	CheckVec (h, "hash_get", ARGS (F (t), QH_S (h, "k"), V (1, 2, 3)), 1, 2, 3);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_S (QH_WarningText (h, 0), "hash: invalid hash table");
	QH_Free (h);
}

// table 0 is the gamestate table: it's there without being made, holds strings
// by default, can't be destroyed, and hash_createtab never hands it out
static void TestTableZeroIsThePersistentGamestateTable (void)
{
	qh_t	*h = Harness ();

	Call (h, "hash_add", ARGS (F (0), QH_S (h, "map"), QH_S (h, "e1m1")));
	Call (h, "hash_destroytab", ARGS (F (0)));
	QT_EQ_S (QH_String (h, "hash_get", ARGS (F (0), QH_S (h, "map"))), "e1m1");
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (8))), 1);
	QH_Free (h);
}

// the keys cb printed since the last look (sorted, joined by commas), forgotten
static void PrintedKeys (qh_t *h, char *out, size_t size)
{
	char	*keys[64], *text = h->host.printed.text ? h->host.printed.text : "", *line;
	int		n = 0, i;

	out[0] = 0;
	for (line = strtok (text, "\n") ; line && n < 64 ; line = strtok (NULL, "\n"))
		keys[n++] = line;
	qsort (keys, (size_t)n, sizeof(keys[0]), CompareStrings);
	for (i = 0 ; i < n ; i++)
	{
		if (i)
			strncat (out, ",", size - strlen (out) - 1);
		strncat (out, keys[i], size - strlen (out) - 1);
	}
	QT_TextFree (&h->host.printed);
}

static void TestGetcbCallsBackForEveryEntry (void)
{
	static const struct
	{
		const char	*key;
		float		x;
	} adds[3] = {{"a", 1}, {"b", 2}, {"c", 3}};
	qh_t		*h = Harness ();
	float		t = QH_Float (h, "hash_createtab", ARGS (F (4)));
	qc_func_t	cb = QC_FindFunction (h->vm, "cb");
	char		keys[256];
	qc_word_t	*value;
	int			i;

	for (i = 0 ; i < 3 ; i++)
		Call (h, "hash_add", ARGS (F (t), QH_S (h, adds[i].key), V (adds[i].x, adds[i].x, adds[i].x)));
	// FTE ships this builtin doing nothing; this calls the callback as documented
	Call (h, "hash_getcb", ARGS (F (t), W (cb)));
	QT_EQ_F (Global (h, "cb_count")->f, 3);
	PrintedKeys (h, keys, sizeof(keys));
	QT_EQ_S (keys, "a,b,c");
	// only the entries of one key
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "b"), V (5, 5, 5), F (HASH_ADD)));
	Call (h, "hash_getcb", ARGS (F (t), W (cb), QH_S (h, "b")));
	QT_EQ_F (Global (h, "cb_count")->f, 5);
	PrintedKeys (h, keys, sizeof(keys));
	QT_EQ_S (keys, "b,b");
	// the newest first: the last call saw the older value
	value = Global (h, "cb_value");
	QT_CHECK (value[0].f == 2 && value[1].f == 2 && value[2].f == 2);
	// string values come as strings
	Call (h, "hash_add", ARGS (F (0), QH_S (h, "s"), QH_S (h, "text")));
	Call (h, "hash_getcb", ARGS (F (0), W (cb)));
	value = Global (h, "cb_value");
	QT_EQ_S (QH_Text (h, value[0].u), "text");
	QH_Free (h);
}

// entries of key k (all in one bucket, HASH_ADD with EV_STRING) until the
// budget refuses one: how many fit
static int Fill (qh_t *h, float t)
{
	int	n = 0;

	for ( ; ; )
	{
		Call (h, "hash_add", ARGS (F (t), QH_S (h, "k"), QH_S (h, "v"), F (513)));
		if (!QH_Word (h, "hash_get", ARGS (F (t), QH_S (h, "k"), F (0), F (0), F ((float)n))))
			return n;
		n++;
		if (!QT_CHECK (n < 100000))
			return n;
	}
}

// Filling a bucket and deleting everything in it frees its storage as well as
// the budget it was charged: churn can't pile up storage the budget no longer
// counts, and every round fits as many entries as the first.
static void TestDeletedEntriesGiveBackTheirStorage (void)
{
	qc_config_t	config;
	qh_t		*h;
	float		t;
	int			first, round, i;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.container_bytes = 16 * 1024;
	h = QH_New (QC_NUMBERING_CSQC, &config, NULL, NULL);
	t = QH_Float (h, "hash_createtab", ARGS (F (4), F (1)));
	first = Fill (h, t);
	if (!QT_CHECK (first > 20))
		printf ("  %d entries\n", first);
	for (round = 0 ; round < 4 ; round++)
	{
		for (i = 0 ; i < first ; i++)
			Call (h, "hash_delete", ARGS (F (t), QH_S (h, "k")));
		QT_EQ_U (QH_Word (h, "hash_get", ARGS (F (t), QH_S (h, "k"))), 0);
		QT_EQ_I (Fill (h, t), first);
	}
	QH_Free (h);
}

int main (void)
{
	TestTablesAreNumberedFromOneAndReused ();
	TestTheTableLimitIsEnforced ();
	TestValuesDefaultToVectors ();
	TestStringValuesAreCopied ();
	TestTypesCanBeRequired ();
	TestReplaceRemovesOnlyTheNewestEntry ();
	TestDeleteReturnsTheNewestValue ();
	TestGetkeyEnumeratesEveryEntry ();
	TestDestroyedAndInvalidTablesAreBuiltinErrors ();
	TestTableZeroIsThePersistentGamestateTable ();
	TestGetcbCallsBackForEveryEntry ();
	TestDeletedEntriesGiveBackTheirStorage ();
	return QT_Finish ("lib_hash", "hash tables behave as FTE's, with the fixes");
}
