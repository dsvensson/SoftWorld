// qc_lib_math.c -- scalar maths and random numbers (docs/spec/builtins.md)
//
// As in FTE's C code, the transcendental functions work in double precision
// on the float argument and round the result to float.

#include "qc_lib.h"

#include <math.h>

static bool QC_Sin (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)sin ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Cos (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)cos ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Tan (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)tan ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Asin (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)asin ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Acos (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)acos ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Atan (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)atan ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Sqrt (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)sqrt ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Floor (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)floor ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Ceil (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)ceil ((double)QC_ArgFloat (vm, 0)));
	return true;
}

static bool QC_Fabs (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)fabs ((double)QC_ArgFloat (vm, 0)));
	return true;
}

// float atan2(float y, float x), in radians
static bool QC_Atan2 (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)atan2 ((double)QC_ArgFloat (vm, 0), (double)QC_ArgFloat (vm, 1)));
	return true;
}

static bool QC_Pow (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)pow ((double)QC_ArgFloat (vm, 0), (double)QC_ArgFloat (vm, 1)));
	return true;
}

// float rint(float): half away from zero, (int)(f +- 0.5) with x86's overflow
static bool QC_Rint (qcvm_t *vm)
{
	double	f = (double)QC_ArgFloat (vm, 0);

	QC_ReturnFloat (vm, (float)QC_DoubleToInt (f > 0 ? f + 0.5 : f - 0.5));
	return true;
}

// float bound(float min, float value, float max): max wins over min, and a NaN
// value passes through
static bool QC_Bound (qcvm_t *vm)
{
	float	lo = QC_ArgFloat (vm, 0), v = QC_ArgFloat (vm, 1), hi = QC_ArgFloat (vm, 2);

	QC_ReturnFloat (vm, v > hi ? hi : v < lo ? lo : v);
	return true;
}

// min and max: of two, FTE's macros (a < b ? a : b); of more, only a strictly
// better value replaces the first
static bool QC_Extreme (qcvm_t *vm, const char *name, bool max)
{
	int		argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, i;
	float	first, second, r, v;

	if (argc < 2)
		return QC_Error (vm, "%s: must supply at least 2 floats", name);
	first = QC_ArgFloat (vm, 0);
	second = QC_ArgFloat (vm, 1);
	if (argc == 2)
	{
		QC_ReturnFloat (vm, (max ? first > second : first < second) ? first : second);
		return true;
	}
	for (r = first, i = 1 ; i < argc ; i++)
	{
		v = QC_ArgFloat (vm, i);
		if (max ? v > r : v < r)
			r = v;
	}
	QC_ReturnFloat (vm, r);
	return true;
}

static bool QC_Min (qcvm_t *vm)
{
	return QC_Extreme (vm, "min", false);
}

static bool QC_Max (qcvm_t *vm)
{
	return QC_Extreme (vm, "max", true);
}

// float mod(float a, float n): a - n * (int)(a / n), the sign of a
static bool QC_Mod (qcvm_t *vm)
{
	float	a = QC_ArgFloat (vm, 0), n = QC_ArgFloat (vm, 1);

	if (n == 0)
	{
		QC_Warning (vm, "mod by zero");
		QC_ReturnFloat (vm, 0);
	}
	else
		QC_ReturnFloat (vm, a - n * (float)QC_FloatToInt (a / n));
	return true;
}

// float bitshift(float n, float count): n << count, or an arithmetic n >> -count;
// counts of 32 or more shift everything out
static bool QC_Bitshift (qcvm_t *vm)
{
	int32_t	n = QC_LibArgInt (vm, 0), q = QC_LibArgInt (vm, 1), r;
	uint32_t	count = q >= 0 ? (uint32_t)q : 0u - (uint32_t)q;

	if (q >= 0)
		r = count < 32 ? (int32_t)((uint32_t)n << count) : 0;
	else
		r = count < 32 ? n >> count : n < 0 ? -1 : 0;
	QC_ReturnFloat (vm, (float)r);
	return true;
}

