// test_qc_lib_json.c -- JSON parsed into the VM's memory (qcvm-rs's
// tests/all/builtins_misc/json.rs, and the unit tests of stdlib/json.rs)

#include "qc_harness.h"
#include "qc_lib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JSTRING		0
#define JNUMBER		1
#define JOBJECT		2
#define JARRAY		3
#define JTRUE		4
#define JFALSE		5
#define JNULL		6

// the JSON builtins are bound by name only
static const char	*named[] = {"json_parse", "json_free", "json_get_value_type", "json_get_name", "json_get_integer",
	"json_get_float", "json_get_string", "json_find_object_child", "json_get_length", "json_get_child_at_index", NULL};

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, QH_Named, (void *)named);
}

// a harness with a heap of heap bytes, in developer mode (builtin errors are warnings)
static qh_t *HeapHarness (uint32_t heap)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.heap_bytes = heap;
	config.developer = true;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, (void *)named);
}

static uint32_t Parse (qh_t *h, const char *text)
{
	return QH_Word (h, "json_parse", ARGS (QH_S (h, text)));
}

static int32_t Type (qh_t *h, uint32_t n)
{
	return QH_Int (h, "json_get_value_type", ARGS (W (n)));
}

static uint32_t Child (qh_t *h, uint32_t n, const char *key)
{
	return QH_Word (h, "json_find_object_child", ARGS (W (n), QH_S (h, key)));
}

static uint32_t At (qh_t *h, uint32_t n, int32_t index)
{
	return QH_Word (h, "json_get_child_at_index", ARGS (W (n), I (index)));
}

static int32_t Len (qh_t *h, uint32_t n)
{
	return QH_Int (h, "json_get_length", ARGS (W (n)));
}

static float Float (qh_t *h, uint32_t n)
{
	return QH_Float (h, "json_get_float", ARGS (W (n)));
}

static int32_t Integer (qh_t *h, uint32_t n)
{
	return QH_Int (h, "json_get_integer", ARGS (W (n)));
}

// whether a string result (NULL for null) is want (NULL: null)
static bool Is (const char *got, const char *want)
{
	if (!got || !want)
		return got == want;
	return !strcmp (got, want);
}

static bool NameIs (qh_t *h, uint32_t n, const char *want)
{
	return Is (QH_OptString (h, "json_get_name", ARGS (W (n))), want);
}

static bool StringIs (qh_t *h, uint32_t n, const char *want)
{
	return Is (QH_OptString (h, "json_get_string", ARGS (W (n))), want);
}

// a node's four words as they are in memory
static void Words (qh_t *h, uint32_t p, uint32_t out[4])
{
	uint8_t	b[16];

	out[0] = out[1] = out[2] = out[3] = 0;
	if (QT_CHECK (QC_ReadMemory (h->vm, p, b, 16)))
		memcpy (out, b, 16);
}

static char *Repeat (const char *s, int n)
{
	size_t	len = strlen (s);
	char	*out = malloc (len * (size_t)n + 1);
	int		i;

	for (i = 0 ; i < n ; i++)
		memcpy (out + len * (size_t)i, s, len);
	out[len * (size_t)n] = 0;
	return out;
}

// n levels of nested arrays
static char *Nested (int n)
{
	char	*out = malloc ((size_t)n * 2 + 1);

	memset (out, '[', (size_t)n);
	memset (out + n, ']', (size_t)n);
	out[n * 2] = 0;
	return out;
}

static const char	*doc = "{\"name\": \"Ranger\", \"hp\": 100, \"pos\": [1, 2.5, -3], \"alive\": true,\n"
	"    \"dead\": false, \"none\": null, \"nested\": {\"k\": \"v\"}}";

