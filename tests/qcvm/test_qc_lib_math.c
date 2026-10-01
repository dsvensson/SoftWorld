// test_qc_lib_math.c -- scalar maths and random numbers (qcvm-rs's
// tests/all/builtins_misc/math.rs)

#include "qc_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char	*extra[] = {"anglesub", NULL};

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, QH_Named, (void *)extra);
}

static uint32_t Bits (float f)
{
	uint32_t	u;

	memcpy (&u, &f, 4);
	return u;
}

static void TestTranscendental (void)
{
	static const struct
	{
		const char	*name;
		float		x;
		double		(*f) (double);
	} cases[] = {{"sin", 1, sin}, {"cos", 1, cos}, {"tan", 0.5f, tan}, {"asin", 0.5f, asin},
		{"acos", 0.5f, acos}, {"atan", 1, atan}, {"sqrt", 2, sqrt}};
	qh_t	*h = Harness ();
	size_t	i;

	// double precision on the float argument, the result rounded to float
	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_U (Bits (QH_Float (h, cases[i].name, ARGS (F (cases[i].x)))), Bits ((float)cases[i].f (cases[i].x))))
			printf ("  %s(%g)\n", cases[i].name, (double)cases[i].x);
	QT_EQ_F (QH_Float (h, "sin", ARGS (F (0))), 0);
	QT_EQ_F (QH_Float (h, "sqrt", ARGS (F (16))), 4);
	QT_CHECK (QT_IsNan (QH_Float (h, "sqrt", ARGS (F (-1)))));
	QT_CHECK (QT_IsNan (QH_Float (h, "asin", ARGS (F (2)))));
	QT_EQ_F (QH_Float (h, "atan2", ARGS (F (1), F (1))), (float)(3.14159265358979323846 / 4));
	QT_EQ_F (QH_Float (h, "atan2", ARGS (F (1), F (0))), (float)(3.14159265358979323846 / 2));
	QT_EQ_F (QH_Float (h, "pow", ARGS (F (2), F (10))), 1024);
	QT_EQ_F (QH_Float (h, "pow", ARGS (F (9), F (0.5f))), 3);
	QH_Free (h);
}

static void TestRounding (void)
{
	static const float	rints[][2] = {{2.5f, 3}, {-2.5f, -3}, {2.4f, 2}, {-2.6f, -3}, {0, 0}};
	qh_t	*h = Harness ();
	size_t	i;

	QT_EQ_F (QH_Float (h, "floor", ARGS (F (-1.5f))), -2);
	QT_EQ_F (QH_Float (h, "floor", ARGS (F (1.5f))), 1);
	QT_EQ_F (QH_Float (h, "ceil", ARGS (F (-1.5f))), -1);
	QT_EQ_F (QH_Float (h, "ceil", ARGS (F (1.2f))), 2);
	QT_EQ_F (QH_Float (h, "fabs", ARGS (F (-3.25f))), 3.25);
	QT_EQ_U (Bits (QH_Float (h, "fabs", ARGS (F (-0.0f)))), 0);
	// rint rounds half away from zero
	for (i = 0 ; i < sizeof(rints) / sizeof(rints[0]) ; i++)
		QT_EQ_F (QH_Float (h, "rint", ARGS (F (rints[i][0]))), rints[i][1]);
	// x86 overflow: out-of-range conversions give INT_MIN
	QT_EQ_F (QH_Float (h, "rint", ARGS (F (3e9f))), -2147483648.0);
	QT_EQ_F (QH_Float (h, "rint", ARGS (F (NAN))), -2147483648.0);
	QH_Free (h);
}

