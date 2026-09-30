// test_qc_lib_charset.c -- the charset settings (utf8_enable and the character
// scheme) across the builtins that honour them, and the decoders and encoders
// under them (qcvm-rs's tests/all/builtins_strings/charset.rs and the unit
// tests of src/stdlib/charset.rs, docs/spec/strings.md)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdio.h>
#include <string.h>

// builtins FTE binds by name that its CSQC declarations don't list
static const char	*extra[] = {"argc", "instr", "ftou", "utof", "strcmp", NULL};

// "héllo" in UTF-8: six bytes, five characters
#define HELLO	"h\xC3\xA9llo"

// a CSQC harness with the charset settings
static qh_t *WithCharset (bool utf8, qc_charscheme_t scheme)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.utf8 = utf8;
	config.charscheme = scheme;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, extra);
}

// every combination of the settings, and whether characters count as UTF-8
static const struct
{
	bool			utf8;
	qc_charscheme_t	scheme;
	bool			chars;
} configs[6] = {
	{false, QC_CHARS_QUAKE, false},
	{false, QC_CHARS_UTF8, false},
	{false, QC_CHARS_ISO8859_1, false},
	{true, QC_CHARS_QUAKE, false},
	{true, QC_CHARS_UTF8, true},
	{true, QC_CHARS_ISO8859_1, false},
};

static const char *SchemeName (qc_charscheme_t scheme)
{
	return scheme == QC_CHARS_UTF8 ? "Utf8" : scheme == QC_CHARS_ISO8859_1 ? "Iso8859_1" : "Quake";
}

static void Context (int i)
{
	printf ("  utf8=%d scheme=%s\n", configs[i].utf8, SchemeName (configs[i].scheme));
}

static void TestLengthsAndOffsets (void)
{
	qh_t	*h;
	float	want;
	int		i;
	bool	ok;

	for (i = 0 ; i < 6 ; i++)
	{
		h = WithCharset (configs[i].utf8, configs[i].scheme);
		ok = QT_EQ_F (QH_Float (h, "strlen", ARGS (QH_S (h, HELLO))), configs[i].chars ? 5 : 6);
		ok &= QT_EQ_F (QH_Float (h, "memstrsize", ARGS (QH_S (h, HELLO))), 6);
		ok &= QT_EQ_S (QH_String (h, "substring", ARGS (QH_S (h, HELLO), F (1), F (2))),
			configs[i].chars ? "\xC3\xA9l" : "\xC3\xA9");
		ok &= QT_EQ_S (QH_String (h, "substring", ARGS (QH_S (h, HELLO), F (-2), F (-1))), "lo");
		ok &= QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, HELLO), QH_S (h, "l"))), configs[i].chars ? 2 : 3);
		ok &= QT_EQ_F (QH_Float (h, "strstrofs", ARGS (QH_S (h, HELLO), QH_S (h, "l"), F (3))), 3);
		if (configs[i].utf8 && configs[i].scheme == QC_CHARS_UTF8)
			want = 233;
		else if (configs[i].utf8 && configs[i].scheme == QC_CHARS_QUAKE)
			want = (float)0xE0C3;
		else
			want = 195;
		ok &= QT_EQ_F (QH_Float (h, "str2chr", ARGS (QH_S (h, HELLO), F (1))), want);
		ok &= QT_EQ_F (QH_Float (h, "str2chr", ARGS (QH_S (h, HELLO), F (-1))), 111);
		// strncmp's length counts characters: "hél" and "héL" differ in the third one
		ok &= QT_EQ_I (QH_Float (h, "strncmp", ARGS (QH_S (h, HELLO), QH_S (h, "h\xC3\xA9Llo"), F (3))) != 0,
			configs[i].chars);
		ok &= QT_EQ_F (QH_Float (h, "strncmp", ARGS (QH_S (h, "xx\xC3\xA9" "a"), QH_S (h, "\xC3\xA9" "b"), F (1), F (2))),
			0);
		// bytes always: strpad, strconv, uri, info, the tokenizers
		ok &= QT_EQ_S (QH_String (h, "strpad", ARGS (F (8), QH_S (h, HELLO))), HELLO "  ");
		ok &= QT_EQ_F (QH_Float (h, "tokenizebyseparator", ARGS (QH_S (h, HELLO), QH_S (h, "\xC3\xA9"))), 2);
		ok &= QT_EQ_F (QH_Float (h, "argv_start_index", ARGS (F (1))), 3);
		if (!ok)
			Context (i);
		QH_Free (h);
	}
}

