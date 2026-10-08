// qc_lib_convert.c -- numbers as text and back: ftos, vtos, etos, itos, htos,
// stoi, stoh, ftoi, itof, ftou, utof, stof, stod, stol, stoul, stov
// (docs/spec/strings.md)

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
==============================================================================

NUMBERS AS TEXT

==============================================================================
*/

// FTE's ftos: integers in decimal; other values with just the decimals to show
// the float's exact value to about 8 significant digits (0.1 is 0.100000001);
// infinities and NaNs as 1.#INF and 1.#NAN with the sign bit shown
void QC_FtosText (qc_sink_t *s, float v)
{
	int32_t		i = QC_FloatToInt (v), e, decimals;
	uint32_t	bits = QC_FloatBits (v), raw = (bits >> 23) & 0xFF;
	float		log10_2 = QC_BitsFloat (0x3E9A209B);		// FTE's 0.30102999957f
	qc_sink_t	t;
	size_t		k, keep = 0;
	char		c;

	if (v == (float)i)
	{
		QC_SinkPrintf (s, "%d", i);
		return;
	}
	if (raw == 0xFF)
	{
		if (bits >> 31)
			QC_SinkPush (s, '-');
		QC_SinkAppend (s, bits & 0x007FFFFF ? "1.#NAN" : "1.#INF", 6);
		return;
	}
	// 8 decimals, and the decimal magnitude of the binary exponent
	e = (int32_t)raw - 127;
	decimals = QC_FloatToInt ((float)-e * log10_2) + 8;
	if (decimals <= 0)
	{
		QC_FormatF (s, (double)v, 0);
		return;
	}
	QC_SinkInit (&t, SIZE_MAX);
	QC_FormatF (&t, (double)v, (size_t)decimals);
	// cut after the last significant digit (zeros before the point are)
	for (k = 0 ; k < t.len ; k++)
	{
		c = t.buf[k];
		if (c >= '1' && c <= '9')
			keep = k + 1;
		else if (c == '.')
			keep = k;
	}
	QC_SinkAppend (s, QC_SinkText (&t), keep);
	s->failed |= t.failed;
	QC_SinkFree (&t);
}

// string ftos(float)
static bool QC_Ftos (qcvm_t *vm)
{
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_FtosText (&s, QC_ArgFloat (vm, 0));
	return QC_LibReturnSink (vm, &s);
}

// string vtos(vector): 'x y z', each C's %f
static bool QC_Vtos (qcvm_t *vm)
{
	float		v[3];
	qc_sink_t	s;
	int			k;

	QC_ArgVector (vm, 0, v);
	QC_SinkInit (&s, SIZE_MAX);
	QC_SinkPush (&s, '\'');
	for (k = 0 ; k < 3 ; k++)
	{
		if (k)
			QC_SinkPush (&s, ' ');
		QC_FormatF (&s, (double)v[k], 6);
	}
	QC_SinkPush (&s, '\'');
	return QC_LibReturnSink (vm, &s);
}

// string etos(entity): "entity N"
static bool QC_Etos (qcvm_t *vm)
{
	char	text[32];

	snprintf (text, sizeof(text), "entity %d", QC_ArgInt (vm, 0));
	return QC_ReturnString (vm, text, strlen (text));
}

// string itos(int): signed decimal
static bool QC_Itos (qcvm_t *vm)
{
	char	text[16];

	snprintf (text, sizeof(text), "%d", QC_ArgInt (vm, 0));
	return QC_ReturnString (vm, text, strlen (text));
}

// string htos(int): eight lowercase hex digits
static bool QC_Htos (qcvm_t *vm)
{
	char	text[16];

	snprintf (text, sizeof(text), "%08x", QC_ArgWord (vm, 0));
	return QC_ReturnString (vm, text, strlen (text));
}

/*
==============================================================================

TEXT AS NUMBERS

==============================================================================
*/

// int stoi(string): C's strtol in base 0 (0x for hex, a leading 0 for octal), cut
// to 32 bits. FTE uses atoi (decimal only), though its documentation promises
// the prefixes; this follows the documentation.
static bool QC_Stoi (qcvm_t *vm)
{
	QC_ReturnInt (vm, (int32_t)QC_Strtol (QC_ArgString (vm, 0), 0, NULL));
	return true;
}

// int stoh(string): C's strtoul in base 16, cut to 32 bits
static bool QC_Stoh (qcvm_t *vm)
{
	QC_ReturnWord (vm, (uint32_t)QC_Strtoul (QC_ArgString (vm, 0), 16, NULL));
	return true;
}

// int ftoi(float): toward zero (x86: INT_MIN out of range or for NaN)
static bool QC_Ftoi (qcvm_t *vm)
{
	QC_ReturnInt (vm, QC_FloatToInt (QC_ArgFloat (vm, 0)));
	return true;
}

// __uint ftou(float): C's float to unsigned as x86-64 compilers make it
static bool QC_Ftou (qcvm_t *vm)
{
	QC_ReturnWord (vm, QC_F2U (QC_ArgFloat (vm, 0)));
	return true;
}

