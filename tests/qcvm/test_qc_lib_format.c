// test_qc_lib_format.c -- sprintf, and quoting for the console (qcvm-rs's
// tests/all/builtins_strings/format.rs and the quoting test of stdlib/format.rs,
// docs/spec/strings.md)

#include "qc_harness.h"
#include "qc_lib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI	3.14159265358979323846f

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

// sprintf's text, checked against want
static void CheckSprintf (qh_t *h, const char *want, int line, int argc, const qc_value_t *args)
{
	if (!QT_EQ_S (QH_String (h, "sprintf", argc, args), want))
		printf ("  sprintf at line %d\n", line);
}

#define SP(want, ...)	CheckSprintf (h, want, __LINE__, ARGS (__VA_ARGS__))
#define S(text)			QH_S (h, text)

// a 64-bit value over two argument slots (for the q modifier)
static qc_value_t Raw64 (uint64_t bits)
{
	return (qc_value_t){{(uint32_t)bits, (uint32_t)(bits >> 32), 0}};
}

static uint64_t DoubleBits (double d)
{
	uint64_t	u;

	memcpy (&u, &d, 8);
	return u;
}

// the examples of docs/spec/strings.md
static void TestSpecExamples (void)
{
	qh_t	*h = Harness ();

	SP ("3", S ("%d"), F (3.7f));
	SP ("-3", S ("%d"), F (-3.7f));
	SP ("10000000000", S ("%d"), F (1e10f));
	SP ("0", S ("%d"), I (5));
	SP ("5", S ("%ld"), I (5));
	SP ("5", S ("%i"), I (5));
	SP ("1084227584", S ("%i"), F (5.0f));
	SP ("5", S ("%hi"), F (5.0f));
	SP ("5.000000", S ("%lf"), I (5));
	SP ("+42", S ("%+d"), F (42.0f));
	SP (" 42", S ("% d"), F (42.0f));
	SP ("42    |", S ("%-6d|"), F (42.0f));
	SP ("-01.50", S ("%06.2f"), F (-1.5f));
	SP (" 3.14|", S ("%5.2f|"), F (PI));
	SP ("ff", S ("%x"), F (255.0f));
	SP ("FF", S ("%X"), F (255.9f));
	SP ("0xff", S ("%#x"), F (255.0f));
	SP ("000000ff", S ("%08x"), F (255.0f));
	SP ("0", S ("%x"), I (255));
	SP ("ffffffff", S ("%lx"), I (-1));
	SP ("000000ff", S ("%p"), I (255));
	SP ("10", S ("%o"), F (8.0f));
	SP ("1.234500e+04", S ("%e"), F (12345.0f));
	SP ("0.1", S ("%g"), F (0.1f));
	SP ("1e+20", S ("%g"), F (1e20f));
	SP ("0.100000", S ("%f"), F (0.1f));
	SP ("0.1000000015", S ("%.10f"), F (0.1f));
	SP ("A", S ("%c"), F (65.0f));
	SP ("A", S ("%c"), F (321.0f));
	SP ("  A", S ("%3c"), F (65.0f));
	SP ("", S ("%c"), F (0.0f));
	SP ("   ab|", S ("%5s|"), S ("ab"));
	SP ("ab   |", S ("%-5s|"), S ("ab"));
	SP ("a", S ("%.1s"), S ("ab"));
	SP ("\"a b\"", S ("%S"), S ("a b"));
	SP ("\\\"say \\\"hi\\\"\"", S ("%S"), S ("say \"hi\""));
	SP ("\\\"x\\ny\"", S ("%S"), S ("x\ny"));
	SP ("1 2 3", S ("%v"), V (1.0f, 2.0f, 3.0f));
	SP ("1.23 0 -0.5", S ("%.3v"), V (1.23456f, 0.0f, -0.5f));
	SP ("    1     2     3", S ("%5v"), V (1.0f, 2.0f, 3.0f));
	SP ("1.00000 2.00000 3.00000", S ("%#v"), V (1.0f, 2.0f, 3.0f));
	SP ("b-a", S ("%2$s-%1$s"), S ("a"), S ("b"));
	SP ("b a", S ("%2$s %s"), S ("a"), S ("b"));
	SP ("   42", S ("%*d"), F (5.0f), F (42.0f));
	SP ("42   |", S ("%*d|"), F (-5.0f), F (42.0f));
	SP ("3.14", S ("%.*f"), F (2.0f), F (PI));
	SP ("42", S ("%*d"), I (5), F (42.0f));
	SP ("0 ", S ("%d %s"));
	SP ("0 0 0", S ("%v"));
	SP ("%", S ("%%"));
	SP ("   ab|", S ("%05s|"), S ("ab"));
	QH_Free (h);
}