static void TestChr2strPerConfig (void)
{
	qh_t		*h;
	const char	*want;
	int			i;

	for (i = 0 ; i < 6 ; i++)
	{
		h = WithCharset (configs[i].utf8, configs[i].scheme);
		if (!configs[i].utf8 && configs[i].scheme == QC_CHARS_UTF8)
			want = "\xE9\xE2\x98\xBA";
		else if (!configs[i].utf8)
			want = "\xE9^U263a";
		else if (configs[i].scheme == QC_CHARS_QUAKE)
			want = "?^U263a";
		else if (configs[i].scheme == QC_CHARS_UTF8)
			want = "\xC3\xA9\xE2\x98\xBA";
		else
			want = "\xE9^U263a";
		if (!QT_EQ_S (QH_String (h, "chr2str", ARGS (F (233), F (9786))), want))
			Context (i);
		QH_Free (h);
	}
}

static void TestCaseAndColourFollowTheSchemeOnly (void)
{
	qh_t		*h;
	const char	*up, *plain;
	int			i;
	bool		ok;

	for (i = 0 ; i < 6 ; i++)
	{
		h = WithCharset (configs[i].utf8, configs[i].scheme);
		switch (configs[i].scheme)
		{
		case QC_CHARS_UTF8:
			up = "A\xEF\xBF\xBD\x0b";
			plain = "ai";
			break;
		case QC_CHARS_ISO8859_1:
			up = "A\xE9\x0b";
			plain = "a\xE9";
			break;
		default:
			up = "A\xE9\x0b";
			plain = "ai";
			break;
		}
		ok = QT_EQ_S (QH_String (h, "strtoupper", ARGS (QH_S (h, "a\xE9\x0b"))), up);
		ok &= QT_EQ_S (QH_String (h, "strdecolorize", ARGS (QH_S (h, "^2a\xE9"))), plain);
		if (!ok)
			Context (i);
		QH_Free (h);
	}
}

/*
==============================================================================

THE DECODERS AND ENCODERS (the unit tests of src/stdlib/charset.rs)

==============================================================================
*/

// the bytes of ch in FTE's UTF-8, into out (at least 8 bytes); their count
static size_t Utf8 (uint32_t ch, uint8_t *out)
{
	qc_sink_t	s;
	size_t		n;

	QC_SinkInit (&s, SIZE_MAX);
	QC_EncodeUtf8 (&s, ch);
	n = s.len;
	memcpy (out, QC_SinkText (&s), n);
	QC_SinkFree (&s);
	return n;
}

// whether the bytes of ch in FTE's UTF-8 are want's
static bool CheckUtf8 (uint32_t ch, const char *want, size_t wantlen)
{
	uint8_t	got[8];
	size_t	n = Utf8 (ch, got);

	if (QT_CHECK (n == wantlen && !memcmp (got, want, n)))
		return true;
	printf ("  U+%X: %zu bytes\n", ch, n);
	return false;
}

// whether a character written in a scheme is want
static bool CheckEncoded (uint32_t ch, qc_charscheme_t scheme, bool markup, const char *want)
{
	qc_sink_t	s;
	bool		ok;

	QC_SinkInit (&s, SIZE_MAX);
	QC_EncodeChar (&s, ch, scheme, markup);
	ok = QT_EQ_S (QC_SinkText (&s), want);
	if (!ok)
		printf ("  U+%X in %s\n", ch, SchemeName (scheme));
	QC_SinkFree (&s);
	return ok;
}