// itof's and utof's bit field: (value >> shift) & mask(count) (count 32: no mask;
// shift and count modulo 32, as x86 shifts)
static float QC_BitfieldToFloat (const qcvm_t *vm, uint32_t value)
{
	uint32_t	shift = QC_F2U (QC_ArgFloat (vm, 1));
	uint32_t	count = QC_Argc (vm) > 2 ? QC_F2U (QC_ArgFloat (vm, 2)) : 24;
	uint32_t	v = value >> (shift & 31);

	if (count != 32)
		v &= (1u << (count & 31)) - 1;
	return (float)v;
}

// float itof(int value, optional float shift, float count = 24): the integer, or
// with two arguments or more the bit field count bits wide at shift
static bool QC_Itof (qcvm_t *vm)
{
	QC_ReturnFloat (vm, QC_Argc (vm) > 1 ? QC_BitfieldToFloat (vm, QC_ArgWord (vm, 0)) : (float)QC_ArgInt (vm, 0));
	return true;
}

// float utof(__uint value, optional float shift, float count = 24)
static bool QC_Utof (qcvm_t *vm)
{
	QC_ReturnFloat (vm, QC_Argc (vm) > 1 ? QC_BitfieldToFloat (vm, QC_ArgWord (vm, 0)) : (float)QC_ArgWord (vm, 0));
	return true;
}

// float stof(string): C's atof, then rounded to float
static bool QC_Stof (qcvm_t *vm)
{
	size_t	used;

	QC_ReturnFloat (vm, (float)QC_Strtod (QC_ArgString (vm, 0), &used));
	return true;
}

// an __out int parameter: the bytes read, if the caller passed it
static void QC_SetUsed (qcvm_t *vm, int i, size_t used)
{
	if (QC_Argc (vm) > i)
		QC_SetArgWord (vm, i, (uint32_t)used);
}

// __double stod(string, optional __out int used): C's strtod, the bytes read in used
static bool QC_Stod (qcvm_t *vm)
{
	size_t	used;

	QC_LibReturnDouble (vm, QC_Strtod (QC_ArgString (vm, 0), &used));
	QC_SetUsed (vm, 1, used);
	return true;
}

// __int64 stol(string, int base, optional __out int used): C's 64-bit strtol
static bool QC_Stol (qcvm_t *vm)
{
	size_t	used;

	QC_LibReturn64 (vm, (uint64_t)QC_Strtol (QC_ArgString (vm, 0), (uint32_t)QC_ArgInt (vm, 1), &used));
	QC_SetUsed (vm, 2, used);
	return true;
}

// __uint64 stoul(string, int base, optional __out int used): C's 64-bit strtoul
static bool QC_Stoul (qcvm_t *vm)
{
	size_t	used;

	QC_LibReturn64 (vm, QC_Strtoul (QC_ArgString (vm, 0), (uint32_t)QC_ArgInt (vm, 1), &used));
	QC_SetUsed (vm, 2, used);
	return true;
}

void QC_ParseVector (const char *s, float out[3])
{
	size_t	i = s[0] == '\'', used;
	int		k;
	char	c;

	out[0] = out[1] = out[2] = 0;
	for (k = 0 ; k < 3 ; k++)
	{
		while (s[i] == ' ' || s[i] == '\t')
			i++;
		out[k] = (float)QC_Strtod (s + i, &used);
		c = s[i];
		if (out[k] == 0 && c != '-' && c != '+' && !(c >= '0' && c <= '9'))
			break;
		while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\'')
			i++;
		if (s[i] == '\'')
			break;
	}
}

// vector stov(string...): 'x y z', of the arguments concatenated
static bool QC_Stov (qcvm_t *vm)
{
	char	*s = QC_LibConcat (vm, 0, NULL);
	float	v[3];

	if (!s)
		return false;
	QC_ParseVector (s, v);
	free (s);
	QC_ReturnVector (vm, v);
	return true;
}

static const qc_libentry_t	qc_convert[] = {
	{"ftos", QC_Ftos, NULL, 0},
	{"vtos", QC_Vtos, NULL, 0},
	{"etos", QC_Etos, NULL, 0},
	{"itos", QC_Itos, NULL, 0},
	{"htos", QC_Htos, NULL, 0},
	{"stoi", QC_Stoi, NULL, 0},
	{"stoh", QC_Stoh, NULL, 0},
	{"ftoi", QC_Ftoi, NULL, 0},
	{"itof", QC_Itof, NULL, 0},
	{"ftou", QC_Ftou, NULL, 0},
	{"utof", QC_Utof, NULL, 0},
	{"stof", QC_Stof, NULL, 0},
	{"stod", QC_Stod, NULL, 0},
	{"stol", QC_Stol, NULL, 0},
	{"stoul", QC_Stoul, NULL, 0},
	{"stov", QC_Stov, NULL, 0},
};

bool QC_RegisterConvert (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_convert, sizeof(qc_convert) / sizeof(qc_convert[0]));
}
