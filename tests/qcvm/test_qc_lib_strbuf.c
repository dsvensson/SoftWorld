// test_qc_lib_strbuf.c -- string buffers (qcvm-rs's tests/all/builtins_misc/strbuf.rs,
// and the unit tests of src/stdlib/strbuf.rs)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOLE	NULL

// a call that must succeed
static void Call (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	char	text[1024];

	if (!QT_CHECK (QH_Call (h, name, argc, args, NULL)))
		printf ("  %s: %s\n", name, QC_ErrorText (QC_LastError (h->vm), text, sizeof(text)));
}

// entry i of buffer b, NULL for a null reference
static const char *Get (qh_t *h, float b, float i)
{
	return QH_OptString (h, "bufstr_get", ARGS (F (b), F (i)));
}

static void Set (qh_t *h, float b, float i, const char *text)
{
	Call (h, "bufstr_set", ARGS (F (b), F (i), QH_S (h, text)));
}

// the buffer's contents are want[0..count) (HOLE for a hole)
static void CheckContents (qh_t *h, float b, const char *const *want, int count, int line)
{
	int			n = (int)QH_Float (h, "buf_getsize", ARGS (F (b))), i;
	const char	*got;
	bool		ok = n == count;

	for (i = 0 ; ok && i < n ; i++)
	{
		got = Get (h, b, (float)i);
		ok = want[i] ? got && !strcmp (got, want[i]) : !got;
	}
	if (!QT_CHECK (ok))
	{
		printf ("  line %d: buffer %g has", line, (double)b);
		for (i = 0 ; i < n ; i++)
		{
			got = Get (h, b, (float)i);
			printf (" %s%s%s", got ? "\"" : "", got ? got : "hole", got ? "\"" : "");
		}
		printf ("\n");
	}
}

#define CONTENTS(h, b, ...)	CheckContents ((h), (b), (const char *const []){__VA_ARGS__}, \
	(int)(sizeof ((const char *const []){__VA_ARGS__}) / sizeof (const char *)), __LINE__)

static qh_t *Limited (void (*edit) (qc_config_t *config))
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	edit (&config);
	return QH_New (QC_NUMBERING_CSQC, &config, NULL, NULL);
}

static void TestCreateSetGetAndHoles (void)
{
	qh_t	*h = QH_Csqc ();

	QT_EQ_F (QH_Float (h, "buf_create", NOARGS), 1);
	QT_EQ_F (QH_Float (h, "buf_create", ARGS (QH_S (h, "STRING"), F (0))), 2);
	// only string buffers exist
	QT_EQ_F (QH_Float (h, "buf_create", ARGS (QH_S (h, "float"))), -1);
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (1))), 0);
	Set (h, 1, 2, "two");
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (1))), 3);
	CONTENTS (h, 1, HOLE, HOLE, "two");
	QT_CHECK (!Get (h, 1, 3));
	QT_CHECK (!Get (h, 1, -1));
	// entries are copies that outlive the temp strings they came from
	QC_CollectGarbage (h->vm);
	QT_EQ_S (Get (h, 1, 2), "two");
	// the handles of deleted buffers are reused
	Call (h, "buf_del", ARGS (F (1)));
	QT_EQ_F (QH_Float (h, "buf_create", NOARGS), 1);
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (1))), 0);
	QH_Free (h);
}

