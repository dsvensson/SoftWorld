// test_qc_lib_tokenize.c -- tokenize, tokenize_console, tokenizebyseparator,
// argv and the rest (qcvm-rs's tests/all/builtins_strings/tokenize.rs and the
// unit tests of src/stdlib/tokenize.rs, docs/spec/strings.md)

#include "qc_harness.h"
#include "qc_lib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// builtins FTE binds by name that its CSQC declarations don't list
static const char	*extra[] = {"argc", "instr", "ftou", "utof", "strcmp", NULL};

// CSQC with QuakeWorld's charset defaults (utf8_enable 0, the Quake scheme)
static qh_t *Harness (void)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, (void *)extra);
}

// the same, with a container budget
static qh_t *Budgeted (size_t bytes)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.container_bytes = bytes;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, (void *)extra);
}

#define MAX_TOKS	16

// the tokens of the last tokenizer, with their start and end offsets
typedef struct
{
	int		count;
	char	*text[MAX_TOKS];
	size_t	len[MAX_TOKS];
	float	start[MAX_TOKS], end[MAX_TOKS];
} toks_t;

static void FreeToks (toks_t *t)
{
	int	i;

	for (i = 0 ; i < t->count && i < MAX_TOKS ; i++)
		free (t->text[i]);
	t->count = 0;
}

// tokenizes with name and reads every token back (argc must agree)
static void Toks (qh_t *h, toks_t *t, const char *name, int argc, const qc_value_t *args)
{
	float		n = QH_Float (h, name, argc, args);
	const char	*s;
	int			k;

	QT_EQ_F (QH_Float (h, "argc", NOARGS), n);
	t->count = (int)n;
	for (k = 0 ; k < t->count && k < MAX_TOKS ; k++)
	{
		s = QH_OptString (h, "argv", ARGS (F ((float)k)));
		if (!QT_CHECK (s != NULL))
			s = "";
		t->len[k] = strlen (s);
		t->text[k] = malloc (t->len[k] + 1);
		memcpy (t->text[k], s, t->len[k] + 1);
		t->start[k] = QH_Float (h, "argv_start_index", ARGS (F ((float)k)));
		t->end[k] = QH_Float (h, "argv_end_index", ARGS (F ((float)k)));
	}
}

// whether the tokens are want's (a NULL-terminated list)
static bool SameTexts (const toks_t *t, const char *const *want)
{
	int	n, i;

	for (n = 0 ; want[n] ; n++)
		;
	if (t->count != n)
		return false;
	for (i = 0 ; i < n ; i++)
		if (strcmp (t->text[i], want[i]))
			return false;
	return true;
}

static void PrintToks (const char *input, const toks_t *t)
{
	int	i;

	printf ("  \"%s\" gave %d:", input, t->count);
	for (i = 0 ; i < t->count && i < MAX_TOKS ; i++)
		printf (" [%s] (%g-%g)", t->text[i], (double)t->start[i], (double)t->end[i]);
	printf ("\n");
}

// tokenizes text with name and checks the tokens' texts
static void Texts (qh_t *h, const char *name, const char *text, const char *const *want)
{
	toks_t	t;

	Toks (h, &t, name, ARGS (QH_S (h, text)));
	if (!QT_CHECK (SameTexts (&t, want)))
		PrintToks (text, &t);
	FreeToks (&t);
}

#define WANT(...)	((const char *const[]){__VA_ARGS__, NULL})
#define NONE		((const char *const[]){NULL})

// the spans of the tokens, against want's pairs
static void Spans (const char *input, const toks_t *t, int count, const float (*want)[2])
{
	int		i;
	bool	ok = t->count == count;

	for (i = 0 ; ok && i < count ; i++)
		ok = t->start[i] == want[i][0] && t->end[i] == want[i][1];
	if (!QT_CHECK (ok))
		PrintToks (input, t);
}

