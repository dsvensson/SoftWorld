// qc_lib_format.c -- sprintf, and quoting for the console (docs/spec/strings.md)
//
// sprintf is C's printf with FTE's conventions, made by qc_dtoa.c's engine:
// numeric conversions read float arguments unless l (int), q (64-bit, two
// words) or %i and %p say otherwise; %v prints a vector, %S a quoted string;
// a malformed directive ends the output, with a warning.

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

// sprintf's output (FTE's buffer is 65536 bytes, the terminator included)
#define QC_SPRINTF_MAX	65535

void QC_QuoteString (qc_sink_t *out, const char *s, size_t len, size_t bufsize)
{
	size_t	budget, i;
	char	c, e;

	if (memchr (s, '\n', len) || memchr (s, '\r', len) || memchr (s, '"', len))
	{
		QC_SinkAppend (out, "\\\"", 2);
		budget = bufsize > 4 ? bufsize - 4 : 0;
		for (i = 0 ; i < len && budget >= 2 ; i++)
		{
			c = s[i];
			switch (c)
			{
			case '\n':	e = 'n'; break;
			case '\r':	e = 'r'; break;
			case '\t':	e = 't'; break;
			case '\'':
			case '"':
			case '\\':
			case '$':	e = c; break;
			default:	e = 0; break;
			}
			if (e)
			{
				QC_SinkPush (out, '\\');
				QC_SinkPush (out, e);
				budget -= 2;
			}
			else
			{
				QC_SinkPush (out, c);
				budget--;
			}
		}
	}
	else
	{
		QC_SinkPush (out, '"');
		budget = bufsize > 3 ? bufsize - 3 : 0;
		QC_SinkAppend (out, s, len < budget ? len : budget);
	}
	QC_SinkPush (out, '"');
}

// sprintf's argument a (from 1); outside those passed, zero
static void QC_SprintfArg (const qcvm_t *vm, int argc, int64_t a, uint32_t w[3])
{
	if (a >= 1 && a < argc)
		QC_ArgRaw (vm, (int)a, w);
	else
		w[0] = w[1] = w[2] = 0;
}

static float QC_SprintfF32 (const qcvm_t *vm, int argc, int64_t a)
{
	uint32_t	w[3];

	QC_SprintfArg (vm, argc, a, w);
	return QC_BitsFloat (w[0]);
}

static uint64_t QC_SprintfU64 (const qcvm_t *vm, int argc, int64_t a)
{
	uint32_t	w[3];

	QC_SprintfArg (vm, argc, a, w);
	return w[0] | ((uint64_t)w[1] << 32);
}

// C's strtol in base 10 on the digits at *i, cut to an int as x86-64 does
static int32_t QC_ParseInt (const char *fmt, size_t *i)
{
	int64_t	v = 0;

	for ( ; fmt[*i] >= '0' && fmt[*i] <= '9' ; (*i)++)
		v = v > INT64_MAX / 10 - 9 ? INT64_MAX : v * 10 + (fmt[*i] - '0');
	return (int32_t)(uint32_t)(uint64_t)v;
}

static bool QC_IsDigit (char c)
{
	return c >= '0' && c <= '9';
}

// FTE's unicode_strpad: %s with width and precision in characters in the UTF-8 scheme
static void QC_FormatStrChars (qc_sink_t *out, const char *s, size_t len, const qc_spec_t *spec,
	qc_charscheme_t scheme)
{
	size_t	max, end, chars, pad;

	if (scheme != QC_CHARS_UTF8)
	{
		QC_FormatStr (out, s, len, spec);
		return;
	}
	max = spec->has_prec ? spec->prec : QC_SinkRoom (out);
	end = QC_ByteOffset ((const uint8_t *)s, len, max, scheme);
	chars = QC_CharCount ((const uint8_t *)s, end, scheme);
	pad = spec->width > chars ? spec->width - chars : 0;
	if (!spec->left)
		QC_SinkFill (out, ' ', pad);
	QC_SinkAppend (out, s, end);
	if (spec->left)
		QC_SinkFill (out, ' ', pad);
}

// the argument position of a * given as N$, or the next one
static bool QC_StarArg (const char *fmt, size_t *i, int64_t *argpos, int64_t *at)
{
	if (QC_IsDigit (fmt[*i]))
	{
		*at = QC_ParseInt (fmt, i);
		if (fmt[*i] != '$')
			return false;
		(*i)++;
	}
	else
		*at = (*argpos)++;
	return true;
}

