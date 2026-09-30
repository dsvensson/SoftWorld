// qc_runner.c -- the builtins of FTE's standalone qcvm test runner
//
// Characterized as a black box: #1 and #20 puts (the string arguments one
// after another), #2 ftos (%g), #3 spawn, #4 remove, #5 vtos ('%g %g %g'), #6
// error, #7 vlen, #8 etos (the entity number), #9 stof, #10 strcat, #11 strcmp
// (its sign), #12 normalize, #13 sqrt, #14 floor, #15 pow, #16 stov, #17 itos
// (hex), #18 ltos (64-bit hex), #19 dtos (%g), #21 putv (%f %f %f\n), #22 putf
// (%f\n) and #23 printf (%d %i %f %g %s).

#include "qc_runner.h"
#include "qc_local.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static qc_runnerhost_t *Host (qcvm_t *vm)
{
	return QC_HostContext (vm);
}

void QR_FormatG (double v, char *buf, size_t size)
{
	if (isnan (v))
		snprintf (buf, size, signbit (v) ? "-nan" : "nan");
	else if (isinf (v))
		snprintf (buf, size, v < 0 ? "-inf" : "inf");
	else
		snprintf (buf, size, "%g", v);
}

void QR_FormatF (double v, char *buf, size_t size)
{
	if (isnan (v))
		snprintf (buf, size, signbit (v) ? "-nan" : "nan");
	else if (isinf (v))
		snprintf (buf, size, v < 0 ? "-inf" : "inf");
	else
		snprintf (buf, size, "%f", v);
}

// the arguments' strings, one after another
static void QR_Concat (qcvm_t *vm, qt_text_t *t)
{
	int	i;

	QT_TextAppend (t, "");
	for (i = 0 ; i < QC_Argc (vm) ; i++)
		QT_TextAppend (t, QC_ArgString (vm, i));
}

static bool QR_ReturnText (qcvm_t *vm, const char *text)
{
	return QC_ReturnString (vm, text, strlen (text));
}

static bool QR_Puts (qcvm_t *vm)
{
	QR_Concat (vm, &Host (vm)->out);
	return true;
}

static bool QR_Ftos (qcvm_t *vm)
{
	char	buf[64];

	QR_FormatG ((double)QC_ArgFloat (vm, 0), buf, sizeof(buf));
	return QR_ReturnText (vm, buf);
}

static bool QR_Spawn (qcvm_t *vm)
{
	qc_ent_t	e;

	if (!QC_Spawn (vm, &e))
		return false;
	QC_ReturnWord (vm, e);
	return true;
}

static bool QR_Remove (qcvm_t *vm)
{
	QC_Remove (vm, QC_ArgWord (vm, 0), false);
	return true;
}

static bool QR_Vtos (qcvm_t *vm)
{
	float	v[3];
	char	x[64], y[64], z[64], buf[200];

	QC_ArgVector (vm, 0, v);
	QR_FormatG (v[0], x, sizeof(x));
	QR_FormatG (v[1], y, sizeof(y));
	QR_FormatG (v[2], z, sizeof(z));
	snprintf (buf, sizeof(buf), "'%s %s %s'", x, y, z);
	return QR_ReturnText (vm, buf);
}

static bool QR_Error (qcvm_t *vm)
{
	qt_text_t	msg = {0};
	bool		r;

	QR_Concat (vm, &msg);
	r = QC_Fail (vm, QC_ERR_QC, 0, "%s", msg.text);
	QT_TextFree (&msg);
	return r;
}