static void TestAddFreeAndCopy (void)
{
	qh_t	*h = QH_Csqc ();
	float	b = QH_Float (h, "buf_create", NOARGS), c;

	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "a"), F (1))), 0);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "b"), F (1))), 1);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "c"), F (1))), 2);
	Call (h, "bufstr_free", ARGS (F (b), F (1)));
	// freeing leaves a hole
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (b))), 3);
	// unordered adds fill the first hole, ordered ones append
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "d"), F (1))), 3);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "e"), F (0))), 1);
	CONTENTS (h, b, "a", "e", "c", "d");
	Call (h, "bufstr_free", ARGS (F (b), F (9)));
	Call (h, "bufstr_free", ARGS (F (b), F (2)));

	c = QH_Float (h, "buf_create", NOARGS);
	Set (h, c, 7, "gone");
	Call (h, "buf_copy", ARGS (F (b), F (c)));
	CONTENTS (h, c, "a", "e", HOLE, "d");
	// a deep copy
	Set (h, b, 0, "changed");
	QT_EQ_S (Get (h, c, 0), "a");
	// copying onto itself, or from or to an invalid handle, does nothing
	Call (h, "buf_copy", ARGS (F (c), F (c)));
	Call (h, "buf_copy", ARGS (F (c), F (99)));
	CONTENTS (h, c, "a", "e", HOLE, "d");
	QH_Free (h);
}

static void TestSortCompactsHolesFirst (void)
{
	qh_t	*h = QH_Csqc ();
	float	b = QH_Float (h, "buf_create", NOARGS);

	Set (h, b, 0, "pear");
	Set (h, b, 2, "apple");
	Set (h, b, 3, "fig");
	Set (h, b, 5, "apricot");
	Call (h, "buf_sort", ARGS (F (b), F (0), F (0)));
	CONTENTS (h, b, "apple", "apricot", "fig", "pear");
	Call (h, "buf_sort", ARGS (F (b), F (0), F (1)));
	CONTENTS (h, b, "pear", "fig", "apricot", "apple");
	// only the first byte counts: apricot and apple compare equal (the sort is
	// stable here, unspecified in FTE)
	Call (h, "buf_sort", ARGS (F (b), F (1), F (0)));
	CONTENTS (h, b, "apricot", "apple", "fig", "pear");
	QH_Free (h);
}

static void TestImplodeGluesOnlyAfterOutput (void)
{
	qh_t		*h = QH_Csqc ();
	float		b = QH_Float (h, "buf_create", NOARGS), e;
	uint32_t	r;

	Set (h, b, 0, "");
	Set (h, b, 1, "a");
	Set (h, b, 3, "b");
	Set (h, b, 4, "");
	// the empty first entry makes no output, so no glue before a; the hole is skipped
	QT_EQ_S (QH_String (h, "buf_implode", ARGS (F (b), QH_S (h, ", "))), "a, b, ");
	e = QH_Float (h, "buf_create", NOARGS);
	r = QH_Word (h, "buf_implode", ARGS (F (e), QH_S (h, ",")));
	QT_CHECK (r != 0);
	QT_EQ_S (QH_Text (h, r), "");
	QH_Free (h);
}

// FTE leaves the result as it was; this returns 0 or null
static void TestInvalidHandlesReturnZeroOrNull (void)
{
	qh_t	*h = QH_Csqc ();
	float	b = QH_Float (h, "buf_create", NOARGS), bad[4];
	int		i;

	Call (h, "buf_del", ARGS (F (b)));
	bad[0] = 0;
	bad[1] = b;
	bad[2] = 7;
	bad[3] = -1;
	for (i = 0 ; i < 4 ; i++)
	{
		QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (bad[i]))), 0);
		QT_EQ_U (QH_Word (h, "buf_implode", ARGS (F (bad[i]), QH_S (h, ","))), 0);
		QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (bad[i]), QH_S (h, "x"), F (1))), 0);
		QT_CHECK (!Get (h, bad[i], 0));
		QT_EQ_F (QH_Float (h, "bufstr_find", ARGS (F (bad[i]), QH_S (h, "x"), F (1))), -1);
		Set (h, bad[i], 0, "x");
		Call (h, "bufstr_free", ARGS (F (bad[i]), F (0)));
		Call (h, "buf_sort", ARGS (F (bad[i]), F (0), F (0)));
		Call (h, "buf_del", ARGS (F (bad[i])));
	}
	QT_EQ_I (QH_NumWarnings (h), 0);
	QH_Free (h);
}

