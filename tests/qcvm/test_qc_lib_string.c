// test_qc_lib_string.c -- lengths, substrings, characters, conversions,
// comparisons, markup, info strings, URIs and the menu's string builtins
// (qcvm-rs's tests/all/builtins_strings/string.rs and the unit tests of
// stdlib/string.rs, docs/spec/strings.md)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// builtins FTE binds by name that its CSQC declarations don't list
static const char	*extra[] = {"argc", "instr", "ftou", "utof", "strcmp", NULL};

// CSQC with the given charset settings
static qh_t *WithCharset (bool utf8, qc_charscheme_t scheme)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.utf8 = utf8;
	config.charscheme = scheme;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, extra);
}

// CSQC with QuakeWorld's charset defaults (utf8_enable 0, the Quake scheme)
static qh_t *Harness (void)
{
	return WithCharset (false, QC_CHARS_QUAKE);
}

// menu QuakeC, for the menu's own builtins
static qh_t *Menu (void)
{
	return QH_New (QC_NUMBERING_MENU, NULL, NULL, NULL);
}

// n copies of c (malloc'd)
static char *Rep (char c, size_t n)
{
	char	*s = malloc (n + 1);

	memset (s, c, n);
	s[n] = 0;
	return s;
}

// the texts joined, up to a NULL (malloc'd)
static char *Join (const char *first, ...)
{
	va_list		args;
	const char	*part;
	size_t		len = 0;
	char		*out;

	va_start (args, first);
	for (part = first ; part ; part = va_arg (args, const char *))
		len += strlen (part);
	va_end (args);
	out = malloc (len + 1);
	out[0] = 0;
	va_start (args, first);
	for (part = first ; part ; part = va_arg (args, const char *))
		strcat (out, part);
	va_end (args);
	return out;
}

// a string result's text and its expectation
static void Eq (qh_t *h, const char *name, int argc, const qc_value_t *args, const char *want)
{
	if (!QT_EQ_S (QH_String (h, name, argc, args), want))
		printf ("  %s\n", name);
}

// a result that is a new temp string, empty
static void CheckEmptyTemp (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	uint32_t	r = QH_Word (h, name, argc, args);

	if (!QT_CHECK (r != 0 && QC_IsTempString (r)))
		printf ("  %s\n", name);
	QT_EQ_S (QH_Text (h, r), "");
}

// string results are new temps, never null
static void TestResultsAreTemps (void)
{
	qh_t	*h = Harness ();

	CheckEmptyTemp (h, "substring", ARGS (QH_S (h, "abc"), F (5), F (1)));
	CheckEmptyTemp (h, "strcat", NOARGS);
	CheckEmptyTemp (h, "strtrim", ARGS (QH_S (h, "   ")));
	CheckEmptyTemp (h, "strreplace", ARGS (QH_S (h, "a"), QH_S (h, ""), QH_S (h, "aaa")));
	CheckEmptyTemp (h, "infoget", ARGS (QH_S (h, ""), QH_S (h, "x")));
	CheckEmptyTemp (h, "strdecolorize", ARGS (QH_S (h, "^1")));
	QH_Free (h);
}

static void TestStrlenStrcatStrzone (void)
{
	qh_t		*h = Harness ();
	qc_value_t	args[8];
	char		*big;
	int			k;

	QT_EQ_F (QH_Float (h, "strlen", ARGS (QH_S (h, "hello"))), 5);
	QT_EQ_F (QH_Float (h, "strlen", ARGS (W (0))), 0);
	QT_EQ_F (QH_Float (h, "strlen", ARGS (QH_S (h, "h\xC3\xA9llo"))), 6);
	QT_EQ_F (QH_Float (h, "memstrsize", ARGS (QH_S (h, "h\xC3\xA9llo"))), 6);
	Eq (h, "strcat", ARGS (QH_S (h, "a"), QH_S (h, "b"), QH_S (h, "c")), "abc");
	Eq (h, "strcat", ARGS (QH_S (h, "a")), "a");
	for (k = 0 ; k < 8 ; k++)
		args[k] = QH_S (h, k % 2 ? "y" : "x");
	Eq (h, "strcat", 8, args, "xyxyxyxy");
	// no length limit
	big = Rep ('z', 100000);
	QT_EQ_U (strlen (QH_String (h, "strcat", ARGS (QH_S (h, big), QH_S (h, big)))), 200000);
	free (big);
	Eq (h, "strzone", ARGS (QH_S (h, "a"), QH_S (h, "b")), "ab");
	QT_CHECK (QH_Call (h, "strunzone", ARGS (QH_S (h, "a")), NULL));
	QH_Free (h);
}

