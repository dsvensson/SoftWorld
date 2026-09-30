// qc_error.c -- runtime errors, warnings and QuakeC backtraces

#include "qc_local.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static char *QC_VFormat (const char *fmt, va_list args)
{
	va_list	copy;
	int		n;
	char	*text;

	va_copy (copy, args);
	n = vsnprintf (NULL, 0, fmt, copy);
	va_end (copy);
	if (n < 0)
		return NULL;
	text = malloc ((size_t)n + 1);
	if (text)
		vsnprintf (text, (size_t)n + 1, fmt, args);
	return text;
}

void QC_FreeBacktrace (qc_backtrace_t *bt)
{
	free (bt->frames);
	*bt = (qc_backtrace_t){0};
}

void QC_ClearError (qc_error_t *e)
{
	free (e->message);
	QC_FreeBacktrace (&e->backtrace);
	*e = (qc_error_t){0};
}

void QC_FreeError (qc_error_t *error)
{
	QC_ClearError (error);
}

bool QC_Fail (qcvm_t *vm, qc_errkind_t kind, int64_t value, const char *fmt, ...)
{
	va_list	args;

	QC_ClearError (&vm->error);
	vm->error.kind = kind;
	vm->error.value = value;
	if (fmt)
	{
		va_start (args, fmt);
		vm->error.message = QC_VFormat (fmt, args);
		va_end (args);
	}
	return false;
}

/*
==============================================================================

BACKTRACES

==============================================================================
*/

static void QC_PushFrame (const qcvm_t *vm, qc_backtrace_t *bt, uint32_t prnum, uint32_t func, uint32_t stmt)
{
	const qc_progs_t	*p;
	const qc_function_t	*f;
	qc_btframe_t		*frame;
	uint32_t			line;

	if (func == QC_NO_FUNCTION || prnum >= vm->numprogs)
		return;
	p = vm->progs[prnum].progs;
	frame = &bt->frames[bt->count++];
	*frame = (qc_btframe_t){.function = QC_FUNC (prnum, func), .name = "", .file = "", .statement = stmt,
		.line = -1};
	if (func < p->numfunctions)
	{
		f = &p->functions[func];
		frame->name = QC_Cstr (p, f->name);
		frame->file = QC_Cstr (p, f->file);
	}
	if (QC_ProgsSourceLine (p, stmt, &line))
		frame->line = line;
}

bool QC_Backtrace (const qcvm_t *vm, qc_backtrace_t *bt)
{
	uint32_t	i;

	*bt = (qc_backtrace_t){0};
	bt->frames = malloc (((size_t)vm->numframes + 1) * sizeof(*bt->frames));
	if (!bt->frames)
		return false;
	QC_PushFrame (vm, bt, vm->x.prnum, vm->x.func, vm->x.pc);
	for (i = vm->numframes ; i-- > 0 ; )
		QC_PushFrame (vm, bt, vm->frames[i].prnum, vm->frames[i].func,
			vm->frames[i].resume_pc ? vm->frames[i].resume_pc - 1 : 0);
	return true;
}

/*
==============================================================================

WARNINGS

==============================================================================
*/

void QC_Warn (qcvm_t *vm, qc_warnkind_t kind, int64_t value, const char *fmt, ...)
{
	qc_warning_t	*w, *grown;
	va_list			args;
	uint32_t		n;

	if (vm->warnings_this_call < UINT32_MAX)
		vm->warnings_this_call++;
	if (vm->warnings_this_call > vm->config.limits.warnings_per_call)
	{
		if (vm->suppressed < UINT32_MAX)
			vm->suppressed++;
		return;
	}
	if (vm->numwarnings == vm->warningsize)
	{
		n = vm->warningsize ? vm->warningsize * 2 : 16;
		grown = realloc (vm->warnings, (size_t)n * sizeof(*grown));
		if (!grown)
			return;
		vm->warnings = grown;
		vm->warningsize = n;
	}
	w = &vm->warnings[vm->numwarnings++];
	*w = (qc_warning_t){.kind = kind, .value = value};
	if (fmt)
	{
		va_start (args, fmt);
		w->message = QC_VFormat (fmt, args);
		va_end (args);
	}
	QC_Backtrace (vm, &w->backtrace);
}

void QC_FlushWarnings (qcvm_t *vm)
{
	qc_warning_t	*list = vm->warnings;
	uint32_t		count = vm->numwarnings, i;

	// a warning callback may run QuakeC, which may warn: take the list first
	vm->warnings = NULL;
	vm->numwarnings = vm->warningsize = 0;
	for (i = 0 ; i < count ; i++)
	{
		if (vm->host.warning)
			vm->host.warning (vm->ctx, &list[i]);
		free (list[i].message);
		QC_FreeBacktrace (&list[i].backtrace);
	}
	free (list);
}

/*
==============================================================================

TEXT

==============================================================================
*/

static const char	*resource_names[] = {"Entities", "TempStrings", "Heap", "Fields", "ProgsArea", "Progs",
	"Threads", "StringBuffers", "HashTables"};