static void TestBoundMinMax (void)
{
	qh_t		*h = Harness ();
	qc_value_t	eight[8];
	int			i;

	QT_EQ_F (QH_Float (h, "bound", ARGS (F (0), F (5), F (10))), 5);
	QT_EQ_F (QH_Float (h, "bound", ARGS (F (0), F (-1), F (10))), 0);
	QT_EQ_F (QH_Float (h, "bound", ARGS (F (0), F (11), F (10))), 10);
	// the maximum wins over the minimum, and a NaN value passes through
	QT_EQ_F (QH_Float (h, "bound", ARGS (F (10), F (5), F (0))), 0);
	QT_CHECK (QT_IsNan (QH_Float (h, "bound", ARGS (F (0), F (NAN), F (10)))));

	QT_EQ_F (QH_Float (h, "min", ARGS (F (3), F (2))), 2);
	QT_EQ_F (QH_Float (h, "max", ARGS (F (3), F (2))), 3);
	QT_EQ_F (QH_Float (h, "min", ARGS (F (3), F (2), F (7), F (-1), F (4))), -1);
	QT_EQ_F (QH_Float (h, "max", ARGS (F (3), F (2), F (7), F (-1), F (4))), 7);
	for (i = 0 ; i < 8 ; i++)
		eight[i] = F ((float)(i + 1));
	QT_EQ_F (QH_Float (h, "max", 8, eight), 8);
	QT_EQ_F (QH_Float (h, "min", 8, eight), 1);
	// NaNs and ties follow the comparisons: a < b ? a : b for two arguments, and
	// only strictly smaller values replace the first for more
	QT_EQ_F (QH_Float (h, "min", ARGS (F (NAN), F (1))), 1);
	QT_CHECK (QT_IsNan (QH_Float (h, "min", ARGS (F (1), F (NAN)))));
	QT_CHECK (QT_IsNan (QH_Float (h, "min", ARGS (F (NAN), F (1), F (2)))));
	QT_EQ_U (Bits (QH_Float (h, "min", ARGS (F (-0.0f), F (0)))), 0);
	QT_EQ_U (Bits (QH_Float (h, "min", ARGS (F (-0.0f), F (0), F (1)))), Bits (-0.0f));
	QT_EQ_F (QH_Float (h, "max", ARGS (F (NAN), F (1))), 1);
	// fewer than two arguments is a builtin error
	QT_EQ_I (QH_Fails (h, "min", ARGS (F (1))), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "at least 2"));
	QT_EQ_I (QH_Fails (h, "max", NOARGS), QC_ERR_BUILTIN);
	QH_Free (h);
}

static void TestModulo (void)
{
	qh_t	*h = Harness (), *ssqc;

	QT_EQ_F (QH_Float (h, "mod", ARGS (F (7), F (3))), 1);
	QT_EQ_F (QH_Float (h, "mod", ARGS (F (-7), F (3))), -1);
	QT_EQ_F (QH_Float (h, "mod", ARGS (F (7), F (-3))), 1);
	QT_EQ_F (QH_Float (h, "mod", ARGS (F (5.5f), F (2))), 1.5);
	QT_EQ_I (QH_NumWarnings (h), 0);
	QT_EQ_F (QH_Float (h, "mod", ARGS (F (1), F (0))), 0);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_S (QH_WarningText (h, 0), "mod by zero");
	QH_Free (h);

	// SSQC's name for it
	ssqc = QH_New (QC_NUMBERING_SSQC, NULL, NULL, NULL);
	QT_EQ_F (QH_Float (ssqc, "modulo", ARGS (F (-7), F (3))), -1);
	QH_Free (ssqc);
}

static void TestBitshift (void)
{
	qh_t	*h = Harness ();

	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (1), F (4))), 16);
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (256), F (-4))), 16);
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (-16), F (-2))), -4);		// right shifts are arithmetic
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (3.9f), F (1.9f))), 6);		// the operands are truncated
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (1), F (31))), -2147483648.0);
	// counts of 32 or more shift everything out, not masked as x86 does
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (1), F (32))), 0);
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (-1), F (-40))), -1);
	QT_EQ_F (QH_Float (h, "bitshift", ARGS (F (5), F (-40))), 0);
	QH_Free (h);
}

static void TestLogarithms (void)
{
	qh_t	*h = Harness ();

	QT_EQ_F (QH_Float (h, "log", ARGS (F (1))), 0);
	QT_EQ_F (QH_Float (h, "log", ARGS (F (1), F (2))), 0);
	QT_EQ_F (QH_Float (h, "log", ARGS (F (8), F (2))), 3);
	QT_EQ_F (QH_Float (h, "log", ARGS (F (1000), F (10))), 3);
	QT_EQ_F (QH_Float (h, "logarithm", ARGS (F (8), F (2))), 3);
	QT_EQ_F (QH_Float (h, "log", ARGS (F (0))), -INFINITY);
	QH_Free (h);
}

