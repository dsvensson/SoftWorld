// test_qc_lib_dtoa.c -- numbers as text and back, as C's printf and strtod make
// them (qcvm-rs's unit tests of stdlib/format.rs, convert.rs and mod.rs)

#include "qc_harness.h"
#include "qc_lib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the text of one conversion
static const char *Fmt (double v, char conv, qc_spec_t spec)
{
	static char	out[4096];
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_FormatFloat (&s, v, conv, &spec);
	snprintf (out, sizeof(out), "%s", QC_SinkText (&s));
	QC_SinkFree (&s);
	return out;
}

static qc_spec_t Prec (size_t prec)
{
	return (qc_spec_t){.has_prec = true, .prec = prec};
}

// %f is exact, rounded half to even: glibc's and Apple's printf are too
static void TestFixed (void)
{
	static const double	values[] = {0.0, 0.1, 0.5, 1.5, 2.5, 0.125, 123.456, 1e-7, 5e-324, 1.7976931348623157e308};
	static const size_t	precs[] = {0, 1, 2, 3, 6, 10, 20};
	char				want[1024];
	size_t				i, j;
	int					sign;
	double				v;

	for (i = 0 ; i < sizeof(values) / sizeof(values[0]) ; i++)
		for (j = 0 ; j < sizeof(precs) / sizeof(precs[0]) ; j++)
			for (sign = 0 ; sign < 2 ; sign++)
			{
				v = sign ? -values[i] : values[i];
				snprintf (want, sizeof(want), "%.*f", (int)precs[j], v);
#ifndef _WIN32
				if (!QT_EQ_S (Fmt (v, 'f', Prec (precs[j])), want))
					printf ("  %.17g with %zu decimals\n", v, precs[j]);
#endif
			}
	QT_EQ_S (Fmt (0.1, 'f', Prec (20)), "0.10000000000000000555");
	QT_EQ_S (Fmt (-0.0, 'f', Prec (6)), "-0.000000");
	QT_EQ_S (Fmt (123.456, 'f', Prec (1)), "123.5");
}

static void TestScientificAndGeneral (void)
{
	qc_spec_t	def = {0};

	QT_EQ_S (Fmt (12345.0, 'e', def), "1.234500e+04");
	QT_EQ_S (Fmt (0.0, 'e', Prec (2)), "0.00e+00");
	QT_EQ_S (Fmt (9.99, 'e', Prec (1)), "1.0e+01");
	QT_EQ_S (Fmt (1e-300, 'E', Prec (3)), "1.000E-300");
	QT_EQ_S (Fmt (0.1, 'g', def), "0.1");
	QT_EQ_S (Fmt (1e20, 'g', def), "1e+20");
	QT_EQ_S (Fmt (100000.0, 'g', def), "100000");
	QT_EQ_S (Fmt (1000000.0, 'g', def), "1e+06");
	QT_EQ_S (Fmt (0.0001, 'g', def), "0.0001");
	QT_EQ_S (Fmt (0.00001, 'g', def), "1e-05");
	QT_EQ_S (Fmt (9.9999995, 'g', def), "10");
	QT_EQ_S (Fmt (0.0, 'g', def), "0");
	QT_EQ_S (Fmt (1.0, 'g', (qc_spec_t){.alt = true}), "1.00000");
	QT_EQ_S (Fmt (NAN, 'f', def), "nan");
	QT_EQ_S (Fmt (-NAN, 'f', def), "-nan");
	QT_EQ_S (Fmt (-INFINITY, 'E', def), "-INF");
	QT_EQ_S (Fmt (INFINITY, 'f', (qc_spec_t){.width = 5, .zero = true}), "  inf");
	QT_EQ_S (Fmt (-1.5, 'f', (qc_spec_t){.width = 6, .zero = true, .has_prec = true, .prec = 2}), "-01.50");
}

static void TestTiesToEven (void)
{
	QT_EQ_S (Fmt (0.5, 'f', Prec (0)), "0");
	QT_EQ_S (Fmt (1.5, 'f', Prec (0)), "2");
	QT_EQ_S (Fmt (2.5, 'f', Prec (0)), "2");
	QT_EQ_S (Fmt (0.125, 'f', Prec (2)), "0.12");
	QT_EQ_S (Fmt (0.375, 'f', Prec (2)), "0.38");
	QT_EQ_S (Fmt (1048576.125, 'f', Prec (2)), "1048576.12");
}