static void TestSetRefusesAbsurdIndices (void)
{
	qh_t	*h = QH_Csqc ();
	float	b = QH_Float (h, "buf_create", NOARGS);

	Set (h, b, 2000000, "far");
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (b))), 0);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_S (QH_WarningText (h, 0), "bufstr_set: index outside sanity range");
	QH_Free (h);
}

static void OneBufferOfThree (qc_config_t *config)
{
	config->limits.string_buffers = 1;
	config->limits.string_buffer_entries = 3;
}

static void TestLimitsAreEnforced (void)
{
	qh_t	*h = Limited (OneBufferOfThree);
	float	b = QH_Float (h, "buf_create", NOARGS);
	int		i;

	QT_EQ_F (QH_Float (h, "buf_create", NOARGS), -1);
	Set (h, b, 3, "no");
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (b))), 0);
	for (i = 0 ; i < 3 ; i++)
		QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "x"), F (1))), i);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, "x"), F (1))), -1);
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (b))), 3);
	QT_EQ_I (QH_NumWarnings (h), 2);
	Call (h, "buf_del", ARGS (F (b)));
	QT_EQ_F (QH_Float (h, "buf_create", NOARGS), 1);
	QH_Free (h);
}

static float Find (qh_t *h, float b, const char *pattern, float rule, int extra, float start, float step)
{
	if (extra == 0)
		return QH_Float (h, "bufstr_find", ARGS (F (b), QH_S (h, pattern), F (rule)));
	if (extra == 1)
		return QH_Float (h, "bufstr_find", ARGS (F (b), QH_S (h, pattern), F (rule), F (start)));
	return QH_Float (h, "bufstr_find", ARGS (F (b), QH_S (h, pattern), F (rule), F (start), F (step)));
}

static void TestFindByRule (void)
{
	qh_t	*h = QH_Csqc ();
	float	b = QH_Float (h, "buf_create", NOARGS);

	Set (h, b, 0, "maps/e1m1.bsp");
	Set (h, b, 1, "maps/E1M2.bsp");
	Set (h, b, 3, "progs/player.mdl");
	Set (h, b, 4, "maps/sub/e1m3.bsp");
	Set (h, b, 5, "e1m1");
	QT_EQ_F (Find (h, b, "e1m1", 1, 0, 0, 0), 5);
	QT_EQ_F (Find (h, b, "maps/", 2, 0, 0, 0), 0);
	QT_EQ_F (Find (h, b, ".mdl", 3, 0, 0, 0), 3);
	QT_EQ_F (Find (h, b, "player", 4, 0, 0, 0), 3);
	QT_EQ_F (Find (h, b, "", 4, 0, 0, 0), 0);
	// wildcards: letters in either case, * doesn't cross a slash
	QT_EQ_F (Find (h, b, "maps/e1m?.bsp", 5, 1, 1, 0), 1);
	QT_EQ_F (Find (h, b, "maps/*.bsp", 0, 1, 2, 0), -1);	// not maps/sub/e1m3.bsp
	QT_EQ_F (Find (h, b, "maps/*/*", 5, 0, 0, 0), 4);
	QT_EQ_F (Find (h, b, "*", 5, 0, 0, 0), 5);
	QT_EQ_F (Find (h, b, "zzz", 9, 0, 0, 0), -1);			// unknown rules are wildcards
	// start and step
	QT_EQ_F (Find (h, b, "maps/", 2, 1, 1, 0), 1);
	QT_EQ_F (Find (h, b, "maps/", 2, 2, 2, 2), 4);		// 2 is a hole
	QT_EQ_F (Find (h, b, "maps/", 2, 2, 1, 3), 1);
	QT_EQ_F (Find (h, b, "e1m", 2, 2, 0, 4), -1);
	QT_EQ_F (Find (h, b, "maps/", 2, 1, -1, 0), -1);
	QT_EQ_F (Find (h, b, "maps/", 2, 2, 0, 0), -1);
	QT_EQ_F (Find (h, b, "maps/", 2, 1, 9, 0), -1);
	QH_Free (h);
}

