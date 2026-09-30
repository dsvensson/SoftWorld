// qc_lib_strftime.c -- strftime (docs/spec/strings.md)
//
// C's strftime in the C locale, as glibc has it: every C99 and POSIX
// conversion and GNU's common ones (%k %l %P %s), the _ - 0 ^ # flags, widths
// and the E and O modifiers. Unknown conversions are copied as they are.

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QC_STRFTIME_MAX		8191		// FTE's 8192-byte buffer

static const char *const	qc_days[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday"};
static const char *const	qc_months[12] = {"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"};

static bool QC_IsLeap (int64_t year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int64_t QC_FloorDiv (int64_t a, int64_t b)
{
	return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

static int64_t QC_FloorMod (int64_t a, int64_t b)
{
	return a - QC_FloorDiv (a, b) * b;
}

// days since 1970-01-01 of a proleptic Gregorian date (month 1-12)
static int64_t QC_DaysFromCivil (int64_t year, int64_t month, int64_t day)
{
	int64_t	y = month <= 2 ? year - 1 : year, era = QC_FloorDiv (y, 400), yoe = QC_FloorMod (y, 400);
	int64_t	mp = QC_FloorMod (month + 9, 12), doy = (153 * mp + 2) / 5 + day - 1;

	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

// ISO 8601's week-based days: the day of the year from the Monday that starts
// week 1 (maybe negative); week 1 has the year's first Thursday
static int64_t QC_IsoWeekDays (int64_t yday, int64_t wday)
{
	return yday - QC_FloorMod (yday - wday + 4 + 378, 7) + 3;
}

// ISO 8601's week-based year and week (1-53)
static void QC_IsoWeek (const qc_calendar_t *t, int64_t *year, int64_t *week)
{
	int64_t	yday = t->yearday, wday = t->weekday, days, next;

	*year = t->year;
	days = QC_IsoWeekDays (yday, wday);
	if (days < 0)
	{
		(*year)--;
		days = QC_IsoWeekDays (yday + (QC_IsLeap (*year) ? 366 : 365), wday);
	}
	else
	{
		next = QC_IsoWeekDays (yday - (QC_IsLeap (*year) ? 366 : 365), wday);
		if (next >= 0)
		{
			(*year)++;
			days = next;
		}
	}
	*week = days / 7 + 1;
}

typedef enum
{
	QC_PAD_DEFAULT,
	QC_PAD_SPACE,
	QC_PAD_NONE,
	QC_PAD_ZERO
} qc_pad_t;

typedef struct
{
	qc_pad_t	pad;
	bool		upper;
	bool		swapcase;
	size_t		width;
} qc_directive_t;

// text padded to the width (with zeros only for the 0 flag)
static void QC_DirText (const qc_directive_t *d, qc_sink_t *out, const char *s, size_t len, bool upper, bool lower)
{
	size_t	w = d->pad == QC_PAD_NONE ? 0 : d->width, i;
	char	c;

	if (w > len)
		QC_SinkFill (out, d->pad == QC_PAD_ZERO ? '0' : ' ', w - len);
	for (i = 0 ; i < len ; i++)
	{
		c = s[i];
		if (upper && c >= 'a' && c <= 'z')
			c -= 32;
		else if (lower && c >= 'A' && c <= 'Z')
			c += 32;
		QC_SinkPush (out, c);
	}
}

// a number of digits digits at least (zeros by default, spaces for the
// conversions that take them or the _ flag, none with -); a width raises it
static void QC_DirNumber (const qc_directive_t *d, qc_sink_t *out, size_t digits, int64_t value, bool spacedefault,
	bool sign)
{
	qc_pad_t	pad = d->pad == QC_PAD_DEFAULT && spacedefault ? QC_PAD_SPACE : d->pad;
	char		body[32], signchar = value < 0 ? '-' : sign ? '+' : 0;
	size_t		len, padding;

	len = (size_t)snprintf (body, sizeof(body), "%llu",
		(unsigned long long)(value < 0 ? 0 - (uint64_t)value : (uint64_t)value));
	if (d->width > digits)
		digits = d->width;
	padding = pad == QC_PAD_NONE || digits <= len + (signchar != 0) ? 0 : digits - len - (signchar != 0);
	if (pad == QC_PAD_SPACE)
		QC_SinkFill (out, ' ', padding);
	if (signchar)
		QC_SinkPush (out, signchar);
	if (pad != QC_PAD_SPACE)
		QC_SinkFill (out, '0', padding);
	QC_SinkAppend (out, body, len);
}

// the modifiers a conversion takes (POSIX): E for c C x X y Y, O for the numeric
// ones (and month names, as glibc)
static bool QC_ModifierOk (char conv, char modifier)
{
	if (!modifier)
		return true;
	if (modifier == 'E')
		return conv && strchr ("cCxXyY", conv);
	return conv && strchr ("bBhdeHImMSuUVwWy", conv);
}

static void QC_FormatTime (qc_sink_t *out, const char *fmt, const qc_calendar_t *t)
{
	int64_t			year = t->year, hour = t->hour, wday = t->weekday, yday = t->yearday, hour12, v, isoyear, isoweek;
	int64_t			days, secs, off, hhmm, monday;
	size_t			i = 0, start, end;
	char			c, conv, modifier;
	qc_directive_t	d;
	const char		*sub, *dayname, *monthname, *text;
	qc_sink_t		inner;
	bool			namesupper, lower;

	dayname = t->weekday < 7 ? qc_days[t->weekday] : "?";
	monthname = t->month < 12 ? qc_months[t->month] : "?";
	hour12 = hour % 12 == 0 ? 12 : hour % 12;
	while ((c = fmt[i]))
	{
		if (c != '%')
		{
			QC_SinkPush (out, c);
			i++;
			continue;
		}
		start = i++;
		d = (qc_directive_t){0};
		for ( ; ; i++)
		{
			if (fmt[i] == '_')
				d.pad = QC_PAD_SPACE;
			else if (fmt[i] == '-')
				d.pad = QC_PAD_NONE;
			else if (fmt[i] == '0' || fmt[i] == '+')
				d.pad = QC_PAD_ZERO;
			else if (fmt[i] == '^')
				d.upper = true;
			else if (fmt[i] == '#')
				d.swapcase = true;
			else
				break;
		}
		for ( ; fmt[i] >= '0' && fmt[i] <= '9' ; i++)
			d.width = d.width > (SIZE_MAX - 9) / 10 ? SIZE_MAX : d.width * 10 + (size_t)(fmt[i] - '0');
		modifier = 0;
		if (fmt[i] == 'E' || fmt[i] == 'O')
			modifier = fmt[i++];
		conv = fmt[i];
		if (!conv || !strchr ("aAbBcCdDeFgGhHIjklmMnpPrRsStTuUVwWxXyYzZ%", conv) || !QC_ModifierOk (conv, modifier))
		{
			// unknown conversions (and a % at the end) are copied as they are
			end = conv ? i + 1 : i;
			QC_SinkAppend (out, fmt + start, end - start);
			i = end;
			continue;
		}
		i++;
		switch (conv)
		{
		case 'c':	sub = "%a %b %e %H:%M:%S %Y"; break;
		case 'D':
		case 'x':	sub = "%m/%d/%y"; break;
		case 'F':	sub = "%Y-%m-%d"; break;
		case 'r':	sub = "%I:%M:%S %p"; break;
		case 'R':	sub = "%H:%M"; break;
		case 'T':
		case 'X':	sub = "%H:%M:%S"; break;
		default:	sub = NULL; break;
		}
		if (sub)
		{
			QC_SinkInit (&inner, QC_SinkRoom (out));
			QC_FormatTime (&inner, sub, t);
			QC_DirText (&d, out, QC_SinkText (&inner), inner.len, d.upper, false);
			QC_SinkFree (&inner);
			continue;
		}
		namesupper = d.upper || d.swapcase;
		switch (conv)
		{
		case 'a':	QC_DirText (&d, out, dayname, strlen (dayname) < 3 ? strlen (dayname) : 3, namesupper, false); break;
		case 'A':	QC_DirText (&d, out, dayname, strlen (dayname), namesupper, false); break;
		case 'b':
		case 'h':	QC_DirText (&d, out, monthname, strlen (monthname) < 3 ? strlen (monthname) : 3, namesupper, false); break;
		case 'B':	QC_DirText (&d, out, monthname, strlen (monthname), namesupper, false); break;
		case 'p':
		case 'P':
			lower = conv == 'P' || d.swapcase;
			QC_DirText (&d, out, hour < 12 ? "AM" : "PM", 2, d.upper && !lower, lower);
			break;
		case 'Z':
			text = t->zone ? t->zone : "";
			QC_DirText (&d, out, text, strlen (text), d.upper && !d.swapcase, d.swapcase);
			break;
		case 'n':	QC_DirText (&d, out, "\n", 1, false, false); break;
		case 't':	QC_DirText (&d, out, "\t", 1, false, false); break;
		case '%':	QC_DirText (&d, out, "%", 1, false, false); break;
		case 'C':	QC_DirNumber (&d, out, 2, QC_FloorDiv (year, 100), false, false); break;
		case 'd':	QC_DirNumber (&d, out, 2, t->day, false, false); break;
		case 'e':	QC_DirNumber (&d, out, 2, t->day, true, false); break;
		case 'g':
			QC_IsoWeek (t, &isoyear, &isoweek);
			QC_DirNumber (&d, out, 2, QC_FloorMod (isoyear, 100), false, false);
			break;
		case 'G':
			QC_IsoWeek (t, &isoyear, &isoweek);
			QC_DirNumber (&d, out, 1, isoyear, false, false);
			break;
		case 'H':	QC_DirNumber (&d, out, 2, hour, false, false); break;
		case 'I':	QC_DirNumber (&d, out, 2, hour12, false, false); break;
		case 'j':	QC_DirNumber (&d, out, 3, yday + 1, false, false); break;
		case 'k':	QC_DirNumber (&d, out, 2, hour, true, false); break;
		case 'l':	QC_DirNumber (&d, out, 2, hour12, true, false); break;
		case 'm':	QC_DirNumber (&d, out, 2, (int64_t)t->month + 1, false, false); break;
		case 'M':	QC_DirNumber (&d, out, 2, t->minute, false, false); break;
		case 's':
			days = QC_DaysFromCivil (year, (int64_t)t->month + 1, t->day);
			secs = days * 86400 + hour * 3600 + (int64_t)t->minute * 60 + t->second - t->utc_offset;
			QC_DirNumber (&d, out, 1, secs, false, false);
			break;
		case 'S':	QC_DirNumber (&d, out, 2, t->second, false, false); break;
		case 'u':	QC_DirNumber (&d, out, 1, QC_FloorMod (wday + 6, 7) + 1, false, false); break;
		case 'U':	QC_DirNumber (&d, out, 2, (yday - wday + 7) / 7, false, false); break;
		case 'V':
			QC_IsoWeek (t, &isoyear, &isoweek);
			QC_DirNumber (&d, out, 2, isoweek, false, false);
			break;
		case 'w':	QC_DirNumber (&d, out, 1, wday, false, false); break;
		case 'W':
			monday = QC_FloorMod (wday + 6, 7);
			QC_DirNumber (&d, out, 2, (yday - monday + 7) / 7, false, false);
			break;
		case 'y':	QC_DirNumber (&d, out, 2, QC_FloorMod (year, 100), false, false); break;
		case 'Y':	QC_DirNumber (&d, out, 1, year, false, false); break;
		case 'z':
			off = t->utc_offset / 60;
			hhmm = (off < 0 ? -off : off) / 60 * 100 + (off < 0 ? -off : off) % 60;
			v = off < 0 ? -hhmm : hhmm;
			QC_DirNumber (&d, out, 5, v, false, off >= 0);
			break;
		default:
			break;
		}
	}
}

// string strftime(float uselocaltime, string format...): the time now (local if
// the first argument isn't zero, else UTC) as C's strftime makes it; "" without
// a time
static bool QC_Strftime (qcvm_t *vm)
{
	bool			local = QC_ArgFloat (vm, 0) != 0;
	char			*fmt = QC_LibConcat (vm, 1, NULL);
	const char		*f;
	qc_calendar_t	t;
	qc_sink_t		out;

	if (!fmt)
		return false;
	QC_SinkInit (&out, QC_STRFTIME_MAX);
	if (QC_LibCalendar (vm, local, &t))
	{
		// FTE replaces exactly %R and %F
		f = !strcmp (fmt, "%R") ? "%H:%M" : !strcmp (fmt, "%F") ? "%Y-%m-%d" : fmt;
		QC_FormatTime (&out, f, &t);
	}
	free (fmt);
	return QC_LibReturnSink (vm, &out);
}

// strftime's text of a time, for the tests
void QC_StrftimeText (qc_sink_t *out, const char *fmt, const qc_calendar_t *t)
{
	const char	*f = !strcmp (fmt, "%R") ? "%H:%M" : !strcmp (fmt, "%F") ? "%Y-%m-%d" : fmt;

	QC_FormatTime (out, f, t);
}

static const qc_libentry_t	qc_strftime[] = {
	{"strftime", QC_Strftime, NULL, 0},
};

bool QC_RegisterStrftime (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_strftime, sizeof(qc_strftime) / sizeof(qc_strftime[0]));
}