static void TestTokenizeExamples (void)
{
	qh_t	*h = Harness ();

	Texts (h, "tokenize", "say hello world", WANT ("say", "hello", "world"));
	Texts (h, "tokenize", "  a   b ", WANT ("a", "b"));
	Texts (h, "tokenize", "", NONE);
	Texts (h, "tokenize", "   ", NONE);
	Texts (h, "tokenize", "\"hello world\" foo", WANT ("hello world", "foo"));
	Texts (h, "tokenize", "a\"b c\"", WANT ("a\"b", "c\""));
	Texts (h, "tokenize", "\"a\"\"b\"", WANT ("a\"b"));
	Texts (h, "tokenize", "\"\"", WANT (""));
	Texts (h, "tokenize", "f(x,y)", WANT ("f", "(", "x", ",", "y", ")"));
	Texts (h, "tokenize", "key:value", WANT ("key", ":", "value"));
	Texts (h, "tokenize", "{a;b}", WANT ("{", "a", ";", "b", "}"));
	Texts (h, "tokenize", "'1 2 3'", WANT ("1 2 3"));
	Texts (h, "tokenize", "a // b", WANT ("a"));
	Texts (h, "tokenize", "a //x\nb", WANT ("a", "\n", "b"));
	Texts (h, "tokenize", "a\nb", WANT ("a", "b"));
	Texts (h, "tokenize", "a//b", WANT ("a//b"));
	Texts (h, "tokenize", "http://foo", WANT ("http", ":"));
	Texts (h, "tokenize", "a /*b*/ c", WANT ("a", "/*b*/", "c"));
	Texts (h, "tokenize", "x=1 [2]", WANT ("x=1", "[", "2", "]"));
	Texts (h, "tokenize", "\"unterminated here", WANT ("unterminated here"));
	Texts (h, "tokenize", "\\\"a\\n\\x41\\q\\\"b\" c", WANT ("a\nA?\"b", "c"));
	Texts (h, "tokenize", "\\\"line\\\ncont\"", WANT ("linecont"));
	Texts (h, "tokenize", "\\\"a\\x00b\" c", WANT ("a", "b\"", "c"));
	QH_Free (h);
}

// FTE reads past the end of an unterminated single-quoted token and mangles '';
// fixed
static void TestSingleQuotesAreBounded (void)
{
	static const float	abc[1][2] = {{0, 4}}, ab_c[2][2] = {{0, 6}, {7, 8}}, quotes[1][2] = {{0, 4}},
		one[1][2] = {{0, 1}};
	qh_t	*h = Harness ();
	toks_t	t;

	Toks (h, &t, "tokenize", ARGS (QH_S (h, "'abc")));
	QT_CHECK (SameTexts (&t, WANT ("abc")));
	Spans ("'abc", &t, 1, abc);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "'a''b' c")));
	QT_CHECK (SameTexts (&t, WANT ("a'b", "c")));
	Spans ("'a''b' c", &t, 2, ab_c);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "''''")));
	QT_CHECK (SameTexts (&t, WANT ("'")));
	Spans ("''''", &t, 1, quotes);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "'")));
	QT_CHECK (SameTexts (&t, WANT ("")));
	Spans ("'", &t, 1, one);
	FreeToks (&t);
	QH_Free (h);
}

static void TestTokenizeConsoleExamples (void)
{
	qh_t	*h = Harness ();

	Texts (h, "tokenize_console", "say 'hi there'", WANT ("say", "'hi", "there'"));
	Texts (h, "tokenize_console", "f(x,y)", WANT ("f(x,y)"));
	Texts (h, "tokenize_console", "http://foo", WANT ("http://foo"));
	Texts (h, "tokenize_console", "a /*b*/ c", WANT ("a", "c"));
	Texts (h, "tokenize_console", "a /*b c", WANT ("a"));
	Texts (h, "tokenize_console", "a //x\nb", WANT ("a", "\n", "b"));
	Texts (h, "tokenize_console", "\"a b\" c", WANT ("a b", "c"));
	Texts (h, "tokenize_console", "\\\"a\\tb\"", WANT ("a\tb"));
	QH_Free (h);
}

