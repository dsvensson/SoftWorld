// test_qc_lib_memory.c -- the QuakeC heap, pointer helpers, temp buffers and
// base64 (qcvm-rs's tests/all/builtins_misc/memory.rs, and the unit tests of
// src/stdlib/memory.rs)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdio.h>
#include <string.h>

static const char	*named[] = {"memrealloc", "memcmp", "base64encode", "base64decode", NULL};

static void Setup (qc_asm_t *a, void *ctx)
{
	(void)ctx;
	QH_AddPeek (a);
	QA_Global (a, "scratch", QC_EV_FLOAT, NULL, 0);
	QH_Named (a, (void *)named);
}

static qh_t *HarnessWith (const qc_config_t *config)
{
	return QH_New (QC_NUMBERING_CSQC, config, Setup, NULL);
}

static qh_t *Harness (void)
{
	return HarnessWith (NULL);
}

// a block of the heap, which must be given
static uint32_t Alloc (qh_t *h, int32_t n)
{
	uint32_t	p = QH_Word (h, "memalloc", ARGS (I (n)));

	QT_CHECK (p != 0);
	return p;
}

// n bytes of VM memory at p are want's
static bool Bytes (qh_t *h, uint32_t p, const void *want, size_t n, int line)
{
	uint8_t	got[256] = {0};
	size_t	i;

	if (!QT_Check (n <= sizeof(got) && QC_ReadMemory (h->vm, p, got, n), "the memory is readable", __FILE__, line))
		return false;
	if (QT_Check (!memcmp (got, want, n), "the bytes are as wanted", __FILE__, line))
		return true;
	printf ("  got");
	for (i = 0 ; i < n ; i++)
		printf (" %02x", got[i]);
	printf ("\n");
	return false;
}
#define BYTES(h, p, want, n)	Bytes ((h), (p), (want), (n), __LINE__)

static const uint8_t	zeros[64] = {0};

static void Write (qh_t *h, uint32_t p, const void *data, size_t n)
{
	QT_CHECK (QC_WriteMemory (h->vm, p, data, n));
}

// the call fails with a builtin error whose message has what in it
static void BuiltinError (qh_t *h, const char *what, int line, const char *name, int argc, const qc_value_t *args)
{
	qc_errkind_t	kind = QH_Fails (h, name, argc, args);

	if (!QT_Check (kind == QC_ERR_BUILTIN && QT_Contains (QH_ErrorMessage (h), what), "a builtin error", __FILE__, line))
		printf ("  %s: kind %d, \"%s\", wanted \"%s\"\n", name, (int)kind, QH_ErrorMessage (h), what);
}
#define BUILTIN_ERROR(h, what, ...)	BuiltinError ((h), (what), __LINE__, __VA_ARGS__)