// one decoded character: its code point, bytes and error
static void CheckDecoded (const char *s, size_t len, uint32_t ch, size_t used, qc_utf8err_t err)
{
	size_t			gotused;
	qc_utf8err_t	goterr;
	uint32_t		got = QC_DecodeUtf8 ((const uint8_t *)s, len, &gotused, &goterr);

	if (!QT_CHECK (got == ch && gotused == used && goterr == err))
		printf ("  decoded U+%X, %zu bytes, error %d\n", got, gotused, goterr);
}

static void CheckScheme (const char *s, qc_charscheme_t scheme, uint32_t ch)
{
	size_t		used;
	uint32_t	got = QC_DecodeChar ((const uint8_t *)s, strlen (s), scheme, &used);

	if (!QT_CHECK (got == ch && used == 1))
		printf ("  decoded U+%X, %zu bytes in %s\n", got, used, SchemeName (scheme));
}

static void TestUtf8EncoderMatchesStd (void)
{
	static const struct
	{
		uint32_t	ch;
		const char	*bytes;
		size_t		len;
	} scalars[] = {
		{1, "\x01", 1},
		{0x41, "A", 1},
		{0x7F, "\x7F", 1},
		{0x80, "\xC2\x80", 2},
		{0x7FF, "\xDF\xBF", 2},
		{0x800, "\xE0\xA0\x80", 3},
		{0xFFFD, "\xEF\xBF\xBD", 3},
		{0x10000, "\xF0\x90\x80\x80", 4},
		{0x10FFFF, "\xF4\x8F\xBF\xBF", 4},
	};
	uint8_t	a[8], b[8];
	size_t	i, na, nb;

	for (i = 0 ; i < sizeof(scalars) / sizeof(scalars[0]) ; i++)
		CheckUtf8 (scalars[i].ch, scalars[i].bytes, scalars[i].len);
	CheckUtf8 (0, "\xC0\x80", 2);
	CheckUtf8 (0x200000, "\xF8\x88\x80\x80\x80", 5);
	CheckUtf8 (0x7FFFFFFF, "\xFD\xBF\xBF\xBF\xBF\xBF", 6);
	na = Utf8 (0x80000000, a);
	nb = Utf8 (QC_REPLACEMENT, b);
	QT_CHECK (na == nb && !memcmp (a, b, na));
}

static void TestUtf8DecoderRoundTripsLongForms (void)
{
	static const uint32_t	chars[] = {0, 0x41, 0x7FF, 0xFFFD, 0x10FFFF, 0x200000, 0x7FFFFFFF};
	uint8_t					bytes[8];
	size_t					i, n, used;
	qc_utf8err_t			err;
	uint32_t				ch;

	for (i = 0 ; i < sizeof(chars) / sizeof(chars[0]) ; i++)
	{
		n = Utf8 (chars[i], bytes);
		ch = QC_DecodeUtf8 (bytes, n, &used, &err);
		if (!QT_CHECK (ch == chars[i] && used == n))
			printf ("  U+%X came back as U+%X, %zu bytes\n", chars[i], ch, used);
	}
}