// float log(float v, optional float base)
static bool QC_Log (qcvm_t *vm)
{
	double	r = log ((double)QC_ArgFloat (vm, 0));

	if (QC_Argc (vm) > 1)
		r /= log ((double)QC_ArgFloat (vm, 1));
	QC_ReturnFloat (vm, (float)r);
	return true;
}

// float anglemod(float): into [0, 360), by the exact remainder where FTE loops
// (forever, for huge values)
static bool QC_Anglemod (qcvm_t *vm)
{
	float	v = QC_ArgFloat (vm, 0), r;

	if (v >= 360)
		r = fmodf (v, 360);
	else if (v < 0)
	{
		r = fmodf (v, 360);
		r = r < 0 ? r + 360 : 0;
	}
	else
		r = v;
	QC_ReturnFloat (vm, r);
	return true;
}

// float anglesub(float a, float b): a - b into [-180, 180]
static bool QC_Anglesub (qcvm_t *vm)
{
	float	v = QC_ArgFloat (vm, 0) - QC_ArgFloat (vm, 1), r;

	if (v > 180)
	{
		r = fmodf (v, 360);
		if (r > 180)
			r -= 360;
	}
	else if (v < -180)
	{
		r = fmodf (v, 360);
		if (r < -180)
			r += 360;
		else if (r == 0)
			r = 0;
	}
	else
		r = v;
	QC_ReturnFloat (vm, r);
	return true;
}

// float random(optional float max) / random(float min, float max): strictly
// inside (0, 1), (k + 0.5) / 32768 for a 15-bit k, then scaled
static bool QC_Random (qcvm_t *vm)
{
	float	r = ((float)QC_Rand15 (vm) + 0.5f) / 32768.0f, lo, hi;

	if (QC_Argc (vm) == 1)
		r *= QC_ArgFloat (vm, 0);
	else if (QC_Argc (vm) > 1)
	{
		lo = QC_ArgFloat (vm, 0);
		hi = QC_ArgFloat (vm, 1);
		r = lo + r * (hi - lo);
	}
	QC_ReturnFloat (vm, r);
	return true;
}

// vector randomvec(): strictly inside the unit sphere, components k / 32767 *
// 2 - 1 for 15-bit draws, drawn again until the length is below 1
static bool QC_Randomvec (qcvm_t *vm)
{
	float	v[3];
	int		k;

	for ( ; ; )
	{
		for (k = 0 ; k < 3 ; k++)
			v[k] = (float)((double)QC_Rand15 (vm) * (2.0 / 32767.0) - 1.0);
		if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 1)
			break;
	}
	QC_ReturnVector (vm, v);
	return true;
}

static const qc_libentry_t	qc_math[] = {
	{"sin", QC_Sin, NULL, 0},
	{"cos", QC_Cos, NULL, 0},
	{"tan", QC_Tan, NULL, 0},
	{"asin", QC_Asin, NULL, 0},
	{"acos", QC_Acos, NULL, 0},
	{"atan", QC_Atan, NULL, 0},
	{"atan2", QC_Atan2, NULL, 0},
	{"sqrt", QC_Sqrt, NULL, 0},
	{"pow", QC_Pow, NULL, 0},
	{"floor", QC_Floor, NULL, 0},
	{"ceil", QC_Ceil, NULL, 0},
	{"fabs", QC_Fabs, NULL, 0},
	{"rint", QC_Rint, NULL, 0},
	{"bound", QC_Bound, NULL, 0},
	{"min", QC_Min, NULL, 0},
	{"max", QC_Max, NULL, 0},
	{"mod", QC_Mod, NULL, 0},
	{"modulo", NULL, "mod", 0},
	{"bitshift", QC_Bitshift, NULL, 0},
	{"log", QC_Log, NULL, 0},
	{"logarithm", NULL, "log", 0},
	{"anglemod", QC_Anglemod, NULL, 0},
	{"anglesub", QC_Anglesub, NULL, 0},
	{"random", QC_Random, NULL, 0},
	{"randomvec", QC_Randomvec, NULL, 0},
};

bool QC_RegisterMath (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_math, sizeof(qc_math) / sizeof(qc_math[0]));
}