static void TestObjectsArraysAndScalars (void)
{
	static const struct
	{
		const char	*key;
		int32_t		type, asint;
	} flags[] = {{"alive", JTRUE, 1}, {"dead", JFALSE, 0}, {"none", JNULL, 0}};
	qh_t		*h = Harness ();
	uint32_t	root, hp, n, pos, second, third, nested, k;
	size_t		i;

	root = Parse (h, doc);
	QT_CHECK (root != 0);
	QT_EQ_I (Type (h, root), JOBJECT);
	QT_EQ_I (Len (h, root), 7);
	QT_CHECK (NameIs (h, root, NULL));

	hp = Child (h, root, "hp");
	QT_EQ_I (Type (h, hp), JNUMBER);
	QT_EQ_F (Float (h, hp), 100);
	QT_EQ_I (Integer (h, hp), 100);
	QT_CHECK (StringIs (h, hp, NULL));
	QT_CHECK (NameIs (h, hp, "hp"));

	n = Child (h, root, "name");
	QT_EQ_I (Type (h, n), JSTRING);
	QT_CHECK (StringIs (h, n, "Ranger"));
	QT_EQ_F (Float (h, n), 0);

	pos = Child (h, root, "pos");
	QT_EQ_I (Type (h, pos), JARRAY);
	QT_EQ_I (Len (h, pos), 3);
	second = At (h, pos, 1);
	QT_EQ_F (Float (h, second), 2.5);
	QT_EQ_I (Integer (h, second), 2);
	// array elements are named by their index, so they can be found by name too
	QT_CHECK (NameIs (h, second, "1"));
	third = Child (h, pos, "2");
	QT_EQ_F (Float (h, third), -3);

	for (i = 0 ; i < sizeof(flags) / sizeof(flags[0]) ; i++)
	{
		n = Child (h, root, flags[i].key);
		if (!QT_EQ_I (Type (h, n), flags[i].type))
			printf ("  %s\n", flags[i].key);
		QT_EQ_I (Integer (h, n), flags[i].asint);
		QT_EQ_F (Float (h, n), (float)flags[i].asint);
		QT_EQ_I (Len (h, n), 0);
	}
	nested = Child (h, root, "nested");
	k = Child (h, nested, "k");
	QT_CHECK (StringIs (h, k, "v"));

	// misses
	QT_EQ_U (Child (h, root, "missing"), 0);
	QT_EQ_U (Child (h, root, "HP"), 0);
	QT_EQ_U (Child (h, hp, "x"), 0);
	QT_EQ_U (At (h, pos, 3), 0);
	QT_EQ_U (At (h, pos, -1), 0);
	QT_EQ_U (At (h, hp, 0), 0);
	QH_Free (h);
}

static void TestNodesAreLaidOutInOneBlock (void)
{
	qh_t		*h = Harness ();
	uint32_t	root, w[4], first[4], pos, nested;
	double		d;

	root = Parse (h, doc);
	// the root: an object, no name, 7 children right after it
	Words (h, root, w);
	QT_CHECK (w[0] == JOBJECT && w[1] == 0 && w[2] == root + 16 && w[3] == 7);
	Words (h, root + 16, first);
	QT_EQ_U (first[0], JSTRING);
	QT_EQ_S (QH_Text (h, first[1]), "name");
	QT_EQ_S (QH_Text (h, first[2]), "Ranger");
	// the containers' children follow in pre-order: pos's 3 elements, then nested's child
	pos = Child (h, root, "pos");
	QT_EQ_U (pos, root + 3 * 16);
	Words (h, pos, w);
	QT_CHECK (w[2] == root + 8 * 16 && w[3] == 3);
	nested = Child (h, root, "nested");
	Words (h, nested, w);
	QT_CHECK (w[2] == root + 11 * 16 && w[3] == 1);
	// numbers are doubles in the second half
	Words (h, root + 2 * 16, w);
	d = QC_BitsDouble (w[2] | ((uint64_t)w[3] << 32));
	QT_EQ_F (d, 100.0);
	// the names follow the nodes, in the same block
	QT_CHECK (first[1] >= root + 12 * 16);
	// one free releases it all
	QH_Raw (h, "json_free", ARGS (W (root)));
	QT_EQ_I (QH_NumWarnings (h), 0);
	QH_Raw (h, "json_free", ARGS (W (root)));
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Free (h);
}

static void TestKeysAndStringsAreUnescaped (void)
{
	qh_t		*h = Harness ();
	uint32_t	root, n, b;

	root = Parse (h, "{\"a\\\"b\": \"tab\\t quote\\\" \\u00e9 \\ud83d\\ude00 \\/\"}");
	QT_EQ_I (Len (h, root), 1);
	n = At (h, root, 0);
	QT_CHECK (NameIs (h, n, "a\"b"));
	QT_CHECK (StringIs (h, n, "tab\t quote\" \xC3\xA9 \xF0\x9F\x98\x80 /"));
	// a NUL would end a QuakeC string, so it's stored as the overlong C0 80, in keys too
	root = Parse (h, "{\"k\\u0000\": \"a\\u0000b\"}");
	b = At (h, root, 0);
	QT_CHECK (NameIs (h, b, "k\xC0\x80"));
	QT_CHECK (StringIs (h, b, "a\xC0\x80" "b"));
	// string values are temp strings that outlive a collection (the tree refers to them)
	QC_CollectGarbage (h->vm);
	QT_CHECK (StringIs (h, b, "a\xC0\x80" "b"));
	QH_Free (h);
}

