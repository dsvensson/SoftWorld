// qc_lib.c -- the standard builtins: their registry, the extensions they make
// complete, and what the library modules share

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
==============================================================================

REGISTRATION

==============================================================================
*/

bool QC_LibRegister (qc_builtins_t *b, const qc_libentry_t *table, size_t count)
{
	qc_numbering_t	numbering = QC_BuiltinsNumbering (b);
	uint32_t		number;
	size_t			i;
	bool			ok;

	for (i = 0 ; i < count ; i++)
	{
		if (!table[i].func)
			ok = QC_BuiltinsAlias (b, table[i].name, table[i].target);
		else if (table[i].fte && numbering != QC_NUMBERING_NONE
			&& !QC_BuiltinNumber (numbering, table[i].name, &number))
			ok = QC_BuiltinsSetNumbered (b, table[i].fte, table[i].name, table[i].func);
		else
			ok = QC_BuiltinsSet (b, table[i].name, table[i].func);
		if (!ok)
			return false;
	}
	return true;
}

qc_builtins_t *QC_BuiltinsStandard (qc_numbering_t numbering)
{
	qc_builtins_t	*b = QC_BuiltinsCreate (numbering);

	if (!b)
		return NULL;
	if (!QC_RegisterConvert (b) || !QC_RegisterEntity (b) || !QC_RegisterHostcalls (b) || !QC_RegisterIntrospect (b)
		|| !QC_RegisterMath (b) || !QC_RegisterReflect (b) || !QC_RegisterTime (b) || !QC_RegisterVector (b))
	{
		QC_BuiltinsFree (b);
		return NULL;
	}
	return b;
}

// FTE extensions whose builtins the standard library implements completely;
// those that also need the engine's (FRIK_FILE, DP_QC_ENTITYDATA...) are the
// host's to answer for
static const char *const qc_extensions[] = {
	"DP_QC_ASINACOSATANATAN2TAN",
	"DP_QC_CHANGEPITCH",
	"DP_QC_COPYENTITY",
	"DP_QC_CVAR_DEFSTRING",
	"DP_QC_CVAR_STRING",
	"DP_QC_CVAR_TYPE",
	"DP_QC_EDICT_NUM",
	"DP_QC_ETOS",
	"DP_QC_FINDCHAIN",
	"DP_QC_FINDCHAINFLAGS",
	"DP_QC_FINDCHAINFLOAT",
	"DP_QC_FINDFLAGS",
	"DP_QC_FINDFLOAT",
	"DP_QC_MINMAXBOUND",
	"DP_QC_RANDOMVEC",
	"DP_QC_SINCOSSQRTPOW",
	"DP_QC_VECTOANGLES_WITH_ROLL",
	"DP_QC_VECTORVECTORS",
	"DP_REGISTERCVAR",
	"DP_SV_PRINT",
	"EXT_BITSHIFT",
	"FTE_CALLTIMEOFDAY",
	"FTE_QC_CHECKCOMMAND",
	"FTE_QC_CROSSPRODUCT",
	"FTE_QC_INTCONV",
};

bool QC_StandardExtension (const char *name)
{
	size_t	i;

	for (i = 0 ; i < sizeof(qc_extensions) / sizeof(qc_extensions[0]) ; i++)
		if (!strcmp (qc_extensions[i], name))
			return true;
	return false;
}

/*
==============================================================================

ARGUMENTS AND RESULTS

==============================================================================
*/

char *QC_LibConcat (qcvm_t *vm, int from, size_t *len)
{
	int			argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, i;
	qc_sink_t	s;
	const char	*text;
	char		*out;

	// a single argument as it is; more are joined, and cut at FTE's buffer size
	QC_SinkInit (&s, argc == from + 1 ? SIZE_MAX : QC_CONCAT_MAX);
	for (i = from ; i < argc ; i++)
	{
		text = QC_ArgString (vm, i);
		QC_SinkAppend (&s, text, strlen (text));
	}
	out = s.buf ? s.buf : calloc (1, 1);
	if (s.failed || !out)
	{
		free (out);
		QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
		return NULL;
	}
	if (len)
		*len = s.len;
	return out;
}

int32_t QC_LibArgInt (const qcvm_t *vm, int i)
{
	return QC_FloatToInt (QC_ArgFloat (vm, i));
}

float QC_LibOptFloat (const qcvm_t *vm, int i, float def)
{
	return QC_Argc (vm) > i ? QC_ArgFloat (vm, i) : def;
}

