// test_qc_lib_convert.c -- numbers as text and back (qcvm-rs's
// tests/all/builtins_strings/convert.rs, docs/spec/strings.md)

#include "qc_harness.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

// builtins FTE binds by name that its CSQC declarations don't list
static const char	*extra[] = {"argc", "instr", "ftou", "utof", "strcmp", NULL};

// CSQC with QuakeWorld's charset defaults (utf8_enable 0, the Quake scheme)
static qh_t *Harness (void)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.utf8 = false;
	config.charscheme = QC_CHARS_QUAKE;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, extra);
}

// ftos prints the float's exact value to FTE's digits
static void TestFtos (void)
{
	static const struct
	{
		float		x;
		const char	*want;
	} cases[] = {
		{0.5f, "0.5"},
		{0.1f, "0.100000001"},
		{0.2f, "0.2"},
		{0.3f, "0.30000001"},
		{1.1f, "1.10000002"},
		{1.0f / 3.0f, "0.33333334"},
		{2.0f / 3.0f, "0.66666669"},
		{0.01f, "0.0099999998"},
		{0.001f, "0.00100000005"},
		{0.00001f, "0.0000099999997"},
		{(float)3.14159265358979323846, "3.14159274"},
		{10.1f, "10.10000038"},
		{16.1f, "16.1000004"},
		{99.99f, "99.9899979"},
		{123.456f, "123.4560013"},
		{1000.5f, "1000.5"},
		{123456.7f, "123456.7031"},
		{1048576.1f, "1048576.12"},		// = 1048576.125
		{(float)16777217, "16777216"},
		{-2147483648.0f, "-2147483648"},
		{2147483648.0f, "2147483648"},
		{3e9f, "3000000000"},
		{1e20f, "100000002004087734272"},
		{FLT_TRUE_MIN, "0.0000000000000000000000000000000000000000000014"},
		{INFINITY, "1.#INF"},
		{-INFINITY, "-1.#INF"},
		{-42, "-42"},
		{0, "0"},
		{-0.0f, "0"},
		{-0.5f, "-0.5"},
		{1e-10f, "0.000000000100000001"},
	};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_S (QH_String (h, "ftos", ARGS (F (cases[i].x))), cases[i].want))
			printf ("  ftos(%.9g)\n", (double)cases[i].x);
	// x86's 0/0 is a NaN with the sign bit set; the sign is shown
	QT_EQ_S (QH_String (h, "ftos", ARGS (F (QC_BitsFloat (0xFFC00000)))), "-1.#NAN");
	QT_EQ_S (QH_String (h, "ftos", ARGS (F (NAN))), "1.#NAN");
	QH_Free (h);
}

// vtos uses C's %f
static void TestVtos (void)
{
	qh_t	*h = Harness ();
	float	nan = QC_BitsFloat (0xFFC00000);

	QT_EQ_S (QH_String (h, "vtos", ARGS (V (1, 2, 3))), "'1.000000 2.000000 3.000000'");
	QT_EQ_S (QH_String (h, "vtos", ARGS (V (0.1f, -0.0f, 1e10f))), "'0.100000 -0.000000 10000000000.000000'");
	QT_EQ_S (QH_String (h, "vtos", ARGS (V (NAN, nan, -INFINITY))), "'nan -nan -inf'");
	QT_EQ_S (QH_String (h, "vtos", ARGS (V (0.0000005f, 0.0000015f, 2.5e-7f))), "'0.000000 0.000002 0.000000'");
	QH_Free (h);
}

static void TestEtosItosHtos (void)
{
	qh_t	*h = Harness ();

	QT_EQ_S (QH_String (h, "etos", ARGS (W (5))), "entity 5");
	QT_EQ_S (QH_String (h, "etos", ARGS (W (0))), "entity 0");
	QT_EQ_S (QH_String (h, "itos", ARGS (I (255))), "255");
	QT_EQ_S (QH_String (h, "itos", ARGS (I (INT32_MIN))), "-2147483648");
	QT_EQ_S (QH_String (h, "htos", ARGS (I (255))), "000000ff");
	QT_EQ_S (QH_String (h, "htos", ARGS (I (-1))), "ffffffff");
	QT_EQ_S (QH_String (h, "htos", ARGS (I (0x1234ABCD))), "1234abcd");
	QH_Free (h);
}

// stoi reads the base from the prefix: FTE's documentation promises base 8, 10
// or 16 by prefix (FTE itself only reads decimal)
static void TestStoi (void)
{
	static const struct
	{
		const char	*text;
		int32_t		want;
	} cases[] = {
		{"  -12abc", -12},
		{"0x1f", 31},
		{"0X1F", 31},
		{"-0x10", -16},
		{"010", 8},
		{"08", 0},
		{"0x", 0},
		{"abc", 0},
		{"", 0},
		{"+7", 7},
		{"\t\n 42", 42},
		{"4294967297", 1},
		{"2147483648", INT32_MIN},
		{"99999999999999999999", -1},
	};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_I (QH_Int (h, "stoi", ARGS (QH_S (h, cases[i].text))), cases[i].want))
			printf ("  stoi(\"%s\")\n", cases[i].text);
	QH_Free (h);
}

// stoh reads hex
static void TestStoh (void)
{
	static const struct
	{
		const char	*text;
		int32_t		want;
	} cases[] = {{"1F", 31}, {"0x1f", 31}, {"10", 16}, {"-1", -1}, {"ffffffff", -1}, {"zz", 0}, {" 7g", 7}};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_I (QH_Int (h, "stoh", ARGS (QH_S (h, cases[i].text))), cases[i].want))
			printf ("  stoh(\"%s\")\n", cases[i].text);
	QH_Free (h);
}

