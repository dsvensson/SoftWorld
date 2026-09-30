// qc_lib_string.c -- the string builtins: strlen, strcat, substring, strconv,
// strpad, strreplace, strncmp, info strings, URIs, decolorizing...
// (docs/spec/strings.md)
//
// Offsets and lengths count bytes, unless the VM's utf8 setting (FTE's
// utf8_enable) is on: then those FTE makes aware of UTF-8 count characters of
// the configured scheme.

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the lengths of FTE's fixed buffers
#define QC_TEMPBUF			4095	// strconv's input, strpad, info strings
#define QC_CASE_MAX			8191	// case conversions
#define QC_FUN_MAX			8191	// the markup parser's characters
#define QC_DECOLOR_MAX		8190	// strdecolorize's output

#define QC_INFO_FIELD_MAX	1022	// keys and values this long aren't found
#define QC_INFO_MAX			4096	// an info string, its terminator included
#define QC_INFO_PAIR_MAX	1023	// a \key\value pair
#define QC_INFO_KEY_MAX		256		// keys are shorter

static bool QC_Utf8On (const qcvm_t *vm)
{
	return vm->config.utf8;
}

static qc_charscheme_t QC_Scheme (const qcvm_t *vm)
{
	return vm->config.charscheme;
}

static int32_t QC_Wrap (int64_t v)
{
	return (int32_t)(uint32_t)(uint64_t)v;
}

// FTE's unicode_byteofsfromcharofs with a C int offset: a negative one (huge as
// unsigned) runs to the end
static size_t QC_ByteOfs (const char *s, size_t len, int32_t chars, qc_charscheme_t scheme)
{
	if (chars < 0)
		return len;
	return QC_ByteOffset ((const uint8_t *)s, len, (size_t)chars, scheme);
}

// the length in bytes, or characters with utf8 on, as a C int
static int32_t QC_Length (const qcvm_t *vm, const char *s, size_t len)
{
	size_t	n = QC_Utf8On (vm) ? QC_CharCount ((const uint8_t *)s, len, QC_Scheme (vm)) : len;

	return n > INT32_MAX ? INT32_MAX : (int32_t)n;
}

// a copy of an argument's text, for when more strings are read before it's used
static char *QC_ArgCopy (qcvm_t *vm, int i, size_t *len)
{
	const char	*s = QC_ArgString (vm, i);
	size_t		n = strlen (s);
	char		*copy = malloc (n + 1);

	if (!copy)
	{
		QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
		return NULL;
	}
	memcpy (copy, s, n + 1);
	if (len)
		*len = n;
	return copy;
}

/*
==============================================================================

LENGTHS, CONCATENATION, SUBSTRINGS

==============================================================================
*/

// float strlen(string): bytes, or characters with utf8 on
static bool QC_Strlen (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);

	QC_ReturnFloat (vm, (float)QC_Length (vm, s, strlen (s)));
	return true;
}

// float memstrsize(string): bytes, whatever the charset
static bool QC_Memstrsize (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)strlen (QC_ArgString (vm, 0)));
	return true;
}

// string strcat(string...) (and strzone): the arguments joined, no length limit
static bool QC_Strcat (qcvm_t *vm)
{
	int			argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, i;
	size_t		total = 0, len;
	qc_sink_t	s;
	const char	*text;

	// refused before a result is made that the temp strings have no room for
	for (i = 0 ; i < argc ; i++)
		total += strlen (QC_ArgString (vm, i));
	if (!QC_StringsFit (&vm->strings, total))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	QC_SinkInit (&s, SIZE_MAX);
	for (i = 0 ; i < argc ; i++)
	{
		text = QC_ArgString (vm, i);
		len = strlen (text);
		QC_SinkAppend (&s, text, len);
	}
	return QC_LibReturnSink (vm, &s);
}

// void strunzone(string): nothing; temp strings are collected
static bool QC_Strunzone (qcvm_t *vm)
{
	(void)vm;
	return true;
}

// string substring(string s, float start, float length): a negative start counts
// from the end; a negative length leaves that many characters but one off the
// end (-1: to the end)
static bool QC_Substring (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	size_t		len = strlen (s), from, n;
	int32_t		start = QC_LibArgInt (vm, 1), length = QC_LibArgInt (vm, 2), slen = QC_Length (vm, s, len);

	if (start < 0)
		start = QC_Wrap ((int64_t)slen + start);
	if (length < 0)
		length = QC_Wrap ((int64_t)slen - start + ((int64_t)length + 1));
	if (start < 0)
		start = 0;
	if (start >= slen || length <= 0)
		return QC_ReturnString (vm, "", 0);
	if (length > slen - start)
		length = slen - start;
	if (QC_Utf8On (vm))
	{
		from = QC_ByteOfs (s, len, start, QC_Scheme (vm));
		n = QC_ByteOfs (s + from, len - from, length, QC_Scheme (vm));
	}
	else
	{
		from = (size_t)start;
		n = (size_t)length;
	}
	if (from > len)
		from = len;
	if (n > len - from)
		n = len - from;
	return QC_ReturnString (vm, s + from, n);
}

int64_t QC_Find (const char *haystack, size_t hlen, const char *needle, size_t nlen)
{
	size_t	*fail, i, k;

	if (!nlen)
		return 0;
	if (nlen > hlen)
		return -1;
	if (nlen <= 16)
	{
		for (i = 0 ; i + nlen <= hlen ; i++)
			if (!memcmp (haystack + i, needle, nlen))
				return (int64_t)i;
		return -1;
	}
	// Knuth-Morris-Pratt, so that untrusted progs can't make a search
	// quadratic; fail[i] the longest proper border of needle[..i]
	fail = malloc (nlen * sizeof(*fail));
	if (!fail)
		return -1;
	fail[0] = 0;
	for (k = 0, i = 1 ; i < nlen ; i++)
	{
		while (k && needle[k] != needle[i])
			k = fail[k - 1];
		if (needle[k] == needle[i])
			k++;
		fail[i] = k;
	}
	for (k = 0, i = 0 ; i < hlen ; i++)
	{
		while (k && needle[k] != haystack[i])
			k = fail[k - 1];
		if (needle[k] == haystack[i])
			k++;
		if (k == nlen)
		{
			free (fail);
			return (int64_t)(i + 1 - k);
		}
	}
	free (fail);
	return -1;
}

