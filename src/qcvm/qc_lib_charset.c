// qc_lib_charset.c -- characters in the Quake, UTF-8 and ISO-8859-1 schemes
// (docs/spec/strings.md)
//
// The conversions FTE makes between bytes and code points. The Quake scheme
// puts its special glyphs (the control bytes and the red high half) in the
// private use area U+E000-U+E0FF; the UTF-8 decoder is lenient as FTE's is
// (overlong forms, modified UTF-8's NUL, CESU-8 surrogate pairs and the 5 and
// 6-byte forms decode; stray bytes go to the private use area). No builtins
// here: the string builtins use these.

#include "qc_lib.h"

#include <stdio.h>

static bool QC_IsCont (const uint8_t *s, size_t len, size_t i)
{
	return i < len && (s[i] & 0xC0) == 0x80;
}

// a sequence without the surrogates' handling
static uint32_t QC_DecodeUtf8Basic (const uint8_t *s, size_t len, size_t *used, qc_utf8err_t *err)
{
	uint8_t		b0 = len ? s[0] : 0, lead_mask;
	uint32_t	n, min, ch, i;

	*used = 1;
	*err = QC_UTF8_OK;
	if (b0 < 0x80)
		return b0;
	if (b0 < 0xC0 || b0 >= 0xFE)
	{
		*err = QC_UTF8_MALFORMED;
		return 0xE000 | b0;
	}
	if (b0 < 0xE0)
	{
		n = 2;
		lead_mask = 0x1F;
		min = 0x80;
	}
	else if (b0 < 0xF0)
	{
		n = 3;
		lead_mask = 0x0F;
		min = 0x800;
	}
	else if (b0 < 0xF8)
	{
		n = 4;
		lead_mask = 0x07;
		min = 0x10000;
	}
	else if (b0 < 0xFC)
	{
		n = 5;
		lead_mask = 0x03;
		min = 0x200000;
	}
	else
	{
		n = 6;
		lead_mask = 0x01;
		min = 0x4000000;
	}
	for (i = 1 ; i < n ; i++)
		if (!QC_IsCont (s, len, i))
		{
			*err = QC_UTF8_MALFORMED;
			return QC_REPLACEMENT;
		}
	for (ch = b0 & lead_mask, i = 1 ; i < n ; i++)
		ch = (ch << 6) | (s[i] & 0x3F);
	*used = n;
	// modified UTF-8 writes NUL as the overlong C0 80, which is taken
	if (ch < min && !(n == 2 && ch == 0))
		*err = QC_UTF8_ILLEGAL;
	return ch;
}

uint32_t QC_DecodeUtf8 (const uint8_t *s, size_t len, size_t *used, qc_utf8err_t *err)
{
	uint32_t		ch = QC_DecodeUtf8Basic (s, len, used, err), low;
	size_t			lowused;
	qc_utf8err_t	lowerr;

	if (*err != QC_UTF8_OK)
		return ch;
	if (ch >= 0xD800 && ch < 0xDC00)
	{
		// CESU-8: a high surrogate and a low one are one character
		low = QC_DecodeUtf8Basic (s + *used, len - *used, &lowused, &lowerr);
		if (*used < len && lowerr == QC_UTF8_OK && low >= 0xDC00 && low < 0xE000)
		{
			ch = (((ch & 0x3FF) << 10) | (low & 0x3FF)) + 0x10000;
			*used += lowused;
		}
		else
			*err = QC_UTF8_LONE_HIGH;
	}
	if (ch >= 0xDC00 && ch < 0xE000)
		*err = QC_UTF8_LOW;
	if (ch == 0xFFFE || ch == 0xFFFF || ch > 0x10FFFF)
		*err = QC_UTF8_ILLEGAL;
	return ch;
}

