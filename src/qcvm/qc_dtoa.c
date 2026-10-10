// qc_dtoa.c -- numbers as text and text as numbers, as C's printf and strtod
// make them in the C locale, the same on every platform
//
// A double's value is exact in binary, so its decimal expansion is finite: at
// most 309 digits before the point and 1074 after it. The conversions take all
// of them (from a small bignum) and round half to even, as glibc's printf does;
// the C conventions go on top (exponents of two digits or more, %g, the flags,
// inf and nan). The platform's snprintf isn't used for floats: UCRT, glibc and
// Apple's differ.

#include "qc_lib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QC_MAX_INT_DIGITS			309		// DBL_MAX
#define QC_MAX_FRACTION_DIGITS		1074	// 2^-1074: %f digits past them are zeros
#define QC_MAX_SIGNIFICANT_DIGITS	767		// %e digits past them are zeros
#define QC_BODY_MAX					(QC_MAX_INT_DIGITS + QC_MAX_FRACTION_DIGITS + 8)

/*
==============================================================================

SINKS

==============================================================================
*/

void QC_SinkInit (qc_sink_t *s, size_t cap)
{
	*s = (qc_sink_t){.cap = cap};
}

void QC_SinkFree (qc_sink_t *s)
{
	free (s->buf);
	*s = (qc_sink_t){0};
}

size_t QC_SinkRoom (const qc_sink_t *s)
{
	return s->cap > s->len ? s->cap - s->len : 0;
}

// room for n more bytes and the NUL; false without the memory (the text stays short)
static bool QC_SinkReserve (qc_sink_t *s, size_t n)
{
	size_t	want = s->len + n + 1, size;
	char	*grown;

	if (want <= s->size)
		return true;
	if (s->failed || want < n)
	{
		s->failed = true;
		return false;
	}
	size = s->size ? s->size : 64;
	while (size < want)
		size = size <= SIZE_MAX / 2 ? size * 2 : want;
	grown = realloc (s->buf, size);
	if (!grown)
	{
		s->failed = true;
		return false;
	}
	s->buf = grown;
	s->size = size;
	return true;
}

void QC_SinkPush (qc_sink_t *s, char c)
{
	QC_SinkAppend (s, &c, 1);
}

void QC_SinkAppend (qc_sink_t *s, const char *text, size_t n)
{
	if (n > QC_SinkRoom (s))
		n = QC_SinkRoom (s);
	if (!n || !QC_SinkReserve (s, n))
		return;
	memcpy (s->buf + s->len, text, n);
	s->len += n;
	s->buf[s->len] = 0;
}

void QC_SinkFill (qc_sink_t *s, char c, size_t n)
{
	if (n > QC_SinkRoom (s))
		n = QC_SinkRoom (s);
	if (!n || !QC_SinkReserve (s, n))
		return;
	memset (s->buf + s->len, c, n);
	s->len += n;
	s->buf[s->len] = 0;
}

void QC_SinkPrintf (qc_sink_t *s, const char *fmt, ...)
{
	va_list	args, again;
	char	small[256], *text = small;
	int		n;

	va_start (args, fmt);
	va_copy (again, args);
	n = vsnprintf (small, sizeof(small), fmt, args);
	if (n >= (int)sizeof(small))
	{
		text = malloc ((size_t)n + 1);
		if (text)
			vsnprintf (text, (size_t)n + 1, fmt, again);
		else
			s->failed = true;
	}
	va_end (again);
	va_end (args);
	if (n > 0 && text)
		QC_SinkAppend (s, text, (size_t)n);
	if (text != small)
		free (text);
}

const char *QC_SinkText (const qc_sink_t *s)
{
	return s->buf ? s->buf : "";
}

/*
==============================================================================

EXACT DIGITS

==============================================================================
*/

// 1280 bits: a double's integer part (1024), or its fraction times 10 (1078)
#define QC_BIG_LIMBS	40