static uint32_t LE32 (const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// memalloc gives zeroed blocks and memfree releases them
static void TestAllocAndFree (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b, z;

	a = Alloc (h, 10);
	b = Alloc (h, 3);
	QT_CHECK (a != b);
	BYTES (h, a, zeros, 10);
	Write (h, a, "0123456789", 10);
	QH_Raw (h, "memfree", ARGS (W (a)));
	// first fit: the freed block comes back, zeroed again
	QT_EQ_U (Alloc (h, 8), a);
	BYTES (h, a, zeros, 8);
	// size 0 still gives a block; memfree ignores null
	z = Alloc (h, 0);
	QH_Raw (h, "memfree", ARGS (W (z)));
	QH_Raw (h, "memfree", ARGS (W (0)));
	QT_EQ_I (QH_NumWarnings (h), 0);
	// freeing twice, or something that isn't a block, warns
	QH_Raw (h, "memfree", ARGS (W (z)));
	QH_Raw (h, "memfree", ARGS (W (a + 4)));
	QT_EQ_I (QH_NumWarnings (h), 2);
	QH_Free (h);
}

// memalloc refuses bad sizes
static void TestAllocRefuses (void)
{
	qh_t		*h = Harness ();
	qc_config_t	config;

	BUILTIN_ERROR (h, "memalloc", "memalloc", ARGS (I (-1)));
	BUILTIN_ERROR (h, "memalloc", "memalloc", ARGS (I (0x01000001)));
	QT_CHECK (QH_Word (h, "memalloc", ARGS (I (0x01000000))) != 0);
	// developer mode: null and a warning
	QC_SetDeveloper (h->vm, true);
	QT_EQ_U (QH_Word (h, "memalloc", ARGS (I (-5))), 0);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_S (QH_WarningText (h, 0), "memalloc: failure (size -5)");
	QH_Free (h);

	// the heap limit
	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.heap_bytes = 256;
	h = HarnessWith (&config);
	Alloc (h, 200);
	BUILTIN_ERROR (h, "memalloc", "memalloc", ARGS (I (100)));
	QH_Free (h);
}

// memrealloc keeps the contents
static void TestRealloc (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b, c, d;

	a = Alloc (h, 4);
	Write (h, a, "abcd", 4);
	b = QH_Word (h, "memrealloc", ARGS (W (a), I (64)));
	QT_CHECK (b != a);
	BYTES (h, b, "abcd\0\0\0\0", 8);
	// the old block was freed
	QH_Raw (h, "memfree", ARGS (W (a)));
	QT_EQ_I (QH_NumWarnings (h), 1);
	c = QH_Word (h, "memrealloc", ARGS (W (b), I (2)));
	BYTES (h, c, "ab", 2);
	// a null pointer allocates
	d = QH_Word (h, "memrealloc", ARGS (W (0), I (16)));
	BYTES (h, d, zeros, 16);
	BUILTIN_ERROR (h, "memrealloc", "memrealloc", ARGS (W (c + 1), I (8)));
	BUILTIN_ERROR (h, "memrealloc", "memrealloc", ARGS (W (c), I (-1)));
	QH_Free (h);
}

// memgetval counts words and memptradd bytes
static void TestWordsAndBytes (void)
{
	qh_t		*h = Harness ();
	uint32_t	p, q, word, type, want = 42;

	p = Alloc (h, 16);
	// heap pointers are large: FTE computes p + ofs * 4 through a float, which
	// would lose precision here; this is exact
	QT_CHECK (p > 1u << 24);
	QH_Raw (h, "memsetval", ARGS (W (p), F (1), I (42)));
	QH_Raw (h, "memsetval", ARGS (W (p), F (2), F (1.5f)));
	BYTES (h, p + 4, &want, 4);
	QT_EQ_I (QH_Int (h, "memgetval", ARGS (W (p), F (1))), 42);
	QT_EQ_F (QH_Float (h, "memgetval", ARGS (W (p), F (2))), 1.5);
	// memptradd adds bytes: + 8 is word 2
	q = QH_Word (h, "memptradd", ARGS (W (p), F (8)));
	QT_EQ_U (q, p + 8);
	QT_EQ_F (QH_Float (h, "memgetval", ARGS (W (q), F (0))), 1.5);
	QT_EQ_I (QH_Int (h, "memgetval", ARGS (W (q), F (-1))), 42);
	QT_EQ_I (QH_NumWarnings (h), 0);
	// globals are memory too
	if (QT_CHECK (QC_FindGlobal (h->vm, "scratch", &word, &type)))
	{
		QH_Raw (h, "memsetval", ARGS (W (h->vm->progs[0].gbase + word * 4), F (0), F (2.5f)));
		QT_EQ_F (QC_Globals (h->vm)[word].f, 2.5);
	}
	QH_Free (h);
}

// memgetval warns about odd offsets and fails outside memory
static void TestWordOddsAndOutside (void)
{
	static const uint8_t	eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	qh_t					*h = Harness ();
	uint32_t				p, buf;

	p = Alloc (h, 16);
	Write (h, p, eight, 8);
	// half a word: a fractional offset and a misaligned pointer, both only warnings
	QT_EQ_U ((uint32_t)QH_Int (h, "memgetval", ARGS (W (p), F (0.5f))), LE32 (eight + 2));
	QT_EQ_I (QH_NumWarnings (h), 2);
	QT_CHECK (QT_Contains (QH_WarningText (h, 0), "non-integer"));
	QT_CHECK (QT_Contains (QH_WarningText (h, 1), "misaligned"));
	QT_EQ_I (QH_Fails (h, "memgetval", ARGS (W (0x7FFF0000), F (0))), QC_ERR_BAD_POINTER_READ);
	QT_EQ_I (QC_LastError (h->vm)->value, 0x7FFF0000);
	QT_EQ_I (QH_Fails (h, "memsetval", ARGS (W (0x7FFF0000), F (1), I (1))), QC_ERR_BAD_POINTER_WRITE);
	QT_EQ_I (QC_LastError (h->vm)->value, 0x7FFF0004);
	QT_EQ_I (QH_Fails (h, "memsetval", ARGS (W (0), F (0), I (1))), QC_ERR_NULL_POINTER_WRITE);
	// negative results and temp buffers are outside linear memory
	QT_CHECK (QH_Fails (h, "memgetval", ARGS (W (p), F (-1e9f))) != QC_ERR_NONE);
	buf = QH_Word (h, "createbuffer", ARGS (I (8)));
	QT_CHECK (QH_Fails (h, "memgetval", ARGS (W (buf), F (0))) != QC_ERR_NONE);
	// developer mode doesn't make these errors warnings
	QC_SetDeveloper (h->vm, true);
	QT_CHECK (QH_Fails (h, "memgetval", ARGS (W (0x7FFF0000), F (0))) != QC_ERR_NONE);
	QH_Free (h);
}

// memptradd refuses odd offsets
static void TestPtraddRefuses (void)
{
	qh_t		*h = Harness ();
	uint32_t	p;

	p = Alloc (h, 16);
	BUILTIN_ERROR (h, "non-integer", "memptradd", ARGS (W (p), F (4.5f)));
	BUILTIN_ERROR (h, "aligned", "memptradd", ARGS (W (p), F (2)));
	BUILTIN_ERROR (h, "negative", "memptradd", ARGS (W (p), F (-4)));
	// FTE carries on in developer mode
	QC_SetDeveloper (h->vm, true);
	QT_EQ_U (QH_Word (h, "memptradd", ARGS (W (p), F (-4))), p - 4);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_S (QH_WarningText (h, 0), "memptradd: negative offset");
	QH_Free (h);
}

// memcpy and memfill8
static void TestCopyAndFill (void)
{
	static const uint8_t	filled[5] = {'a', 0xFF, 0xFF, 0xFF, 'e'};
	qh_t					*h = Harness ();
	uint32_t				a, b;

	a = Alloc (h, 16);
	b = Alloc (h, 16);
	Write (h, a, "abcdefgh", 8);
	QH_Raw (h, "memcpy", ARGS (W (b), W (a), I (8)));
	BYTES (h, b, "abcdefgh", 8);
	// offsets are bytes; the source's comes first (as FTE implements it)
	QH_Raw (h, "memcpy", ARGS (W (b), W (a), I (2), I (6), I (1)));
	BYTES (h, b, "aghdefgh", 8);
	// overlapping copies behave as memmove
	QH_Raw (h, "memcpy", ARGS (W (a + 2), W (a), I (6)));
	BYTES (h, a, "ababcdef", 8);
	QH_Raw (h, "memcpy", ARGS (W (a), W (a + 2), I (6)));
	BYTES (h, a, "abcdefef", 8);
	// size 0 does nothing, even with null pointers
	QH_Raw (h, "memcpy", ARGS (W (0), W (0), I (0)));
	BUILTIN_ERROR (h, "invalid size", "memcpy", ARGS (W (b), W (a), I (-1)));
	BUILTIN_ERROR (h, "invalid dest", "memcpy", ARGS (W (0), W (a), I (4)));
	BUILTIN_ERROR (h, "invalid source", "memcpy", ARGS (W (b), W (0x7FFF0000), I (4)));

	QH_Raw (h, "memfill8", ARGS (W (b), I (0x1FF), I (3), I (1)));
	BYTES (h, b, filled, 5);
	QH_Raw (h, "memfill8", ARGS (W (b), I (0), I (0)));
	BUILTIN_ERROR (h, "invalid dest", "memfill8", ARGS (W (b), I (0), I (-1)));
	BUILTIN_ERROR (h, "invalid dest", "memfill8", ARGS (W (0), I (0), I (4)));
	QH_Free (h);
}

// memcmp compares bytes
static void TestCompare (void)
{
	qh_t		*h = Harness ();
	uint32_t	a, b;

	a = Alloc (h, 16);
	b = Alloc (h, 16);
	Write (h, a, "hello world", 11);
	Write (h, b, "help", 4);
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (a), W (b), I (3))), 0);
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (a), W (b), I (4))), 'l' - 'p');
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (b), W (a), I (4))), 'p' - 'l');
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (a), W (b), I (0))), 0);
	// offsets: a + 2 ("llo") against b + 2 ("lp")
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (a), W (b), I (1), I (2), I (2))), 0);
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (a), W (b), I (2), I (2), I (2))), 'l' - 'p');
	// the first offset is the first pointer's (as documented; FTE's implementation
	// swaps them): a + 2 ("l") against b ("h")
	QT_EQ_I (QH_Int (h, "memcmp", ARGS (W (a), W (b), I (1), I (2), I (0))), 'l' - 'h');
	BUILTIN_ERROR (h, "invalid size", "memcmp", ARGS (W (a), W (b), I (-1)));
	BUILTIN_ERROR (h, "invalid", "memcmp", ARGS (W (0x7FFF0000), W (b), I (1)));
	QH_Free (h);
}

