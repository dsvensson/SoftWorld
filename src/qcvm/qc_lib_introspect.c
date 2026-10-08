// qc_lib_introspect.c -- the VM seen from QuakeC: checkbuiltin, isfunction,
// callfunction, the extern builtins, abort, tracing and dumps
// (docs/spec/builtins.md)

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

// arguments from on, as raw words to pass on
static int QC_RawArgs (const qcvm_t *vm, int from, qc_value_t args[8])
{
	int		argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, n = 0, i;

	for (i = from ; i < argc ; i++)
		args[n++] = QC_ArgValue (vm, i);
	return n;
}

// calls f, its result becoming the builtin's
static bool QC_CallRaw (qcvm_t *vm, qc_func_t f, int argc, const qc_value_t *args)
{
	qc_value_t	ret;

	if (!QC_Call (vm, f, argc, args, &ret))
		return false;
	QC_ReturnRaw (vm, ret.w);
	return true;
}

// float checkbuiltin(__variant funcref): whether it's a builtin this VM has
static bool QC_Checkbuiltin (qcvm_t *vm)
{
	QC_LibReturnBool (vm, QC_IsBuiltinBound (vm, QC_ArgWord (vm, 0)));
	return true;
}

// float isfunction(string name)
static bool QC_Isfunction (qcvm_t *vm)
{
	QC_LibReturnBool (vm, QC_LibFindFunction (vm, -2, QC_ArgString (vm, 0)) != 0);
	return true;
}

// void callfunction(..., string name): calls the function named last with the
// arguments before it, its result left as the builtin's; nothing if there's none
static bool QC_Callfunction (qcvm_t *vm)
{
	int			argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, i;
	qc_value_t	args[8];
	qc_func_t	f;

	if (argc < 1)
		return QC_Error (vm, "callfunction needs at least one argument");
	f = QC_LibFindFunction (vm, -2, QC_ArgString (vm, argc - 1));
	for (i = 0 ; i < argc - 1 ; i++)
		args[i] = QC_ArgValue (vm, i);
	return f ? QC_CallRaw (vm, f, argc - 1, args) : true;
}

// __variant externcall(float prnum, string name, ...): a function of another
// progs by name (prnum 0 the main one, -1 the running one, -2 any); without one,
// MissingFunc(name, ...) if the progs has that
static bool QC_Externcall (qcvm_t *vm)
{
	int32_t		prnum = QC_LibArgInt (vm, 0);
	uint32_t	nameref = QC_ArgWord (vm, 1);
	qc_value_t	args[8], with[8];
	int			n = QC_RawArgs (vm, 2, args), i;
	char		*name;
	qc_func_t	f;
	bool		ok;

	name = malloc (strlen (QC_ArgString (vm, 1)) + 1);
	if (!name)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	strcpy (name, QC_ArgString (vm, 1));
	if ((f = QC_LibFindFunction (vm, prnum, name)))
		ok = QC_CallRaw (vm, f, n, args);
	else if ((f = QC_LibFindFunction (vm, prnum, "MissingFunc")))
	{
		with[0] = QC_ValWord (nameref);
		for (i = 0 ; i < n && i < 7 ; i++)
			with[i + 1] = args[i];
		ok = QC_CallRaw (vm, f, i + 1, with);
	}
	else
		ok = QC_Error (vm, "Couldn't find function %s", name);
	free (name);
	return ok;
}

// __variant externrefcall(float prnum, __variant func, ...): calls a function
// reference with the rest of the arguments
static bool QC_Externrefcall (qcvm_t *vm)
{
	qc_value_t	args[8];
	int			n = QC_RawArgs (vm, 2, args);

	return QC_CallRaw (vm, QC_ArgWord (vm, 1), n, args);
}

// a global of the progs prnum names: its S byte offset and type
static bool QC_ExternGlobal (const qcvm_t *vm, int32_t prnum, const char *name, uint32_t *ofs, uint32_t *type)
{
	const qc_def_t	*d;
	uint32_t		pr, end;
	int64_t			at;

	QC_LibProgsRange (vm, prnum, &pr, &end);
	for ( ; pr < end ; pr++)
	{
		d = QC_GlobalDefRaw (vm->progs[pr].progs, name);
		if (!d)
			continue;
		at = QC_GlobalOffset (vm, pr, d->ofs);
		if (at < 0 || at > UINT32_MAX)
			continue;
		*ofs = (uint32_t)at;
		*type = d->type;
		return true;
	}
	return false;
}

// __variant externvalue(float prnum, string name, ...): a global's value (three
// words), or with a leading & its address; without such a global, the function
// of that name (or 0)
static bool QC_Externvalue (qcvm_t *vm)
{
	int32_t		prnum = QC_LibArgInt (vm, 0);
	char		*name = QC_LibConcat (vm, 1, NULL);
	uint32_t	w[3] = {0, 0, 0}, ofs, type, k;

	if (!name)
		return false;
	if (name[0] == '&')
	{
		if (QC_ExternGlobal (vm, prnum, name + 1, &ofs, &type))
			w[0] = ofs;
	}
	else if (QC_ExternGlobal (vm, prnum, name, &ofs, &type))
	{
		for (k = 0 ; k < 3 ; k++)
			w[k] = QC_GetS (&vm->mem, (uint64_t)ofs + (uint64_t)k * 4);
	}
	else
		w[0] = QC_LibFindFunction (vm, prnum, name);
	free (name);
	QC_ReturnRaw (vm, w);
	return true;
}

