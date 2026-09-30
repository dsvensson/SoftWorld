// qc_call.c -- calls from the host into QuakeC, and builtins
//
// The interpreter (qc_exec.c) leaves its loop for a builtin, an animation
// opcode, a statement to trace, a spent budget or a fault; the loop here does
// what that needs and re-enters. Builtins may call back into QuakeC to any
// depth up to the re-entrancy limit: all that must survive lives in the VM.

#include "qc_builtins.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// instructions the interpreter runs between deadline checks
#define BUDGET_CHUNK	(1u << 16)

typedef enum
{
	QC_FLOW_CONTINUE,
	QC_FLOW_RETURN,			// abort: back at the host's call
	QC_FLOW_ERROR
} qc_flow_t;

static double QC_HostClock (const qcvm_t *vm)
{
	struct timespec	ts;

	if (vm->host.clock)
		return vm->host.clock (vm->ctx);
	if (!timespec_get (&ts, TIME_UTC))
		return 0;
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void QC_StartBudgets (qcvm_t *vm)
{
	vm->budget = vm->config.limits.runaway;
	vm->has_deadline = vm->config.limits.deadline > 0;
	if (vm->has_deadline)
		vm->deadline = QC_HostClock (vm) + vm->config.limits.deadline;
}

// gives the error a backtrace (unless it has one) and unwinds to depth
static bool QC_Failed (qcvm_t *vm, uint32_t depth)
{
	if (!vm->error.backtrace.count)
	{
		QC_FreeBacktrace (&vm->error.backtrace);
		QC_Backtrace (vm, &vm->error.backtrace);
	}
	QC_Unwind (vm, depth);
	return false;
}

/*
==============================================================================

ARGUMENTS AND RESULTS

==============================================================================
*/

void QC_ArgRaw (const qcvm_t *vm, int i, uint32_t out[3])
{
	uint64_t	at = (uint64_t)vm->progs[vm->x.prnum].gbase + QC_OFS_PARM0 + (uint64_t)i * 12;

	if (i < 0 || i >= 8)
	{
		out[0] = out[1] = out[2] = 0;
		return;
	}
	out[0] = QC_GetS (&vm->mem, at);
	out[1] = QC_GetS (&vm->mem, at + 4);
	out[2] = QC_GetS (&vm->mem, at + 8);
}

void QC_ReturnRaw (qcvm_t *vm, const uint32_t w[3])
{
	uint64_t	at = (uint64_t)vm->progs[vm->x.prnum].gbase + QC_OFS_RETURN;

	QC_SetS (&vm->mem, at, w[0]);
	QC_SetS (&vm->mem, at + 4, w[1]);
	QC_SetS (&vm->mem, at + 8, w[2]);
}

int QC_Argc (const qcvm_t *vm)
{
	return (int)vm->argc;
}

qc_value_t QC_ArgValue (const qcvm_t *vm, int i)
{
	qc_value_t	v;

	QC_ArgRaw (vm, i, v.w);
	return v;
}

uint32_t QC_ArgWord (const qcvm_t *vm, int i)
{
	qc_value_t	v = QC_ArgValue (vm, i);

	return v.w[0];
}

float QC_ArgFloat (const qcvm_t *vm, int i)
{
	return QC_BitsFloat (QC_ArgWord (vm, i));
}

int32_t QC_ArgInt (const qcvm_t *vm, int i)
{
	return (int32_t)QC_ArgWord (vm, i);
}

void QC_ArgVector (const qcvm_t *vm, int i, float out[3])
{
	qc_value_t	v = QC_ArgValue (vm, i);

	out[0] = QC_BitsFloat (v.w[0]);
	out[1] = QC_BitsFloat (v.w[1]);
	out[2] = QC_BitsFloat (v.w[2]);
}

const char *QC_ArgString (qcvm_t *vm, int i)
{
	return QC_String (vm, QC_ArgWord (vm, i));
}

void QC_ReturnValue (qcvm_t *vm, qc_value_t v)
{
	QC_ReturnRaw (vm, v.w);
}

void QC_ReturnFloat (qcvm_t *vm, float f)
{
	QC_ReturnValue (vm, QC_ValFloat (f));
}

void QC_ReturnInt (qcvm_t *vm, int32_t i)
{
	QC_ReturnValue (vm, QC_ValInt (i));
}

void QC_ReturnWord (qcvm_t *vm, uint32_t u)
{
	QC_ReturnValue (vm, QC_ValWord (u));
}

void QC_ReturnVector (qcvm_t *vm, const float v[3])
{
	QC_ReturnValue (vm, QC_ValVector (v[0], v[1], v[2]));
}

bool QC_ReturnString (qcvm_t *vm, const char *text, size_t len)
{
	uint32_t	r = QC_NewTemp (vm, text, len);

	if (!r)
		return false;
	QC_ReturnWord (vm, r);
	return true;
}

qc_func_t QC_BuiltinFunction (const qcvm_t *vm, uint32_t *number, const char **name)
{
	uint32_t			pr = QC_FUNC_PROGS (vm->builtin), index = QC_FUNC_INDEX (vm->builtin);
	const qc_function_t	*f = NULL;

	if (pr < vm->numprogs && index < vm->progs[pr].progs->numfunctions)
		f = &vm->progs[pr].progs->functions[index];
	if (number)
		*number = f && f->kind == QC_FUNC_BUILTIN ? f->number : 0;
	if (name)
		*name = f ? QC_Cstr (vm->progs[pr].progs, f->name) : "";
	return vm->builtin;
}

static char *QC_Message (const char *fmt, va_list args)
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

bool QC_Error (qcvm_t *vm, const char *fmt, ...)
{
	va_list	args;

	QC_Fail (vm, QC_ERR_BUILTIN, 0, NULL);
	va_start (args, fmt);
	vm->error.message = QC_Message (fmt, args);
	va_end (args);
	return false;
}

bool QC_HostError (qcvm_t *vm, const char *fmt, ...)
{
	va_list	args;

	QC_Fail (vm, QC_ERR_HOST, 0, NULL);
	va_start (args, fmt);
	vm->error.message = QC_Message (fmt, args);
	va_end (args);
	return false;
}

bool QC_Abort (qcvm_t *vm, qc_value_t ret)
{
	vm->aborting = true;
	memcpy (vm->abort_ret, ret.w, sizeof(vm->abort_ret));
	return false;
}

void QC_Warning (qcvm_t *vm, const char *fmt, ...)
{
	va_list	args;
	char	*text;

	va_start (args, fmt);
	text = QC_Message (fmt, args);
	va_end (args);
	QC_Warn (vm, QC_WARN_BUILTIN, 0, "%s", text ? text : "");
	free (text);
}

/*
==============================================================================

THE OUTER LOOP

==============================================================================
*/

// calls builtin slot, applying FTE's rules for builtin errors
static qc_flow_t QC_RunBuiltin (qcvm_t *vm, uint32_t slot, qc_func_t func, uint32_t exit_depth)
{
	qc_builtin_t	f = QC_BuiltinSlot (vm->builtins, slot);
	static const uint32_t	zero[3] = {0, 0, 0};
	char			text[1024];

	if (!f)
	{
		QC_MissingBuiltin (vm, func);
		QC_Failed (vm, exit_depth);
		return QC_FLOW_ERROR;
	}
	vm->builtin = func;
	vm->aborting = false;
	QC_ClearError (&vm->error);
	if (f (vm))
		return QC_FLOW_CONTINUE;
	if (vm->aborting)
	{
		// unwind to this host call, which returns normally
		vm->aborting = false;
		vm->has_abort_ret = true;
		QC_ClearError (&vm->error);
		QC_Unwind (vm, exit_depth);
		return QC_FLOW_RETURN;
	}
	if (vm->error.kind == QC_ERR_NONE)
		QC_Fail (vm, QC_ERR_HOST, 0, "builtin failed");
	if (vm->config.developer && vm->error.kind == QC_ERR_BUILTIN)
	{
		// FTE's developer mode: only a warning, with a zero result
		QC_ErrorText (&vm->error, text, sizeof(text));
		QC_ClearError (&vm->error);
		QC_Warn (vm, QC_WARN_BUILTIN, 0, "%s", text);
		QC_ReturnRaw (vm, zero);
		QC_FlushWarnings (vm);
		return QC_FLOW_CONTINUE;
	}
	QC_Failed (vm, exit_depth);
	return QC_FLOW_ERROR;
}

static void QC_SetField1 (qcvm_t *vm, uint32_t e, int64_t field, uint32_t v)
{
	uint8_t	*p;

	if (field >= 0 && (p = QC_FieldPtr (&vm->mem, e, (uint32_t)field, 1)))
		memcpy (p, &v, 4);
}

// FTE's CSQC behaviour for the animation opcodes
static void QC_DefaultStateOp (qcvm_t *vm, const qc_stateop_t *op)
{
	const qc_statehandles_t	*h = &vm->progs[vm->x.prnum].state;
	float					time, step = vm->config.state_step, cur, lo, hi, dir, next, n;
	uint32_t				self, e, w;
	int64_t					frame_f;
	uint8_t					*p;

	if (h->self_g < 0 || h->time_g < 0)
	{
		QC_Warn (vm, QC_WARN_BUILTIN, 0, "state opcode needs `self` and `time`");
		return;
	}
	time = QC_BitsFloat (QC_GetS (&vm->mem, (uint64_t)h->time_g));
	self = QC_GetS (&vm->mem, (uint64_t)h->self_g);
	switch (op->kind)
	{
	case QC_STATE_STATE:
		QC_SetField1 (vm, self, h->nextthink_f, QC_FloatBits (time + step));
		QC_SetField1 (vm, self, h->think_f, op->func);
		QC_SetField1 (vm, self, h->frame_f, QC_FloatBits (op->frame));
		break;
	case QC_STATE_CSTATE:
	case QC_STATE_CWSTATE:
		QC_SetField1 (vm, self, h->nextthink_f, QC_FloatBits (time + step));
		QC_SetField1 (vm, self, h->think_f, op->func);
		if (h->cycle_wrapped_g >= 0)
			QC_SetS (&vm->mem, (uint64_t)h->cycle_wrapped_g, 0);
		frame_f = op->kind == QC_STATE_CSTATE ? h->frame_f : h->weaponframe_f;
		if (frame_f < 0 || !(p = QC_FieldPtr (&vm->mem, self, (uint32_t)frame_f, 1)))
			return;
		memcpy (&w, p, 4);
		cur = QC_BitsFloat (w);
		if (op->first > op->last)
		{
			lo = op->last;
			hi = op->first;
			dir = -1;
		}
		else
		{
			lo = op->first;
			hi = op->last;
			dir = 1;
		}
		if (cur < lo || cur > hi)
			next = op->first;
		else
		{
			n = cur + dir;
			if (n < lo || n > hi)
			{
				if (h->cycle_wrapped_g >= 0)
					QC_SetS (&vm->mem, (uint64_t)h->cycle_wrapped_g, QC_FloatBits (1));
				next = op->first;
			}
			else
				next = n;
		}
		w = QC_FloatBits (next);
		memcpy (p, &w, 4);
		break;
	case QC_STATE_THINKTIME:
		e = op->ent < vm->mem.num_edicts ? op->ent : 0;
		QC_SetField1 (vm, e, h->nextthink_f, QC_FloatBits (time + op->delay));
		break;
	}
}

// the statement about to run, for the host's trace: "function: disassembly"
static void QC_TraceLine (qcvm_t *vm)
{
	const qc_progs_t	*p = vm->progs[vm->x.prnum].progs;
	char				stmt[1024], line[1200];

	QC_DisassembleStatement (p, vm->x.pc, stmt, sizeof(stmt));
	snprintf (line, sizeof(line), "%s: %s", vm->x.func < p->numfunctions
		? QC_Cstr (p, p->functions[vm->x.func].name) : "?", stmt);
	if (vm->host.trace)
		vm->host.trace (vm->ctx, line);
}

static bool QC_ExecuteInner (qcvm_t *vm, uint32_t exit_depth)
{
	qc_exit_t	exit;
	uint32_t	chunk, left;
	bool		handled;

	for ( ; ; )
	{
		// with a deadline the interpreter gets the budget in chunks, and the
		// deadline is checked between them
		chunk = vm->has_deadline && vm->budget > BUDGET_CHUNK ? BUDGET_CHUNK : vm->budget;
		left = chunk;
		exit = QC_Run (vm, exit_depth, &left);
		if (exit.kind != QC_EXIT_BUDGET)
			vm->budget -= chunk - left;
		switch (exit.kind)
		{
		case QC_EXIT_BUDGET:
			if (vm->budget <= chunk)
			{
				vm->budget = 0;
				QC_FlushWarnings (vm);
				QC_Fail (vm, QC_ERR_RUNAWAY, 0, NULL);
				return QC_Failed (vm, exit_depth);
			}
			// the count that ran out is taken again when the statement runs
			vm->budget -= chunk - 1;
			if (vm->has_deadline && QC_HostClock (vm) >= vm->deadline)
			{
				QC_FlushWarnings (vm);
				QC_Fail (vm, QC_ERR_DEADLINE, 0, NULL);
				return QC_Failed (vm, exit_depth);
			}
			break;
		case QC_EXIT_RETURNED:
			QC_FlushWarnings (vm);
			return true;
		case QC_EXIT_BUILTIN:
			QC_FlushWarnings (vm);
			switch (QC_RunBuiltin (vm, exit.slot, exit.func, exit_depth))
			{
			case QC_FLOW_ERROR:
				return false;
			case QC_FLOW_RETURN:
				return true;
			default:
				break;
			}
			break;
		case QC_EXIT_STATEOP:
			handled = false;
			if (vm->host.state_op && !vm->host.state_op (vm->ctx, vm, &exit.op, &handled))
			{
				if (vm->error.kind == QC_ERR_NONE)
					QC_Fail (vm, QC_ERR_HOST, 0, "state opcode failed");
				return QC_Failed (vm, exit_depth);
			}
			if (!handled)
				QC_DefaultStateOp (vm, &exit.op);
			break;
		case QC_EXIT_FAULT:
			QC_FlushWarnings (vm);
			return QC_Failed (vm, exit_depth);
		case QC_EXIT_TRACE:
			QC_TraceLine (vm);
			break;
		}
	}
}

bool QC_Execute (qcvm_t *vm, uint32_t exit_depth)
{
	uint32_t	saved = vm->entry_depth;
	bool		ok;

	vm->entry_depth = exit_depth;
	ok = QC_ExecuteInner (vm, exit_depth);
	vm->entry_depth = saved;
	return ok;
}

bool QC_Call (qcvm_t *vm, qc_func_t f, int argc, const qc_value_t *args, qc_value_t *ret)
{
	uint32_t			pr = QC_FUNC_PROGS (f), index = QC_FUNC_INDEX (f), args_pr, depth, saved_prnum;
	uint32_t			saved_argc, i, k;
	qc_func_t			saved_builtin;
	const qc_callee_t	*callee = NULL;
	bool				top_level, ok;
	uint64_t			at;
	uint32_t			r[3];
	qc_warning_t		*grown;

	if (vm->nesting >= vm->config.limits.reentry)
		return QC_Fail (vm, QC_ERR_REENTRANCY, 0, NULL);
	if (pr < vm->numprogs && index < vm->progs[pr].progs->numfunctions)
		callee = &vm->progs[pr].callees[index];
	top_level = !vm->nesting;
	if (top_level)
	{
		vm->warnings_this_call = 0;
		vm->suppressed = 0;
		QC_StartBudgets (vm);
	}
	saved_argc = vm->argc;
	saved_builtin = vm->builtin;

	// QuakeC takes its arguments from the calling context's PARM slots (entering
	// a function of another progs copies them over); builtins read their own
	// progs' slots
	args_pr = callee && callee->kind == QC_CALLEE_QC ? vm->x.prnum : pr;
	if (argc < 0 || argc > 8)
		return QC_Fail (vm, QC_ERR_TOO_MANY_ARGUMENTS, argc, NULL);
	if (args_pr < vm->numprogs)
		for (i = 0 ; i < (uint32_t)argc ; i++)
		{
			at = (uint64_t)vm->progs[args_pr].gbase + QC_OFS_PARM0 + (uint64_t)i * 12;
			for (k = 0 ; k < 3 ; k++)
				QC_SetS (&vm->mem, at + k * 4, args[i].w[k]);
		}
	vm->argc = (uint32_t)argc;
	vm->nesting++;

	if (!callee)
		ok = QC_Fail (vm, QC_ERR_INVALID_FUNCTION, f, NULL);
	else switch (callee->kind)
	{
	case QC_CALLEE_QC:
		depth = vm->numframes;
		if (QC_Enter (vm, pr, index, vm->x.pc))
			ok = QC_Execute (vm, depth);
		else
			ok = QC_Failed (vm, depth);
		break;
	case QC_CALLEE_BUILTIN:
		saved_prnum = vm->x.prnum;
		vm->x.prnum = pr;
		ok = QC_RunBuiltin (vm, callee->slot, f, vm->numframes) != QC_FLOW_ERROR;
		vm->x.prnum = saved_prnum;
		break;
	case QC_CALLEE_NULL:
		ok = QC_Fail (vm, QC_ERR_NULL_FUNCTION, 0, NULL);
		break;
	case QC_CALLEE_MISSING:
		QC_MissingBuiltin (vm, f);
		ok = QC_Failed (vm, vm->numframes);
		break;
	case QC_CALLEE_INVALID:
	default:
		ok = QC_Fail (vm, QC_ERR_INVALID_FUNCTION, f, NULL);
		break;
	}

	vm->nesting--;
	vm->argc = saved_argc;
	vm->builtin = saved_builtin;
	if (vm->has_abort_ret)
	{
		memcpy (r, vm->abort_ret, sizeof(r));
		vm->has_abort_ret = false;
	}
	else
	{
		at = (uint64_t)vm->progs[pr < vm->numprogs ? args_pr : 0].gbase + QC_OFS_RETURN;
		for (k = 0 ; k < 3 ; k++)
			r[k] = QC_GetS (&vm->mem, at + k * 4);
	}
	if (ret)
		memcpy (ret->w, r, sizeof(r));
	if (top_level)
	{
		if (vm->suppressed)
		{
			if (vm->numwarnings == vm->warningsize)
			{
				k = vm->warningsize ? vm->warningsize * 2 : 16;
				grown = realloc (vm->warnings, (size_t)k * sizeof(*grown));
				if (grown)
				{
					vm->warnings = grown;
					vm->warningsize = k;
				}
			}
			if (vm->numwarnings < vm->warningsize)
				vm->warnings[vm->numwarnings++] = (qc_warning_t){.kind = QC_WARN_SUPPRESSED, .value = vm->suppressed};
		}
		QC_FlushWarnings (vm);
		if (QC_WantsCollection (&vm->strings))
			QC_CollectNow (vm);
	}
	return ok;
}

bool QC_CallAs (qcvm_t *vm, qc_ent_t self, qc_func_t f, int argc, const qc_value_t *args, qc_value_t *ret)
{
	int64_t		self_g = vm->progs[0].state.self_g;
	uint32_t	saved;
	bool		ok;

	if (self_g < 0)
		return QC_Call (vm, f, argc, args, ret);
	saved = QC_GetS (&vm->mem, (uint64_t)self_g);
	QC_SetS (&vm->mem, (uint64_t)self_g, self);
	ok = QC_Call (vm, f, argc, args, ret);
	QC_SetS (&vm->mem, (uint64_t)self_g, saved);
	return ok;
}

void QC_Abandon (qcvm_t *vm)
{
	QC_Unwind (vm, 0);
	vm->x = (qc_exec_t){.func = QC_NO_FUNCTION};
	vm->nesting = 0;
	vm->entry_depth = 0;
	vm->argc = 0;
	vm->builtin = 0;
	vm->aborting = false;
	vm->has_abort_ret = false;
	vm->traced = false;
	// the warnings of the abandoned call are dropped
	{
		void	(*warning) (void *, const qc_warning_t *) = vm->host.warning;

		vm->host.warning = NULL;
		QC_FlushWarnings (vm);
		vm->host.warning = warning;
	}
}

/*
==============================================================================

LOOKUPS

==============================================================================
*/

qc_func_t QC_FindFunctionIn (const qcvm_t *vm, uint32_t pr, const char *name)
{
	const qc_progs_t	*p;
	const qc_def_t		*d;
	int64_t				word = -1;
	uint32_t			i, v, index;

	if (pr >= vm->numprogs)
		return 0;
	p = vm->progs[pr].progs;
	d = QC_GlobalDefRaw (p, name);
	if (d && d->type == QC_EV_FUNCTION)
		word = d->ofs;
	else if (d)
	{
		// a field or variable may share the name: look for the function global itself
		for (i = 0 ; i < p->numglobaldefs ; i++)
			if (p->globaldefs[i].type == QC_EV_FUNCTION && !strcmp (QC_Cstr (p, p->globaldefs[i].name), name))
			{
				word = p->globaldefs[i].ofs;
				break;
			}
	}
	if (word >= 0)
	{
		v = QC_GetS (&vm->mem, (uint64_t)vm->progs[pr].gbase + (uint64_t)word * 4);
		return QC_FUNC_INDEX (v) ? v : 0;
	}
	if (!QC_MapGet (&p->functions_by_name, name, strlen (name), &index))
		return 0;
	return QC_FUNC (pr, index);
}

qc_func_t QC_FindFunction (const qcvm_t *vm, const char *name)
{
	return QC_FindFunctionIn (vm, 0, name);
}

void QC_SetTrace (qcvm_t *vm, bool on)
{
	vm->trace = on;
}

bool QC_IsTracing (const qcvm_t *vm)
{
	return vm->trace;
}

void QC_SetProfiling (qcvm_t *vm, bool on)
{
	uint32_t	i;

	for (i = 0 ; on && i < vm->numprogs ; i++)
		if (!vm->progs[i].profile)
		{
			vm->progs[i].profile = calloc ((size_t)vm->progs[i].progs->numfunctions + 1, sizeof(uint64_t));
			if (!vm->progs[i].profile)
				on = false;
		}
	vm->profiling = on;
}

const uint64_t *QC_Profile (const qcvm_t *vm, uint32_t pr, uint32_t *count)
{
	if (pr >= vm->numprogs)
		return NULL;
	*count = vm->progs[pr].progs->numfunctions;
	return vm->progs[pr].profile;
}

void QC_ClearProfile (qcvm_t *vm)
{
	uint32_t	i;

	for (i = 0 ; i < vm->numprogs ; i++)
		if (vm->progs[i].profile)
			memset (vm->progs[i].profile, 0, (size_t)vm->progs[i].progs->numfunctions * sizeof(uint64_t));
}

const char *QC_CallerName (const qcvm_t *vm)
{
	const qc_progs_t	*p;

	if (vm->x.func == QC_NO_FUNCTION || vm->x.prnum >= vm->numprogs)
		return "";
	p = vm->progs[vm->x.prnum].progs;
	return vm->x.func < p->numfunctions ? QC_Cstr (p, p->functions[vm->x.func].name) : "";
}

bool QC_IsBuiltinBound (const qcvm_t *vm, qc_func_t f)
{
	uint32_t	pr = QC_FUNC_PROGS (f), index = QC_FUNC_INDEX (f);

	return pr < vm->numprogs && index < vm->progs[pr].progs->numfunctions
		&& vm->progs[pr].callees[index].kind == QC_CALLEE_BUILTIN;
}

uint32_t QC_UnboundBuiltins (const qcvm_t *vm, bool reachable, qc_unbound_t *out, uint32_t max)
{
	const qc_progstate_t	*ps;
	const qc_function_t		*f;
	uint32_t				pr, i, n = 0, ncalled, *called;
	bool					found;

	for (pr = 0 ; pr < vm->numprogs ; pr++)
	{
		ps = &vm->progs[pr];
		called = NULL;
		ncalled = 0;
		if (reachable)
		{
			ncalled = QC_ProgsCalledBuiltins (ps->progs, NULL, 0);
			called = malloc (((size_t)ncalled + 1) * sizeof(*called));
			if (!called)
				continue;
			QC_ProgsCalledBuiltins (ps->progs, called, ncalled);
		}
		for (i = 0 ; i < ps->progs->numfunctions ; i++)
		{
			if (ps->callees[i].kind != QC_CALLEE_MISSING)
				continue;
			if (reachable)
			{
				uint32_t	lo = 0, hi = ncalled, mid;

				found = false;
				while (lo < hi && !found)
				{
					mid = lo + (hi - lo) / 2;
					if (called[mid] == i)
						found = true;
					else if (called[mid] < i)
						lo = mid + 1;
					else
						hi = mid;
				}
				if (!found)
					continue;
			}
			f = &ps->progs->functions[i];
			if (n < max)
				out[n] = (qc_unbound_t){QC_FUNC (pr, i), f->kind == QC_FUNC_BUILTIN ? f->number : 0,
					QC_Cstr (ps->progs, f->name)};
			n++;
		}
		free (called);
	}
	return n;
}