static void TestCvarlistAndLoadfileUseTheHost (void)
{
	static const char	lines[] = "one\r\ntwo\n\nthree";
	qh_t				*h = QH_Csqc ();
	float				b;

	QH_SetCvar (h, "sv_gravity", "800");
	QH_SetCvar (h, "cl_yawspeed", "140");
	QH_SetCvar (h, "sv_friction", "4");
	QH_AddFile (h, "lines.txt", lines, sizeof(lines) - 1);
	b = QH_Float (h, "buf_create", NOARGS);
	Set (h, b, 5, "old");
	Call (h, "buf_cvarlist", ARGS (F (b), QH_S (h, "sv_"), QH_S (h, "")));
	CONTENTS (h, b, "sv_friction", "sv_gravity");

	QT_EQ_F (QH_Float (h, "buf_loadfile", ARGS (QH_S (h, "missing.txt"), F (b))), 0);
	QT_EQ_F (QH_Float (h, "buf_loadfile", ARGS (QH_S (h, "lines.txt"), F (b))), 1);
	// the lines are appended, without their line breaks
	CONTENTS (h, b, "sv_friction", "sv_gravity", "one", "two", "", "three");
	QT_EQ_F (QH_Float (h, "buf_loadfile", ARGS (QH_S (h, "lines.txt"), F (99))), 0);
	QH_Free (h);
}

static void SixteenKiB (qc_config_t *config)
{
	config->limits.container_bytes = 16 * 1024;
}

// Hash tables and string buffers share one memory budget (limits.container_bytes):
// adds past it fail with a warning, and freeing gives the room back.
static void TestHashTablesAndBuffersShareAMemoryBudget (void)
{
	qh_t	*h = Limited (SixteenKiB);
	char	*big = malloc (6001);
	float	t, b;

	memset (big, 'x', 6000);
	big[6000] = 0;
	// a table of 65536 buckets needs far more than 16 KiB
	QT_EQ_F (QH_Float (h, "hash_createtab", ARGS (F (65536))), 0);
	t = QH_Float (h, "hash_createtab", ARGS (F (16), F (1)));
	QT_CHECK (t > 0);

	b = QH_Float (h, "buf_create", NOARGS);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, big), F (1))), 0);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, big), F (1))), 1);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, big), F (1))), -1);		// over the budget
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "key"), QH_S (h, big)));
	QT_EQ_U (QH_Word (h, "hash_get", ARGS (F (t), QH_S (h, "key"))), 0);			// not stored
	QT_CHECK (QH_NumWarnings (h) > 0);

	// freeing an entry makes room again
	Call (h, "bufstr_free", ARGS (F (b), F (0)));
	Call (h, "hash_add", ARGS (F (t), QH_S (h, "key"), QH_S (h, big)));
	QT_EQ_U (strlen (QH_String (h, "hash_get", ARGS (F (t), QH_S (h, "key")))), 6000);
	// deleting the buffer and the table gives everything back
	Call (h, "buf_del", ARGS (F (b)));
	Call (h, "hash_destroytab", ARGS (F (t)));
	QT_CHECK (QH_Float (h, "hash_createtab", ARGS (F (64))) > 0);
	b = QH_Float (h, "buf_create", NOARGS);
	QT_EQ_F (QH_Float (h, "bufstr_add", ARGS (F (b), QH_S (h, big), F (1))), 0);
	free (big);
	QH_Free (h);
}

static void SixtyFourKiBOfTemps (qc_config_t *config)
{
	config->limits.temp_string_bytes = 64 * 1024;
}