static void TestSubstring (void)
{
	static const struct
	{
		float		start, len;
		const char	*want;
	} cases[] = {
		{1, 3, "ell"},
		{-3, 2, "ll"},
		{1, -1, "ello"},
		{0, -2, "hell"},
		{-10, 3, "hel"},
		{-10, -1, "hello"},
		{3, 100, "lo"},
		{5, 1, ""},
		{2, 0, ""},
		{4.9f, 1, "o"},
		{0, 3e9f, ""},
		{-1, -1, "o"},
		{0, 5, "hello"},
	};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_S (QH_String (h, "substring", ARGS (QH_S (h, "hello"), F (cases[i].start), F (cases[i].len))),
			cases[i].want))
			printf ("  substring(hello, %g, %g)\n", (double)cases[i].start, (double)cases[i].len);
	QH_Free (h);
}

static void TestStrstrofs (void)
{
	qh_t	*h = Harness ();

	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abcabc"), QH_S (h, "bc"))), 1);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abcabc"), QH_S (h, "bc"), F (2))), 4);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abc"), QH_S (h, ""))), 0);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abc"), QH_S (h, ""), F (3))), 3);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abc"), QH_S (h, ""), F (4))), -1);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abc"), QH_S (h, "a"), F (-1))), -1);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abc"), QH_S (h, "x"))), -1);
	QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, "abc"), QH_S (h, "c"), F (2))), 2);
	QH_Free (h);
}

static void TestStr2chr (void)
{
	static const float	cases[][2] = {{0, 97}, {2, 99}, {3, 0}, {4, 0}, {-1, 99}, {-3, 97}, {-4, 0}, {1.9f, 98},
		{-0.5f, 97}};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_F (QH_Float (h, "str2chr", ARGS (QH_S (h, "abc"), F (cases[i][0]))), cases[i][1]))
			printf ("  str2chr(abc, %g)\n", (double)cases[i][0]);
	QT_EQ_F (QH_Float (h, "str2chr", ARGS (QH_S (h, "abc"))), 97);
	QT_EQ_F (QH_Float (h, "str2chr", ARGS (QH_S (h, "\xE1"), F (0))), 225);
	QH_Free (h);
	h = WithCharset (true, QC_CHARS_QUAKE);
	QT_EQ_F (QH_Float (h, "str2chr", ARGS (QH_S (h, "\xE1"), F (0))), (float)0xE0E1);
	QH_Free (h);
}

static void TestChr2str (void)
{
	qh_t	*h = Harness ();

	Eq (h, "chr2str", ARGS (F (72), F (105)), "Hi");
	Eq (h, "chr2str", ARGS (F (225)), "\xE1");
	Eq (h, "chr2str", ARGS (F (9786)), "^U263a");
	Eq (h, "chr2str", ARGS (F (256)), "^U0100");
	Eq (h, "chr2str", ARGS (F (128512)), "^{1f600}");
	Eq (h, "chr2str", ARGS (F (57537)), "\xC1");
	Eq (h, "chr2str", ARGS (F (57354)), "^Ue00a");
	Eq (h, "chr2str", ARGS (F (65), F (0), F (66)), "A");
	Eq (h, "chr2str", ARGS (F (-1)), "\xFF");
	Eq (h, "chr2str", ARGS (F (-128)), "\x80");
	// the Quake encoder round-trips \v (FTE can't write U+E00B)
	Eq (h, "chr2str", ARGS (F (57355)), "\x0B");
	QH_Free (h);
	h = WithCharset (false, QC_CHARS_UTF8);
	Eq (h, "chr2str", ARGS (F (9786)), "\xE2\x98\xBA");
	Eq (h, "chr2str", ARGS (F (57409)), "\xEE\x81\x81");
	Eq (h, "chr2str", ARGS (F (200)), "\xC8");
	QH_Free (h);
	h = WithCharset (true, QC_CHARS_QUAKE);
	Eq (h, "chr2str", ARGS (F (200)), "?");
	Eq (h, "chr2str", ARGS (F (0)), "?");
	Eq (h, "chr2str", ARGS (F (65), F (57537)), "A\xC1");
	QH_Free (h);
	h = WithCharset (true, QC_CHARS_UTF8);
	Eq (h, "chr2str", ARGS (F (0)), "\xC0\x80");
	Eq (h, "chr2str", ARGS (F (200)), "\xC3\x88");
	Eq (h, "chr2str", ARGS (F (-1)), "\xEF\xBF\xBD");
	QH_Free (h);
}