static void TestMembersKeepTheirOrderAndDuplicates (void)
{
	static const char	*keys[3] = {"z", "a", "z"};
	static const float	values[3] = {1, 2, 3};
	qh_t				*h = Harness ();
	uint32_t			root, n, z;
	int					k;

	root = Parse (h, "{\"z\": 1, \"a\": 2, \"z\": 3}");
	QT_EQ_I (Len (h, root), 3);
	for (k = 0 ; k < 3 ; k++)
	{
		n = At (h, root, k);
		QT_CHECK (NameIs (h, n, keys[k]));
		QT_EQ_F (Float (h, n), values[k]);
	}
	// a lookup by name finds the first
	z = Child (h, root, "z");
	QT_EQ_F (Float (h, z), 1);
	QH_Free (h);
}

static void TestDocumentsMustBeStrictJson (void)
{
	// FTE's parser takes all of these; strict JSON doesn't
	static const char	*lenient[] = {"// comment\n[1]", "[1, /* block */ 2]", "[1, 2, ]", "{\"a\": 1,}", "[Infinity]",
		"[0x10]", "[abc]", "[TRUE]", "[Null]", "{\"a\": 1, \"b\" , \"c\": 2}", "[\"\\q\"]"};
	static const int32_t	types[4] = {JTRUE, JNULL, JFALSE, JNUMBER};
	static const struct
	{
		const char	*text;
		int32_t		type;
	} scalars[] = {{" 42 ", JNUMBER}, {"\"text\"", JSTRING}, {"[]", JARRAY}, {"{}", JOBJECT}};
	qh_t		*h = Harness ();
	uint32_t	root, n;
	size_t		i;
	int			k;

	for (i = 0 ; i < sizeof(lenient) / sizeof(lenient[0]) ; i++)
		if (!QT_EQ_U (Parse (h, lenient[i]), 0))
			printf ("  %s\n", lenient[i]);
	// a byte-order mark is skipped, and scalars may stand at the root
	root = Parse (h, "\xEF\xBB\xBF[true, null, false, 1e2]");
	for (k = 0 ; k < 4 ; k++)
	{
		n = At (h, root, k);
		QT_EQ_I (Type (h, n), types[k]);
	}
	for (i = 0 ; i < sizeof(scalars) / sizeof(scalars[0]) ; i++)
	{
		n = Parse (h, scalars[i].text);
		if (!QT_EQ_I (Type (h, n), scalars[i].type))
			printf ("  %s\n", scalars[i].text);
		QT_EQ_I (Len (h, n), 0);
	}
	// numbers past 64-bit integers are doubles
	n = Parse (h, "123456789012345678901234567890");
	QT_EQ_F (Float (h, n), 1.2345679e29f);
	QT_EQ_I (Integer (h, n), INT32_MIN);		// x86's conversion
	QH_Free (h);
}

static void TestInvalidDocumentsParseToNull (void)
{
	static const char	*invalid[] = {"", "   ", "{", "[1 2]", "{} x", "\"unterminated", "[,]", "{\"a\" 1}", "]"};
	qh_t				*h = Harness ();
	char				*text;
	size_t				i;

	for (i = 0 ; i < sizeof(invalid) / sizeof(invalid[0]) ; i++)
		if (!QT_EQ_U (Parse (h, invalid[i]), 0))
			printf ("  \"%s\"\n", invalid[i]);
	// nesting is bounded
	text = Nested (200);
	QT_EQ_U (Parse (h, text), 0);
	free (text);
	text = Nested (100);
	QT_CHECK (Parse (h, text) != 0);
	free (text);
	QH_Free (h);
}

