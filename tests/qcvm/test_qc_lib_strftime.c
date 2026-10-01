// test_qc_lib_strftime.c -- strftime (qcvm-rs's tests/all/builtins_strings/strftime.rs
// and the unit tests of stdlib/strftime.rs, docs/spec/strings.md)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// builtins FTE binds by name that its CSQC declarations don't list
static const char	*extra[] = {"argc", "instr", "ftou", "utof", "strcmp", NULL};

// CSQC with QuakeWorld's charset defaults (utf8_enable 0, the Quake scheme)
static qh_t *Harness (void)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.utf8 = false;
	config.charscheme = QC_CHARS_QUAKE;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, (void *)extra);
}

// 2024-02-29 12:34:56 UTC, a Thursday
static qh_t *Fixed (void)
{
	qh_t	*h = Harness ();

	h->host.has_now = true;
	QC_CalendarFromUnix (1709210096, &h->host.now);
	return h;
}

// strftime's text, checked against want
static void CheckStrftime (qh_t *h, const char *want, int line, int argc, const qc_value_t *args)
{
	if (!QT_EQ_S (QH_String (h, "strftime", argc, args), want))
		printf ("  strftime at line %d\n", line);
}

#define ST(want, ...)	CheckStrftime (h, want, __LINE__, ARGS (__VA_ARGS__))
#define S(text)			QH_S (h, text)

static void TestFormatsTheHostTime (void)
{
	qh_t	*h = Fixed ();

	ST ("2024-02-29 12:34:56", F (0), S ("%Y-%m-%d %H:%M:%S"));
	ST ("Thu 29 Feb 2024", F (1), S ("%a %d %b %Y"));
	ST ("Thursday, February 29", F (0), S ("%A, %B %e"));
	ST ("Thu Feb 29 12:34:56 2024", F (0), S ("%c"));
	ST ("02/29/24 12:34:56 02/29/24 12:34:56", F (0), S ("%x %X %D %T"));
	ST ("12:34 PM|12|12|pm", F (0), S ("%I:%M %p|%l|%k|%P"));
	ST ("060 08 09 09 2024 24 4 4", F (0), S ("%j %U %W %V %G %g %u %w"));
	ST ("1709210096 +0000 UTC 20 24 %", F (0), S ("%s %z %Z %C %y %%"));
	ST ("\n\t", F (0), S ("%n%t"));
	ST ("", F (0), S (""));
	QH_Free (h);
}

// exactly %R and %F are FTE's shortcuts; the arguments are joined
static void TestWholeFormatShortcuts (void)
{
	qh_t	*h = Fixed ();

	ST ("12:34", F (0), S ("%R"));
	ST ("2024-02-29", F (0), S ("%F"));
	ST ("12:34 2024-02-29", F (0), S ("%R %F"));
	ST ("2024-02", F (0), S ("%Y"), S ("-"), S ("%m"));
	QH_Free (h);
}

static void TestFlagsWidthsAndUnknown (void)
{
	qh_t	*h = Fixed ();

	ST ("THU FEBRUARY PM pm", F (0), S ("%^a %#B %^p %#p"));
	ST ("2/29  2 00029  29", F (0), S ("%-m/%-d %_m %05d %3e"));
	ST ("  Thursday|Thursday|00Thursday", F (0), S ("%10A|%-10A|%010A"));
	ST ("2024 29 24 56", F (0), S ("%EY %Oe %Ey %OS"));
	ST ("%Q %Ea %Oz %5", F (0), S ("%Q %Ea %Oz %5"));
	ST ("100%", F (0), S ("100%"));
	ST ("      Thu Feb 29 12:34:56 2024|", F (0), S ("%30c|"));
	ST ("THU FEB 29 12:34:56 2024", F (0), S ("%^c"));
	QH_Free (h);
}