static void TestCaseConversion (void)
{
	qh_t	*h = Harness ();
	char	*long_a;

	Eq (h, "strtoupper", ARGS (QH_S (h, "Hello, World!")), "HELLO, WORLD!");
	Eq (h, "strtolower", ARGS (QH_S (h, "Hello, World!")), "hello, world!");
	Eq (h, "strtoupper", ARGS (QH_S (h, "\xE1" "bc")), "\xE1" "BC");
	// fixed: FTE turns \v into '?'
	Eq (h, "strtoupper", ARGS (QH_S (h, "a\x0b" "b")), "A\x0b" "B");
	Eq (h, "strtolower", ARGS (QH_S (h, "\x01\x7F\xFF\n\tX")), "\x01\x7F\xFF\n\tx");
	QH_Free (h);
	h = WithCharset (false, QC_CHARS_UTF8);
	Eq (h, "strtoupper", ARGS (QH_S (h, "h\xC3\xA9llo")), "H\xC3\xA9LLO");
	Eq (h, "strtoupper", ARGS (QH_S (h, "\x80")), "\xEE\x82\x80");
	Eq (h, "strtoupper", ARGS (QH_S (h, "\xE1" "bc")), "\xEF\xBF\xBD" "BC");
	Eq (h, "strtoupper", ARGS (QH_S (h, "\xC1\xA1")), "A");
	Eq (h, "strtoupper", ARGS (QH_S (h, "\xEE\x81\xA1")), "\xEE\x81\x81");
	Eq (h, "strtolower", ARGS (QH_S (h, "\xEE\x81\x81\xC3\x89")), "\xEE\x81\xA1\xC3\x89");
	QH_Free (h);
	h = WithCharset (true, QC_CHARS_ISO8859_1);
	Eq (h, "strtoupper", ARGS (QH_S (h, "\xE9" "a")), "\xE9" "A");
	QH_Free (h);
	// the output is 8191 bytes at most
	h = Harness ();
	long_a = Rep ('a', 9000);
	QT_EQ_U (strlen (QH_String (h, "strtoupper", ARGS (QH_S (h, long_a)))), 8191);
	free (long_a);
	QH_Free (h);
}

static const char *Strconv (qh_t *h, float c, float a, float n, const char *text)
{
	return QH_String (h, "strconv", ARGS (F (c), F (a), F (n), QH_S (h, text)));
}

static void TestStrconv (void)
{
	qh_t	*h = Harness ();
	char	*long_x;

	QT_EQ_S (Strconv (h, 1, 0, 0, "Hello WORLD"), "hello world");
	QT_EQ_S (Strconv (h, 2, 0, 0, "\xE8i"), "\xC8I");
	QT_EQ_S (Strconv (h, 0, 2, 2, "Score: 10"), "\xD3\xE3\xEF\xF2\xE5\xBA\xA0\xB1\xB0");
	QT_EQ_S (Strconv (h, 0, 1, 1, "\xD3\xE3\xEF\xF2\xE5\xBA\xA0\xB1\xB0"), "Score: 10");
	QT_EQ_S (Strconv (h, 0, 0, 3, "123"), "\x13\x14\x15");
	QT_EQ_S (Strconv (h, 0, 0, 4, "05"), "\x92\x97");
	QT_EQ_S (Strconv (h, 0, 5, 0, "ab cd"), "\xE1" "b c\xE4");
	QT_EQ_S (Strconv (h, 0, 6, 0, "abcd"), "a\xE2" "c\xE4");
	QT_EQ_S (Strconv (h, 0, 0, 5, "\x92"), "\x92");
	QT_EQ_S (Strconv (h, 0, 2, 0, "\x01\x8F!"), "\x01\x8F\xA1");
	// the arguments after the third are joined; the input is cut to 4095 bytes
	Eq (h, "strconv", ARGS (F (2), F (0), F (0), QH_S (h, "a"), QH_S (h, "b")), "AB");
	long_x = Rep ('x', 5000);
	QT_EQ_U (strlen (Strconv (h, 0, 0, 0, long_x)), 4095);
	free (long_x);
	QH_Free (h);
}

static void TestStrpadStrtrim (void)
{
	qh_t	*h = Harness ();
	char	*long_a;

	Eq (h, "strpad", ARGS (F (5), QH_S (h, "ab")), "ab   ");
	Eq (h, "strpad", ARGS (F (-5), QH_S (h, "ab")), "   ab");
	Eq (h, "strpad", ARGS (F (2), QH_S (h, "abcd")), "abcd");
	Eq (h, "strpad", ARGS (F (-2), QH_S (h, "abcd")), "abcd");
	Eq (h, "strpad", ARGS (F (-3), QH_S (h, "a"), QH_S (h, "b")), " ab");
	Eq (h, "strpad", ARGS (F (-5.5f), QH_S (h, "a")), "    a");
	QT_EQ_U (strlen (QH_String (h, "strpad", ARGS (F (10000), QH_S (h, "a")))), 4095);
	QT_EQ_U (strlen (QH_String (h, "strpad", ARGS (F (-10000), QH_S (h, "a")))), 4095);
	long_a = Rep ('a', 5000);
	QT_EQ_U (strlen (QH_String (h, "strpad", ARGS (F (0), QH_S (h, long_a)))), 4095);
	free (long_a);
	Eq (h, "strtrim", ARGS (QH_S (h, "  a b \n")), "a b");
	Eq (h, "strtrim", ARGS (QH_S (h, "\x0b x")), "\x0b x");
	Eq (h, "strtrim", ARGS (QH_S (h, "\t\r\nx\x0c")), "x\x0c");
	QH_Free (h);
}