typedef struct
{
	uint32_t	w[QC_BIG_LIMBS];		// least significant first
	uint32_t	n;						// limbs in use; the top one non-zero
} qc_big_t;

static void QC_BigSet (qc_big_t *b, uint64_t v)
{
	b->w[0] = (uint32_t)v;
	b->w[1] = (uint32_t)(v >> 32);
	b->n = b->w[1] ? 2 : b->w[0] ? 1 : 0;
}

static void QC_BigShl (qc_big_t *b, uint32_t bits)
{
	uint32_t	limbs = bits / 32, shift = bits % 32, i;

	if (!b->n)
		return;
	b->w[b->n] = 0;
	for (i = b->n + 1 ; i-- > 0 ; )
		b->w[i + limbs] = shift ? (b->w[i] << shift) | (i ? b->w[i - 1] >> (32 - shift) : 0) : b->w[i];
	for (i = 0 ; i < limbs ; i++)
		b->w[i] = 0;
	b->n += limbs + 1;
	while (b->n && !b->w[b->n - 1])
		b->n--;
}

static void QC_BigMulSmall (qc_big_t *b, uint32_t m)
{
	uint64_t	carry = 0;
	uint32_t	i;

	for (i = 0 ; i < b->n ; i++)
	{
		carry += (uint64_t)b->w[i] * m;
		b->w[i] = (uint32_t)carry;
		carry >>= 32;
	}
	if (carry)
		b->w[b->n++] = (uint32_t)carry;
}

// b /= d, returning the remainder
static uint32_t QC_BigDivSmall (qc_big_t *b, uint32_t d)
{
	uint64_t	rem = 0;
	uint32_t	i;

	for (i = b->n ; i-- > 0 ; )
	{
		rem = (rem << 32) | b->w[i];
		b->w[i] = (uint32_t)(rem / d);
		rem %= d;
	}
	while (b->n && !b->w[b->n - 1])
		b->n--;
	return (uint32_t)rem;
}

// the decimal digits of a finite |v|: the integer part's (at least "0"), then
// the fraction's, up to its last non-zero one
typedef struct
{
	char	d[QC_MAX_INT_DIGITS + QC_MAX_FRACTION_DIGITS + 16];
	size_t	count;
	size_t	point;			// digits before the point
} qc_digits_t;

static void QC_ExactDigits (double v, qc_digits_t *out)
{
	uint64_t	bits = QC_DoubleBits (v) & ~(1ull << 63), m = bits & ((1ull << 52) - 1);
	int			rawexp = (int)(bits >> 52), e;
	uint32_t	k, i, chunk, limb, bit, digit;
	qc_big_t	big;
	char		rev[QC_MAX_INT_DIGITS + 32];
	size_t		n = 0, j;

	if (rawexp)
	{
		m |= 1ull << 52;
		e = rawexp - 1075;
	}
	else
		e = -1074;

	// the integer part, a chunk of nine digits at a time from the least significant
	if (e >= 0)
	{
		QC_BigSet (&big, m);
		QC_BigShl (&big, (uint32_t)e);
	}
	else
		QC_BigSet (&big, -e < 64 ? m >> -e : 0);
	while (big.n)
	{
		chunk = QC_BigDivSmall (&big, 1000000000);
		for (j = 0 ; j < 9 ; j++, chunk /= 10)
			rev[n++] = (char)('0' + chunk % 10);
	}
	while (n > 1 && rev[n - 1] == '0')
		n--;
	if (!n)
		rev[n++] = '0';
	for (j = 0 ; j < n ; j++)
		out->d[j] = rev[n - 1 - j];
	out->count = out->point = n;

	// the fraction, f / 2^k: each digit is the bits past k of f * 10
	if (e >= 0)
		return;
	k = (uint32_t)-e;
	QC_BigSet (&big, k >= 64 ? m : m & ((1ull << k) - 1));
	limb = k / 32;
	bit = k % 32;
	for (i = 0 ; i < k && big.n ; i++)
	{
		QC_BigMulSmall (&big, 10);
		digit = limb < big.n ? big.w[limb] >> bit : 0;
		if (bit && limb + 1 < big.n)
			digit |= big.w[limb + 1] << (32 - bit);
		out->d[out->count++] = (char)('0' + (digit & 15));
		if (limb < big.n)
		{
			big.w[limb] &= bit ? (1u << bit) - 1 : 0;
			big.n = limb + 1;
			while (big.n && !big.w[big.n - 1])
				big.n--;
		}
	}
}