// a malformed directive ends the output, with a warning
static void TestFormatErrorsStopTheOutput (void)
{
	static const char	*cases[][2] = {{"ab%kcd", "ab"}, {"x%", "x"}, {"A%.f", "A"}, {"%5-d", ""}, {"1%a2", "1"},
		{"1%n2", "1"}, {"1%I2", "1"}, {"q%*1d", "q"}, {"q%.*1f", "q"}, {"ok %d then %y", "ok 1 then "}};
	qh_t	*h = Harness ();
	size_t	i;
	int		before;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
	{
		before = QH_NumWarnings (h);
		SP (cases[i][1], S (cases[i][0]), F (1.0f));
		if (!QT_CHECK (QH_NumWarnings (h) > before))
			printf ("  %s: no warning\n", cases[i][0]);
	}
	// a valid format warns about nothing
	before = QH_NumWarnings (h);
	SP ("1%", S ("%d%%"), F (1.0f));
	QT_EQ_I (QH_NumWarnings (h), before);
	QH_Free (h);
}

static void TestFlagsAndWidths (void)
{
	qh_t	*h = Harness ();

	SP ("00042", S ("%05d"), F (42.0f));
	SP ("42   |", S ("%-05d|"), F (42.0f));
	SP ("42   |", S ("%0-5d|"), F (42.0f));
	SP ("7", S ("%00d"), F (7.0f));
	SP ("+007", S ("%+.3d"), F (7.0f));
	SP ("    -007", S ("%08.3d"), F (-7.0f));
	SP ("+1", S ("% +d"), F (1.0f));
	SP ("|", S ("%.0d|"), F (0.0f));
	SP ("010", S ("%#o"), F (8.0f));
	SP ("010", S ("%#.3o"), F (8.0f));
	SP ("0", S ("%#o"), F (0.0f));
	SP ("0XFF", S ("%#X"), F (255.0f));
	SP ("0", S ("%#x"), F (0.0f));
	SP ("5", S ("%+u"), F (5.0f));
	SP ("18446744073709551615", S ("%u"), F (-1.0f));
	SP ("ffffffffffffffff", S ("%x"), F (-1.0f));
	SP ("4294967295", S ("%lu"), I (-1));
	SP ("-9223372036854775808", S ("%d"), F (NAN));
	SP ("-9223372036854775808", S ("%d"), F (1e19f));
	SP ("0", S ("%d"), F (-0.0f));
	SP ("000000AB", S ("%P"), I (0xAB));
	SP ("00ab|", S ("%4p|"), I (0xAB));
	SP ("ab        |", S ("%-10p|"), I (0xAB));
	SP ("0.000000e+00", S ("%e"), F (0.0f));
	SP ("-1.500000E-07", S ("%E"), F (-1.5e-7f));
	SP ("1e+04", S ("%.0e"), F (12345.0f));
	SP ("1.e+04", S ("%#.0e"), F (12345.0f));
	SP ("3.", S ("%#.0f"), F (3.0f));
	SP ("2", S ("%.0f"), F (2.5f));
	SP ("4", S ("%.0f"), F (3.5f));
	SP ("1.00", S ("%.2f"), F (1.005f));
	SP ("100000", S ("%g"), F (100000.0f));
	SP ("1e+06", S ("%g"), F (1e6f));
	SP ("1E-05", S ("%G"), F (1e-5f));
	SP ("0.0001", S ("%g"), F (0.0001f));
	SP ("1.23e+03", S ("%.3g"), F (1234.5f));
	SP ("0.500000", S ("%#g"), F (0.5f));
	SP ("-0", S ("%g"), F (-0.0f));
	SP ("000003.142", S ("%010.3f"), F (PI));
	SP ("+1.0e+00", S ("%+.1e"), F (1.0f));
	SP ("inf", S ("%f"), F (INFINITY));
	SP ("-INF", S ("%F"), F (-INFINITY));
	SP ("  nan|", S ("%5.1f|"), F (NAN));
	SP ("    -inf", S ("%08.1f"), F (-INFINITY));
	SP ("+inf", S ("%+f"), F (INFINITY));
	SP ("-NAN", S ("%E"), F (QC_BitsFloat (0xFFC00000)));
	SP ("abcdefg|", S ("%5s|"), S ("abcdefg"));
	SP ("abc", S ("%.3s"), S ("abcdef"));
	SP ("|", S ("%.0s|"), S ("abc"));
	SP ("A  |", S ("%-3c|"), F (65.0f));
	SP ("  |", S ("%3c|"), F (0.0f));
	SP ("|", S ("%-3c|"), F (0.0f));
	SP ("A", S ("%c"), F (-191.0f));
	SP ("B", S ("%lc"), I (66));
	SP ("   ab|", S ("%#5s|"), S ("ab"));
	SP ("\"ab\"    |", S ("%-8S|"), S ("ab"));
	SP ("\"ab", S ("%.3S"), S ("abc"));
	SP ("", S ("%s"), F (0.0f));
	SP ("", S ("%s"), S (""));
	SP ("1E-05 1E+06 0.5", S ("%V"), V (1e-5f, 1e6f, 0.5f));
	SP ("1 -2 3", S ("%lv"), ((qc_value_t){{1, (uint32_t)-2, 3}}));
	SP ("1    2    3   |", S ("%-4v|"), V (1.0f, 2.0f, 3.0f));
	SP ("ab", S ("%s%s%s"), S ("a"), S ("b"));
	QH_Free (h);
}