static void TestStrreplace (void)
{
	qh_t	*h = Harness ();
	char	*long_a, *long_y;

	Eq (h, "strreplace", ARGS (QH_S (h, "a"), QH_S (h, "bb"), QH_S (h, "banana")), "bbbnbbnbb");
	Eq (h, "strreplace", ARGS (QH_S (h, "aa"), QH_S (h, "b"), QH_S (h, "aaa")), "ba");
	Eq (h, "strreplace", ARGS (QH_S (h, ""), QH_S (h, "x"), QH_S (h, "abc")), "abc");
	Eq (h, "strreplace", ARGS (QH_S (h, "a"), QH_S (h, "a"), QH_S (h, "aaa")), "aaa");
	Eq (h, "strreplace", ARGS (QH_S (h, "A"), QH_S (h, "x"), QH_S (h, "abc")), "abc");
	Eq (h, "strireplace", ARGS (QH_S (h, "AB"), QH_S (h, "x"), QH_S (h, "abAb")), "xx");
	Eq (h, "strireplace", ARGS (QH_S (h, "b"), QH_S (h, "Q"), QH_S (h, "aBc")), "aQc");
	// the output stops once it reaches 4094 - len(replace) bytes
	long_a = Rep ('a', 5000);
	QT_EQ_U (strlen (QH_String (h, "strreplace", ARGS (QH_S (h, "x"), QH_S (h, "y"), QH_S (h, long_a)))), 4093);
	free (long_a);
	long_y = Rep ('y', 4094);
	Eq (h, "strreplace", ARGS (QH_S (h, "x"), QH_S (h, long_y), QH_S (h, "abc")), "");
	free (long_y);
	QH_Free (h);
}

static void TestStrncmp (void)
{
	qh_t	*h = WithCharset (false, QC_CHARS_QUAKE);

	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "hello"), QH_S (h, "help"), F (3))), 0);
	QT_CHECK (QH_Float (h, "strncmp", ARGS (QH_S (h, "hello"), QH_S (h, "help"), F (4))) < 0);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "xxhello"), QH_S (h, "hello"), F (5), F (2))), 0);
	// fixed: FTE ignores s2ofs
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "hello"), QH_S (h, "xxhel"), F (3), F (0), F (2))), 0);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "a"), QH_S (h, "c"))), -2);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "abc"), QH_S (h, "ab"))), 99);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "abc"), QH_S (h, "abc"))), 0);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "\xE9"), QH_S (h, "a"))), 136);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "abc"), QH_S (h, "abd"), F (-1))), -1);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "abc"), QH_S (h, "xyz"), F (0))), 0);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "abc"), QH_S (h, ""), F (5), F (10))), 0);
	QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "abc"), QH_S (h, ""), F (5), F (-1))), 0);
	QT_EQ_F (QH_Float (h, "strcmp", ARGS (QH_S (h, "b"), QH_S (h, "a"))), 1);
	QH_Free (h);
}

// strcasecmp as C has it
static void TestStrcasecmp (void)
{
	qh_t	*h = Harness ();

	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "abc"), QH_S (h, "ABC"))), 0);
	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "a"), QH_S (h, "b"))), -1);
	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "b"), QH_S (h, "A"))), 1);
	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "abc"), QH_S (h, "abcd"))), -1);
	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "zz"), QH_S (h, "A"))), 1);
	// fixed: FTE folds to upper case ('_' > 'A') and compares signed chars
	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "_"), QH_S (h, "a"))), -1);
	QT_EQ_F (QH_Float (h, "strcasecmp", ARGS (QH_S (h, "\xE9"), QH_S (h, "a"))), 1);
	QT_EQ_F (QH_Float (h, "strncasecmp", ARGS (QH_S (h, "abc"), QH_S (h, "ABD"), F (2))), 0);
	QT_EQ_F (QH_Float (h, "strncasecmp", ARGS (QH_S (h, "abc"), QH_S (h, "ABD"), F (3))), -1);
	QT_EQ_F (QH_Float (h, "strncasecmp", ARGS (QH_S (h, "xxABC"), QH_S (h, "abc"), F (3), F (2))), 0);
	QT_EQ_F (QH_Float (h, "strncasecmp", ARGS (QH_S (h, "ABC"), QH_S (h, "xxabc"), F (3), F (0), F (2))), 0);
	QT_EQ_F (QH_Float (h, "strncasecmp", ARGS (QH_S (h, "abc"), QH_S (h, "ABCD"), F (-1))), -1);
	QH_Free (h);
}

// strdecolorize's text, and strlennocol's length of it
static void CheckDecolorize (qh_t *h, const char *text, const char *want)
{
	if (!QT_EQ_S (QH_String (h, "strdecolorize", ARGS (QH_S (h, text))), want))
		printf ("  strdecolorize(\"%s\")\n", text);
	QT_EQ_F (QH_Float (h, "strlennocol", ARGS (QH_S (h, text))), (float)strlen (want));
}