// the first keep digits (zeros past count) rounded half to even by those after;
// true if they rounded up from all nines, for a 1 in front of them (zeros)
static bool QC_RoundDigits (const char *d, size_t count, size_t keep, char *out)
{
	char	next = keep < count ? d[keep] : '0';
	bool	rest = false, up;
	size_t	i;

	for (i = 0 ; i < keep ; i++)
		out[i] = i < count ? d[i] : '0';
	for (i = keep + 1 ; i < count && !rest ; i++)
		rest = d[i] != '0';
	up = next > '5' || (next == '5' && (rest || (keep && ((out[keep - 1] - '0') & 1))));
	if (!up)
		return false;
	for (i = keep ; i-- > 0 ; )
	{
		if (out[i] != '9')
		{
			out[i]++;
			return false;
		}
		out[i] = '0';
	}
	return true;
}

// %f's digits of a finite |v| with prec decimals: the body, and the zeros after
// it past the exact expansion
static size_t QC_FixedDigits (double v, size_t prec, bool alt, char *body, size_t *trail)
{
	qc_digits_t	dg;
	size_t		exact = prec < QC_MAX_FRACTION_DIGITS ? prec : QC_MAX_FRACTION_DIGITS, len = 0;
	char		kept[sizeof(dg.d) + 1];

	QC_ExactDigits (fabs (v), &dg);
	if (QC_RoundDigits (dg.d, dg.count, dg.point + exact, kept))
		body[len++] = '1';
	memcpy (body + len, kept, dg.point);
	len += dg.point;
	if (exact || alt)
		body[len++] = '.';
	memcpy (body + len, kept + dg.point, exact);
	len += exact;
	*trail = prec - exact;
	return len;
}

// %e's digits of a finite |v| with prec decimals: the body, the zeros after it
// and the decimal exponent
static size_t QC_SciDigits (double v, size_t prec, bool alt, char *body, size_t *trail, int64_t *exp10)
{
	qc_digits_t	dg;
	size_t		exact = prec < QC_MAX_SIGNIFICANT_DIGITS ? prec : QC_MAX_SIGNIFICANT_DIGITS, first, len = 0;
	char		kept[QC_MAX_SIGNIFICANT_DIGITS + 2];

	QC_ExactDigits (fabs (v), &dg);
	for (first = 0 ; first < dg.count && dg.d[first] == '0' ; first++)
		;
	if (first == dg.count)
	{
		memset (kept, '0', exact + 1);
		*exp10 = 0;
	}
	else
	{
		*exp10 = (int64_t)dg.point - 1 - (int64_t)first;
		if (QC_RoundDigits (dg.d + first, dg.count - first, exact + 1, kept))
		{
			kept[0] = '1';		// the rest are zeros already
			(*exp10)++;
		}
	}
	body[len++] = kept[0];
	if (exact || alt)
		body[len++] = '.';
	memcpy (body + len, kept + 1, exact);
	len += exact;
	*trail = prec - exact;
	return len;
}

/*
==============================================================================

PRINTF'S CONVERSIONS

==============================================================================
*/

// a formatted number: sign, prefix, lead zeros, body, trail zeros and suffix
typedef struct
{
	char		sign;			// 0 for none
	const char	*prefix;
	size_t		lead;
	const char	*body;
	size_t		bodylen;
	size_t		trail;
	const char	*suffix;
	size_t		suffixlen;
} qc_pieces_t;