static bool QR_Vlen (qcvm_t *vm)
{
	float	v[3];

	QC_ArgVector (vm, 0, v);
	QC_ReturnFloat (vm, sqrtf (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
	return true;
}

static bool QR_Etos (qcvm_t *vm)
{
	char	buf[16];

	snprintf (buf, sizeof(buf), "%u", QC_ArgWord (vm, 0));
	return QR_ReturnText (vm, buf);
}

// the numeric start of s: a sign, digits and a point, parsed if they make a number
static double QR_Atof (const char *s)
{
	char	num[64];
	size_t	n = 0;
	int		dots = 0, digits = 0;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
		s++;
	for ( ; *s && n + 1 < sizeof(num) ; s++)
	{
		if (*s >= '0' && *s <= '9')
			digits++;
		else if (*s == '.')
			dots++;
		else if (!((*s == '-' || *s == '+') && !n))
			break;
		num[n++] = *s;
	}
	num[n] = 0;
	if (!digits || dots > 1)
		return 0;
	return strtod (num, NULL);
}

static bool QR_Stof (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)QR_Atof (QC_ArgString (vm, 0)));
	return true;
}

static bool QR_Strcat (qcvm_t *vm)
{
	qt_text_t	t = {0};
	bool		r;

	QR_Concat (vm, &t);
	r = QC_ReturnString (vm, t.text, t.len);
	QT_TextFree (&t);
	return r;
}

static bool QR_Strcmp (qcvm_t *vm)
{
	int	c = strcmp (QC_ArgString (vm, 0), QC_ArgString (vm, 1));

	QC_ReturnFloat (vm, c < 0 ? -1.0f : c > 0 ? 1.0f : 0.0f);
	return true;
}

static bool QR_Normalize (qcvm_t *vm)
{
	float	v[3], len;

	QC_ArgVector (vm, 0, v);
	len = sqrtf (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len == 0)
		v[0] = v[1] = v[2] = 0;
	else
	{
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
	QC_ReturnVector (vm, v);
	return true;
}

static bool QR_Sqrt (qcvm_t *vm)
{
	QC_ReturnFloat (vm, sqrtf (QC_ArgFloat (vm, 0)));
	return true;
}

static bool QR_Floor (qcvm_t *vm)
{
	QC_ReturnFloat (vm, floorf (QC_ArgFloat (vm, 0)));
	return true;
}

static bool QR_Pow (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)pow ((double)QC_ArgFloat (vm, 0), (double)QC_ArgFloat (vm, 1)));
	return true;
}

static bool QR_Stov (qcvm_t *vm)
{
	char	buf[256], *p, *tok;
	float	v[3] = {0, 0, 0};
	int		k = 0;

	snprintf (buf, sizeof(buf), "%s", QC_ArgString (vm, 0));
	for (p = buf ; *p ; p++)
		if (*p == '\'')
			*p = ' ';
	for (tok = strtok (buf, " \t\n\r") ; tok && k < 3 ; tok = strtok (NULL, " \t\n\r"))
		v[k++] = (float)QR_Atof (tok);
	QC_ReturnVector (vm, v);
	return true;
}

static bool QR_Itos (qcvm_t *vm)
{
	char	buf[16];

	snprintf (buf, sizeof(buf), "%x", QC_ArgWord (vm, 0));
	return QR_ReturnText (vm, buf);
}

static bool QR_Ltos (qcvm_t *vm)
{
	qc_value_t	w = QC_ArgValue (vm, 0);
	char		buf[32];

	snprintf (buf, sizeof(buf), "%llx", (unsigned long long)w.w[0] | (unsigned long long)w.w[1] << 32);
	return QR_ReturnText (vm, buf);
}

static bool QR_Dtos (qcvm_t *vm)
{
	qc_value_t	w = QC_ArgValue (vm, 0);
	char		buf[64];

	QR_FormatG (QC_BitsDouble ((uint64_t)w.w[0] | (uint64_t)w.w[1] << 32), buf, sizeof(buf));
	return QR_ReturnText (vm, buf);
}