static void TestDecolorizeQuake (void)
{
	static const char	*cases[][2] = {
		{"^1Red ^7White", "Red White"},
		{"^^1", "^1"},
		{"a^", "a^"},
		{"^z", "^z"},
		{"^&F0text", "text"},
		{"^&-Ftext", "text"},
		{"^&G0x", "^&G0x"},
		{"^&f0x", "^&f0x"},
		{"^xF00red", "red"},
		{"^xf0ared", "red"},
		{"^xZZ", "ZZ"},
		{"^[link\\url\\http://x^]", "link"},
		{"^[abc", "[abc"},
		{"a^]b", "a]b"},
		{"&cf00red&r", "red"},
		{"&cxyz", "&cxyz"},
		{"rock&roll", "rockoll"},
		{"\x01hi", "hi"},
		{"\x02hi", "hi"},
		{"a\x01" "b", "a\x01" "b"},
		{"\xC8\xE9", "Hi"},
		{"\x80\x9F\x7F", "\x80\x9F\x7F"},
		{"\x0b\t\n\r", "\x0b\t\n\r"},
		{"^U0041", "A"},
		{"^U263a", "?"},
		{"^Ue0c1", "\xC1"},
		{"^{41}^{}x", "A?x"},
		{"^{1f600}", "?"},
		{"^{41", "A"},
		{"^b^m^h^a^d^s^r", ""},
		{"^`u8:\xC3\xA9`=x", "?x"},
		{"^`u8:^1a", "a"},
		{"=`k8:\xC1`=b", "?b"},
	};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		CheckDecolorize (h, cases[i][0], cases[i][1]);
	QH_Free (h);
}

// fixed: FTE takes six bytes after ^U without looking for hex digits
static void TestDecolorizeCaretUNeedsHex (void)
{
	qh_t	*h = Harness ();

	Eq (h, "strdecolorize", ARGS (QH_S (h, "^Uzz12x")), "^Uzz12x");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^U004")), "^U004");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^U")), "^U");
	QH_Free (h);
}

static void TestDecolorizeUtf8 (void)
{
	qh_t	*h = WithCharset (false, QC_CHARS_UTF8);

	Eq (h, "strdecolorize", ARGS (QH_S (h, "^U263a")), "\xE2\x98\xBA");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^1h\xC3\xA9llo")), "h\xC3\xA9llo");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^{1f600}")), "\xF0\x9F\x98\x80");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^{110000}")), "\xEF\xBF\xBD");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^`u8:\xC3\xA9`=")), "\xC3\xA9");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "=`k8:\xC1\xE1`=")), "\xD0\xB0\xD0\x90");
	// the first malformed sequence switches the rest of the string to Quake's rules
	Eq (h, "strdecolorize", ARGS (QH_S (h, "\xC3\xA9\xFF" "a\x01\xE9")), "\xC3\xA9\x7F" "a\xEE\x80\x81i");
	QT_EQ_F (QH_Float (h, "strlennocol", ARGS (QH_S (h, "^1\xC3\xA9"))), 2);
	QH_Free (h);
	h = WithCharset (false, QC_CHARS_ISO8859_1);
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^1\xE9\x01")), "\xE9\x01");
	QH_Free (h);
}

static const char *Infoget (qh_t *h, const char *info, const char *key)
{
	return QH_String (h, "infoget", ARGS (QH_S (h, info), QH_S (h, key)));
}

static void TestInfoget (void)
{
	qh_t	*h = Harness ();
	char	*v, *info;

	QT_EQ_S (Infoget (h, "\\name\\bob\\team\\red", "team"), "red");
	QT_EQ_S (Infoget (h, "name\\bob", "name"), "bob");
	QT_EQ_S (Infoget (h, "\\name\\bob", "Name"), "");
	QT_EQ_S (Infoget (h, "\\a\\\\b\\2", "b"), "2");
	QT_EQ_S (Infoget (h, "\\a\\1\\b", "b"), "");
	QT_EQ_S (Infoget (h, "\\a\\1", "a"), "1");
	v = Rep ('v', 1022);
	info = Join ("\\k\\", v, NULL);
	QT_EQ_S (Infoget (h, info, "k"), "");
	free (v);
	free (info);
	QH_Free (h);
}

static const char *Infoadd (qh_t *h, const char *info, const char *key, const char *value)
{
	return QH_String (h, "infoadd", ARGS (QH_S (h, info), QH_S (h, key), QH_S (h, value)));
}

static void TestInfoadd (void)
{
	qh_t	*h = Harness ();
	char	*long_key, *long_v;

	QT_EQ_S (Infoadd (h, "", "name", "bob"), "\\name\\bob");
	QT_EQ_S (Infoadd (h, "\\name\\bob", "team", "red"), "\\name\\bob\\team\\red");
	QT_EQ_S (Infoadd (h, "\\name\\bob\\team\\red", "name", "al"), "\\team\\red\\name\\al");
	QT_EQ_S (Infoadd (h, "\\name\\bob\\team\\red", "name", ""), "\\team\\red");
	QT_EQ_S (Infoadd (h, "\\name\\bob", "x", "a\\b"), "\\name\\bob");
	QT_EQ_S (Infoadd (h, "\\name\\bob", "x", "a\nb"), "\\name\\bob\\x\\ab");
	QT_EQ_S (Infoadd (h, "\\name\\bob", "x\"", "1"), "\\name\\bob");
	QT_EQ_S (Infoadd (h, "", "*star", "1"), "\\*star\\1");
	QT_EQ_S (Infoadd (h, "\\a\\1\\a\\2", "a", "3"), "\\a\\2\\a\\3");
	long_key = Rep ('k', 256);
	QT_EQ_S (Infoadd (h, "\\a\\1", long_key, "v"), "\\a\\1");
	free (long_key);
	// the value is the arguments from the third on, joined
	Eq (h, "infoadd", ARGS (QH_S (h, ""), QH_S (h, "k"), QH_S (h, "a"), QH_S (h, "b")), "\\k\\ab");
	// the pair is cut to 1023 bytes
	long_v = Rep ('v', 2000);
	QT_EQ_U (strlen (Infoadd (h, "", "k", long_v)), 1023);
	free (long_v);
	QH_Free (h);
}