static void TestAnglemod (void)
{
	static const float	cases[][2] = {{370, 10}, {-10, 350}, {360, 0}, {720, 0}, {-720, 0}, {359.5f, 359.5f},
		{-0.25f, 359.75f}, {1000.25f, 280.25f}};
	qh_t	*h = Harness ();
	float	r;
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_U (Bits (QH_Float (h, "anglemod", ARGS (F (cases[i][0])))), Bits (cases[i][1])))
			printf ("  anglemod(%g)\n", (double)cases[i][0]);
	QT_EQ_U (Bits (QH_Float (h, "anglemod", ARGS (F (-0.0f)))), Bits (-0.0f));
	QT_CHECK (QT_IsNan (QH_Float (h, "anglemod", ARGS (F (NAN)))));
	// FTE loops forever on huge values; the remainder is exact
	r = QH_Float (h, "anglemod", ARGS (F (1e10f)));
	QT_CHECK (r >= 0 && r < 360);
	QT_EQ_F (r, fmodf (1e10f, 360));
	QH_Free (h);
}

static void TestAnglesub (void)
{
	static const float	cases[][3] = {{10, 350, 20}, {350, 10, -20}, {180, 0, 180}, {0, 180, -180},
		{540, 0, 180}, {-540, 0, -180}, {90, 0, 90}, {-360, 0, 0}};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_U (Bits (QH_Float (h, "anglesub", ARGS (F (cases[i][0]), F (cases[i][1])))), Bits (cases[i][2])))
			printf ("  anglesub(%g, %g)\n", (double)cases[i][0], (double)cases[i][1]);
	QH_Free (h);
}

static void TestRandom (void)
{
	qh_t			*h = Harness (), *a, *b;
	qc_config_t		config;
	float			r, k;
	int				i;
	uint32_t		drawsa[16], drawsb[16];
	uint64_t		seed;
	bool			same;

	for (i = 0 ; i < 2000 ; i++)
	{
		r = QH_Float (h, "random", NOARGS);
		if (!QT_CHECK (r > 0 && r < 1))
			break;
		// (k + 0.5) / 32768 for a 15-bit k
		k = r * 32768.0f - 0.5f;
		QT_CHECK (k == floorf (k) && k >= 0 && k <= 32767);
		r = QH_Float (h, "random", ARGS (F (10)));
		QT_CHECK (r > 0 && r < 10);
		r = QH_Float (h, "random", ARGS (F (5), F (6)));
		QT_CHECK (r > 5 && r < 6);
	}
	// a reversed range still interpolates
	r = QH_Float (h, "random", ARGS (F (6), F (5)));
	QT_CHECK (r > 5 && r < 6);
	QH_Free (h);

	// seeded: the same seed draws the same numbers
	for (seed = 1 ; seed <= 2 ; seed++)
	{
		QC_DefaultConfig (&config, QC_CSQC);
		config.seed = 1;
		a = QH_New (QC_NUMBERING_CSQC, &config, NULL, NULL);
		config.seed = seed;
		b = QH_New (QC_NUMBERING_CSQC, &config, NULL, NULL);
		for (i = 0 ; i < 16 ; i++)
		{
			drawsa[i] = Bits (QH_Float (a, "random", NOARGS));
			drawsb[i] = Bits (QH_Float (b, "random", NOARGS));
		}
		same = !memcmp (drawsa, drawsb, sizeof(drawsa));
		QT_CHECK (same == (seed == 1));
		QH_Free (a);
		QH_Free (b);
	}
}

static void TestRandomvec (void)
{
	qh_t	*h = Harness ();
	float	v[3];
	double	k;
	int		i, c;

	for (i = 0 ; i < 1000 ; i++)
	{
		QH_Vector (h, v, "randomvec", NOARGS);
		if (!QT_CHECK (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 1))
			break;
		for (c = 0 ; c < 3 ; c++)
		{
			// k / 32767 * 2 - 1 for a 15-bit k
			k = round (((double)v[c] + 1.0) * 32767.0 / 2.0);
			QT_EQ_F ((float)(k * (2.0 / 32767.0) - 1.0), v[c]);
		}
	}
	QH_Free (h);
}

int main (void)
{
	TestTranscendental ();
	TestRounding ();
	TestBoundMinMax ();
	TestModulo ();
	TestBitshift ();
	TestLogarithms ();
	TestAnglemod ();
	TestAnglesub ();
	TestRandom ();
	TestRandomvec ();
	return QT_Finish ("lib_math", "the maths builtins give FTE's results");
}