static void TestStringsConvertLikeAtoiAndAtof (void)
{
	static const struct
	{
		int32_t	i;
		float	f;
	} want[4] = {{42, 42.0f}, {1, 15.0f}, {-2, -2.9f}, {0, 0.0f}};
	qh_t		*h = Harness ();
	uint32_t	root, n;
	int			k;

	root = Parse (h, "[\"42abc\", \" 1.5e1x\", \"-2.9\", \"x\"]");
	for (k = 0 ; k < 4 ; k++)
	{
		n = At (h, root, k);
		QT_EQ_I (Integer (h, n), want[k].i);
		QT_EQ_F (Float (h, n), want[k].f);
	}
	n = Parse (h, "3.99");
	QT_EQ_I (Integer (h, n), 3);
	QH_Free (h);
}

static void TestNullAndBadNodes (void)
{
	qh_t	*h = Harness ();

	// a null node reads as JSON null
	QT_EQ_I (Type (h, 0), JNULL);
	QT_EQ_I (Len (h, 0), 0);
	QT_CHECK (StringIs (h, 0, NULL));
	QT_CHECK (NameIs (h, 0, NULL));
	QT_EQ_U (At (h, 0, 0), 0);
	// a pointer outside the VM's memory is a builtin error...
	QT_EQ_I (QH_Fails (h, "json_get_value_type", ARGS (W (0x7FFFFFF0))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "bad node"));
	// ...after which FTE carries on with a null node in developer mode
	QC_SetDeveloper (h->vm, true);
	QT_EQ_I (Type (h, 0x7FFFFFF0), JNULL);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Free (h);
}

// A document whose tree can't fit the heap is refused after a pass that
// allocates nothing, however wide; documents that fit still parse.
static void TestDocumentsTooLargeForTheHeapAreRefusedUpFront (void)
{
	qh_t	*h = HeapHarness (64 * 1024);
	char	*items = Repeat ("0,", 200000), *wide = malloc (strlen (items) + 3);
	clock_t	started;

	snprintf (wide, strlen (items) + 3, "[%s]", items);
	started = clock ();
	QT_EQ_U (Parse (h, wide), 0);
	QT_CHECK ((double)(clock () - started) / CLOCKS_PER_SEC < 2.0);
	QT_CHECK (QH_NumWarnings (h) > 0);		// developer mode reports the builtin error as a warning
	QT_CHECK (Parse (h, "[1, 2, {\"a\": \"b\"}]") != 0);
	free (items);
	free (wide);
	QH_Free (h);
}

/*
------------------------------------------------------------------------------
the token soup
------------------------------------------------------------------------------
*/

// walks a parsed tree, checking each container's children; the node count
static int Walk (qh_t *h, uint32_t n, int depth)
{
	char	index[16];
	int32_t	t, k, len;
	int		count = 1;
	uint32_t	c;

	QT_CHECK (depth <= 130);
	t = Type (h, n);
	if (t != JOBJECT && t != JARRAY)
		return 1;
	len = Len (h, n);
	for (k = 0 ; k < len ; k++)
	{
		c = At (h, n, k);
		if (!QT_CHECK (c != 0))
			break;
		if (t == JARRAY)
		{
			snprintf (index, sizeof(index), "%d", k);
			QT_CHECK (NameIs (h, c, index));
		}
		count += Walk (h, c, depth + 1);
	}
	return count;
}

// any mix of JSON's tokens parses to a consistent tree or to null
static void TestTokenSoupParsesConsistently (void)
{
	static const char	*tokens[] = {"[", "]", "{", "}", ",", ":", "\"k\"", "\"\\u0000\"", "1", "-2.5e3", "null", "true",
		" "};
	uint64_t			rng = 0x150A;
	char				text[48 * 16 + 1];
	uint32_t			parts, root, i;
	int					c;
	qh_t				*h;

	for (c = 0 ; c < 128 ; c++)
	{
		text[0] = 0;
		parts = QT_RandBelow (&rng, 48);
		for (i = 0 ; i < parts ; i++)
			strcat (text, tokens[QT_RandBelow (&rng, (uint32_t)(sizeof(tokens) / sizeof(tokens[0])))]);
		h = Harness ();
		root = Parse (h, text);
		if (root && !QT_CHECK (Walk (h, root, 0) >= 1))
			printf ("  %s\n", text);
		QH_Free (h);
	}
}

/*
------------------------------------------------------------------------------
the unit tests of stdlib/json.rs, through json_parse
------------------------------------------------------------------------------
*/