static void TestInfoaddSizeLimits (void)
{
	qh_t	*h = Harness ();
	char	*w, *x, *v, *y, *z150, *z200, *x5000, *info, *want;
	int		before;

	// refused for its size: the string comes back as it was (FTE drops the old pair first)
	w = Rep ('w', 4089);
	info = Join ("\\k\\\\z\\", w, NULL);
	QT_EQ_U (strlen (info), 4095);
	before = QH_NumWarnings (h);
	QT_EQ_S (Infoadd (h, info, "k", "v"), info);
	QT_CHECK (QH_NumWarnings (h) > before);
	free (info);
	free (w);
	// a key whose value no longer fits: *ver is dropped to make room
	x = Rep ('x', 100);
	v = Rep ('v', 50);
	y = Rep ('y', 3900);
	z150 = Rep ('z', 150);
	z200 = Rep ('z', 200);
	info = Join ("\\a\\", x, "\\*ver\\", v, "\\b\\", y, NULL);
	want = Join ("\\b\\", y, "\\a\\", z150, NULL);
	QT_EQ_S (Infoadd (h, info, "a", z150), want);
	free (want);
	// too big even without *ver: as it was but for the dropped *ver
	want = Join ("\\a\\", x, "\\b\\", y, NULL);
	QT_EQ_S (Infoadd (h, info, "a", z200), want);
	free (want);
	free (info);
	// info strings are cut to 4095 bytes first
	x5000 = Rep ('x', 5000);
	info = Join ("\\a\\", x5000, NULL);
	QT_EQ_S (Infoadd (h, info, "a", ""), "");
	free (info);
	free (x5000);
	free (x);
	free (v);
	free (y);
	free (z150);
	free (z200);
	QH_Free (h);
}

static void TestUriEscaping (void)
{
	qh_t	*h = Harness ();
	char	*spaces, *long_a;

	Eq (h, "uri_escape", ARGS (QH_S (h, "a b")), "a%20b");
	Eq (h, "uri_escape", ARGS (QH_S (h, "100%")), "100%25");
	Eq (h, "uri_escape", ARGS (QH_S (h, "\xC3\xA9")), "%C3%A9");
	Eq (h, "uri_escape", ARGS (QH_S (h, "~x-y_z.0")), "~x-y_z.0");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "%41")), "A");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "%4")), "%4");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "%zz")), "%zz");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "%%41")), "%A");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "a%00b")), "a");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "a+b")), "a+b");
	Eq (h, "uri_unescape", ARGS (QH_S (h, "%c3%A9")), "\xC3\xA9");
	spaces = Rep (' ', 5000);
	QT_EQ_U (strlen (QH_String (h, "uri_escape", ARGS (QH_S (h, spaces)))), 8190);
	free (spaces);
	long_a = Rep ('a', 9000);
	QT_EQ_U (strlen (QH_String (h, "uri_unescape", ARGS (QH_S (h, long_a)))), 8190);
	free (long_a);
	QH_Free (h);
}

// argescape quotes as sprintf's %S does
static void TestArgescape (void)
{
	qh_t	*h = Harness ();
	char	*long_a;

	Eq (h, "argescape", ARGS (QH_S (h, "a b")), "\"a b\"");
	Eq (h, "argescape", ARGS (QH_S (h, "say \"hi\"")), "\\\"say \\\"hi\\\"\"");
	Eq (h, "argescape", ARGS (QH_S (h, "x\ny\t'$\\")), "\\\"x\\ny\\t\\'\\$\\\\\"");
	Eq (h, "argescape", ARGS (QH_S (h, "")), "\"\"");
	long_a = Rep ('a', 9000);
	QT_EQ_U (strlen (QH_String (h, "argescape", ARGS (QH_S (h, long_a)))), 8191);
	free (long_a);
	QH_Free (h);
}

// a string of the progs' own, and the extra names
static void InstrSetup (qc_asm_t *a, void *ctx)
{
	*(uint32_t *)ctx = QA_String (a, "hello world");
	QH_Named (a, (void *)extra);
}