const char *QC_ErrorText (const qc_error_t *e, char *buf, size_t size)
{
	const char	*msg = e->message ? e->message : "";

	switch (e->kind)
	{
	case QC_ERR_NONE:					snprintf (buf, size, "no error"); break;
	case QC_ERR_BAD_OPCODE:				snprintf (buf, size, "bad opcode %lld", (long long)e->value); break;
	case QC_ERR_JUMP_OUT_OF_RANGE:		snprintf (buf, size, "jump out of range"); break;
	case QC_ERR_NULL_FUNCTION:			snprintf (buf, size, "NULL function"); break;
	case QC_ERR_INVALID_FUNCTION:		snprintf (buf, size, "invalid function 0x%llx", (unsigned long long)e->value); break;
	case QC_ERR_BUILTIN_NOT_IMPLEMENTED:
		snprintf (buf, size, "Builtin %lld:%s not implemented", (long long)e->value, msg);
		break;
	case QC_ERR_CALL_DEPTH:				snprintf (buf, size, "stack overflow"); break;
	case QC_ERR_LOCAL_STACK:			snprintf (buf, size, "local stack overflow"); break;
	case QC_ERR_REENTRANCY:				snprintf (buf, size, "too many nested calls into QuakeC"); break;
	case QC_ERR_TOO_MANY_ARGUMENTS:
		snprintf (buf, size, "%lld arguments passed; QuakeC takes at most 8", (long long)e->value);
		break;
	case QC_ERR_RUNAWAY:				snprintf (buf, size, "runaway loop error"); break;
	case QC_ERR_DEADLINE:				snprintf (buf, size, "deadline exceeded"); break;
	case QC_ERR_BAD_POINTER_READ:		snprintf (buf, size, "bad pointer read (0x%llx)", (unsigned long long)e->value); break;
	case QC_ERR_BAD_POINTER_WRITE:		snprintf (buf, size, "bad pointer write (0x%llx)", (unsigned long long)e->value); break;
	case QC_ERR_NULL_POINTER_WRITE:		snprintf (buf, size, "null pointer write"); break;
	case QC_ERR_ARRAY_INDEX:			snprintf (buf, size, "array index %lld out of bounds", (long long)e->value); break;
	case QC_ERR_BOUND_CHECK:
		snprintf (buf, size, "array index %lld out of bounds [%u, %u)", (long long)e->value, e->low, e->high);
		break;
	case QC_ERR_PUSHED_TOO_MUCH:		snprintf (buf, size, "pushed too much"); break;
	case QC_ERR_GADDRESS:				snprintf (buf, size, "GADDRESS is not implemented"); break;
	case QC_ERR_STRING_CASE_RANGE:		snprintf (buf, size, "string CASERANGE is not supported"); break;
	case QC_ERR_NO_FREE_EDICTS:			snprintf (buf, size, "no free edicts"); break;
	case QC_ERR_OUT_OF_MEMORY:
		snprintf (buf, size, "out of memory (%s)", e->value >= 0 && e->value <= QC_RES_HASH_TABLES
			? resource_names[e->value] : "?");
		break;
	case QC_ERR_QC:
	case QC_ERR_BUILTIN:
	case QC_ERR_HOST:
	default:
		snprintf (buf, size, "%s", msg);
		break;
	}
	return buf;
}

// a frame as "name (file:line) @ statement n"
static int QC_FrameText (const qc_btframe_t *f, char *buf, size_t size)
{
	char	where[512] = "";

	if (*f->file)
	{
		if (f->line >= 0)
			snprintf (where, sizeof(where), " (%s:%lld)", f->file, (long long)f->line);
		else
			snprintf (where, sizeof(where), " (%s)", f->file);
	}
	return snprintf (buf, size, "%s%s @ statement %u", f->name, where, f->statement);
}

const char *QC_WarningText (const qc_warning_t *w, char *buf, size_t size)
{
	char	kind[512];
	int		n;

	switch (w->kind)
	{
	case QC_WARN_BAD_ENTITY:		snprintf (kind, sizeof(kind), "bad entity index %lld", (long long)w->value); break;
	case QC_WARN_BAD_FIELD:			snprintf (kind, sizeof(kind), "bad field offset %lld", (long long)w->value); break;
	case QC_WARN_READONLY_ENTITY:
		snprintf (kind, sizeof(kind), "write to protected entity %lld skipped", (long long)w->value);
		break;
	case QC_WARN_BAD_STRING:
		snprintf (kind, sizeof(kind), "invalid string reference 0x%llx", (unsigned long long)w->value);
		break;
	case QC_WARN_SUPPRESSED:		snprintf (kind, sizeof(kind), "%lld more warnings suppressed", (long long)w->value); break;
	case QC_WARN_BUILTIN:
	default:
		snprintf (kind, sizeof(kind), "%s", w->message ? w->message : "");
		break;
	}
	n = snprintf (buf, size, "%s", kind);
	if (w->backtrace.count && n >= 0 && (size_t)n < size)
	{
		n += snprintf (buf + n, size - (size_t)n, " in ");
		if ((size_t)n < size)
			QC_FrameText (&w->backtrace.frames[0], buf + n, size - (size_t)n);
	}
	return buf;
}

const char *QC_BacktraceText (const qc_backtrace_t *bt, char *buf, size_t size)
{
	size_t		len = 0;
	uint32_t	i;
	int			n;

	if (!size)
		return buf;
	buf[0] = 0;
	for (i = 0 ; i < bt->count && len + 1 < size ; i++)
	{
		n = snprintf (buf + len, size - len, "  ");
		if (n > 0)
			len += (size_t)n < size - len ? (size_t)n : size - len - 1;
		n = QC_FrameText (&bt->frames[i], buf + len, size - len);
		if (n > 0)
			len += (size_t)n < size - len ? (size_t)n : size - len - 1;
		n = snprintf (buf + len, size - len, "\n");
		if (n > 0)
			len += (size_t)n < size - len ? (size_t)n : size - len - 1;
	}
	return buf;
}