static void TestFtoiItof (void)
{
	qh_t	*h = Harness ();

	QT_EQ_I (QH_Int (h, "ftoi", ARGS (F (3.9f))), 3);
	QT_EQ_I (QH_Int (h, "ftoi", ARGS (F (-3.9f))), -3);
	QT_EQ_I (QH_Int (h, "ftoi", ARGS (F (NAN))), INT32_MIN);
	QT_EQ_I (QH_Int (h, "ftoi", ARGS (F (3e9f))), INT32_MIN);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (5))), 5);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (-7))), -7);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (16777217))), 16777216);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (16777219))), 16777220);
	// bit fields: (value >> shift) & mask(count), count 24 by default
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (0x12345678), F (8), F (8))), 86);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (-1), F (0), F (32))), 4294967296.0);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (-1), F (4))), 16777215);
	// shift and count are taken modulo 32, as x86 shifts
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (0x30), F (36), F (33))), 1);
	QT_EQ_F (QH_Float (h, "itof", ARGS (I (0x30), F (4), F (0))), 0);
	QH_Free (h);
}

static void TestFtouUtof (void)
{
	qh_t	*h = Harness ();

	QT_EQ_U (QH_Word (h, "ftou", ARGS (F (-1))), UINT32_MAX);
	QT_EQ_U (QH_Word (h, "ftou", ARGS (F (4294967040.0f))), 4294967040u);
	QT_EQ_U (QH_Word (h, "ftou", ARGS (F (3.7f))), 3);
	QT_EQ_F (QH_Float (h, "utof", ARGS (I (-1))), 4294967296.0);
	QT_EQ_F (QH_Float (h, "utof", ARGS (I (0xF0), F (4), F (2))), 3);
	QH_Free (h);
}

// stof is C's atof
static void TestStof (void)
{
	static const struct
	{
		const char	*text;
		float		want;
	} cases[] = {
		{"  12.5xyz", 12.5f},
		{".5", 0.5f},
		{"1e3", 1000},
		{"1,5", 1},
		{"\"5\"", 0},
		{"1e39", INFINITY},
		{"-1e39", -INFINITY},
		{"0x10", 16},
		{"0x1p4", 16},
		{"0x1.8", 1.5f},
		{"inf", INFINITY},
		{"-Infinity", -INFINITY},
		{"\x0b\x0c\r3", 3},
		{"abc", 0},
		{"", 0},
		{"1e-50", 0},
		{"0.1", 0.1f},
		{"16777217", 16777216},
		{"3.4028235e38", FLT_MAX},
	};
	qh_t	*h = Harness ();
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_U (QC_FloatBits (QH_Float (h, "stof", ARGS (QH_S (h, cases[i].text)))),
			QC_FloatBits (cases[i].want)))
			printf ("  stof(\"%s\")\n", cases[i].text);
	QT_CHECK (isnan (QH_Float (h, "stof", ARGS (QH_S (h, "nan")))));
	QT_CHECK (isnan (QH_Float (h, "stof", ARGS (QH_S (h, "NaN(123)")))));
	QT_CHECK (signbit (QH_Float (h, "stof", ARGS (QH_S (h, "-nan")))));
	// double rounding as C does it: the decimal to double (1 + 2^-24, a float
	// tie), then to float (ties to even), not straight to float (which rounds up)
	QT_EQ_F (QH_Float (h, "stof", ARGS (QH_S (h, "1.00000005960464477539062500001"))), 1);
	QH_Free (h);
}

static void TestStov (void)
{
	static const struct
	{
		const char	*text;
		float		want[3];
	} cases[] = {
		{"'1 2 3'", {1, 2, 3}},
		{"1\t2\t3", {1, 2, 3}},
		{"'1 2'", {1, 2, 0}},
		{"1,2,3", {1, 0, 0}},
		{"1\n2\n3", {1, 0, 0}},
		{"1-2 3", {1, 3, 0}},
		{"0 1 2", {0, 1, 2}},
		{".0 1 2", {0, 0, 0}},
		{" '1 2 3'", {0, 0, 0}},
		{"(1 2 3)", {0, 0, 0}},
		{"1e3 2 3", {1000, 2, 3}},
		{"-0 5 6", {-0.0f, 5, 6}},
		{"'1 2 3' 4", {1, 2, 3}},
		{"1 2 3 4", {1, 2, 3}},
		{"", {0, 0, 0}},
	};
	qh_t	*h = Harness ();
	float	v[3];
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
	{
		QH_Vector (h, v, "stov", ARGS (QH_S (h, cases[i].text)));
		if (!QT_CHECK (v[0] == cases[i].want[0] && v[1] == cases[i].want[1] && v[2] == cases[i].want[2]))
			printf ("  stov(\"%s\") = %g %g %g\n", cases[i].text, (double)v[0], (double)v[1], (double)v[2]);
	}
	QH_Vector (h, v, "stov", ARGS (QH_S (h, "nan 1 2")));
	QT_CHECK (isnan (v[0]) && v[1] == 1 && v[2] == 2);
	// the arguments are concatenated
	QH_Vector (h, v, "stov", ARGS (QH_S (h, "'1 "), QH_S (h, "2 "), QH_S (h, "3'")));
	QT_CHECK (v[0] == 1 && v[1] == 2 && v[2] == 3);
	QH_Free (h);
}

int main (void)
{
	TestFtos ();
	TestVtos ();
	TestEtosItosHtos ();
	TestStoi ();
	TestStoh ();
	TestFtoiItof ();
	TestFtouUtof ();
	TestStof ();
	TestStov ();
	return QT_Finish ("lib_convert", "numbers become text and text numbers as FTE makes them");
}