// buf_implode sizes its result (the glue repeats once an entry) before making
// it, and refuses one the temp strings have no room for
static void TestBufImplodeRefusesOversizedResultsUpFront (void)
{
	qh_t	*h = Limited (SixtyFourKiBOfTemps);
	float	b = QH_Float (h, "buf_create", NOARGS);
	char	*glue = malloc (4097);
	int		i;

	for (i = 0 ; i < 64 ; i++)
		Call (h, "bufstr_add", ARGS (F (b), QH_S (h, "x"), F (1)));
	memset (glue, '-', 4096);
	glue[4096] = 0;
	QT_EQ_I (QH_Fails (h, "buf_implode", ARGS (F (b), QH_S (h, glue))), QC_ERR_OUT_OF_MEMORY);
	QT_EQ_I (QC_LastError (h->vm)->value, QC_RES_TEMP_STRINGS);
	// a result that fits is still made
	QT_EQ_U (strlen (QH_String (h, "buf_implode", ARGS (F (b), QH_S (h, ",")))), 64 + 63);
	free (glue);
	QH_Free (h);
}

/*
==============================================================================

THE UNIT TESTS OF src/stdlib/strbuf.rs

==============================================================================
*/

static bool Wild (const char *pattern, const char *s)
{
	return QC_WildCompare (pattern, s, strlen (s));
}

static void TestWildcards (void)
{
	QT_CHECK (Wild ("*", ""));
	QT_CHECK (Wild ("*", "abc"));
	QT_CHECK (Wild ("a*c", "ABC"));
	QT_CHECK (Wild ("a?c", "axc"));
	QT_CHECK (!Wild ("a?c", "ac"));
	QT_CHECK (!Wild ("a*", "a/b"));
	QT_CHECK (Wild ("a*/b", "ax/b"));
	QT_CHECK (Wild ("a?b", "a/b"));
	QT_CHECK (Wild ("**x", "abx"));
	QT_CHECK (!Wild ("", "x"));
	QT_CHECK (Wild ("", ""));
}

// the line splitter, through buf_loadfile: \r\n and \n end lines, a last line
// without one keeps its \r, a NUL ends a line, an empty file has none
static void TestLines (void)
{
	static const char	crlf[] = "a\r\nb\n\nc\r", nul[] = "x\0y\n";
	qh_t				*h = QH_Csqc ();
	float				b;

	QH_AddFile (h, "crlf.txt", crlf, sizeof(crlf) - 1);
	QH_AddFile (h, "nul.txt", nul, sizeof(nul) - 1);
	QH_AddFile (h, "empty.txt", "", 0);
	b = QH_Float (h, "buf_create", NOARGS);
	QT_EQ_F (QH_Float (h, "buf_loadfile", ARGS (QH_S (h, "crlf.txt"), F (b))), 1);
	CONTENTS (h, b, "a", "b", "", "c\r");
	b = QH_Float (h, "buf_create", NOARGS);
	QT_EQ_F (QH_Float (h, "buf_loadfile", ARGS (QH_S (h, "nul.txt"), F (b))), 1);
	CONTENTS (h, b, "x");
	b = QH_Float (h, "buf_create", NOARGS);
	QT_EQ_F (QH_Float (h, "buf_loadfile", ARGS (QH_S (h, "empty.txt"), F (b))), 1);
	QT_EQ_F (QH_Float (h, "buf_getsize", ARGS (F (b))), 0);
	QH_Free (h);
}

int main (void)
{
	TestCreateSetGetAndHoles ();
	TestAddFreeAndCopy ();
	TestSortCompactsHolesFirst ();
	TestImplodeGluesOnlyAfterOutput ();
	TestInvalidHandlesReturnZeroOrNull ();
	TestSetRefusesAbsurdIndices ();
	TestLimitsAreEnforced ();
	TestFindByRule ();
	TestCvarlistAndLoadfileUseTheHost ();
	TestHashTablesAndBuffersShareAMemoryBudget ();
	TestBufImplodeRefusesOversizedResultsUpFront ();
	TestWildcards ();
	TestLines ();
	return QT_Finish ("lib_strbuf", "string buffers behave as FTE's, with the fixes");
}