// Formats the builtin's format with its arguments from 1 on; false (the output
// so far, and the warning in *bad) at a malformed directive
static bool QC_SprintfImpl (qcvm_t *vm, qc_sink_t *out, size_t *bad)
{
	const char		*fmt = QC_ArgString (vm, 0), *str;
	int				argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8;
	int64_t			argpos = 1, thisarg, at, iv;
	size_t			i = 0, start, pad;
	uint64_t		uv;
	int32_t			width, prec;
	int				isfloat;		// -1 unset
	bool			is64, bytemode;
	char			c, conv, first, g, *nul;
	qc_spec_t		spec;
	uint32_t		w[3], code, k;
	double			v;
	qc_sink_t		tmp;

	while ((c = fmt[i]))
	{
		if (c != '%')
		{
			QC_SinkPush (out, c);
			i++;
			continue;
		}
		start = i++;
		if (fmt[i] == '%')
		{
			QC_SinkPush (out, '%');
			i++;
			continue;
		}
		spec = (qc_spec_t){0};
		width = -1;
		prec = -1;
		thisarg = -1;
		isfloat = -1;
		is64 = false;

		// a number just after the % is an argument position (N$) or the width
		if (QC_IsDigit (fmt[i]))
		{
			first = fmt[i];
			iv = QC_ParseInt (fmt, &i);
			if (fmt[i] == '$')
			{
				thisarg = iv;
				i++;
			}
			else
			{
				width = (int32_t)iv;
				if (first == '0')
				{
					spec.zero = true;
					if (!width)
						width = -1;
				}
			}
		}
		// the flags and the width, unless the width came first
		if (width < 0)
		{
			for ( ; ; i++)
			{
				if (fmt[i] == '#')
					spec.alt = true;
				else if (fmt[i] == '0')
					spec.zero = true;
				else if (fmt[i] == '-')
					spec.left = true;
				else if (fmt[i] == ' ')
					spec.space = true;
				else if (fmt[i] == '+')
					spec.plus = true;
				else
					break;
			}
			if (fmt[i] == '*')
			{
				i++;
				if (!QC_StarArg (fmt, &i, &argpos, &at))
				{
					*bad = start;
					return false;
				}
				width = QC_FloatToInt (QC_SprintfF32 (vm, argc, at));
				if (width < 0)
				{
					spec.left = true;
					width = (int32_t)(0u - (uint32_t)width);
				}
			}
			else if (QC_IsDigit (fmt[i]))
			{
				width = QC_ParseInt (fmt, &i);
				if (width < 0)
				{
					spec.left = true;
					width = (int32_t)(0u - (uint32_t)width);
				}
			}
		}
		// the precision
		if (fmt[i] == '.')
		{
			i++;
			if (fmt[i] == '*')
			{
				i++;
				if (!QC_StarArg (fmt, &i, &argpos, &at))
				{
					*bad = start;
					return false;
				}
				prec = QC_FloatToInt (QC_SprintfF32 (vm, argc, at));
			}
			else if (QC_IsDigit (fmt[i]))
				prec = QC_ParseInt (fmt, &i);
			else
			{
				*bad = start;
				return false;
			}
		}
		// length modifiers
		for ( ; ; i++)
		{
			if (fmt[i] == 'h')
				isfloat = 1;
			else if (fmt[i] == 'l' || fmt[i] == 'L')
				isfloat = 0;
			else if (fmt[i] == 'q')
				is64 = true;
			else if (fmt[i] != 'j' && fmt[i] != 'z' && fmt[i] != 't')
				break;
		}
		conv = fmt[i];
		if (conv == 'p' || conv == 'P')
		{
			spec.zero = true;
			if (width < 0)
				width = 8;
			if (isfloat < 0)
				isfloat = 0;
		}
		else if (conv == 'i' && isfloat < 0)
			isfloat = 0;
		if (isfloat < 0)
			isfloat = 1;
		if (thisarg < 0)
			thisarg = argpos++;
		spec.width = width > 0 ? (size_t)width : 0;
		spec.has_prec = prec >= 0;
		spec.prec = prec >= 0 ? (size_t)prec : 0;

		if (QC_SinkRoom (out))
		{
			switch (conv)
			{
			case 'd':
			case 'i':
				if (is64)
					iv = isfloat ? QC_D2I64 (QC_BitsDouble (QC_SprintfU64 (vm, argc, thisarg)))
						: (int64_t)QC_SprintfU64 (vm, argc, thisarg);
				else
				{
					QC_SprintfArg (vm, argc, thisarg, w);
					iv = isfloat ? QC_D2I64 ((double)QC_BitsFloat (w[0])) : (int32_t)w[0];
				}
				QC_FormatInt (out, iv < 0, iv < 0 ? 0 - (uint64_t)iv : (uint64_t)iv, 10, false, true, &spec);
				break;
			case 'o':
			case 'u':
			case 'x':
			case 'X':
			case 'p':
			case 'P':
				if (is64)
					uv = isfloat ? QC_D2U64 (QC_BitsDouble (QC_SprintfU64 (vm, argc, thisarg)))
						: QC_SprintfU64 (vm, argc, thisarg);
				else
				{
					QC_SprintfArg (vm, argc, thisarg, w);
					uv = isfloat ? QC_D2U64 ((double)QC_BitsFloat (w[0])) : w[0];
				}
				QC_FormatInt (out, false, uv, conv == 'o' ? 8 : conv == 'u' ? 10 : 16, conv == 'X' || conv == 'P',
					false, &spec);
				break;
			case 'e':
			case 'E':
			case 'f':
			case 'F':
			case 'g':
			case 'G':
				if (is64)
				{
					uv = QC_SprintfU64 (vm, argc, thisarg);
					v = isfloat ? QC_BitsDouble (uv) : (double)(int64_t)uv;
				}
				else
				{
					QC_SprintfArg (vm, argc, thisarg, w);
					v = isfloat ? (double)QC_BitsFloat (w[0]) : (double)(int32_t)w[0];
				}
				QC_FormatFloat (out, v, conv, &spec);
				break;
			case 'v':
			case 'V':
				g = conv == 'V' ? 'G' : 'g';
				QC_SprintfArg (vm, argc, thisarg, w);
				for (k = 0 ; k < 3 ; k++)
				{
					if (k)
						QC_SinkPush (out, ' ');
					QC_FormatFloat (out, isfloat ? (double)QC_BitsFloat (w[k]) : (double)(int32_t)w[k], g, &spec);
				}
				break;
			case 'c':
				QC_SprintfArg (vm, argc, thisarg, w);
				code = isfloat ? QC_F2U (QC_BitsFloat (w[0])) : w[0];
				bytemode = spec.alt || !vm->config.utf8;
				spec.alt = false;
				if (bytemode)
				{
					// C's %c writes the low byte; FTE then cuts its output at a NUL
					c = (char)(code & 0xFF);
					pad = spec.width > 1 ? spec.width - 1 : 0;
					if (c)
					{
						spec.has_prec = false;
						QC_FormatStr (out, &c, 1, &spec);
					}
					else if (!spec.left)
						QC_SinkFill (out, ' ', pad);
				}
				else
				{
					QC_SinkInit (&tmp, SIZE_MAX);
					QC_EncodeChar (&tmp, code, vm->config.charscheme, false);
					if (tmp.buf && (nul = memchr (tmp.buf, 0, tmp.len)))
						tmp.len = (size_t)(nul - tmp.buf);
					QC_FormatStrChars (out, QC_SinkText (&tmp), tmp.len, &spec, vm->config.charscheme);
					QC_SinkFree (&tmp);
				}
				break;
			case 's':
			case 'S':
				str = thisarg >= 1 && thisarg < argc ? QC_ArgString (vm, (int)thisarg) : "";
				QC_SinkInit (&tmp, SIZE_MAX);
				if (conv == 'S')
					QC_QuoteString (&tmp, str, strlen (str), 65536);
				else
					QC_SinkAppend (&tmp, str, strlen (str));
				if (spec.alt || !vm->config.utf8)
					QC_FormatStr (out, QC_SinkText (&tmp), tmp.len, &spec);
				else
					QC_FormatStrChars (out, QC_SinkText (&tmp), tmp.len, &spec, vm->config.charscheme);
				QC_SinkFree (&tmp);
				break;
			default:
				*bad = start;
				return false;
			}
		}
		if (conv)
			i++;
	}
	return true;
}

// string sprintf(string fmt, ...) (#627); 65535 bytes at most
static bool QC_Sprintf (qcvm_t *vm)
{
	qc_sink_t	out;
	size_t		bad = 0;
	char		*fmt;

	QC_SinkInit (&out, QC_SPRINTF_MAX);
	if (!QC_SprintfImpl (vm, &out, &bad))
	{
		fmt = (char *)QC_ArgString (vm, 0);
		QC_Warning (vm, "sprintf: bad format string: %s", fmt + bad);
	}
	return QC_LibReturnSink (vm, &out);
}

static const qc_libentry_t	qc_format[] = {
	{"sprintf", QC_Sprintf, NULL, 0},
};

bool QC_RegisterFormat (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_format, sizeof(qc_format) / sizeof(qc_format[0]));
}