// createbuffer gives temp buffers that grow
static void TestCreatebuffer (void)
{
	qh_t		*h = Harness ();
	uint32_t	buf, a;
	int32_t		seven = 7;

	QT_EQ_U (QH_Word (h, "createbuffer", ARGS (I (0))), 0);
	QT_EQ_U (QH_Word (h, "createbuffer", ARGS (I (-3))), 0);
	buf = QH_Word (h, "createbuffer", ARGS (I (8)));
	QT_CHECK (QC_IsTempString (buf));
	QT_EQ_I (QH_Peek (h, buf, 0), 0);
	a = Alloc (h, 16);
	Write (h, a, &seven, 4);
	QH_Raw (h, "memcpy", ARGS (W (buf), W (a), I (4), I (0), I (4)));
	QT_EQ_I (QH_Peek (h, buf, 1), 7);
	// writing past the end grows the buffer
	QH_Raw (h, "memcpy", ARGS (W (buf), W (a), I (4), I (0), I (100)));
	QT_EQ_I (QH_Peek (h, buf, 25), 7);
	QH_Raw (h, "memfill8", ARGS (W (buf), I (1), I (4), I (200)));
	QT_EQ_I (QH_Peek (h, buf, 50), 0x01010101);
	// reading from it too
	QH_Raw (h, "memcpy", ARGS (W (a), W (buf), I (4), I (100)));
	BYTES (h, a, &seven, 4);
	BUILTIN_ERROR (h, "invalid source", "memcpy", ARGS (W (a), W (buf), I (4), I (10000)));
	// not freeable
	QH_Raw (h, "memfree", ARGS (W (buf)));
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Free (h);
}

