// qc_lib_time.c -- gettime and calltimeofday (docs/spec/builtins.md)

#include "qc_lib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// the clock gettime's type t reads: 1 the time now to the millisecond below, 5
// the host's simulation time, anything else the frame's real time
static double QC_GettimeClock (qcvm_t *vm, int32_t t)
{
	double	now = QC_Now (vm), sim;

	if (t == 1)
		return floor (now * 1000.0) / 1000.0;
	if (t == 5 && vm->host.sim_time && vm->host.sim_time (vm->ctx, &sim))
		return sim;
	return now;
}

// float gettime(optional float type) (FTE's CSQC calls it gettimef), in seconds
static bool QC_Gettime (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)QC_GettimeClock (vm, QC_FloatToInt (QC_LibOptFloat (vm, 0, 0))));
	return true;
}

// __double gettimed(optional int type): gettime in double precision
static bool QC_Gettimed (qcvm_t *vm)
{
	QC_LibReturnDouble (vm, QC_GettimeClock (vm, QC_Argc (vm) > 0 ? QC_ArgInt (vm, 0) : 0));
	return true;
}

// void calltimeofday(): if the progs has timeofday, calls timeofday(second,
// minute, hour, day, month, year, text) with the local time (month 0-11, text
// like "Wed Sep 23, 14:05:09 2026")
static bool QC_Calltimeofday (qcvm_t *vm)
{
	static const char *const	weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const	months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
		"Oct", "Nov", "Dec"};
	qc_func_t					f = QC_LibFindFunction (vm, -2, "timeofday");
	qc_calendar_t				t;
	qc_value_t					args[7];
	char						text[64];
	qc_str_t					s;

	if (!f || !QC_LibCalendar (vm, true, &t))
		return true;
	snprintf (text, sizeof(text), "%s %s %02u, %02u:%02u:%02u %d", t.weekday < 7 ? weekdays[t.weekday] : "?",
		t.month < 12 ? months[t.month] : "?", t.day, t.hour, t.minute, t.second, t.year);
	s = QC_TempString (vm, text, strlen (text));
	if (!s)
		return false;
	args[0] = QC_ValFloat ((float)t.second);
	args[1] = QC_ValFloat ((float)t.minute);
	args[2] = QC_ValFloat ((float)t.hour);
	args[3] = QC_ValFloat ((float)t.day);
	args[4] = QC_ValFloat ((float)t.month);
	args[5] = QC_ValFloat ((float)t.year);
	args[6] = QC_ValWord (s);
	return QC_Call (vm, f, 7, args, NULL);
}

static const qc_libentry_t	qc_time[] = {
	{"gettime", QC_Gettime, NULL, 0},
	{"gettimef", NULL, "gettime", 0},
	{"gettimed", QC_Gettimed, NULL, 0},
	{"calltimeofday", QC_Calltimeofday, NULL, 0},
};

bool QC_RegisterTime (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_time, sizeof(qc_time) / sizeof(qc_time[0]));
}