static void TestTokenOffsets (void)
{
	static const float	hello[2][2] = {{0, 13}, {14, 17}}, abcde[3][2] = {{2, 4}, {5, 10}, {11, 12}},
		comment[3][2] = {{0, 1}, {2, 6}, {6, 7}}, call[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}};
	qh_t	*h = Harness ();
	toks_t	t;

	Toks (h, &t, "tokenize", ARGS (QH_S (h, "\"hello world\" foo")));
	Spans ("\"hello world\" foo", &t, 2, hello);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "  ab \"c d\" e")));
	Spans ("  ab \"c d\" e", &t, 3, abcde);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "a //x\nb")));
	Spans ("a //x\\nb", &t, 3, comment);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "f(x)")));
	Spans ("f(x)", &t, 4, call);
	FreeToks (&t);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "\"abc")));
	QT_CHECK (t.count >= 1 && t.start[0] == 0 && t.end[0] == 4);
	FreeToks (&t);
	Toks (h, &t, "tokenize_console", ARGS (QH_S (h, "a /*b*/ c")));
	QT_CHECK (t.count >= 2 && t.start[1] == 2 && t.end[1] == 9);
	FreeToks (&t);
	QH_Free (h);
}

// a word longer than FTE's token buffer goes on as the next token
static void TestLongTokensContinueAsTheNextToken (void)
{
	qh_t	*h = Harness ();
	char	*word = malloc (65541);
	toks_t	t;

	memset (word, 'w', 65540);
	word[65540] = 0;
	Toks (h, &t, "tokenize", ARGS (QH_S (h, word)));
	if (QT_EQ_I (t.count, 2))
	{
		QT_EQ_U (t.len[0], 65535);
		QT_CHECK (t.len[1] == 5 && t.start[1] == 65535 && t.end[1] == 65540);
	}
	FreeToks (&t);
	free (word);
	QH_Free (h);
}

static void TestArgvIndexing (void)
{
	qh_t		*h = Harness ();
	uint32_t	r1, r2;

	QT_EQ_F (QH_Float (h, "tokenize", ARGS (QH_S (h, "a b c"))), 3);
	QT_EQ_S (QH_String (h, "argv", ARGS (F (0))), "a");
	QT_EQ_S (QH_String (h, "argv", ARGS (F (2.9f))), "c");
	QT_EQ_S (QH_String (h, "argv", ARGS (F (-1))), "c");
	QT_EQ_S (QH_String (h, "argv", ARGS (F (-3))), "a");
	QT_CHECK (QH_OptString (h, "argv", ARGS (F (3))) == NULL);
	QT_CHECK (QH_OptString (h, "argv", ARGS (F (-4))) == NULL);
	QT_CHECK (QH_OptString (h, "argv", ARGS (F (NAN))) == NULL);
	QT_EQ_F (QH_Float (h, "argv_start_index", ARGS (F (-1))), 4);
	QT_EQ_F (QH_Float (h, "argv_end_index", ARGS (F (-1))), 5);
	QT_EQ_F (QH_Float (h, "argv_start_index", ARGS (F (3))), -1);
	QT_EQ_F (QH_Float (h, "argv_end_index", ARGS (F (-4))), -1);
	// each call gives a new copy
	r1 = QH_Word (h, "argv", ARGS (F (0)));
	r2 = QH_Word (h, "argv", ARGS (F (0)));
	QT_CHECK (r1 != r2);
	// an empty token is an empty string, not null
	QH_Float (h, "tokenize", ARGS (QH_S (h, "\"\"")));
	QT_CHECK (QH_OptString (h, "argv", ARGS (F (0))) != NULL && !strcmp (h->result, ""));
	// a new tokenize replaces the list
	QT_EQ_F (QH_Float (h, "tokenize", ARGS (QH_S (h, ""))), 0);
	QT_EQ_F (QH_Float (h, "argc", NOARGS), 0);
	QT_CHECK (QH_OptString (h, "argv", ARGS (F (0))) == NULL);
	QH_Free (h);
}