// base64 round trips through the heap
static void TestBase64Heap (void)
{
	static const uint8_t	decoded[2] = {0xFB, 0xFF};
	qh_t					*h = Harness ();
	uint32_t				a, p;

	a = Alloc (h, 8);
	Write (h, a, "foobar", 6);
	QT_EQ_S (QH_String (h, "base64encode", ARGS (W (a), I (6))), "Zm9vYmFy");
	QT_EQ_S (QH_String (h, "base64encode", ARGS (W (a), I (4))), "Zm9vYg==");
	QT_EQ_S (QH_String (h, "base64encode", ARGS (W (a), I (0))), "");
	// temp strings are readable memory too
	QT_EQ_S (QH_String (h, "base64encode", ARGS (QH_S (h, "hi"), I (2))), "aGk=");
	BUILTIN_ERROR (h, "invalid", "base64encode", ARGS (W (0x7FFF0000), I (4)));
	BUILTIN_ERROR (h, "invalid", "base64encode", ARGS (W (a), I (-1)));

	p = QH_Word (h, "base64decode", ARGS (QH_S (h, "Zm9vYmFy"), I (0)));
	QT_EQ_U (QH_ParmWord (h, 1), 6);
	BYTES (h, p, "foobar", 7);
	QH_Raw (h, "memfree", ARGS (W (p)));
	// FTE's lenient decoder: line breaks skipped, URL-safe symbols, padding ends the data
	p = QH_Word (h, "base64decode", ARGS (QH_S (h, "Zm9v\nYg==trailing"), I (0)));
	QT_EQ_U (QH_ParmWord (h, 1), 4);
	BYTES (h, p, "foob", 4);
	p = QH_Word (h, "base64decode", ARGS (QH_S (h, "-_8"), I (0)));
	QT_EQ_U (QH_ParmWord (h, 1), 2);
	BYTES (h, p, decoded, 2);
	// an empty string still gives a (NUL-terminated) block
	p = QH_Word (h, "base64decode", ARGS (QH_S (h, ""), I (9)));
	QT_CHECK (p != 0);
	QT_EQ_U (QH_ParmWord (h, 1), 0);
	QT_EQ_I (QH_NumWarnings (h), 0);
	QH_Free (h);
}