// float strstrofs(string s, string sub, optional float start): where the first
// sub at or after start is, or -1
static bool QC_Strstrofs (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0), *sub = QC_ArgString (vm, 1);
	size_t		len = strlen (s), at;
	int32_t		start = QC_FloatToInt (QC_LibOptFloat (vm, 2, 0));
	int64_t		first = QC_Utf8On (vm) ? (int64_t)QC_ByteOfs (s, len, start, QC_Scheme (vm)) : start, k;

	if (first != 0 && (first < 0 || first > (int64_t)len))
	{
		QC_ReturnFloat (vm, -1);
		return true;
	}
	k = QC_Find (s + first, len - (size_t)first, sub, strlen (sub));
	if (k < 0)
	{
		QC_ReturnFloat (vm, -1);
		return true;
	}
	at = (size_t)(first + k);
	QC_ReturnFloat (vm, (float)(QC_Utf8On (vm) ? QC_CharOffset ((const uint8_t *)s, len, at, QC_Scheme (vm)) : at));
	return true;
}

// float str2chr(string s, optional float index): the byte (or with utf8 on, the
// character) at index; negative counts from the end; 0 out of range
static bool QC_Str2chr (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	size_t		len = strlen (s), used;
	int32_t		ofs = QC_FloatToInt (QC_LibOptFloat (vm, 1, 0));
	uint32_t	r = 0;

	if (QC_Utf8On (vm))
	{
		if (ofs < 0)
			ofs = QC_Wrap ((int64_t)QC_Length (vm, s, len) + ofs);
		ofs = QC_Wrap ((int64_t)QC_ByteOfs (s, len, ofs, QC_Scheme (vm)));
	}
	else if (ofs < 0)
		ofs = QC_Wrap ((int64_t)(len > INT32_MAX ? INT32_MAX : (int32_t)len) + ofs);
	if (!(ofs != 0 && (ofs < 0 || (size_t)ofs > len)) && (size_t)ofs < len)
		r = QC_Utf8On (vm) ? QC_DecodeChar ((const uint8_t *)s + ofs, len - (size_t)ofs, QC_Scheme (vm), &used)
			: (uint8_t)s[ofs];
	QC_ReturnFloat (vm, (float)r);
	return true;
}

// string chr2str(float...): a character an argument. Without utf8, codes up to
// 255 are bytes; larger ones are written in the charset's scheme, with FTE's
// ^U and ^{} markup where it can't. A 0 ends the string.
static bool QC_Chr2str (qcvm_t *vm)
{
	int			argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, i;
	int32_t		ch;
	qc_sink_t	s;
	char		*nul;

	QC_SinkInit (&s, SIZE_MAX);
	for (i = 0 ; i < argc ; i++)
	{
		ch = QC_FloatToInt (QC_ArgFloat (vm, i));
		if (QC_Utf8On (vm) || ch > 0xFF)
			QC_EncodeChar (&s, (uint32_t)ch, QC_Scheme (vm), ch > 0xFF);
		else
			QC_SinkPush (&s, (char)(uint8_t)ch);
	}
	if (s.buf && (nul = memchr (s.buf, 0, s.len)))
		s.len = (size_t)(nul - s.buf);
	return QC_LibReturnSink (vm, &s);
}

/*
==============================================================================

CONVERSIONS

==============================================================================
*/

static uint8_t QC_ConvDigit (uint8_t b, uint8_t base, int32_t mode)
{
	uint8_t	newbase;

	switch (mode)
	{
	case 1:		newbase = '0'; break;
	case 2:		newbase = '0' + 0x80; break;
	case 3:		newbase = '0' - 30; break;
	case 4:		newbase = '0' + 0x80 - 30; break;
	default:	newbase = base; break;
	}
	return (uint8_t)(b - base + newbase);
}

static uint8_t QC_ConvAlpha (uint8_t b, uint8_t casebase, uint8_t colourbase, int32_t ccase, int32_t redalpha,
	size_t i)
{
	uint8_t	letter = (uint8_t)(b - casebase - colourbase), colour, c;

	switch (redalpha)
	{
	case 1:		colour = 0; break;
	case 2:		colour = 0x80; break;
	case 5:		colour = i % 2 == 0 ? 0x80 : 0; break;
	case 6:		colour = i % 2 != 0 ? 0x80 : 0; break;
	default:	colour = colourbase; break;
	}
	c = ccase == 1 ? 'a' : ccase == 2 ? 'A' : casebase;
	return (uint8_t)(letter + c + colour);
}

// string strconv(float ccase, float redalpha, float redchars, string...): Quake's
// characters between cases and white, red and gold (ccase 1 lower, 2 upper;
// redalpha 1 white, 2 red, 5 and 6 alternating; redchars for digits 1 white, 2
// red, 3 gold low, 4 gold high)
static bool QC_Strconv (qcvm_t *vm)
{
	int32_t		ccase = QC_LibArgInt (vm, 0), redalpha = QC_LibArgInt (vm, 1), rednum = QC_LibArgInt (vm, 2);
	size_t		len, i;
	char		*text = QC_LibConcat (vm, 3, &len);
	uint8_t		b;
	bool		ok;

	if (!text)
		return false;
	if (len > QC_TEMPBUF)
		len = QC_TEMPBUF;
	for (i = 0 ; i < len ; i++)
	{
		b = (uint8_t)text[i];
		if (b >= '0' && b <= '9')
			b = QC_ConvDigit (b, '0', rednum);
		else if (b >= 0xB0 && b <= 0xB9)
			b = QC_ConvDigit (b, 0xB0, rednum);
		else if (b >= 0x92 && b <= 0x9B)
			b = QC_ConvDigit (b, 0x92, rednum);
		else if (b >= 0x12 && b <= 0x1B)
			b = QC_ConvDigit (b, 0x12, rednum);
		else if (b >= 'a' && b <= 'z')
			b = QC_ConvAlpha (b, 'a', 0, ccase, redalpha, i);
		else if (b >= 'A' && b <= 'Z')
			b = QC_ConvAlpha (b, 'A', 0, ccase, redalpha, i);
		else if (b >= 0xE1 && b <= 0xFA)
			b = QC_ConvAlpha (b, 'a', 0x80, ccase, redalpha, i);
		else if (b >= 0xC1 && b <= 0xDA)
			b = QC_ConvAlpha (b, 'A', 0x80, ccase, redalpha, i);
		else if ((b & 0x7F) >= 0x10 && redalpha == 1)
			b &= 0x7F;
		else if ((b & 0x7F) >= 0x10 && redalpha == 2)
			b |= 0x80;
		text[i] = (char)b;
	}
	ok = QC_ReturnString (vm, text, len);
	free (text);
	return ok;
}