// instr points into its input
static void TestInstr (void)
{
	uint32_t	ofs = 0, r;
	qh_t		*h = QH_New (QC_NUMBERING_CSQC, NULL, InstrSetup, &ofs);

	r = QH_Word (h, "instr", ARGS (W (ofs), QH_S (h, "wor")));
	QT_EQ_U (r, ofs + 6);
	QT_EQ_S (QH_Text (h, r), "world");
	r = QH_Word (h, "instr", ARGS (W (ofs), QH_S (h, "")));
	QT_EQ_U (r, ofs);
	// temp strings: a new temp with the rest
	QT_EQ_S (QH_OptString (h, "instr", ARGS (QH_S (h, "hello world"), QH_S (h, "o w"))), "o world");
	QT_EQ_S (QH_OptString (h, "instr", ARGS (QH_S (h, "hello world"), QH_S (h, "wo"), QH_S (h, "r"))), "world");
	QT_CHECK (QH_OptString (h, "instr", ARGS (QH_S (h, "hello world"), QH_S (h, "xyz"))) == NULL);
	QH_Free (h);
}

static void TestMenuValidstring (void)
{
	qh_t	*h = Menu ();

	QT_EQ_F (QH_Float (h, "validstring", ARGS (W (0))), 0);
	QT_EQ_F (QH_Float (h, "validstring", ARGS (QH_S (h, ""))), 1);
	QT_EQ_F (QH_Float (h, "validstring", ARGS (QH_S (h, "x"))), 1);
	QH_Free (h);
}

static void TestMenuAltstr (void)
{
	qh_t	*h = Menu ();

	QT_EQ_F (QH_Float (h, "altstr_count", ARGS (QH_S (h, "'a' 'b' 'c'"))), 3);
	QT_EQ_F (QH_Float (h, "altstr_count", ARGS (QH_S (h, "'a\\'b' 'c'"))), 2);
	QT_EQ_F (QH_Float (h, "altstr_count", ARGS (QH_S (h, "'a"))), 0);
	QT_EQ_F (QH_Float (h, "altstr_count", ARGS (QH_S (h, "'a' 'b"))), 1);
	Eq (h, "altstr_prepare", ARGS (QH_S (h, "it's 'x'")), "it\\'s \\'x\\'");
	Eq (h, "altstr_prepare", ARGS (QH_S (h, "a\\b")), "a\\b");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'one' 'two'"), F (1)), "two");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'one' 'two'"), F (0)), "one");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'a\\'b' 'c'"), F (0)), "a'b");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'a\\'b' 'c'"), F (1)), "c");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'a' 'b'"), F (5)), "");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'a' 'b'"), F (-1)), "");
	Eq (h, "altstr_get", ARGS (QH_S (h, "'a' 'unterminated"), F (1)), "unterminated");
	Eq (h, "altstr_set", ARGS (QH_S (h, "'one' 'two'"), F (1), QH_S (h, "x")), "'one' 'x'");
	Eq (h, "altstr_set", ARGS (QH_S (h, "'one' 'two'"), F (0), QH_S (h, "")), "'' 'two'");
	Eq (h, "altstr_set", ARGS (QH_S (h, "'a\\'b' 'c'"), F (1), QH_S (h, "z")), "'a\\'b' 'z'");
	Eq (h, "altstr_set", ARGS (QH_S (h, "'a\\'b' 'c'"), F (0), QH_S (h, "q")), "'q' 'c'");
	Eq (h, "altstr_set", ARGS (QH_S (h, "'a'"), F (3), QH_S (h, "x")), "'a'x");
	Eq (h, "altstr_set", ARGS (QH_S (h, "'a' 'b"), F (1), QH_S (h, "x")), "'a' 'x");
	QH_Free (h);
}

// the menu's builtins are found by its own numbers
static void TestMenuNumbers (void)
{
	qh_t	*h = Menu ();

	QT_EQ_F (QH_Float (h, "strlen", ARGS (QH_S (h, "abc"))), 3);
	Eq (h, "strcat", ARGS (QH_S (h, "a"), QH_S (h, "b")), "ab");
	Eq (h, "ftos", ARGS (F (0.5f)), "0.5");
	QT_EQ_F (QH_Float (h, "stof", ARGS (QH_S (h, "2.5"))), 2.5);
	Eq (h, "substring", ARGS (QH_S (h, "hello"), F (1), F (2)), "el");
	QH_Free (h);
}

/*
==============================================================================

THE UNIT TESTS OF STDLIB/STRING.RS

==============================================================================
*/

// the first needle by comparing at every offset
static int64_t Naive (const char *hay, size_t hlen, const char *needle, size_t nlen)
{
	size_t	i;

	for (i = 0 ; i + nlen <= hlen ; i++)
		if (!memcmp (hay + i, needle, nlen))
			return (int64_t)i;
	return -1;
}