static void TestIntegers (void)
{
	qc_sink_t	s;

	QC_SinkInit (&s, 100);
	QC_FormatInt (&s, false, 255, 16, false, false, &(qc_spec_t){.alt = true});
	QC_FormatInt (&s, false, 0, 10, false, false, &(qc_spec_t){.has_prec = true, .prec = 0});
	QC_SinkPush (&s, '|');
	QC_FormatInt (&s, true, 42, 10, false, true, &(qc_spec_t){.width = 6, .zero = true});
	QC_SinkPush (&s, '|');
	QC_FormatInt (&s, false, 8, 8, false, false, &(qc_spec_t){.alt = true});
	QC_SinkPush (&s, '|');
	QC_FormatInt (&s, false, 7, 10, false, true, &(qc_spec_t){.has_prec = true, .prec = 3, .width = 5});
	QT_EQ_S (QC_SinkText (&s), "0xff|-00042|010|  007");
	QC_SinkFree (&s);
}

static void TestSinkCap (void)
{
	qc_sink_t	s;

	QC_SinkInit (&s, 4);
	QC_FormatInt (&s, false, 1, 10, false, false, &(qc_spec_t){.width = (size_t)1 << 40});
	QT_EQ_S (QC_SinkText (&s), "    ");
	QC_SinkFree (&s);
}

// digits past a double's exact expansion are zeros, filled without formatting them
static void TestHugePrecisions (void)
{
	qc_sink_t	s;
	size_t		i;
	bool		zeros;
	double		tiny;
	uint64_t	one = 1;

	QC_SinkInit (&s, SIZE_MAX);
	QC_FormatF (&s, 0.5, 3000);
	QT_CHECK (s.len == 3002 && !strncmp (s.buf, "0.5", 3));
	for (zeros = true, i = 3 ; i < s.len ; i++)
		zeros &= s.buf[i] == '0';
	QT_CHECK (zeros);
	QC_SinkFree (&s);

	// 2^-1074 has exactly 1074 fraction digits, the last a 5
	memcpy (&tiny, &one, 8);
	QC_SinkInit (&s, SIZE_MAX);
	QC_FormatF (&s, tiny, 1100);
	QT_CHECK (s.len == 1102 && s.buf[1075] == '5');
	for (zeros = true, i = 1076 ; i < s.len ; i++)
		zeros &= s.buf[i] == '0';
	QT_CHECK (zeros);
	QC_SinkFree (&s);

	QC_SinkInit (&s, SIZE_MAX);
	QC_FormatE (&s, 1.0, 2000);
	QT_CHECK (s.len == 2006 && !strncmp (s.buf, "1.", 2) && !strcmp (s.buf + s.len - 4, "e+00"));
	QC_SinkFree (&s);

	QC_SinkInit (&s, 10);
	QC_FormatFloat (&s, 1.0, 'f', &(qc_spec_t){.has_prec = true, .prec = SIZE_MAX});
	QT_EQ_S (QC_SinkText (&s), "1.00000000");
	QC_SinkFree (&s);
}

// strtod's value and the bytes it read
static void CheckStrtod (const char *s, double want, size_t wantused)
{
	size_t	used;
	double	v = QC_Strtod (s, &used);

	if (!QT_CHECK (!memcmp (&v, &want, 8) && used == wantused))
		printf ("  strtod(\"%s\") = %.17g, %zu read\n", s, v, used);
}