// tokenizebyseparator's texts
static void Separated (qh_t *h, int argc, const qc_value_t *args, const char *const *want)
{
	toks_t	t;

	Toks (h, &t, "tokenizebyseparator", argc, args);
	if (!QT_CHECK (SameTexts (&t, want)))
		PrintToks ("(tokenizebyseparator)", &t);
	FreeToks (&t);
}

static void TestTokenizebyseparatorExamples (void)
{
	static const float	spans[3][2] = {{0, 1}, {3, 4}, {5, 6}};
	qh_t	*h = Harness ();
	toks_t	t;

	Separated (h, ARGS (QH_S (h, "a,b,,c"), QH_S (h, ",")), WANT ("a", "b", "", "c"));
	Separated (h, ARGS (QH_S (h, "a,"), QH_S (h, ",")), WANT ("a", ""));
	Separated (h, ARGS (QH_S (h, ","), QH_S (h, ",")), WANT ("", ""));
	Separated (h, ARGS (QH_S (h, "abc"), QH_S (h, ",")), WANT ("abc"));
	Separated (h, ARGS (QH_S (h, ""), QH_S (h, ",")), NONE);
	Separated (h, ARGS (QH_S (h, "a::b:c"), QH_S (h, "::"), QH_S (h, ":")), WANT ("a", "b", "c"));
	Separated (h, ARGS (QH_S (h, "a::b"), QH_S (h, ":"), QH_S (h, "::")), WANT ("a", "", "b"));
	Separated (h, ARGS (QH_S (h, "abc")), WANT ("abc"));
	Separated (h, ARGS (QH_S (h, " a , b "), QH_S (h, ",")), WANT (" a ", " b "));
	// an empty separator made FTE loop forever; it's ignored
	Separated (h, ARGS (QH_S (h, "a,b"), QH_S (h, ""), QH_S (h, ",")), WANT ("a", "b"));
	Separated (h, ARGS (QH_S (h, "ab"), QH_S (h, "")), WANT ("ab"));
	Toks (h, &t, "tokenizebyseparator", ARGS (QH_S (h, "a::b:c"), QH_S (h, "::"), QH_S (h, ":")));
	Spans ("a::b:c", &t, 3, spans);
	FreeToks (&t);
	// up to seven separators
	Separated (h, ARGS (QH_S (h, "1a2b3c4d5e6f7g8"), QH_S (h, "a"), QH_S (h, "b"), QH_S (h, "c"), QH_S (h, "d"),
		QH_S (h, "e"), QH_S (h, "f"), QH_S (h, "g")), WANT ("1", "2", "3", "4", "5", "6", "7", "8"));
	// bytes compare as they are
	QT_EQ_F (QH_Float (h, "tokenizebyseparator", ARGS (QH_S (h, "x\xFFy"), QH_S (h, "\xFF"))), 2);
	QH_Free (h);
}

// A token list shares the container budget: long lists of empty or one-byte
// tokens stop at it (with a warning), stay so after collections, and give their
// storage back when replaced.
static void TestTokenListsAreBounded (void)
{
	qh_t	*h = Budgeted (8 * 1024);
	char	*commas = malloc (4097), *letters = malloc (8193);
	float	n, m;
	int		i;

	memset (commas, ',', 4096);
	commas[4096] = 0;
	for (i = 0 ; i < 4096 ; i++)
	{
		letters[2 * i] = 'a';
		letters[2 * i + 1] = ' ';
	}
	letters[8192] = 0;
	n = QH_Float (h, "tokenizebyseparator", ARGS (QH_S (h, commas), QH_S (h, ",")));
	if (!QT_CHECK (n > 10 && n < 4097))
		printf ("  %g tokens\n", (double)n);
	QT_CHECK (QH_NumWarnings (h) > 0);
	QC_CollectGarbage (h->vm);
	QT_EQ_F (QH_Float (h, "argc", NOARGS), n);
	m = QH_Float (h, "tokenize", ARGS (QH_S (h, letters)));
	if (!QT_CHECK (m > 10 && m < 4096))
		printf ("  %g tokens\n", (double)m);
	// replacing the list gives its storage back: the whole budget is there again
	QT_EQ_F (QH_Float (h, "tokenize", ARGS (QH_S (h, "x y"))), 2);
	QT_CHECK (QH_Float (h, "hash_createtab", ARGS (F (64))) > 0);
	free (commas);
	free (letters);
	QH_Free (h);
}