static void QC_Emit (qc_sink_t *s, const qc_pieces_t *p, const qc_spec_t *spec, bool zero_pad)
{
	size_t	prefixlen = p->prefix ? strlen (p->prefix) : 0;
	size_t	len = (p->sign ? 1 : 0) + prefixlen + p->lead + p->bodylen + p->trail + p->suffixlen;
	size_t	pad = spec->width > len ? spec->width - len : 0;

	if (!spec->left && !zero_pad)
		QC_SinkFill (s, ' ', pad);
	if (p->sign)
		QC_SinkPush (s, p->sign);
	QC_SinkAppend (s, p->prefix ? p->prefix : "", prefixlen);
	if (!spec->left && zero_pad)
		QC_SinkFill (s, '0', pad);
	QC_SinkFill (s, '0', p->lead);
	QC_SinkAppend (s, p->body, p->bodylen);
	QC_SinkFill (s, '0', p->trail);
	QC_SinkAppend (s, p->suffix ? p->suffix : "", p->suffixlen);
	if (spec->left)
		QC_SinkFill (s, ' ', pad);
}

// e+XX: two digits or more
static size_t QC_ExponentSuffix (int64_t x, bool upper, char *out)
{
	return (size_t)snprintf (out, 32, "%c%c%02llu", upper ? 'E' : 'e', x < 0 ? '-' : '+',
		(unsigned long long)(x < 0 ? 0 - (uint64_t)x : (uint64_t)x));
}