// void externset(float prnum, __variant value, string name, ...): writes a
// global of another progs (three words for a vector, two for the 64-bit types)
static bool QC_Externset (qcvm_t *vm)
{
	int32_t		prnum = QC_LibArgInt (vm, 0);
	qc_value_t	value = QC_ArgValue (vm, 1);
	char		*name = QC_LibConcat (vm, 2, NULL);
	uint32_t	ofs, type, words, k;

	if (!name)
		return false;
	if (QC_ExternGlobal (vm, prnum, name, &ofs, &type))
	{
		words = type == QC_EV_VECTOR ? 3 : type == QC_EV_INT64 || type == QC_EV_UINT64 || type == QC_EV_DOUBLE ? 2 : 1;
		for (k = 0 ; k < words ; k++)
			QC_SetS (&vm->mem, (uint64_t)ofs + (uint64_t)k * 4, value.w[k]);
	}
	free (name);
	return true;
}

// void abort(optional __variant ret): unwinds QuakeC to the host's call that
// started it, which returns ret
static bool QC_AbortBuiltin (qcvm_t *vm)
{
	return QC_Abort (vm, QC_Argc (vm) > 0 ? QC_ArgValue (vm, 0) : QC_ValWord (0));
}

static bool QC_Traceon (qcvm_t *vm)
{
	QC_SetTrace (vm, true);
	return true;
}

static bool QC_Traceoff (qcvm_t *vm)
{
	QC_SetTrace (vm, false);
	return true;
}

// void setwatchpoint(string name, float type, void *ptr): a warning (with the
// stack) whenever the value of the type at ptr changes, until the next call;
// a null ptr stops it
static bool QC_Setwatchpoint (qcvm_t *vm)
{
	uint32_t	ptr = QC_ArgWord (vm, 2);

	if (!QC_SetWatch (vm, ptr ? QC_ArgString (vm, 0) : NULL, (uint32_t)QC_LibArgInt (vm, 1), ptr))
		QC_Warning (vm, "setwatchpoint: %#x isn't memory to watch", ptr);
	return true;
}

// void breakpoint(): a "break statement" warning with the stack
static bool QC_Breakpoint (qcvm_t *vm)
{
	QC_Warning (vm, "break statement");
	return true;
}

static void QC_Dump (qcvm_t *vm, qc_dumpkind_t kind, qc_sink_t *s)
{
	if (vm->host.dump)
		vm->host.dump (vm->ctx, kind, QC_SinkText (s));
	QC_SinkFree (s);
}

// void coredump(): the call stack, the saved globals and every entity, to the
// host's dump
static bool QC_Coredump (qcvm_t *vm)
{
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_CoredumpText (vm, &s);
	QC_Dump (vm, QC_DUMP_COREDUMP, &s);
	return true;
}

// void eprint(entity e): the entity's non-zero fields, to the host's dump
static bool QC_Eprint (qcvm_t *vm)
{
	uint32_t	e = QC_LibEntArg (vm, 0);
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_SinkPrintf (&s, "Entity %u:\n", e);
	QC_EntityBlock (vm, &s, e);
	QC_SinkPush (&s, '\n');
	QC_Dump (vm, QC_DUMP_ENTITY, &s);
	return true;
}

// void stackdump() (menu): the QuakeC call stack, to the host's dump
static bool QC_Stackdump (qcvm_t *vm)
{
	qc_sink_t	s;

	QC_SinkInit (&s, SIZE_MAX);
	QC_BacktraceSink (vm, &s);
	QC_Dump (vm, QC_DUMP_TRACE, &s);
	return true;
}

// void crash() (menu): an error naming the builtin
static bool QC_Crash (qcvm_t *vm)
{
	const char	*name = "";

	QC_BuiltinFunction (vm, NULL, &name);
	return QC_Fail (vm, QC_ERR_QC, 0, "%s called", *name ? name : "?unknown?");
}

static const qc_libentry_t	qc_introspect[] = {
	{"checkbuiltin", QC_Checkbuiltin, NULL, 0},
	{"isfunction", QC_Isfunction, NULL, 0},
	{"callfunction", QC_Callfunction, NULL, 605},
	{"externcall", QC_Externcall, NULL, 0},
	{"externvalue", QC_Externvalue, NULL, 0},
	{"externset", QC_Externset, NULL, 0},
	{"externrefcall", QC_Externrefcall, NULL, 205},
	{"abort", QC_AbortBuiltin, NULL, 0},
	{"traceon", QC_Traceon, NULL, 0},
	{"traceoff", QC_Traceoff, NULL, 0},
	{"setwatchpoint", QC_Setwatchpoint, NULL, 0},
	{"breakpoint", QC_Breakpoint, NULL, 0},
	{"coredump", QC_Coredump, NULL, 0},
	{"eprint", QC_Eprint, NULL, 0},
	{"stackdump", QC_Stackdump, NULL, 0},
	{"crash", QC_Crash, NULL, 0},
};

bool QC_RegisterIntrospect (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_introspect, sizeof(qc_introspect) / sizeof(qc_introspect[0]));
}