static void TestStrtod (void)
{
	size_t	used;
	double	v;

	CheckStrtod ("  12.5xyz", 12.5, 6);
	CheckStrtod (".5", 0.5, 2);
	CheckStrtod ("5.", 5.0, 2);
	CheckStrtod ("1e3", 1000.0, 3);
	CheckStrtod ("1e", 1.0, 1);
	CheckStrtod ("1e+", 1.0, 1);
	CheckStrtod ("-", 0.0, 0);
	CheckStrtod (".", 0.0, 0);
	CheckStrtod ("0x10", 16.0, 4);
	CheckStrtod ("0x1p4", 16.0, 5);
	CheckStrtod ("0x1.8p1", 3.0, 7);
	CheckStrtod ("0x.8", 0.5, 4);
	CheckStrtod ("0x", 0.0, 1);
	CheckStrtod ("0xg", 0.0, 1);
	CheckStrtod ("0x1p", 1.0, 3);
	QT_EQ_F (QC_Strtod ("-0x1p-1074", &used), -5e-324);
	QT_EQ_F (QC_Strtod ("0x1p-1080", &used), 0.0);
	QT_EQ_F (QC_Strtod ("0x1p99999999999999", &used), INFINITY);
	QT_EQ_F (QC_Strtod ("0x1.fffffffffffff8p0", &used), 2.0);
	QT_EQ_F (QC_Strtod ("0x1.fffffffffffff7ffffffffffffffffffffffffffff1p0", &used), 1.9999999999999998);
	CheckStrtod ("INF", INFINITY, 3);
	CheckStrtod ("-Infinity!", -INFINITY, 9);
	v = QC_Strtod ("nan(abc)x", &used);
	QT_CHECK (isnan (v) && !signbit (v) && used == 8);
	v = QC_Strtod ("-nan(", &used);
	QT_CHECK (isnan (v) && signbit (v) && used == 4);
	QT_EQ_F (QC_Strtod ("1e-99999999999999999999", &used), 0.0);
	QT_EQ_F (QC_Strtod ("1e99999999999999999999", &used), INFINITY);
	QT_EQ_F (QC_Strtod ("000000000000000000000000000001e-5", &used), 1e-5);
	QT_EQ_F (QC_Strtod ("2.4703282292062328e-324", &used), 5e-324);
	v = QC_Strtod ("-0", &used);
	QT_CHECK (v == 0 && signbit (v));
}

static void TestCIntegers (void)
{
	QT_EQ_I (QC_Strtol ("  -12abc", 0), -12);
	QT_EQ_I (QC_Strtol ("0x1f", 0), 31);
	QT_EQ_I (QC_Strtol ("010", 0), 8);
	QT_EQ_I (QC_Strtol ("08", 0), 0);
	QT_EQ_I (QC_Strtol ("0x", 0), 0);
	QT_EQ_I (QC_Strtol ("99999999999999999999", 10), INT64_MAX);
	QT_EQ_I (QC_Strtol ("-99999999999999999999", 10), INT64_MIN);
	QT_EQ_I (QC_Strtol ("-9223372036854775808", 10), INT64_MIN);
	QT_EQ_U (QC_Strtoul ("-1", 16), UINT64_MAX);
	QT_EQ_U (QC_Strtoul ("0x1F", 16), 31);
	QT_EQ_U (QC_Strtoul ("1ffffffffffffffff", 16), UINT64_MAX);
}

static const char *Ftos (float v)
{
	static char	out[256];
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_FtosText (&s, v);
	snprintf (out, sizeof(out), "%s", QC_SinkText (&s));
	QC_SinkFree (&s);
	return out;
}

static void TestFtosExact (void)
{
	QT_EQ_S (Ftos (0.1f), "0.100000001");
	QT_EQ_S (Ftos (-0.0f), "0");
	QT_EQ_S (Ftos (NAN), "1.#NAN");
	QT_EQ_S (Ftos (-NAN), "-1.#NAN");
}

static void TestCivilDates (void)
{
	qc_calendar_t	t;

	QC_CalendarFromUnix (0, &t);
	QT_CHECK (t.year == 1970 && t.month == 0 && t.day == 1 && t.weekday == 4 && t.yearday == 0);
	// 2024-02-29 12:34:56 UTC, a Thursday
	QC_CalendarFromUnix (1709210096, &t);
	QT_CHECK (t.year == 2024 && t.month == 1 && t.day == 29 && t.hour == 12 && t.minute == 34 && t.second == 56);
	QT_CHECK (t.weekday == 4 && t.yearday == 59);
	// 2026-12-31 is day 364 of a year that isn't a leap year
	QC_CalendarFromUnix (1798675200, &t);
	QT_CHECK (t.year == 2026 && t.month == 11 && t.day == 31 && t.yearday == 364);
	// before 1970: 1969-12-31 23:59:59, a Wednesday
	QC_CalendarFromUnix (-1, &t);
	QT_CHECK (t.year == 1969 && t.month == 11 && t.day == 31 && t.hour == 23 && t.second == 59 && t.weekday == 3);
}

int main (void)
{
	TestFixed ();
	TestScientificAndGeneral ();
	TestTiesToEven ();
	TestIntegers ();
	TestSinkCap ();
	TestHugePrecisions ();
	TestStrtod ();
	TestCIntegers ();
	TestFtosExact ();
	TestCivilDates ();
	return QT_Finish ("lib_dtoa", "numbers become text and text numbers as C makes them");
}