void QC_FormatFloat (qc_sink_t *s, double v, char conv, const qc_spec_t *spec)
{
	bool		upper = conv >= 'A' && conv <= 'Z';
	char		lower = (char)(upper ? conv - 'A' + 'a' : conv);
	size_t		prec = spec->has_prec ? spec->prec : 6, decimals;
	char		*body, suffix[32];
	int64_t		x;
	qc_pieces_t	p = {0};

	p.sign = signbit (v) ? '-' : spec->plus ? '+' : spec->space ? ' ' : 0;
	if (!isfinite (v))
	{
		p.body = isnan (v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
		p.bodylen = 3;
		QC_Emit (s, &p, spec, false);
		return;
	}
	body = malloc (QC_BODY_MAX);
	if (!body)
	{
		s->failed = true;
		return;
	}
	p.body = body;
	if (lower == 'f')
		p.bodylen = QC_FixedDigits (v, prec, spec->alt, body, &p.trail);
	else if (lower == 'e')
	{
		p.bodylen = QC_SciDigits (v, prec, spec->alt, body, &p.trail, &x);
		p.suffixlen = QC_ExponentSuffix (x, upper, suffix);
		p.suffix = suffix;
	}
	else
	{
		if (!prec)
			prec = 1;
		// the exponent after rounding to prec significant digits picks the style
		QC_SciDigits (v, prec - 1, false, body, &p.trail, &x);
		if (x < (int64_t)prec && x >= -4)
		{
			decimals = (size_t)((int64_t)prec - 1 - x);
			p.bodylen = QC_FixedDigits (v, decimals, spec->alt, body, &p.trail);
		}
		else
		{
			p.bodylen = QC_SciDigits (v, prec - 1, spec->alt, body, &p.trail, &x);
			p.suffixlen = QC_ExponentSuffix (x, upper, suffix);
			p.suffix = suffix;
		}
		if (!spec->alt)
		{
			p.trail = 0;
			if (memchr (body, '.', p.bodylen))
			{
				while (p.bodylen && body[p.bodylen - 1] == '0')
					p.bodylen--;
				if (p.bodylen && body[p.bodylen - 1] == '.')
					p.bodylen--;
			}
		}
	}
	QC_Emit (s, &p, spec, spec->zero && !spec->left);
	free (body);
}

void QC_FormatInt (qc_sink_t *s, bool negative, uint64_t magnitude, uint32_t radix, bool upper,
	bool is_signed, const qc_spec_t *spec)
{
	static const char	lowerdigits[] = "0123456789abcdef", upperdigits[] = "0123456789ABCDEF";
	const char			*digits = upper ? upperdigits : lowerdigits;
	char				rev[72], body[72];
	size_t				n = 0, len = 0;
	qc_pieces_t			p = {0};

	if (magnitude || !spec->has_prec || spec->prec)
	{
		do
		{
			rev[n++] = digits[magnitude % radix];
			magnitude /= radix;
		} while (magnitude);
	}
	p.lead = spec->has_prec && spec->prec > n ? spec->prec - n : 0;
	if (spec->alt && radix == 8 && !p.lead && !(n && rev[n - 1] == '0'))
		body[len++] = '0';
	while (n)
		body[len++] = rev[--n];
	p.body = body;
	p.bodylen = len;
	p.sign = negative ? '-' : is_signed && spec->plus ? '+' : is_signed && spec->space ? ' ' : 0;
	if (spec->alt && radix == 16 && len && !(len == 1 && body[0] == '0'))
		p.prefix = upper ? "0X" : "0x";
	QC_Emit (s, &p, spec, spec->zero && !spec->left && !spec->has_prec);
}

void QC_FormatStr (qc_sink_t *s, const char *text, size_t len, const qc_spec_t *spec)
{
	size_t	pad;

	if (spec->has_prec && spec->prec < len)
		len = spec->prec;
	pad = spec->width > len ? spec->width - len : 0;
	if (!spec->left)
		QC_SinkFill (s, ' ', pad);
	QC_SinkAppend (s, text, len);
	if (spec->left)
		QC_SinkFill (s, ' ', pad);
}

void QC_FormatF (qc_sink_t *s, double v, size_t prec)
{
	QC_FormatFloat (s, v, 'f', &(qc_spec_t){.has_prec = true, .prec = prec});
}

void QC_FormatE (qc_sink_t *s, double v, size_t prec)
{
	QC_FormatFloat (s, v, 'e', &(qc_spec_t){.has_prec = true, .prec = prec});
}

void QC_FormatG (qc_sink_t *s, double v, size_t prec)
{
	QC_FormatFloat (s, v, 'g', &(qc_spec_t){.has_prec = true, .prec = prec});
}

/*
==============================================================================

TEXT AS NUMBERS

==============================================================================
*/

bool QC_IsCSpace (int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == 0x0C || c == '\r';
}

// a digit's value in base 36, or 99
static uint32_t QC_DigitValue (char c)
{
	if (c >= '0' && c <= '9')
		return (uint32_t)(c - '0');
	if (c >= 'a' && c <= 'z')
		return (uint32_t)(c - 'a' + 10);
	if (c >= 'A' && c <= 'Z')
		return (uint32_t)(c - 'A' + 10);
	return 99;
}

// what strtol and strtoul share: white space, a sign, an optional 0x (base 16,
// or base 0 which also takes a leading 0 for octal), digits; the bytes read in
// *used (0 without a digit), as C's endptr. A base but 0 or 2 to 36 reads nothing.
static uint64_t QC_ParseCInteger (const char *s, uint32_t base, bool *negative, bool *overflow, size_t *used)
{
	size_t		i = 0, start;
	bool		hex;
	uint64_t	value = 0;
	uint32_t	d;

	*negative = *overflow = false;
	if (used)
		*used = 0;
	if (base == 1 || base > 36)
		return 0;
	while (QC_IsCSpace (s[i]))
		i++;
	*negative = s[i] == '-';
	if (s[i] == '-' || s[i] == '+')
		i++;
	hex = s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X') && QC_DigitValue (s[i + 2]) < 16;
	if (!base)
		base = hex ? 16 : s[i] == '0' ? 8 : 10;
	if (base == 16 && hex)
		i += 2;
	for (start = i ; (d = QC_DigitValue (s[i])) < base ; i++)
	{
		if (value > (UINT64_MAX - d) / base)
			*overflow = true;
		else
			value = value * base + d;
	}
	if (used)
		*used = i > start ? i : 0;
	return value;
}

int64_t QC_Strtol (const char *s, uint32_t base, size_t *used)
{
	bool		negative, overflow;
	uint64_t	mag = QC_ParseCInteger (s, base, &negative, &overflow, used);

	if (overflow)
		return negative ? INT64_MIN : INT64_MAX;
	if (!negative)
		return mag > INT64_MAX ? INT64_MAX : (int64_t)mag;
	return mag > (uint64_t)INT64_MAX + 1 ? INT64_MIN : (int64_t)(0 - mag);
}

uint64_t QC_Strtoul (const char *s, uint32_t base, size_t *used)
{
	bool		negative, overflow;
	uint64_t	mag = QC_ParseCInteger (s, base, &negative, &overflow, used);

	if (overflow)
		return UINT64_MAX;
	return negative ? 0 - mag : mag;
}

static bool QC_StartsCI (const char *s, const char *prefix)
{
	for ( ; *prefix ; s++, prefix++)
		if ((*s | 0x20) != *prefix)
			return false;
	return true;
}

// the hex digits kept of a hex float: 160 bits, far more than a double needs,
// so that a sticky digit for the rest rounds correctly
#define QC_HEX_DIGITS_KEPT	40

// the double nearest to digits * 2^scale (hex digit values, the most significant
// first, no leading zeros), exact ties to even
static double QC_HexDigitsToDouble (const uint8_t *digits, size_t count, int64_t scale)
{
	uint64_t	m = 0, half, dropped, kept;
	bool		sticky = false, roundhalf, rest;
	int64_t		exp = scale, lead, drop;
	size_t		k;
	int			lz;

	// the first sixteen make the mantissa; the rest only carry weight
	for (k = 0 ; k < count && k < 16 ; k++)
		m = (m << 4) | digits[k];
	for ( ; k < count ; k++)
	{
		sticky |= digits[k] != 0;
		exp += 4;
	}
	if (!m)
		return 0.0;
	// normalized, the leading one at bit 63: m * 2^exp, and the sticky bits
	for (lz = 0 ; !(m & (1ull << 63)) ; lz++)
		m <<= 1;
	exp -= lz;
	lead = exp + 63;
	if (lead > 1023)
		return INFINITY;
	// 11 bits to drop for a normal double, more for a subnormal one
	drop = 11 + (-1022 - lead > 0 ? -1022 - lead : 0);
	if (drop > 64)
		return 0.0;
	if (drop == 64)
	{
		kept = 0;
		roundhalf = (m >> 63) == 1;
		rest = (m << 1) != 0 || sticky;
	}
	else
	{
		half = 1ull << (drop - 1);
		dropped = m & ((half << 1) - 1);
		kept = m >> drop;
		roundhalf = (dropped & half) != 0;
		rest = (dropped & (half - 1)) != 0 || sticky;
	}
	if (roundhalf && (rest || (kept & 1)))
		kept++;
	// at most 54 bits, which the scale puts on the double grid exactly
	return ldexp ((double)kept, (int)(exp + drop));
}

// the part of a hex float after 0x: the value and the bytes read, or false
// without a hex digit
static bool QC_ParseHexFloat (const char *s, double *value, size_t *used)
{
	size_t		i = 0, count = 0;
	uint8_t		digits[QC_HEX_DIGITS_KEPT + 1];
	bool		sticky = false, any = false, point = false, negative;
	int64_t		scale = 0, e, top;
	uint32_t	h;
	size_t		j;

	for ( ; ; i++)
	{
		if (s[i] == '.' && !point)
		{
			point = true;
			continue;
		}
		if ((h = QC_DigitValue (s[i])) >= 16)
			break;
		any = true;
		if (!count && !h)
			;		// leading zeros only shift the fraction's digits
		else if (count < QC_HEX_DIGITS_KEPT)
			digits[count++] = (uint8_t)h;
		else
		{
			sticky |= h != 0;
			scale += 4;
		}
		if (point)
			scale -= 4;
	}
	if (!any)
		return false;
	if (s[i] == 'p' || s[i] == 'P')
	{
		j = i + 1;
		negative = s[j] == '-';
		if (s[j] == '-' || s[j] == '+')
			j++;
		if (s[j] >= '0' && s[j] <= '9')
		{
			for (e = 0 ; s[j] >= '0' && s[j] <= '9' ; j++)
				if (e < 1000000000)
					e = e * 10 + (s[j] - '0');
			scale += negative ? -e : e;
			i = j;
		}
	}
	*used = i;
	if (!count)
	{
		*value = 0.0;
		return true;
	}
	if (sticky)
	{
		digits[count++] = 1;
		scale -= 4;
	}
	top = scale + (int64_t)count * 4;
	if (top > 1100)
		*value = INFINITY;
	else if (top < -1200)
		*value = 0.0;
	else
		*value = QC_HexDigitsToDouble (digits, count, scale);
	return true;
}

double QC_Strtod (const char *s, size_t *used)
{
	size_t		i = 0, j, k, n, start, digits;
	bool		negative, any = false, point = false, negexp;
	double		value;
	int64_t		frac = 0, exp = 0;
	char		*canon;

	while (QC_IsCSpace (s[i]))
		i++;
	negative = s[i] == '-';
	if (s[i] == '-' || s[i] == '+')
		i++;
	if (QC_StartsCI (s + i, "inf"))
	{
		*used = i + (QC_StartsCI (s + i, "infinity") ? 8 : 3);
		return negative ? -INFINITY : INFINITY;
	}
	if (QC_StartsCI (s + i, "nan"))
	{
		n = 3;
		if (s[i + 3] == '(')
		{
			for (k = i + 4 ; (s[k] >= '0' && s[k] <= '9') || ((s[k] | 0x20) >= 'a' && (s[k] | 0x20) <= 'z')
				|| s[k] == '_' ; k++)
				;
			if (s[k] == ')')
				n = k + 1 - i;
		}
		*used = i + n;
		return negative ? -NAN : NAN;
	}
	if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X'))
	{
		if (QC_ParseHexFloat (s + i + 2, &value, &n))
			*used = i + 2 + n;
		else
		{
			value = 0.0;
			*used = i + 1;
		}
		return negative ? -value : value;
	}

	// decimal: digits, a point and digits, an exponent; made "digits e exponent"
	// for the platform's strtod, correctly rounded and with no point for a locale
	// to spell differently
	for (j = i, start = 0, digits = 0 ; ; j++)
	{
		if (s[j] == '.' && !point)
		{
			point = true;
			continue;
		}
		if (s[j] < '0' || s[j] > '9')
			break;
		any = true;
		if (!digits && s[j] == '0')
			;		// leading zeros
		else if (!digits++)
			start = j;
		if (point)
			frac++;
	}
	if (!any)
	{
		*used = 0;
		return 0.0;
	}
	if (s[j] == 'e' || s[j] == 'E')
	{
		k = j + 1;
		negexp = s[k] == '-';
		if (s[k] == '-' || s[k] == '+')
			k++;
		if (s[k] >= '0' && s[k] <= '9')
		{
			for ( ; s[k] >= '0' && s[k] <= '9' ; k++)
				if (exp < 1000000000000ll)
					exp = exp * 10 + (s[k] - '0');
			if (negexp)
				exp = -exp;
			j = k;
		}
	}
	*used = j;
	if (!digits)
		return negative ? -0.0 : 0.0;
	exp -= frac;
	if (exp < -1000000000)
		exp = -1000000000;
	if (exp > 1000000000)
		exp = 1000000000;
	canon = malloc (digits + 24);
	if (!canon)
		return negative ? -0.0 : 0.0;
	for (k = start, n = 0 ; n < digits ; k++)
		if (s[k] != '.')
			canon[n++] = s[k];
	snprintf (canon + n, 24, "e%lld", (long long)exp);
	value = strtod (canon, NULL);
	free (canon);
	return negative ? -value : value;
}