static bool QR_Putv (qcvm_t *vm)
{
	float	v[3];
	char	x[64], y[64], z[64];

	QC_ArgVector (vm, 0, v);
	QR_FormatF (v[0], x, sizeof(x));
	QR_FormatF (v[1], y, sizeof(y));
	QR_FormatF (v[2], z, sizeof(z));
	QT_TextAppend (&Host (vm)->out, x);
	QT_TextAppend (&Host (vm)->out, " ");
	QT_TextAppend (&Host (vm)->out, y);
	QT_TextAppend (&Host (vm)->out, " ");
	QT_TextAppend (&Host (vm)->out, z);
	QT_TextAppend (&Host (vm)->out, "\n");
	return true;
}

static bool QR_Putf (qcvm_t *vm)
{
	char	buf[64];

	QR_FormatF (QC_ArgFloat (vm, 0), buf, sizeof(buf));
	QT_TextAppend (&Host (vm)->out, buf);
	QT_TextAppend (&Host (vm)->out, "\n");
	return true;
}

// a float to int as Rust's `as` makes it: saturating, NaN 0
static int32_t QR_Saturate (float f)
{
	if (f != f)
		return 0;
	if (f >= 2147483648.0f)
		return INT32_MAX;
	if (f <= -2147483648.0f)
		return INT32_MIN;
	return (int32_t)f;
}

static bool QR_Printf (qcvm_t *vm)
{
	char		fmt[1024], text[256], c;
	qt_text_t	*out = &Host (vm)->out;
	const char	*p;
	int			arg = 1;

	snprintf (fmt, sizeof(fmt), "%s", QC_ArgString (vm, 0));
	QT_TextAppend (out, "");
	for (p = fmt ; *p ; p++)
	{
		if (*p != '%')
		{
			text[0] = *p;
			text[1] = 0;
			QT_TextAppend (out, text);
			continue;
		}
		if (!(c = *++p))
			break;
		switch (c)
		{
		case 'd':	snprintf (text, sizeof(text), "%d", QR_Saturate (QC_ArgFloat (vm, arg))); break;
		case 'i':	snprintf (text, sizeof(text), "%d", QC_ArgInt (vm, arg)); break;
		case 'f':	QR_FormatF (QC_ArgFloat (vm, arg), text, sizeof(text)); break;
		case 'g':	QR_FormatG (QC_ArgFloat (vm, arg), text, sizeof(text)); break;
		case 's':	snprintf (text, sizeof(text), "%s", QC_ArgString (vm, arg)); break;
		case '%':
			QT_TextAppend (out, "%");
			continue;
		default:	snprintf (text, sizeof(text), "%%%c", c); break;
		}
		arg++;
		QT_TextAppend (out, text);
	}
	return true;
}

qc_builtins_t *QR_Builtins (void)
{
	static const struct
	{
		uint32_t		number;
		const char		*name;
		qc_builtin_t	func;
	} table[] = {
		{1, "puts", QR_Puts}, {2, "ftos", QR_Ftos}, {3, "spawn", QR_Spawn}, {4, "remove", QR_Remove},
		{5, "vtos", QR_Vtos}, {6, "error", QR_Error}, {7, "vlen", QR_Vlen}, {8, "etos", QR_Etos},
		{9, "stof", QR_Stof}, {10, "strcat", QR_Strcat}, {11, "strcmp", QR_Strcmp},
		{12, "normalize", QR_Normalize}, {13, "sqrt", QR_Sqrt}, {14, "floor", QR_Floor}, {15, "pow", QR_Pow},
		{16, "stov", QR_Stov}, {17, "itos", QR_Itos}, {18, "ltos", QR_Ltos}, {19, "dtos", QR_Dtos},
		{20, "puts2", QR_Puts}, {21, "putv", QR_Putv}, {22, "putf", QR_Putf}, {23, "printf", QR_Printf},
	};
	qc_builtins_t	*b = QC_BuiltinsCreate (QC_NUMBERING_NONE);
	size_t			i;

	for (i = 0 ; b && i < sizeof(table) / sizeof(table[0]) ; i++)
		QC_BuiltinsSetNumbered (b, table[i].number, table[i].name, table[i].func);
	return b;
}