// string strpad(float pad, string...): spaces to pad bytes, on the right for a
// positive pad, the left for a negative one (4095 bytes at most in all)
static bool QC_Strpad (qcvm_t *vm)
{
	int64_t		pad = QC_LibArgInt (vm, 0), spaces;
	size_t		len, room, n;
	char		*src = QC_LibConcat (vm, 1, &len);
	qc_sink_t	s;

	if (!src)
		return false;
	QC_SinkInit (&s, SIZE_MAX);
	if (pad < 0)
	{
		spaces = -pad - (int64_t)len;
		spaces = spaces < 0 ? 0 : spaces > QC_TEMPBUF ? QC_TEMPBUF : spaces;
		QC_SinkFill (&s, ' ', (size_t)spaces);
		room = QC_TEMPBUF - (size_t)spaces;
		QC_SinkAppend (&s, src, len < room ? len : room);
	}
	else
	{
		n = len < QC_TEMPBUF ? len : QC_TEMPBUF;
		QC_SinkAppend (&s, src, n);
		spaces = (pad < QC_TEMPBUF ? pad : QC_TEMPBUF) - (int64_t)len;
		if (spaces > 0)
			QC_SinkFill (&s, ' ', (size_t)spaces);
	}
	free (src);
	return QC_LibReturnSink (vm, &s);
}