uint32_t QC_LibEntArg (qcvm_t *vm, int i)
{
	uint32_t	e = QC_ArgWord (vm, i);

	if (e < vm->mem.num_edicts)
		return e;
	QC_Warn (vm, QC_WARN_BAD_ENTITY, e, NULL);
	return 0;
}

void QC_LibSetArgWord (qcvm_t *vm, int i, uint32_t word)
{
	if (i >= 0 && i < 8)
		QC_SetS (&vm->mem, (uint64_t)QC_GBase (vm) + QC_OFS_PARM0 + (uint64_t)i * 12, word);
}

void QC_LibReturnBool (qcvm_t *vm, bool b)
{
	QC_ReturnFloat (vm, b ? 1.0f : 0.0f);
}

bool QC_LibReturnOptString (qcvm_t *vm, const char *text)
{
	if (text)
		return QC_ReturnString (vm, text, strlen (text));
	QC_ReturnWord (vm, 0);
	return true;
}

bool QC_LibSoftError (qcvm_t *vm, const char *fmt, ...)
{
	static const uint32_t	zero[3];
	char					text[1024];
	va_list					args;

	va_start (args, fmt);
	vsnprintf (text, sizeof(text), fmt, args);
	va_end (args);
	if (vm->config.developer)
	{
		QC_Warning (vm, "%s", text);
		QC_ReturnRaw (vm, zero);
		return true;
	}
	return QC_Error (vm, "%s", text);
}

/*
==============================================================================

GLOBALS, FIELDS AND FUNCTIONS

==============================================================================
*/

bool QC_LibGlobal (const qcvm_t *vm, const char *name, uint32_t want, uint32_t *ofs)
{
	const qc_progstate_t	*ps;
	const qc_def_t			*d;
	uint64_t				at;

	if (vm->x.prnum >= vm->numprogs)
		return false;
	ps = &vm->progs[vm->x.prnum];
	d = QC_GlobalDefRaw (ps->progs, name);
	if (!d || d->type != want)
		return false;
	at = (uint64_t)ps->gbase + (uint64_t)d->ofs * 4;
	if (at > UINT32_MAX)
		return false;
	*ofs = (uint32_t)at;
	return true;
}

int64_t QC_LibField (const qcvm_t *vm, const char *name)
{
	const qc_fieldentry_t	*f = QC_FieldEntry (vm, name);

	return f ? (int64_t)f->ofs : -1;
}

bool QC_LibFieldOk (const qcvm_t *vm, uint32_t f, uint32_t words)
{
	return ((uint64_t)f + words) * 4 <= vm->mem.field_bytes;
}

uint32_t QC_LibWord (const qcvm_t *vm, uint32_t e, uint32_t f)
{
	const uint8_t	*p = QC_FieldPtr (&vm->mem, e, f, 1);
	uint32_t		v = 0;

	if (p)
		memcpy (&v, p, 4);
	return v;
}

float QC_LibFloat (const qcvm_t *vm, uint32_t e, uint32_t f)
{
	return QC_BitsFloat (QC_LibWord (vm, e, f));
}

void QC_LibVector (const qcvm_t *vm, uint32_t e, uint32_t f, float out[3])
{
	uint32_t	k;

	for (k = 0 ; k < 3 ; k++)
		out[k] = QC_LibFloat (vm, e, f + k);
}

void QC_LibSetWord (qcvm_t *vm, uint32_t e, uint32_t f, uint32_t v)
{
	uint8_t	*p = QC_FieldPtr (&vm->mem, e, f, 1);

	if (p)
		memcpy (p, &v, 4);
}

void QC_LibSetVector (qcvm_t *vm, uint32_t e, uint32_t f, const float v[3])
{
	uint32_t	k;

	for (k = 0 ; k < 3 ; k++)
		QC_LibSetWord (vm, e, f + k, QC_FloatBits (v[k]));
}

float QC_LibNamedFloat (const qcvm_t *vm, uint32_t e, const char *name)
{
	int64_t	f = QC_LibField (vm, name);

	return f < 0 ? 0.0f : QC_LibFloat (vm, e, (uint32_t)f);
}

void QC_LibNamedVector (const qcvm_t *vm, uint32_t e, const char *name, float out[3])
{
	int64_t	f = QC_LibField (vm, name);

	if (f < 0)
		out[0] = out[1] = out[2] = 0;
	else
		QC_LibVector (vm, e, (uint32_t)f, out);
}