// the encoder's text of some bytes, and the decoder's bytes of some text
static const char *Encode (const char *data, size_t len)
{
	static char	out[64];
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_Base64Encode (&s, (const uint8_t *)data, len);
	snprintf (out, sizeof(out), "%s", QC_SinkText (&s));
	QC_SinkFree (&s);
	return out;
}

static bool Decodes (const char *text, size_t cap, const void *want, size_t wantlen)
{
	qc_sink_t	s;
	bool		ok;

	QC_SinkInit (&s, SIZE_MAX);
	QC_Base64Decode (&s, text, strlen (text), cap);
	ok = s.len == wantlen && (!wantlen || !memcmp (s.buf, want, wantlen));
	QC_SinkFree (&s);
	return ok;
}

// base64 round trips (the unit test)
static void TestBase64RoundTrip (void)
{
	static const char	*cases[] = {"f", "fo", "foo", "foob", "fooba", "foobar"};
	char				enc[64];
	size_t				i, len;

	QT_EQ_S (Encode ("", 0), "");
	QT_EQ_S (Encode ("f", 1), "Zg==");
	QT_EQ_S (Encode ("fo", 2), "Zm8=");
	QT_EQ_S (Encode ("foo", 3), "Zm9v");
	QT_EQ_S (Encode ("foobar", 6), "Zm9vYmFy");
	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
	{
		len = strlen (cases[i]);
		snprintf (enc, sizeof(enc), "%s", Encode (cases[i], len));
		if (!QT_CHECK (Decodes (enc, QC_Base64Capacity (strlen (enc)), cases[i], len)))
			printf ("  %s\n", cases[i]);
	}
}

// base64's quirks: URL-safe symbols, line breaks, invalid symbols (the unit test)
static void TestBase64Quirks (void)
{
	static const uint8_t	urlsafe[3] = {0xFB, 0xFF, 0xBF}, invalid[1] = {0x64};

	QT_CHECK (Decodes ("-_-_", 4, urlsafe, 3));
	QT_CHECK (Decodes ("Zm9v\nYmFy", 10, "foobar", 6));
	QT_CHECK (Decodes ("Z!==", 4, invalid, 1));
	QT_CHECK (Decodes ("Z", 4, "", 0));
}

int main (void)
{
	TestAllocAndFree ();
	TestAllocRefuses ();
	TestRealloc ();
	TestWordsAndBytes ();
	TestWordOddsAndOutside ();
	TestPtraddRefuses ();
	TestCopyAndFill ();
	TestCompare ();
	TestCreatebuffer ();
	TestBase64Heap ();
	TestBase64RoundTrip ();
	TestBase64Quirks ();
	return QT_Finish ("lib_memory", "the heap, pointer, buffer and base64 builtins behave as FTE's");
}
