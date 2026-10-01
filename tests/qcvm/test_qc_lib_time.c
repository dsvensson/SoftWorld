// test_qc_lib_time.c -- gettime and calltimeofday (qcvm-rs's
// tests/all/builtins_misc/time.rs)

#include "qc_harness.h"

#include <stdio.h>

static const char	*parts[6] = {"t_sec", "t_min", "t_hour", "t_day", "t_mon", "t_year"};

// timeofday(sec, min, hour, day, mon, year, text) storing its arguments in globals
static void Setup (qc_asm_t *a, void *ctx)
{
	static const char		*named[] = {"gettimef", "gettimed", NULL};
	static const uint8_t	sizes[7] = {1, 1, 1, 1, 1, 1, 1};
	uint32_t				globals[6], text, k;
	qa_func_t				f;

	(void)ctx;
	QH_Named (a, (void *)named);
	for (k = 0 ; k < 6 ; k++)
		globals[k] = QA_Global (a, parts[k], QC_EV_FLOAT, NULL, 0);
	text = QA_Global (a, "t_text", QC_EV_STRING, NULL, 0);
	f = QA_Function (a, "timeofday", sizes, 7, 0);
	for (k = 0 ; k < 6 ; k++)
		QA_Emit (a, QOP_STORE_F, QA_Local (f, k), globals[k], 0);
	QA_Emit (a, QOP_STORE_S, QA_Local (f, 6), text, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
}

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, Setup, NULL);
}

static qc_word_t GlobalValue (qh_t *h, const char *name)
{
	uint32_t	word = 0, type;

	if (!QC_FindGlobal (h->vm, name, &word, &type))
		printf ("  no global %s\n", name);
	return QC_Globals (h->vm)[word];
}

static double Double (qc_value_t r)
{
	return QC_BitsDouble ((uint64_t)r.w[0] | ((uint64_t)r.w[1] << 32));
}

static void TestGettimeTypes (void)
{
	qh_t	*h = Harness ();

	QC_SetTime (h->vm, 12.345678);
	QT_EQ_F (QH_Float (h, "gettime", NOARGS), 12.345678f);
	QT_EQ_F (QH_Float (h, "gettime", ARGS (F (0))), 12.345678f);
	QT_EQ_F (QH_Float (h, "gettime", ARGS (F (7))), 12.345678f);		// unknown types read the frame time
	QT_EQ_F (QH_Float (h, "gettime", ARGS (F (1))), 12.345f);		// whole milliseconds
	// type 5 is the host's simulation time, if it has one
	QT_EQ_F (QH_Float (h, "gettime", ARGS (F (5))), 12.345678f);
	h->host.has_sim_time = true;
	h->host.sim_time = 99.5;
	QT_EQ_F (QH_Float (h, "gettime", ARGS (F (5))), 99.5);
	// FTE's CSQC name, and the double-precision one (whose type is an int)
	QT_EQ_F (QH_Float (h, "gettimef", ARGS (F (5))), 99.5);
	QT_EQ_F (Double (QH_Raw (h, "gettimed", ARGS (I (0)))), 12.345678);
	QT_EQ_F (Double (QH_Raw (h, "gettimed", NOARGS)), 12.345678);
	QH_Free (h);
}

// gettime defaults to the time since the VM was made
static void TestGettimeDefault (void)
{
	qh_t	*h = QH_Csqc ();
	float	t = QH_Float (h, "gettime", NOARGS);

	if (!QT_CHECK (t >= 0 && t < 60))
		printf ("  gettime %g\n", (double)t);
	QH_Free (h);
}

// calltimeofday calls timeofday with the local time
static void TestCalltimeofday (void)
{
	static const float	want[6] = {9, 5, 14, 3, 8, 2026};		// the month counts from 0
	qh_t				*h = Harness ();
	int					k;

	h->host.has_now = true;
	h->host.now = (qc_calendar_t){.year = 2026, .month = 8, .day = 3, .hour = 14, .minute = 5, .second = 9,
		.weekday = 4, .yearday = 245, .utc_offset = 7200, .zone = "CEST"};
	QH_Raw (h, "calltimeofday", NOARGS);
	for (k = 0 ; k < 6 ; k++)
		if (!QT_EQ_F (GlobalValue (h, parts[k]).f, want[k]))
			printf ("  %s\n", parts[k]);
	QT_EQ_S (QH_Text (h, GlobalValue (h, "t_text").u), "Thu Sep 03, 14:05:09 2026");
	QH_Free (h);

	// without a timeofday function nothing happens
	h = QH_Csqc ();
	QH_Raw (h, "calltimeofday", NOARGS);
	QH_Free (h);
}

int main (void)
{
	TestGettimeTypes ();
	TestGettimeDefault ();
	TestCalltimeofday ();
	return QT_Finish ("lib_time", "gettime and calltimeofday behave as FTE's");
}