uint32_t QC_DecodeChar (const uint8_t *s, size_t len, qc_charscheme_t scheme, size_t *used)
{
	uint32_t		b = len ? s[0] : 0;
	qc_utf8err_t	err;

	*used = 1;
	switch (scheme)
	{
	case QC_CHARS_UTF8:
		return QC_DecodeUtf8 (s, len, used, &err);
	case QC_CHARS_ISO8859_1:
		return b;
	default:
		if (b != 0 && b != 0x0A && b != 0x09 && b != 0x0D && !(b >= 0x20 && b < 0x80))
			return 0xE000 | b;
		return b;
	}
}

size_t QC_CharCount (const uint8_t *s, size_t len, qc_charscheme_t scheme)
{
	size_t	n = 0, at = 0, used;

	if (scheme != QC_CHARS_UTF8)
		return len;
	for ( ; at < len ; at += used, n++)
		QC_DecodeChar (s + at, len - at, scheme, &used);
	return n;
}

size_t QC_ByteOffset (const uint8_t *s, size_t len, size_t index, qc_charscheme_t scheme)
{
	size_t	at = 0, used;

	if (scheme != QC_CHARS_UTF8)
		return index < len ? index : len;
	for ( ; at < len && index ; at += used, index--)
		QC_DecodeChar (s + at, len - at, scheme, &used);
	return at < len ? at : len;
}

size_t QC_CharOffset (const uint8_t *s, size_t len, size_t ofs, qc_charscheme_t scheme)
{
	size_t	n = 0, at = 0, used;

	if (scheme != QC_CHARS_UTF8)
		return ofs < len ? ofs : len;
	for ( ; at < len ; at += used, n++)
	{
		QC_DecodeChar (s + at, len - at, scheme, &used);
		if (at + used > ofs)
			break;
	}
	return n;
}

void QC_EncodeUtf8 (qc_sink_t *out, uint32_t ch)
{
	uint8_t		bytes[6];
	uint32_t	n, k;

	if (ch > 0x7FFFFFFF)
		ch = QC_REPLACEMENT;
	if (!ch)
	{
		QC_SinkAppend (out, "\xC0\x80", 2);
		return;
	}
	if (ch < 0x80)
	{
		QC_SinkPush (out, (char)ch);
		return;
	}
	n = ch < 0x800 ? 2 : ch < 0x10000 ? 3 : ch < 0x200000 ? 4 : ch < 0x4000000 ? 5 : 6;
	bytes[0] = (uint8_t)(((0xFF00u >> n) & 0xFF) | (ch >> ((n - 1) * 6)));
	for (k = 1 ; k < n ; k++)
		bytes[k] = (uint8_t)(0x80 | ((ch >> ((n - 1 - k) * 6)) & 0x3F));
	QC_SinkAppend (out, (const char *)bytes, n);
}

// FTE's markup for a character the charset can't write: ^Uxxxx, or ^{x...}
// past U+FFFF (lowercase hex)
static void QC_EncodeMarkup (qc_sink_t *out, uint32_t ch)
{
	if (ch > 0xFFFF)
		QC_SinkPrintf (out, "^{%x}", ch);
	else
		QC_SinkPrintf (out, "^U%04x", ch);
}

void QC_EncodeChar (qc_sink_t *out, uint32_t ch, qc_charscheme_t scheme, bool markup)
{
	bool	direct;

	switch (scheme)
	{
	case QC_CHARS_UTF8:
		QC_EncodeUtf8 (out, ch);
		return;
	case QC_CHARS_ISO8859_1:
		direct = ch < 0x100 || (ch >= 0xE020 && ch < 0xE080);
		break;
	default:
		// U+E00B goes back to its byte (FTE can't write \v); tab, newline and
		// carriage return stay plain ASCII
		direct = (ch >= 0x20 && ch < 0x80) || ch == 0x09 || ch == 0x0A || ch == 0x0B || ch == 0x0D
			|| (ch >= 0xE000 && ch <= 0xE0FF && ch != 0xE009 && ch != 0xE00A && ch != 0xE00D);
		break;
	}
	if (direct)
		QC_SinkPush (out, (char)(ch & 0xFF));
	else if (markup)
		QC_EncodeMarkup (out, ch);
	else
		QC_SinkPush (out, '?');
}