// atof and atoi as json_get_float and json_get_integer read strings with them
static void TestNumbers (void)
{
	size_t	used;

	QT_EQ_F (QC_Strtod ("  -1.5e2x", &used), -150.0);
	QT_EQ_F (QC_Strtod ("0x10", &used), 16.0);
	QT_EQ_F (QC_Strtod ("0x1p-1", &used), 0.5);
	QT_CHECK (isinf (QC_Strtod ("Infinity", &used)));
	QT_CHECK (isnan (QC_Strtod ("nan", &used)));
	QT_EQ_F (QC_Strtod (".5", &used), 0.5);
	QT_EQ_F (QC_Strtod ("abc", &used), 0.0);
	QT_EQ_F (QC_Strtod ("1e", &used), 1.0);
	QT_EQ_I ((int32_t)QC_Strtol (" 42abc", 10), 42);
	QT_EQ_I ((int32_t)QC_Strtol ("-7", 10), -7);
	QT_EQ_I ((int32_t)QC_Strtol ("4294967297", 10), 1);
	QT_EQ_I ((int32_t)QC_Strtol ("99999999999999999999", 10), -1);
}

static void TestStrictGrammar (void)
{
	static const char	*valid[] = {"{}", " [1, \"two\", {\"k\": null}] ", "42"};
	static const char	*invalid[] = {"", "[1,]", "{\"a\":1,}", "[1 2]", "{} x", "// c\n[]", "[abc]", "[TRUE]"};
	// and the grammar serde_json holds strings and numbers to
	static const char	*stricter[] = {
		"[\"\\ud83d\"]", "[\"\\ude00\"]", "[\"\\ud83dx\"]", "[\"\\ud83d\\u0041\"]",		// lone surrogates
		"[\"a\tb\"]", "[\"a\nb\"]", "[\"\x01\"]",										// control characters
		"[\"\xFF\"]", "[\"\xC0\xAF\"]", "[\"\xED\xA0\x80\"]", "[\"\xC3\"]",				// not UTF-8
		"[\"\\u12\"]", "[\"\\x41\"]",													// bad escapes
		"[01]", "[-01]", "-", "[-]", ".5", "[.5]", "1.", "[1.]", "[1e]", "[1e+]", "+1", "[1] x", "1 2",
		"{\"a\":1}}", "{1: 2}", "{\"a\"}", "nul", "tru"};
	static const char	*fine[] = {"[-0]", "[0.5e-3]", "[1E+2]", "[-1.25e-2]", "\"\\u00e9\"", "\"\xC3\xA9\"",
		"[\"\\b\\f\\n\\r\\t\\/\\\\\\\"\"]", "\t\n\r [ ] \t\n\r"};
	qh_t				*h = Harness ();
	char				*text;
	size_t				i;

	for (i = 0 ; i < sizeof(valid) / sizeof(valid[0]) ; i++)
		if (!QT_CHECK (Parse (h, valid[i]) != 0))
			printf ("  %s\n", valid[i]);
	for (i = 0 ; i < sizeof(invalid) / sizeof(invalid[0]) ; i++)
		if (!QT_EQ_U (Parse (h, invalid[i]), 0))
			printf ("  \"%s\"\n", invalid[i]);
	for (i = 0 ; i < sizeof(stricter) / sizeof(stricter[0]) ; i++)
		if (!QT_EQ_U (Parse (h, stricter[i]), 0))
			printf ("  \"%s\"\n", stricter[i]);
	for (i = 0 ; i < sizeof(fine) / sizeof(fine[0]) ; i++)
		if (!QT_CHECK (Parse (h, fine[i]) != 0))
			printf ("  \"%s\"\n", fine[i]);
	// nesting is bounded: 127 levels deep at most (serde_json's limit)
	text = Nested (200);
	QT_EQ_U (Parse (h, text), 0);
	free (text);
	text = Nested (127);
	QT_CHECK (Parse (h, text) != 0);
	free (text);
	text = Nested (128);
	QT_EQ_U (Parse (h, text), 0);
	free (text);
	QH_Free (h);
}