static void TestUtf8DecoderIsLenient (void)
{
	size_t			used;
	qc_utf8err_t	err;

	CheckDecoded ("\x80", 1, 0xE080, 1, QC_UTF8_MALFORMED);
	QT_CHECK (QC_DecodeUtf8 ((const uint8_t *)"\xFF", 1, &used, &err) == 0xE0FF && used == 1);
	QT_CHECK (QC_DecodeUtf8 ((const uint8_t *)"\xE1" "bc", 3, &used, &err) == QC_REPLACEMENT && used == 1);
	CheckDecoded ("\xC1\xA1", 2, 0x61, 2, QC_UTF8_ILLEGAL);
	CheckDecoded ("\xC0\x80", 2, 0, 2, QC_UTF8_OK);
	// CESU-8's surrogate pair for U+1F600
	CheckDecoded ("\xED\xA0\xBD\xED\xB8\x80", 6, 0x1F600, 6, QC_UTF8_OK);
	CheckDecoded ("\xED\xA0\xBDx", 4, 0xD83D, 3, QC_UTF8_LONE_HIGH);
	QC_DecodeUtf8 ((const uint8_t *)"\xED\xB8\x80", 3, &used, &err);
	QT_EQ_I (err, QC_UTF8_LOW);
	// a six-byte form cut short is malformed (FTE would read past the end)
	QT_CHECK (QC_DecodeUtf8 ((const uint8_t *)"\xFD\xBF\xBF\xBF\xBF", 5, &used, &err) == QC_REPLACEMENT && used == 1);
}

static void TestQuakeScheme (void)
{
	CheckScheme ("a", QC_CHARS_QUAKE, 0x61);
	CheckScheme ("\n", QC_CHARS_QUAKE, 0x0A);
	CheckScheme ("\x0B", QC_CHARS_QUAKE, 0xE00B);
	CheckScheme ("\x7F", QC_CHARS_QUAKE, 0x7F);
	CheckScheme ("\xE1", QC_CHARS_QUAKE, 0xE0E1);
	CheckEncoded (0xE0E1, QC_CHARS_QUAKE, false, "\xE1");
	CheckEncoded (0xE00B, QC_CHARS_QUAKE, false, "\x0B");
	CheckEncoded (0xE00A, QC_CHARS_QUAKE, true, "^Ue00a");
	CheckEncoded (0x263A, QC_CHARS_QUAKE, true, "^U263a");
	CheckEncoded (0x263A, QC_CHARS_QUAKE, false, "?");
	CheckEncoded (0x1F600, QC_CHARS_QUAKE, true, "^{1f600}");
	CheckEncoded (0xC8, QC_CHARS_QUAKE, false, "?");
}

static void TestIsoScheme (void)
{
	CheckScheme ("\xE9", QC_CHARS_ISO8859_1, 0xE9);
	CheckEncoded (0xE9, QC_CHARS_ISO8859_1, false, "\xE9");
	CheckEncoded (0xE041, QC_CHARS_ISO8859_1, false, "A");
	CheckEncoded (0x263A, QC_CHARS_ISO8859_1, true, "^U263a");
}

static void TestOffsets (void)
{
	const uint8_t	*s = (const uint8_t *)HELLO;
	size_t			len = strlen (HELLO);

	QT_EQ_U (QC_CharCount (s, len, QC_CHARS_UTF8), 5);
	QT_EQ_U (QC_CharCount (s, len, QC_CHARS_QUAKE), 6);
	QT_EQ_U (QC_ByteOffset (s, len, 2, QC_CHARS_UTF8), 3);
	QT_EQ_U (QC_ByteOffset (s, len, 9, QC_CHARS_UTF8), 6);
	QT_EQ_U (QC_CharOffset (s, len, 3, QC_CHARS_UTF8), 2);
	QT_EQ_U (QC_CharOffset (s, len, 2, QC_CHARS_UTF8), 1);
}

int main (void)
{
	TestLengthsAndOffsets ();
	TestChr2strPerConfig ();
	TestCaseAndColourFollowTheSchemeOnly ();
	TestUtf8EncoderMatchesStd ();
	TestUtf8DecoderRoundTripsLongForms ();
	TestUtf8DecoderIsLenient ();
	TestQuakeScheme ();
	TestIsoScheme ();
	TestOffsets ();
	return QT_Finish ("lib_charset", "the charset settings and schemes read and write characters as FTE does");
}