static bool QC_IsTrimSpace (char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// string strtrim(string): without spaces, tabs, newlines and carriage returns at
// either end
static bool QC_Strtrim (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	size_t		len = strlen (s), start = 0;

	while (start < len && QC_IsTrimSpace (s[start]))
		start++;
	while (len > start && QC_IsTrimSpace (s[len - 1]))
		len--;
	return QC_ReturnString (vm, s + start, len - start);
}

// FTE's strreplace: left to right, not overlapping, stopping once the output
// reaches 4094 bytes less replace's length
static bool QC_ReplaceAll (qcvm_t *vm, bool ignorecase)
{
	size_t		slen, rlen, len, i = 0, limit;
	char		*search = QC_ArgCopy (vm, 0, &slen), *replace, *subject;
	qc_sink_t	s;
	bool		hit;

	if (!search)
		return false;
	if (!(replace = QC_ArgCopy (vm, 1, &rlen)))
	{
		free (search);
		return false;
	}
	subject = (char *)QC_ArgString (vm, 2);
	len = strlen (subject);
	QC_SinkInit (&s, SIZE_MAX);
	if (!slen)
		QC_SinkAppend (&s, subject, len);
	else if (rlen <= 4094)
	{
		limit = 4094 - rlen;
		while (i < len && s.len < limit)
		{
			hit = len - i >= slen && (ignorecase ? QC_LibEqualFold (subject + i, search, slen)
				: !memcmp (subject + i, search, slen));
			if (hit)
			{
				QC_SinkAppend (&s, replace, rlen);
				i += slen;
			}
			else
				QC_SinkPush (&s, subject[i++]);
		}
	}
	free (search);
	free (replace);
	return QC_LibReturnSink (vm, &s);
}

// string strreplace(string search, string replace, string subject)
static bool QC_Strreplace (qcvm_t *vm)
{
	return QC_ReplaceAll (vm, false);
}

// string strireplace(string search, string replace, string subject): the search
// ignoring ASCII case
static bool QC_Strireplace (qcvm_t *vm)
{
	return QC_ReplaceAll (vm, true);
}

/*
==============================================================================

COMPARISONS

==============================================================================
*/

static int QC_Lower (int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// C's strncmp (n SIZE_MAX for strcmp): the difference of the first differing
// bytes, unsigned, as glibc on x86-64 gives it
static int32_t QC_CStrncmp (const char *a, size_t alen, const char *b, size_t blen, size_t n, bool fold)
{
	size_t	i;
	int		x, y;

	for (i = 0 ; i < n ; i++)
	{
		x = i < alen ? (uint8_t)a[i] : 0;
		y = i < blen ? (uint8_t)b[i] : 0;
		if (fold)
		{
			x = QC_Lower (x);
			y = QC_Lower (y);
		}
		if (x != y || !x)
			return x - y;
	}
	return 0;
}

// the comparisons' optional len, s1ofs and s2ofs: byte offsets and a byte limit
static int32_t QC_Compare (qcvm_t *vm, bool fold)
{
	size_t			alen, blen, aofs = 0, bofs = 0, n = SIZE_MAX, la, lb;
	char			*a = QC_ArgCopy (vm, 0, &alen);
	const char		*b;
	int32_t			len, ao, bo, r;
	qc_charscheme_t	scheme = QC_Scheme (vm);

	if (!a)
		return 0;
	b = QC_ArgString (vm, 1);
	blen = strlen (b);
	if (QC_Argc (vm) > 2)
	{
		len = QC_LibArgInt (vm, 2);
		ao = QC_Argc (vm) > 3 ? QC_LibArgInt (vm, 3) : 0;
		bo = QC_Argc (vm) > 4 ? QC_LibArgInt (vm, 4) : 0;
		if (QC_Utf8On (vm))
		{
			aofs = ao ? QC_ByteOfs (a, alen, ao, scheme) : 0;
			bofs = bo ? QC_ByteOfs (b, blen, bo, scheme) : 0;
			la = QC_ByteOfs (a + aofs, alen - aofs, len, scheme);
			lb = QC_ByteOfs (b + bofs, blen - bofs, len, scheme);
			n = la > lb ? la : lb;
		}
		else
		{
			aofs = ao >= 0 && (size_t)ao <= alen ? (size_t)ao : alen;
			bofs = bo >= 0 && (size_t)bo <= blen ? (size_t)bo : blen;
			n = len >= 0 ? (size_t)len : SIZE_MAX;
		}
	}
	r = QC_CStrncmp (a + aofs, alen - aofs, b + bofs, blen - bofs, n, fold);
	free (a);
	return r;
}

// float strncmp(string s1, string s2, optional float len, optional float s1ofs,
// optional float s2ofs) (and strcmp): C's strcmp or strncmp of s1 + s1ofs and
// s2 + s2ofs, the byte difference. FTE ignores s2ofs; this applies it.
static bool QC_Strncmp (qcvm_t *vm)
{
	int32_t	r = QC_Compare (vm, false);

	if (vm->error.kind != QC_ERR_NONE)
		return false;
	QC_ReturnFloat (vm, (float)r);
	return true;
}

// float strcasecmp(string s1, string s2, ...) (and strncasecmp): C's comparison
// ignoring case (ASCII letters to lower case, bytes unsigned): -1, 0 or 1. FTE
// folds to upper case and compares signed chars.
static bool QC_Strcasecmp (qcvm_t *vm)
{
	int32_t	r = QC_Compare (vm, true);

	if (vm->error.kind != QC_ERR_NONE)
		return false;
	QC_ReturnFloat (vm, r < 0 ? -1.0f : r > 0 ? 1.0f : 0.0f);
	return true;
}

/*
==============================================================================

CASE

==============================================================================
*/

// FTE's unicode_strtoupper and unicode_strtolower: decoded with the scheme,
// ASCII letters mapped (in U+E020-U+E07F too), written back without markup
static bool QC_ChangeCase (qcvm_t *vm, bool upper)
{
	const uint8_t	*s = (const uint8_t *)QC_ArgString (vm, 0);
	size_t			len = strlen ((const char *)s), at, used;
	qc_charscheme_t	scheme = QC_Scheme (vm);
	uint32_t		c, low;
	qc_sink_t		out, ch;

	QC_SinkInit (&out, SIZE_MAX);
	for (at = 0 ; at < len ; at += used)
	{
		c = QC_DecodeChar (s + at, len - at, scheme, &used);
		low = c >= 0xE020 && c <= 0xE07F ? c & 0x7F : c;
		if (low < 0x80)
		{
			if (upper && low >= 'a' && low <= 'z')
				low -= 32;
			else if (!upper && low >= 'A' && low <= 'Z')
				low += 32;
		}
		c = c >= 0xE020 && c <= 0xE07F ? low | (c & 0xFF80) : low;
		QC_SinkInit (&ch, SIZE_MAX);
		QC_EncodeChar (&ch, c, scheme, false);
		if (out.len + ch.len > QC_CASE_MAX)
		{
			QC_SinkFree (&ch);
			break;
		}
		QC_SinkAppend (&out, QC_SinkText (&ch), ch.len);
		QC_SinkFree (&ch);
	}
	return QC_LibReturnSink (vm, &out);
}

// string strtolower(string): ASCII letters to lower case
static bool QC_Strtolower (qcvm_t *vm)
{
	return QC_ChangeCase (vm, false);
}

// string strtoupper(string): ASCII letters to upper case
static bool QC_Strtoupper (qcvm_t *vm)
{
	return QC_ChangeCase (vm, true);
}

/*
==============================================================================

MARKUP

==============================================================================
*/

// how the markup parser reads bytes that aren't markup
typedef enum
{
	QC_BYTES_QUAKE,			// Quake's glyphs: the high half red text, controls glyphs
	QC_BYTES_RAW,			// the byte is the code point (ISO-8859-1, ASCII in UTF-8)
	QC_BYTES_UTF8,			// UTF-8, until the first malformed sequence makes the rest QUAKE
	QC_BYTES_FORCED_UTF8	// UTF-8 that doesn't switch (^`u8: sections)
} qc_bytesmode_t;

// the markup parser's output: characters, some hidden
typedef struct
{
	uint32_t	*ch;
	bool		*hidden;
	size_t		count, size;
	size_t		units;
} qc_funtext_t;

// adds a character; false once the buffer is full
static bool QC_FunPush (qc_funtext_t *t, uint32_t ch, bool hidden)
{
	size_t		cost = ch > 0xFFFF ? 2 : 1, size;
	uint32_t	*gc;
	bool		*gh;

	if (t->units + cost > QC_FUN_MAX)
		return false;
	if (t->count == t->size)
	{
		size = t->size ? t->size * 2 : 64;
		gc = realloc (t->ch, size * sizeof(*gc));
		if (gc)
			t->ch = gc;
		gh = realloc (t->hidden, size * sizeof(*gh));
		if (gh)
			t->hidden = gh;
		if (!gc || !gh)
			return false;
		t->size = size;
	}
	t->units += cost;
	t->ch[t->count] = ch;
	t->hidden[t->count++] = hidden;
	return true;
}

static bool QC_IsHexAt (const uint8_t *s, size_t len, size_t i)
{
	return i < len && ((s[i] >= '0' && s[i] <= '9') || ((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'f'));
}

static uint32_t QC_HexVal (uint8_t c)
{
	return c <= '9' ? (uint32_t)(c - '0') : (uint32_t)((c | 0x20) - 'a' + 10);
}

// ^& colour digits: 0-9, A-F (upper case only) or -
static bool QC_IsExtendedCode (const uint8_t *s, size_t len, size_t i)
{
	return i < len && ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F') || s[i] == '-');
}

// KOI8 (KOI8-R, with KOI8-U's and KOI8-RU's) to Unicode, for ezQuake's =`k8:...`= markup
static uint32_t QC_Koi8ToUnicode (uint8_t b)
{
	// ю а б ц д е ф г х и й к л м н о п я р с т у ж в ь ы з ш э щ ч ъ
	static const uint16_t	high[32] = {0x44E, 0x430, 0x431, 0x446, 0x434, 0x435, 0x444, 0x433, 0x445, 0x438,
		0x439, 0x43A, 0x43B, 0x43C, 0x43D, 0x43E, 0x43F, 0x44F, 0x440, 0x441, 0x442, 0x443, 0x436, 0x432, 0x44C,
		0x44B, 0x437, 0x448, 0x44D, 0x449, 0x447, 0x44A};

	if (b >= 0xE0)
		return high[b - 0xE0] - 0x20u;		// the capitals
	if (b >= 0xC0)
		return high[b - 0xC0];
	switch (b)
	{
	case 0xA3:	return 0x451;
	case 0xB3:	return 0x401;
	case 0xA4:	return 0x454;
	case 0xB4:	return 0x404;
	case 0xA6:	return 0x456;
	case 0xB6:	return 0x406;
	case 0xA7:	return 0x457;
	case 0xB7:	return 0x407;
	case 0xAE:	return 0x45E;
	case 0xBE:	return 0x40E;
	case 0xAF:	return 0x42A;
	default:	return b;
	}
}

// a byte that isn't markup, as the parser reads it
static uint32_t QC_Literal (uint8_t c, qc_bytesmode_t mode)
{
	if (mode != QC_BYTES_QUAKE)
		return c;
	if (c == '\n' || c == '\r' || c == '\t' || c == 0x0B || (c >= 0x20 && c <= 0x7E))
		return c;
	if (c >= 0xA0)
		return c & 0x7F;
	return 0xE000u | c;
}

// the end of a ^`u8: or =`k8: section: its "`=" terminator, or the end
static size_t QC_SectionEnd (const uint8_t *s, size_t len, size_t from)
{
	size_t	i;

	for (i = from ; i + 1 < len ; i++)
		if (s[i] == '`' && s[i + 1] == '=')
			return i;
	return len;
}

// how deep sections nest before more markers are just skipped (hostile input
// can't make the recursion unbounded)
#define QC_SECTION_DEPTH	16

// FTE's COM_ParseFunString as strdecolorize uses it: colour and link markup
// parsed into characters, some hidden; false once the buffer is full
static bool QC_ParseMarkup (const uint8_t *s, size_t len, qc_bytesmode_t mode, qc_funtext_t *out, uint32_t depth)
{
	size_t			i = len && (s[0] == 1 || s[0] == 2), used, skip, from, end, k, last;
	int64_t			link = -1;
	uint32_t		ch;
	uint8_t			c;
	qc_utf8err_t	err;
	qc_sink_t		utf8;
	bool			ok;

	while (i < len)
	{
		c = s[i];
		if ((c & 0x80) && (mode == QC_BYTES_UTF8 || mode == QC_BYTES_FORCED_UTF8))
		{
			ch = QC_DecodeUtf8 (s + i, len - i, &used, &err);
			if (err != QC_UTF8_OK && mode == QC_BYTES_UTF8)
				mode = QC_BYTES_QUAKE;
			else
			{
				if (!QC_FunPush (out, ch > 0x10FFFF ? QC_REPLACEMENT : ch, false))
					return false;
				i += used;
				continue;
			}
		}
		if (c == '^')
		{
			skip = 0;
			switch (i + 1 < len ? s[i + 1] : 0)
			{
			case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
			case 'b': case 'd': case 'm': case 'a': case 'h': case 's': case 'r':
				skip = 2;
				break;
			case '&':
				if (QC_IsExtendedCode (s, len, i + 2) && QC_IsExtendedCode (s, len, i + 3))
					skip = 4;
				break;
			case 'x':
				skip = QC_IsHexAt (s, len, i + 2) && QC_IsHexAt (s, len, i + 3) && QC_IsHexAt (s, len, i + 4) ? 5 : 2;
				break;
			case '[':
				if (link < 0)
				{
					link = (int64_t)out->count;
					if (!QC_FunPush (out, '[', false))
						return false;
					skip = 2;
				}
				break;
			case ']':
				if (!QC_FunPush (out, ']', link >= 0))
					return false;
				if (link >= 0)
				{
					// the [ and everything from the link's first \ on are hidden
					last = out->count - 1;
					for (k = (size_t)link + 1 ; k < last && out->ch[k] != '\\' ; k++)
						;
					out->hidden[link] = true;
					for ( ; k < out->count ; k++)
						out->hidden[k] = true;
					link = -1;
				}
				skip = 2;
				break;
			case '`':
				if (len - i >= 5 && !memcmp (s + i + 2, "u8:", 3))
				{
					from = i + 5;
					if (depth >= QC_SECTION_DEPTH)
					{
						i = from;
						continue;
					}
					end = QC_SectionEnd (s, len, from);
					if (!QC_ParseMarkup (s + from, end - from, QC_BYTES_FORCED_UTF8, out, depth + 1))
						return false;
					i = end + 2 < len ? end + 2 : len;
					continue;
				}
				break;
			case 'U':
				if (QC_IsHexAt (s, len, i + 2) && QC_IsHexAt (s, len, i + 3) && QC_IsHexAt (s, len, i + 4)
					&& QC_IsHexAt (s, len, i + 5))
				{
					for (ch = 0, k = 2 ; k < 6 ; k++)
						ch = (ch << 4) | QC_HexVal (s[i + k]);
					if (!QC_FunPush (out, ch, false))
						return false;
					skip = 6;
				}
				break;
			case '{':
				for (ch = 0, k = 2 ; QC_IsHexAt (s, len, i + k) ; k++)
					ch = (ch << 4) | QC_HexVal (s[i + k]);
				if (i + k < len && s[i + k] == '}')
					k++;
				if (!QC_FunPush (out, ch > 0x10FFFF ? QC_REPLACEMENT : ch, false))
					return false;
				skip = k;
				break;
			case '^':
				// ^^ is a caret: the first dropped, the second read as a plain byte
				i++;
				break;
			default:
				break;
			}
			if (skip)
			{
				i += skip;
				continue;
			}
		}
		else if (c == '&' && i + 1 < len && s[i + 1] == 'c' && QC_IsHexAt (s, len, i + 2) && QC_IsHexAt (s, len, i + 3)
			&& QC_IsHexAt (s, len, i + 4))
		{
			i += 5;
			continue;
		}
		else if (c == '&' && i + 1 < len && s[i + 1] == 'r')
		{
			i += 2;
			continue;
		}
		else if (c == '=' && len - i >= 5 && !memcmp (s + i + 1, "`k8:", 4))
		{
			from = i + 5;
			if (depth >= QC_SECTION_DEPTH)
			{
				i = from;
				continue;
			}
			end = QC_SectionEnd (s, len, from);
			QC_SinkInit (&utf8, SIZE_MAX);
			for (k = from ; k < end ; k++)
				QC_EncodeUtf8 (&utf8, QC_Koi8ToUnicode (s[k]));
			ok = QC_ParseMarkup ((const uint8_t *)QC_SinkText (&utf8), utf8.len, QC_BYTES_FORCED_UTF8, out, depth + 1);
			QC_SinkFree (&utf8);
			if (!ok)
				return false;
			i = end + 2 < len ? end + 2 : len;
			continue;
		}
		if (!QC_FunPush (out, QC_Literal (s[i], mode == QC_BYTES_QUAKE ? QC_BYTES_QUAKE : QC_BYTES_RAW), false))
			return false;
		i++;
	}
	return true;
}

// FTE's strdecolorize: colour codes and other markup removed (link targets
// dropped, ^^ made ^, ^Uxxxx and ^{x} made characters), the visible characters
// written back in the scheme (Quake's red text comes back white)
static void QC_Decolorize (const char *s, qc_charscheme_t scheme, qc_sink_t *out)
{
	qc_bytesmode_t	mode = scheme == QC_CHARS_QUAKE ? QC_BYTES_QUAKE : scheme == QC_CHARS_UTF8 ? QC_BYTES_UTF8 : QC_BYTES_RAW;
	qc_funtext_t	text = {0};
	qc_sink_t		ch;
	size_t			i;

	QC_ParseMarkup ((const uint8_t *)s, strlen (s), mode, &text, 0);
	for (i = 0 ; i < text.count ; i++)
	{
		if (text.hidden[i])
			continue;
		QC_SinkInit (&ch, SIZE_MAX);
		QC_EncodeChar (&ch, text.ch[i], scheme, false);
		if (out->len + ch.len > QC_DECOLOR_MAX)
		{
			QC_SinkFree (&ch);
			break;
		}
		QC_SinkAppend (out, QC_SinkText (&ch), ch.len);
		QC_SinkFree (&ch);
	}
	free (text.ch);
	free (text.hidden);
}

// string strdecolorize(string): the text without colour codes and markup
static bool QC_Strdecolorize (qcvm_t *vm)
{
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_Decolorize (QC_ArgString (vm, 0), QC_Scheme (vm), &s);
	return QC_LibReturnSink (vm, &s);
}

// float strlennocol(string): the bytes of strdecolorize's text
static bool QC_Strlennocol (qcvm_t *vm)
{
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_Decolorize (QC_ArgString (vm, 0), QC_Scheme (vm), &s);
	QC_ReturnFloat (vm, (float)s.len);
	QC_SinkFree (&s);
	return true;
}

/*
==============================================================================

INFO STRINGS

==============================================================================
*/

// the value of key in a \key\value info string (and its length), or ""
static const char *QC_InfoGet (const char *info, size_t len, const char *key, size_t keylen, size_t *vlen)
{
	const char	*rest = info, *end = info + len, *bs, *value;
	size_t		klen, vl;

	*vlen = 0;
	if (rest < end && *rest == '\\')
		rest++;
	for ( ; ; )
	{
		bs = memchr (rest, '\\', (size_t)(end - rest));
		if (!bs)
			return "";
		klen = (size_t)(bs - rest);
		if (klen >= QC_INFO_FIELD_MAX)
			return "";
		value = bs + 1;
		bs = memchr (value, '\\', (size_t)(end - value));
		vl = bs ? (size_t)(bs - value) : (size_t)(end - value);
		if (vl >= QC_INFO_FIELD_MAX)
			return "";
		if (klen == keylen && !memcmp (rest, key, klen))
		{
			*vlen = vl;
			return value;
		}
		if (!bs)
			return "";
		rest = bs + 1;
	}
}

// removes the first \key\value pair of key
static void QC_InfoRemove (char *info, size_t *len, const char *key, size_t keylen)
{
	size_t	start = 0, p, keyend, end;
	char	*bs;

	for ( ; ; )
	{
		p = start;
		if (p < *len && info[p] == '\\')
			p++;
		bs = memchr (info + p, '\\', *len - p);
		if (!bs)
			return;
		keyend = (size_t)(bs - info);
		bs = memchr (info + keyend + 1, '\\', *len - keyend - 1);
		end = bs ? (size_t)(bs - info) : *len;
		if (keyend - p == keylen && !memcmp (info + p, key, keylen))
		{
			memmove (info + start, info + end, *len - end);
			*len -= end - start;
			return;
		}
		if (end >= *len)
			return;
		start = end;
	}
}

// FTE's Info_SetValueForStarKey on an info string: false (the string as it was,
// with *reason) if the change is refused
static bool QC_InfoSet (const char *info, const char *key, const char *value, qc_sink_t *out, const char **reason)
{
	size_t		ilen = strlen (info), klen = strlen (key), vlen = strlen (value), len, oldlen, needed, i, pairlen;
	char		*s, pair[QC_INFO_PAIR_MAX + 1];
	const char	*old;

	*reason = NULL;
	if (ilen > QC_INFO_MAX - 1)
		ilen = QC_INFO_MAX - 1;
	s = malloc (ilen + 1);
	if (!s)
	{
		out->failed = true;
		return false;
	}
	memcpy (s, info, ilen);
	len = ilen;
	if (strchr (key, '\\') || strchr (value, '\\'))
		*reason = "infoadd: keys and values can't contain a \\";
	else if (strchr (key, '"') || strchr (value, '"'))
		*reason = "infoadd: keys and values can't contain a \"";
	else if (klen >= QC_INFO_KEY_MAX)
		*reason = "infoadd: keys must be shorter than 256 characters";
	for ( ; !*reason ; )
	{
		old = QC_InfoGet (s, len, key, klen, &oldlen);
		needed = vlen + len + 1 > oldlen ? vlen + len + 1 - oldlen : 0;
		if (oldlen && needed > QC_INFO_MAX)
		{
			QC_InfoGet (s, len, "*ver", 4, &oldlen);
			if (oldlen)
			{
				QC_InfoRemove (s, &len, "*ver", 4);
				continue;
			}
			*reason = "infoadd: info string length exceeded";
		}
		(void)old;
		break;
	}
	if (*reason)
	{
		QC_SinkAppend (out, s, len);
		free (s);
		return false;
	}
	QC_InfoRemove (s, &len, key, klen);
	if (vlen)
	{
		pairlen = (size_t)snprintf (pair, sizeof(pair), "\\%s\\%s", key, value);
		if (pairlen > QC_INFO_PAIR_MAX)
			pairlen = QC_INFO_PAIR_MAX;
		if (pairlen + len + 1 > QC_INFO_MAX)
		{
			// FTE has removed the old pair by now; this leaves the string as it was
			*reason = "infoadd: info string length exceeded";
			QC_SinkAppend (out, info, ilen);
			free (s);
			return false;
		}
		QC_SinkAppend (out, s, len);
		for (i = 0 ; i < pairlen ; i++)
			if ((uint8_t)pair[i] > 13)
				QC_SinkPush (out, pair[i]);
	}
	else
		QC_SinkAppend (out, s, len);
	free (s);
	return true;
}

// string infoadd(string info, string key, string value...): sets (or with an
// empty value removes) a key; a refused change (a \ or " in key or value, a key
// of 256 bytes or more, a string past 4096 bytes) gives the string back as it
// was, with a warning
static bool QC_Infoadd (qcvm_t *vm)
{
	char		*value = QC_LibConcat (vm, 2, NULL), *info, *key;
	const char	*reason;
	qc_sink_t	s;

	if (!value)
		return false;
	info = QC_ArgCopy (vm, 0, NULL);
	key = info ? QC_ArgCopy (vm, 1, NULL) : NULL;
	if (!key)
	{
		free (value);
		free (info);
		return false;
	}
	QC_SinkInit (&s, SIZE_MAX);
	QC_InfoSet (info, key, value, &s, &reason);
	if (reason)
		QC_Warning (vm, "%s", reason);
	free (value);
	free (info);
	free (key);
	return QC_LibReturnSink (vm, &s);
}

// string infoget(string info, string key): key's value, or ""
static bool QC_Infoget (qcvm_t *vm)
{
	size_t		ilen, vlen;
	char		*info = QC_ArgCopy (vm, 0, &ilen);
	const char	*key, *value;
	bool		ok;

	if (!info)
		return false;
	key = QC_ArgString (vm, 1);
	value = QC_InfoGet (info, ilen, key, strlen (key), &vlen);
	ok = QC_ReturnString (vm, value, vlen);
	free (info);
	return ok;
}

/*
==============================================================================

URIS AND QUOTING

==============================================================================
*/

// string uri_escape(string): every byte but A-Z a-z 0-9 . - _ ~ as %XX
static bool QC_UriEscape (qcvm_t *vm)
{
	static const char	hex[] = "0123456789ABCDEF";
	const uint8_t		*s = (const uint8_t *)QC_ArgString (vm, 0);
	qc_sink_t			out;
	uint8_t				c;

	QC_SinkInit (&out, SIZE_MAX);
	for ( ; *s && out.len < 8188 ; s++)
	{
		c = *s;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'
			|| c == '_' || c == '~')
			QC_SinkPush (&out, (char)c);
		else
		{
			QC_SinkPush (&out, '%');
			QC_SinkPush (&out, hex[c >> 4]);
			QC_SinkPush (&out, hex[c & 15]);
		}
	}
	return QC_LibReturnSink (vm, &out);
}

// string uri_unescape(string): %XX decoded (a NUL decoded ends the string)
static bool QC_UriUnescape (qcvm_t *vm)
{
	const uint8_t	*s = (const uint8_t *)QC_ArgString (vm, 0);
	size_t			len = strlen ((const char *)s), i = 0;
	qc_sink_t		out;
	char			*nul;

	QC_SinkInit (&out, SIZE_MAX);
	while (i < len && out.len < 8190)
	{
		if (s[i] == '%' && QC_IsHexAt (s, len, i + 1) && QC_IsHexAt (s, len, i + 2))
		{
			QC_SinkPush (&out, (char)((QC_HexVal (s[i + 1]) << 4) | QC_HexVal (s[i + 2])));
			i += 3;
		}
		else
			QC_SinkPush (&out, (char)s[i++]);
	}
	if (out.buf && (nul = memchr (out.buf, 0, out.len)))
		out.len = (size_t)(nul - out.buf);
	return QC_LibReturnSink (vm, &out);
}

// string argescape(string): quoted for the console's tokenizer to read back as
// one argument (sprintf's %S)
static bool QC_Argescape (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	qc_sink_t	out;

	QC_SinkInit (&out, SIZE_MAX);
	QC_QuoteString (&out, s, strlen (s), 8192);
	return QC_LibReturnSink (vm, &out);
}

/*
==============================================================================

THE REST

==============================================================================
*/

// string instr(string s, string token...): the rest of s from the first token
// (the tokens joined), or null without one. For a string in the VM's memory
// the result points into s; otherwise it's a new temp string.
static bool QC_Instr (qcvm_t *vm)
{
	uint32_t	r = QC_ArgWord (vm, 0), q;
	size_t		nlen;
	char		*needle = QC_LibConcat (vm, 1, &nlen);
	const char	*s;
	int64_t		k;

	if (!needle)
		return false;
	s = QC_ArgString (vm, 0);
	k = QC_Find (s, strlen (s), needle, nlen);
	free (needle);
	if (k < 0)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	q = r + (uint32_t)k;
	if (!(r & 0x80000000u) && q >= r && !(q & 0x80000000u))
	{
		QC_ReturnWord (vm, q);
		return true;
	}
	return QC_ReturnString (vm, s + k, strlen (s + k));
}

// float validstring(string): whether the reference isn't null (menu QuakeC)
static bool QC_Validstring (qcvm_t *vm)
{
	QC_LibReturnBool (vm, QC_ArgWord (vm, 0) != 0);
	return true;
}

/*
------------------------------------------------------------------------------
DarkPlaces' alt strings (menu QuakeC): single-quoted strings, 'one' 'two', a \
escaping the next byte inside the quotes and out
------------------------------------------------------------------------------
*/

// the index just after the n-th unescaped quote (from 1), or -1
static int64_t QC_AfterQuote (const char *s, size_t len, int64_t n)
{
	int64_t	seen = 0;
	size_t	i = 0;

	if (n <= 0)
		return -1;
	while (i < len)
	{
		if (s[i] == '\\')
		{
			i += 2;
			continue;
		}
		if (s[i++] == '\'' && ++seen == n)
			return (int64_t)i;
	}
	return -1;
}

// the next unescaped quote at or after from, or the end
static size_t QC_ClosingQuote (const char *s, size_t len, size_t from)
{
	size_t	i = from;

	while (i < len)
	{
		if (s[i] == '\\')
			i += 2;
		else if (s[i] == '\'')
			return i;
		else
			i++;
	}
	return len;
}

// float altstr_count(string): the quoted strings (unescaped quotes / 2)
static bool QC_AltstrCount (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	size_t		len = strlen (s), i = 0, quotes = 0;

	while (i < len)
	{
		if (s[i] == '\\')
		{
			i += 2;
			continue;
		}
		quotes += s[i++] == '\'';
	}
	QC_ReturnFloat (vm, (float)(quotes / 2));
	return true;
}

// string altstr_prepare(string): single quotes escaped (' as \')
static bool QC_AltstrPrepare (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	qc_sink_t	out;

	QC_SinkInit (&out, SIZE_MAX);
	for ( ; *s ; s++)
	{
		if (*s == '\'')
			QC_SinkPush (&out, '\\');
		QC_SinkPush (&out, *s);
	}
	return QC_LibReturnSink (vm, &out);
}

// string altstr_get(string str, float num): quoted string num's contents (from
// 0), unescaped, or ""
static bool QC_AltstrGet (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	size_t		len = strlen (s), i;
	int64_t		start = QC_AfterQuote (s, len, (int64_t)QC_LibArgInt (vm, 1) * 2 + 1);
	qc_sink_t	out;

	QC_SinkInit (&out, SIZE_MAX);
	for (i = start < 0 ? len : (size_t)start ; i < len && s[i] != '\'' ; )
	{
		if (s[i] == '\\')
		{
			if (i + 1 >= len)
				break;
			QC_SinkPush (&out, s[i + 1]);
			i += 2;
		}
		else
			QC_SinkPush (&out, s[i++]);
	}
	return QC_LibReturnSink (vm, &out);
}

// string altstr_set(string str, float num, string value): quoted string num's
// contents replaced by value (escaped already: altstr_prepare); as DarkPlaces
// and FTE have it, a missing string num gets value appended
static bool QC_AltstrSet (qcvm_t *vm)
{
	size_t		len, start, end;
	char		*s = QC_ArgCopy (vm, 0, &len);
	const char	*value;
	int64_t		after;
	qc_sink_t	out;

	if (!s)
		return false;
	after = QC_AfterQuote (s, len, (int64_t)QC_LibArgInt (vm, 1) * 2 + 1);
	value = QC_ArgString (vm, 2);
	start = after < 0 ? len : (size_t)after;
	end = after < 0 ? len : QC_ClosingQuote (s, len, start);
	QC_SinkInit (&out, SIZE_MAX);
	QC_SinkAppend (&out, s, start);
	QC_SinkAppend (&out, value, strlen (value));
	QC_SinkAppend (&out, s + end, len - end);
	free (s);
	return QC_LibReturnSink (vm, &out);
}

static const qc_libentry_t	qc_string[] = {
	{"strlen", QC_Strlen, NULL, 0},
	{"memstrsize", QC_Memstrsize, NULL, 0},
	{"strcat", QC_Strcat, NULL, 0},
	{"strzone", QC_Strcat, NULL, 0},
	{"strunzone", QC_Strunzone, NULL, 0},
	{"substring", QC_Substring, NULL, 0},
	{"strstrofs", QC_Strstrofs, NULL, 0},
	{"str2chr", QC_Str2chr, NULL, 0},
	{"chr2str", QC_Chr2str, NULL, 0},
	{"strconv", QC_Strconv, NULL, 0},
	{"strpad", QC_Strpad, NULL, 0},
	{"strtrim", QC_Strtrim, NULL, 0},
	{"strreplace", QC_Strreplace, NULL, 0},
	{"strireplace", QC_Strireplace, NULL, 0},
	{"strncmp", QC_Strncmp, NULL, 0},
	{"strcmp", QC_Strncmp, NULL, 0},
	{"strcasecmp", QC_Strcasecmp, NULL, 0},
	{"strncasecmp", QC_Strcasecmp, NULL, 0},
	{"strtolower", QC_Strtolower, NULL, 0},
	{"strtoupper", QC_Strtoupper, NULL, 0},
	{"strdecolorize", QC_Strdecolorize, NULL, 0},
	{"strlennocol", QC_Strlennocol, NULL, 0},
	{"infoadd", QC_Infoadd, NULL, 0},
	{"infoget", QC_Infoget, NULL, 0},
	{"uri_escape", QC_UriEscape, NULL, 0},
	{"uri_unescape", QC_UriUnescape, NULL, 0},
	{"argescape", QC_Argescape, NULL, 0},
	{"instr", QC_Instr, NULL, 0},
	{"validstring", QC_Validstring, NULL, 0},
	{"altstr_count", QC_AltstrCount, NULL, 0},
	{"altstr_prepare", QC_AltstrPrepare, NULL, 0},
	{"altstr_get", QC_AltstrGet, NULL, 0},
	{"altstr_set", QC_AltstrSet, NULL, 0},
};

bool QC_RegisterString (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_string, sizeof(qc_string) / sizeof(qc_string[0]));
}