static void TestPositionalAndStarArguments (void)
{
	qh_t	*h = Harness ();

	SP ("0", S ("%3$d"), F (1.0f), F (2.0f));
	SP ("1 1 1", S ("%1$d %1$d %d"), F (1.0f), F (2.0f));
	SP ("   42|", S ("%1$*2$d|"), F (42.0f), F (5.0f));
	SP ("3.1|", S ("%.*2$f|"), F (PI), F (1.0f));
	SP ("   3.14|", S ("%*.*f|"), F (7.0f), F (2.0f), F (PI));
	SP ("3.141593", S ("%.*f"), F (-1.0f), F (PI));
	SP ("0|7", S ("%0$d|%d"), F (7.0f));
	// * always reads a float, even for an int argument (whose bits are a tiny float)
	SP ("2", S ("%.*f"), I (2), F (1.5f));
	// a huge * width reads as INT_MIN, which is no width at all
	SP ("1|", S ("%*d|"), F (-3e9f), F (1.0f));
	QH_Free (h);
}

static void TestLengthModifiers (void)
{
	qh_t	*h = Harness ();

	SP ("5", S ("%hhd"), F (5.9f));
	SP ("-5", S ("%lld"), I (-5));
	SP ("1 2 3", S ("%jd %zd %td"), F (1.0f), F (2.0f), F (3.0f));
	SP ("5.000000", S ("%Lf"), I (5));
	SP ("1084227584", S ("%li"), F (5.0f));
	SP ("0", S ("%hx"), I (255));
	// q: 64-bit arguments over two slots (a double, or an int64 with l)
	SP ("1000000000000000", S ("%qd"), Raw64 (DoubleBits (1e15)));
	SP ("-5", S ("%lqd"), Raw64 ((uint64_t)-5));
	SP ("-5", S ("%qi"), Raw64 ((uint64_t)-5));
	SP ("123456789abcdef0", S ("%lqx"), Raw64 (0x123456789ABCDEF0ull));
	SP ("0.100000", S ("%qf"), Raw64 (DoubleBits (0.1)));
	SP ("0.10000000000000001", S ("%.17qg"), Raw64 (DoubleBits (0.1)));
	SP ("-3.000000", S ("%lqf"), Raw64 ((uint64_t)-3));
	SP ("18446744073709551615", S ("%qu"), Raw64 (DoubleBits (-1.0)));
	QH_Free (h);
}

// a string of n copies of c, then tail (malloc'd)
static char *Repeat (char c, size_t n, const char *tail)
{
	size_t	tlen = strlen (tail);
	char	*s = malloc (n + tlen + 1);

	memset (s, c, n);
	memcpy (s + n, tail, tlen + 1);
	return s;
}