static void TestEdgeDates (void)
{
	qh_t	*h = Harness ();

	// 2021-01-01 (a Friday) is in ISO week 53 of 2020
	h->host.has_now = true;
	QC_CalendarFromUnix (1609459200, &h->host.now);
	ST ("2020-W53-5 001 00 00", F (0), S ("%G-W%V-%u %j %U %W"));
	// a local time with the host's offset and zone name
	h->host.now = (qc_calendar_t){.year = 1999, .month = 11, .day = 31, .hour = 23, .minute = 59, .second = 60,
		.weekday = 5, .yearday = 364, .utc_offset = -(5 * 3600 + 30 * 60), .zone = "XST"};
	ST ("1999-12-31 23:59:60 -0530 XST xst 11PM", F (1), S ("%F %T %z %Z %#Z %I%p"));
	// seconds since the epoch take the offset into account
	ST ("946704600", F (1), S ("%s"));
	QH_Free (h);
}

static void TestOutputIsLimited (void)
{
	qh_t	*h = Fixed ();
	char	*big = malloc (9001);

	memset (big, 'x', 9000);
	big[9000] = 0;
	QT_EQ_U (strlen (QH_String (h, "strftime", ARGS (F (0), S (big)))), 8191);
	QT_EQ_U (strlen (QH_String (h, "strftime", ARGS (F (0), S ("%99999Y")))), 8191);
	free (big);
	QH_Free (h);
}

// strftime's text of a time secs after 1970 (UTC)
static const char *Ftime (const char *fmt, int64_t secs)
{
	static char		out[512];
	qc_calendar_t	t;
	qc_sink_t		s;

	QC_CalendarFromUnix (secs, &t);
	QC_SinkInit (&s, 8191);
	QC_StrftimeText (&s, fmt, &t);
	snprintf (out, sizeof(out), "%s", QC_SinkText (&s));
	QC_SinkFree (&s);
	return out;
}

// every conversion (the unit test of stdlib/strftime.rs)
static void TestConversions (void)
{
	// 2024-02-29 12:34:56 UTC, a Thursday
	int64_t	t = 1709210096;

	QT_EQ_S (Ftime ("%Y-%m-%d %H:%M:%S", t), "2024-02-29 12:34:56");
	QT_EQ_S (Ftime ("%a %A %b %B %h", t), "Thu Thursday Feb February Feb");
	QT_EQ_S (Ftime ("%c", t), "Thu Feb 29 12:34:56 2024");
	QT_EQ_S (Ftime ("%D|%F|%R|%T|%r", t), "02/29/24|2024-02-29|12:34|12:34:56|12:34:56 PM");
	QT_EQ_S (Ftime ("%e|%j|%u|%w|%U|%W|%V|%G|%g", t), "29|060|4|4|08|09|09|2024|24");
	QT_EQ_S (Ftime ("%I %l %k %p %P", t), "12 12 12 PM pm");
	QT_EQ_S (Ftime ("%s %z %Z %%", t), "1709210096 +0000 UTC %");
	QT_EQ_S (Ftime ("%C %y %n%t", t), "20 24 \n\t");
	QT_EQ_S (Ftime ("%^a %#b %10Y %-d %_5m %05e", t), "THU FEB 0000002024 29     2 00029");
	QT_EQ_S (Ftime ("%Ey %Od %Ea %q %", t), "24 29 %Ea %q %");
	QT_EQ_S (Ftime ("%R", 0), "00:00");
}

static void TestIsoWeeks (void)
{
	// 2021-01-01 (a Friday) is in week 53 of 2020; 2024-12-30 (a Monday) in week 1 of 2025
	QT_EQ_S (Ftime ("%G-W%V-%u", 1609459200), "2020-W53-5");
	QT_EQ_S (Ftime ("%G-W%V-%u", 1735516800), "2025-W01-1");
}

int main (void)
{
	TestFormatsTheHostTime ();
	TestWholeFormatShortcuts ();
	TestFlagsWidthsAndUnknown ();
	TestEdgeDates ();
	TestOutputIsLimited ();
	TestConversions ();
	TestIsoWeeks ();
	return QT_Finish ("lib_strftime", "strftime formats as glibc's does in the C locale");
}