/*
==============================================================================

THE SPLITTERS (the unit tests of src/stdlib/tokenize.rs, through the builtins)

==============================================================================
*/

static void TestQcMode (void)
{
	qh_t	*h = Harness ();
	toks_t	t;

	Texts (h, "tokenize", "say hello world", WANT ("say", "hello", "world"));
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "f(x,y)")));
	QT_EQ_I (t.count, 6);
	FreeToks (&t);
	Texts (h, "tokenize", "a //x\nb", WANT ("a", "\n", "b"));
	Texts (h, "tokenize", "'a''b' c", WANT ("a'b", "c"));
	Texts (h, "tokenize", "'abc", WANT ("abc"));
	Texts (h, "tokenize", "\\\"a\\tb\\x41\\q\" c", WANT ("a\tbA?", "c"));
	Texts (h, "tokenize", "\\\"a\\x22b\"", WANT ("a", "b\""));
	QH_Free (h);
}

static void TestLongWordsSplit (void)
{
	qh_t	*h = Harness ();
	char	*s = malloc (65539);
	toks_t	t;

	memset (s, 'a', 65538);
	s[65538] = 0;
	Toks (h, &t, "tokenize_console", ARGS (QH_S (h, s)));
	if (QT_EQ_I (t.count, 2))
	{
		QT_EQ_U (t.len[0], 65535);
		QT_CHECK (t.start[1] == 65535 && t.end[1] == 65538);
	}
	FreeToks (&t);
	free (s);
	QH_Free (h);
}

static void TestSeparators (void)
{
	qh_t	*h = Harness ();

	Separated (h, ARGS (QH_S (h, "a,b,,c"), QH_S (h, ",")), WANT ("a", "b", "", "c"));
	Separated (h, ARGS (QH_S (h, "a::b"), QH_S (h, ":"), QH_S (h, "::")), WANT ("a", "", "b"));
	Separated (h, ARGS (QH_S (h, "ab"), QH_S (h, "")), WANT ("ab"));
	QH_Free (h);
}

// while a list is made, a token costs two token records and its text
static void TestBudgetStopsTokenizing (void)
{
	qh_t	*h = Budgeted (sizeof(qc_token_t) * 2 * 10);
	char	commas[1001];
	toks_t	t;

	memset (commas, ',', 1000);
	commas[1000] = 0;
	QT_EQ_F (QH_Float (h, "tokenizebyseparator", ARGS (QH_S (h, commas), QH_S (h, ","))), 10);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Free (h);

	h = Budgeted (sizeof(qc_token_t) * 2 * 3 + 3);
	Toks (h, &t, "tokenize", ARGS (QH_S (h, "a b c d")));
	QT_CHECK (SameTexts (&t, WANT ("a", "b", "c")));
	QT_EQ_I (QH_NumWarnings (h), 1);
	FreeToks (&t);
	QH_Free (h);
}

int main (void)
{
	TestTokenizeExamples ();
	TestSingleQuotesAreBounded ();
	TestTokenizeConsoleExamples ();
	TestTokenOffsets ();
	TestLongTokensContinueAsTheNextToken ();
	TestArgvIndexing ();
	TestTokenizebyseparatorExamples ();
	TestTokenListsAreBounded ();
	TestQcMode ();
	TestLongWordsSplit ();
	TestSeparators ();
	TestBudgetStopsTokenizing ();
	return QT_Finish ("lib_tokenize", "the tokenizers split and index as FTE's do");
}