static void TestOutputIsCapped (void)
{
	qh_t		*h = Harness ();
	const char	*out;
	char		*big;
	size_t		i;
	bool		spaces;

	out = QH_String (h, "sprintf", ARGS (S ("%70000d"), F (1.0f)));
	QT_EQ_U (strlen (out), 65535);
	for (spaces = true, i = 0 ; out[i] ; i++)
		spaces &= out[i] == ' ';
	QT_CHECK (spaces);
	out = QH_String (h, "sprintf", ARGS (S ("%-70000d"), F (1.0f)));
	QT_EQ_U (strlen (out), 65535);
	QT_EQ_I (out[0], '1');
	out = QH_String (h, "sprintf", ARGS (S ("%.100000f"), F (0.5f)));
	QT_EQ_U (strlen (out), 65535);
	QT_CHECK (!strncmp (out, "0.5000", 6));
	big = Repeat ('x', 70000, "");
	out = QH_String (h, "sprintf", ARGS (S (big)));
	QT_EQ_U (strlen (out), 65535);
	free (big);
	// directives past the cap still take their arguments; nothing more is written
	big = Repeat ('y', 65535, "%d%s");
	out = QH_String (h, "sprintf", ARGS (S (big), F (1.0f), S ("z")));
	QT_EQ_U (strlen (out), 65535);
	free (big);
	// a huge precision on a tiny value prints its exact digits, then zeros
	out = QH_String (h, "sprintf", ARGS (S ("%.1100f"), F (QC_BitsFloat (1))));
	QT_EQ_U (strlen (out), 1102);
	QT_CHECK (!strncmp (out, "0.000000000000000000000000000000000000000000001401298464", 56));
	QH_Free (h);
}

// %c and %s count characters with utf8 on
static void TestPercentCAndSCountCharacters (void)
{
	qh_t	*h;

	// utf8_enable 1 and the UTF-8 scheme: %c writes code points, widths count characters
	h = WithCharset (true, QC_CHARS_UTF8);
	SP ("\xE2\x98\xBA", S ("%c"), F (9786.0f));
	SP ("  \xC3\xA9|", S ("%3c|"), F (233.0f));
	SP ("\xC0\x80", S ("%c"), F (0.0f));
	SP (":", S ("%#c"), F (9786.0f));
	SP ("   h\xC3\xA9|", S ("%5s|"), S ("h\xC3\xA9"));
	SP ("\xC3\xA9   |", S ("%-4s|"), S ("\xC3\xA9"));
	SP ("\xC3\xA9|", S ("%.1s|"), S ("\xC3\xA9" "a"));
	SP ("  h\xC3\xA9|", S ("%#5s|"), S ("h\xC3\xA9"));
	SP (" \"\xC3\xA9\"|", S ("%4S|"), S ("\xC3\xA9"));
	QH_Free (h);
	// utf8_enable 1 and the Quake scheme: a byte a character; what it can't write is ?
	h = WithCharset (true, QC_CHARS_QUAKE);
	SP ("?A", S ("%c%c"), F (200.0f), F (65.0f));
	SP ("\xC1", S ("%c"), F (57537.0f));
	SP ("h\xC3\xA9|", S ("%3s|"), S ("h\xC3\xA9"));
	QH_Free (h);
	// utf8_enable 1 and ISO-8859-1: bytes
	h = WithCharset (true, QC_CHARS_ISO8859_1);
	SP ("\xE9", S ("%c"), F (233.0f));
	SP ("|", S ("%c|"), F (0.0f));
	QH_Free (h);
	// utf8_enable 0: %c is C's %c whatever the scheme
	h = WithCharset (false, QC_CHARS_UTF8);
	SP ("\xE9", S ("%c"), F (233.0f));
	SP (" \xC3\xA9|", S ("%3s|"), S ("\xC3\xA9"));
	QH_Free (h);
}

static const char *Quote (const char *s, size_t bufsize)
{
	static char	out[256];
	qc_sink_t	sink;

	QC_SinkInit (&sink, SIZE_MAX);
	QC_QuoteString (&sink, s, strlen (s), bufsize);
	snprintf (out, sizeof(out), "%s", QC_SinkText (&sink));
	QC_SinkFree (&sink);
	return out;
}

// FTE's COM_QuotedString
static void TestQuoting (void)
{
	QT_EQ_S (Quote ("a b", 100), "\"a b\"");
	QT_EQ_S (Quote ("say \"hi\"", 100), "\\\"say \\\"hi\\\"\"");
	QT_EQ_S (Quote ("x\ny$", 100), "\\\"x\\ny\\$\"");
	QT_EQ_S (Quote ("abcdef", 6), "\"abc\"");
}

int main (void)
{
	TestSpecExamples ();
	TestFormatErrorsStopTheOutput ();
	TestFlagsAndWidths ();
	TestPositionalAndStarArguments ();
	TestLengthModifiers ();
	TestOutputIsCapped ();
	TestPercentCAndSCountCharacters ();
	TestQuoting ();
	return QT_Finish ("lib_format", "sprintf formats as glibc's printf does, with FTE's conventions");
}