void QC_LibProgsRange (const qcvm_t *vm, int32_t prnum, uint32_t *first, uint32_t *end)
{
	*first = *end = 0;
	if (prnum == -2)
		*end = vm->numprogs;
	else if (prnum == -1)
	{
		*first = vm->x.prnum;
		*end = vm->x.prnum < vm->numprogs ? vm->x.prnum + 1 : vm->x.prnum;
	}
	else if (prnum >= 0 && (uint32_t)prnum < vm->numprogs)
	{
		*first = (uint32_t)prnum;
		*end = (uint32_t)prnum + 1;
	}
}

// C's atoi of the progs number before a ':'
static int32_t QC_PrefixNumber (const char *s, size_t len)
{
	size_t	i = 0;
	bool	negative = false;
	int64_t	v = 0;

	while (i < len && QC_IsCSpace (s[i]))
		i++;
	if (i < len && (s[i] == '-' || s[i] == '+'))
		negative = s[i++] == '-';
	for ( ; i < len && s[i] >= '0' && s[i] <= '9' ; i++)
		if (v <= INT32_MAX)
			v = v * 10 + (s[i] - '0');
	if (negative)
		return v > INT32_MAX ? INT32_MIN : (int32_t)-v;
	return v > INT32_MAX ? INT32_MAX : (int32_t)v;
}

qc_func_t QC_LibFindFunction (const qcvm_t *vm, int32_t prnum, const char *name)
{
	const char	*colon = strchr (name, ':');
	uint32_t	pr, end;
	qc_func_t	f;

	if (colon)
	{
		prnum = QC_PrefixNumber (name, (size_t)(colon - name));
		name = colon + 1;
	}
	QC_LibProgsRange (vm, prnum, &pr, &end);
	for ( ; pr < end ; pr++)
		if ((f = QC_FindFunctionIn (vm, pr, name)))
			return f;
	return 0;
}

void QC_BacktraceSink (const qcvm_t *vm, qc_sink_t *s)
{
	qc_backtrace_t		bt;
	const qc_btframe_t	*f;
	uint32_t			i;

	if (!QC_Backtrace (vm, &bt))
	{
		s->failed = true;
		return;
	}
	for (i = 0 ; i < bt.count ; i++)
	{
		f = &bt.frames[i];
		QC_SinkPrintf (s, "  %s", f->name);
		if (*f->file && f->line >= 0)
			QC_SinkPrintf (s, " (%s:%lld)", f->file, (long long)f->line);
		else if (*f->file)
			QC_SinkPrintf (s, " (%s)", f->file);
		QC_SinkPrintf (s, " @ statement %u\n", f->statement);
	}
	QC_FreeBacktrace (&bt);
}

/*
==============================================================================

CALENDARS

==============================================================================
*/

void QC_CalendarFromUnix (int64_t secs, qc_calendar_t *out)
{
	static const int64_t	start[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	int64_t					days, rem, z, era, doe, yoe, doy, mp, day, month, year;
	bool					leap;

	// civil from days (Howard Hinnant's algorithm), floor division throughout
	days = secs / 86400 - (secs % 86400 < 0);
	rem = secs - days * 86400;
	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	year = yoe + era * 400 + (month <= 2);
	leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	*out = (qc_calendar_t){
		.year = year > INT32_MAX ? INT32_MAX : year < INT32_MIN ? INT32_MIN : (int32_t)year,
		.month = (uint32_t)(month - 1),
		.day = (uint32_t)day,
		.hour = (uint32_t)(rem / 3600),
		.minute = (uint32_t)(rem % 3600 / 60),
		.second = (uint32_t)(rem % 60),
		.weekday = (uint32_t)(((days + 4) % 7 + 7) % 7),
		.yearday = (uint32_t)(start[month - 1] + day - 1 + (leap && month > 2)),
		.utc_offset = 0,
		.zone = "UTC",
	};
}

bool QC_LibCalendar (qcvm_t *vm, bool local, qc_calendar_t *out)
{
	time_t	now;

	if (vm->host.calendar_time && vm->host.calendar_time (vm->ctx, local, out))
		return true;
	now = time (NULL);
	if (now == (time_t)-1)
		return false;
	QC_CalendarFromUnix ((int64_t)now, out);
	return true;
}