// the search (Knuth-Morris-Pratt for long needles) finds what comparing finds
static void TestSubstringSearch (void)
{
	static const char	hay[] = "abababababcabababababababcababababababababababcabcabcX";
	static const char	absent[] = "ababababababababababababababababababab";
	size_t				hlen = sizeof(hay) - 1, start, len, max;
	char				*big, *needle;

	for (start = 0 ; start < hlen ; start++)
	{
		max = hlen - start < 40 ? hlen - start : 40;
		for (len = 17 ; len <= max ; len++)
			if (!QT_EQ_I (QC_Find (hay, hlen, hay + start, len), Naive (hay, hlen, hay + start, len)))
				printf ("  start %zu, length %zu\n", start, len);
	}
	QT_EQ_I (QC_Find (hay, hlen, absent, sizeof(absent) - 1), -1);
	QT_EQ_I (QC_Find ("abc", 3, "", 0), 0);
	// linear time on a pathological input
	big = Rep ('a', 1u << 20);
	needle = Rep ('a', (1u << 16) + 1);
	needle[1u << 16] = 'b';
	QT_EQ_I (QC_Find (big, 1u << 20, needle, (1u << 16) + 1), -1);
	free (big);
	free (needle);
}

// info_get, through infoget
static void TestInfoStrings (void)
{
	qh_t	*h = Harness ();
	char	*v, *info;

	QT_EQ_S (Infoget (h, "\\name\\bob\\team\\red", "team"), "red");
	QT_EQ_S (Infoget (h, "name\\bob", "name"), "bob");
	QT_EQ_S (Infoget (h, "\\name\\bob", "Name"), "");
	QT_EQ_S (Infoget (h, "\\a\\\\b\\2", "b"), "2");
	QT_EQ_S (Infoget (h, "\\a\\1\\b", "b"), "");
	v = Rep ('v', 1022);
	info = Join ("\\k\\", v, NULL);
	QT_EQ_S (Infoget (h, info, "k"), "");
	info[strlen (info) - 1] = 0;
	QT_EQ_U (strlen (Infoget (h, info, "k")), 1021);
	free (v);
	free (info);
	QH_Free (h);
}

// info_set, through infoadd
static void TestInfoSetRules (void)
{
	qh_t	*h = Harness ();
	int		before;

	QT_EQ_S (Infoadd (h, "", "name", "bob"), "\\name\\bob");
	QT_EQ_S (Infoadd (h, "\\name\\bob\\team\\red", "name", "al"), "\\team\\red\\name\\al");
	QT_EQ_S (Infoadd (h, "\\name\\bob\\team\\red", "name", ""), "\\team\\red");
	before = QH_NumWarnings (h);
	QT_EQ_S (Infoadd (h, "\\name\\bob", "x", "a\\b"), "\\name\\bob");
	QT_CHECK (QH_NumWarnings (h) > before);
	QT_EQ_S (Infoadd (h, "\\name\\bob", "x", "a\nb"), "\\name\\bob\\x\\ab");
	QH_Free (h);
}

// nested ^`u8: and =`k8: sections don't recurse without a bound
static void TestNestedSections (void)
{
	qh_t	*h = Harness ();
	char	*s;
	size_t	i;

	s = malloc (100000 * 5 + 5);
	for (i = 0 ; i < 100000 ; i++)
		memcpy (s + i * 5, "^`u8:", 5);
	memcpy (s + 100000 * 5, "x`=y", 5);
	Eq (h, "strdecolorize", ARGS (QH_S (h, s)), "xy");
	for (i = 0 ; i < 100000 ; i++)
		memcpy (s + i * 5, "=`k8:", 5);
	Eq (h, "strdecolorize", ARGS (QH_S (h, s)), "xy");
	free (s);
	QH_Free (h);
}

static void TestMarkup (void)
{
	qh_t	*h = Harness ();

	Eq (h, "strdecolorize", ARGS (QH_S (h, "^1Red ^7White")), "Red White");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^[link\\url\\http://x^]")), "link");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "^Uzz12")), "^Uzz12");
	Eq (h, "strdecolorize", ARGS (QH_S (h, "=`k8:\xC1`=x")), "?x");
	QH_Free (h);
	h = WithCharset (false, QC_CHARS_UTF8);
	Eq (h, "strdecolorize", ARGS (QH_S (h, "=`k8:\xC1`=")), "\xD0\xB0");
	QH_Free (h);
}

int main (void)
{
	TestResultsAreTemps ();
	TestStrlenStrcatStrzone ();
	TestSubstring ();
	TestStrstrofs ();
	TestStr2chr ();
	TestChr2str ();
	TestCaseConversion ();
	TestStrconv ();
	TestStrpadStrtrim ();
	TestStrreplace ();
	TestStrncmp ();
	TestStrcasecmp ();
	TestDecolorizeQuake ();
	TestDecolorizeCaretUNeedsHex ();
	TestDecolorizeUtf8 ();
	TestInfoget ();
	TestInfoadd ();
	TestInfoaddSizeLimits ();
	TestUriEscaping ();
	TestArgescape ();
	TestInstr ();
	TestMenuValidstring ();
	TestMenuAltstr ();
	TestMenuNumbers ();
	TestSubstringSearch ();
	TestInfoStrings ();
	TestInfoSetRules ();
	TestNestedSections ();
	TestMarkup ();
	return QT_Finish ("lib_string", "the string builtins give FTE's results, with qcvm's fixes");
}