// The layout's size and the containers' child counts are known without making
// anything, and passing the limit stops the parse early. The heap hands out
// 16-byte granules and caps itself at heap_bytes rounded down to 16, so the
// limit is checked at that granularity.
static void TestLayoutSizeAndLimit (void)
{
	const char	*d = "{\"ab\": [1, \"x\"], \"\": null, \"n\\u0000\": {}}";
	qh_t		*h;
	uint32_t	root, ab, empty, n;
	char		*items, *wide;

	// root, "ab", its two elements, the null with no name and "n\0": 6 nodes;
	// the names "ab", "0", "1" and "n" C0 80 (each with its NUL) are 11 bytes,
	// 107 in all, 112 as the heap hands it out
	h = HeapHarness (112);
	root = Parse (h, d);
	if (QT_CHECK (root != 0))
	{
		QT_EQ_I (Walk (h, root, 0), 6);
		QT_EQ_I (Len (h, root), 3);
		ab = Child (h, root, "ab");
		QT_EQ_I (Len (h, ab), 2);
		n = At (h, root, 2);
		QT_EQ_I (Len (h, n), 0);
		// pre-order: the root's children, then ab's
		QT_EQ_U (ab, root + 16);
		QT_EQ_U (At (h, ab, 0), root + 4 * 16);
		// the names after the 6 nodes, in the order they were written
		QT_EQ_U (QH_Word (h, "json_get_name", ARGS (W (ab))), root + 6 * 16);
		QT_EQ_U (QH_Word (h, "json_get_name", ARGS (W (At (h, ab, 0)))), root + 6 * 16 + 3);
		QT_EQ_U (QH_Word (h, "json_get_name", ARGS (W (At (h, ab, 1)))), root + 6 * 16 + 5);
		QT_EQ_U (QH_Word (h, "json_get_name", ARGS (W (n))), root + 6 * 16 + 7);
		QT_CHECK (NameIs (h, n, "n\xC0\x80"));
		// an empty key is a null name, taking no bytes
		empty = At (h, root, 1);
		QT_EQ_I (Type (h, empty), JNULL);
		QT_CHECK (NameIs (h, empty, NULL));
	}
	QT_EQ_I (QH_NumWarnings (h), 0);
	QH_Free (h);

	// one granule less, and the document doesn't fit
	h = HeapHarness (96);
	QT_EQ_U (Parse (h, d), 0);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Free (h);

	h = HeapHarness (64 * 1024);
	items = Repeat ("0,", 100000);
	wide = malloc (strlen (items) + 4);
	snprintf (wide, strlen (items) + 4, "[%s0]", items);
	QT_EQ_U (Parse (h, wide), 0);
	QT_EQ_I (QH_NumWarnings (h), 1);
	free (items);
	free (wide);
	QH_Free (h);
}

static void TestNulIsStoredOverlong (void)
{
	qh_t		*h = Harness ();
	uint32_t	root, n;
	char		*items, *text;

	root = Parse (h, "{\"a\\u0000b\": 1}");
	n = At (h, root, 0);
	QT_CHECK (NameIs (h, n, "a\xC0\x80" "b"));
	QT_EQ_U (strlen (QH_String (h, "json_get_name", ARGS (W (n)))), 4);
	// elements are named by their index's digits: "0" and "10"
	items = Repeat ("0,", 10);
	text = malloc (strlen (items) + 4);
	snprintf (text, strlen (items) + 4, "[%s0]", items);
	root = Parse (h, text);
	QT_EQ_I (Len (h, root), 11);
	QT_CHECK (NameIs (h, At (h, root, 0), "0"));
	QT_CHECK (NameIs (h, At (h, root, 10), "10"));
	free (items);
	free (text);
	QH_Free (h);
}

int main (void)
{
	TestObjectsArraysAndScalars ();
	TestNodesAreLaidOutInOneBlock ();
	TestKeysAndStringsAreUnescaped ();
	TestMembersKeepTheirOrderAndDuplicates ();
	TestDocumentsMustBeStrictJson ();
	TestInvalidDocumentsParseToNull ();
	TestStringsConvertLikeAtoiAndAtof ();
	TestNullAndBadNodes ();
	TestDocumentsTooLargeForTheHeapAreRefusedUpFront ();
	TestTokenSoupParsesConsistently ();
	TestNumbers ();
	TestStrictGrammar ();
	TestLayoutSizeAndLimit ();
	TestNulIsStoredOverlong ();
	return QT_Finish ("lib_json", "JSON parses strictly into one heap block QuakeC walks");
}
