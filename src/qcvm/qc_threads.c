// qc_threads.c -- QuakeC threads: FTE's sleep and fork
//
// A thread is a snapshot of the QuakeC call stack from the nearest engine
// boundary up to the suspending builtin: the chain of activations, the part of
// the local stack they use (saved locals and pushed memory), and the current
// values of their locals. Resuming rebuilds the same frames, so the local
// stack's layout is the same, writes the captured words back, and carries on
// after the suspending call. Threads resume only when no QuakeC runs.

#include "qc_local.h"

#include <stdlib.h>

// one function activation of a sleeping thread
typedef struct
{
	uint32_t		prnum;
	uint32_t		func;
	uint32_t		pc;				// the statement to carry on at
	uint32_t		pushed;
	uint32_t		switch_ref;
	qc_switchkind_t	switch_kind;
} qc_activation_t;

// the captured locals of a function: where they are in S, and their bytes
typedef struct
{
	uint32_t	at, len;
} qc_localsrange_t;

// self or other when the thread went to sleep
typedef struct
{
	bool		present;
	uint32_t	at;				// the global's S offset
	uint32_t	ent;
	uint32_t	serial;
} qc_selfslot_t;

struct qc_thread_s
{
	float				wake;			// the value of time at which it wakes
	qc_activation_t		*chain;			// the root first
	uint32_t			numchain;
	uint8_t				*data;			// the captured local stack, then each function's locals
	size_t				len;
	size_t				stack_len;
	qc_localsrange_t	*locals;		// in data's order
	uint32_t			numlocals;
	uint32_t			resume[3];		// what the suspending call returns on waking
	qc_selfslot_t		selves[2];		// self and other
};

static void QC_FreeThread (qc_thread_t *t)
{
	free (t->chain);
	free (t->data);
	free (t->locals);
	*t = (qc_thread_t){0};
}

void QC_FreeThreads (qcvm_t *vm)
{
	uint32_t	i;

	for (i = 0 ; i < vm->numthreads ; i++)
		QC_FreeThread (&vm->threads[i]);
	free (vm->threads);
	vm->threads = NULL;
	vm->numthreads = vm->threadsize = 0;
}

void QC_MarkThreads (qcvm_t *vm, uint8_t *marks)
{
	uint32_t	i;

	for (i = 0 ; i < vm->numthreads ; i++)
		QC_GCMark (&vm->strings, marks, vm->threads[i].data, vm->threads[i].len);
}

uint32_t QC_SleepingThreads (const qcvm_t *vm)
{
	return vm->numthreads;
}

// the S offset of a global of progs pr by name, or -1
static int64_t QC_NamedGlobal (const qcvm_t *vm, uint32_t pr, const char *name)
{
	const qc_def_t	*d;

	if (pr >= vm->numprogs || !(d = QC_GlobalDefRaw (vm->progs[pr].progs, name)))
		return -1;
	return (int64_t)vm->progs[pr].gbase + (int64_t)d->ofs * 4;
}

// the main progs' time, by which threads wake
static float QC_ThreadTime (const qcvm_t *vm)
{
	int64_t	at = QC_NamedGlobal (vm, 0, "time");

	return at >= 0 ? QC_BitsFloat (QC_GetS (&vm->mem, (uint64_t)at)) : 0;
}

static bool QC_Push (qc_thread_t **threads, uint32_t *count, uint32_t *size, const qc_thread_t *t)
{
	qc_thread_t	*grown;
	uint32_t	n;

	if (*count == *size)
	{
		n = *size ? *size * 2 : 8;
		grown = realloc (*threads, (size_t)n * sizeof(*grown));
		if (!grown)
			return false;
		*threads = grown;
		*size = n;
	}
	(*threads)[(*count)++] = *t;
	return true;
}

/*
==============================================================================

SUSPENDING

==============================================================================
*/

typedef enum
{
	QC_SNAP_OK,
	QC_SNAP_NONE,			// no QuakeC runs
	QC_SNAP_NOMEM
} qc_snapresult_t;

// captures the running QuakeC thread, from the nearest engine boundary
static qc_snapresult_t QC_Snapshot (const qcvm_t *vm, qc_thread_t *t)
{
	const qc_mem_t			*m = &vm->mem;
	const qc_frame_t		*f;
	const qc_activation_t	*a;
	const qc_progstate_t	*ps;
	const qc_function_t		*fn;
	uint64_t				start, end, at, len, total;
	uint32_t				i, k, top, e;
	int64_t					g;

	if (vm->entry_depth >= vm->numframes)
		return QC_SNAP_NONE;
	start = m->ls_base + (uint64_t)vm->frames[vm->entry_depth].locals_at * 4;
	end = m->ls_base + ((uint64_t)vm->x.ls_top + vm->x.pushed) * 4;
	if (end < start || end > m->s_len)
		return QC_SNAP_NONE;

	t->chain = malloc (((size_t)vm->numframes - vm->entry_depth) * sizeof(*t->chain));
	t->locals = malloc (((size_t)vm->numframes - vm->entry_depth) * sizeof(*t->locals));
	if (!t->chain || !t->locals)
		return QC_SNAP_NOMEM;
	for (i = vm->entry_depth + 1 ; i < vm->numframes ; i++)
	{
		f = &vm->frames[i];
		t->chain[t->numchain++] = (qc_activation_t){f->prnum, f->func, f->resume_pc, f->pushed, f->switch_ref,
			f->switch_kind};
	}
	t->chain[t->numchain++] = (qc_activation_t){vm->x.prnum, vm->x.func, vm->x.pc, vm->x.pushed, vm->x.switch_ref,
		vm->x.switch_kind};

	// the locals of each function once
	total = t->stack_len = (size_t)(end - start);
	for (i = 0 ; i < t->numchain ; i++)
	{
		a = &t->chain[i];
		for (k = 0 ; k < i ; k++)
			if (t->chain[k].prnum == a->prnum && t->chain[k].func == a->func)
				break;
		if (k < i)
			continue;
		if (a->prnum >= vm->numprogs || a->func >= vm->progs[a->prnum].progs->numfunctions)
			return QC_SNAP_NONE;
		ps = &vm->progs[a->prnum];
		fn = &ps->progs->functions[a->func];
		at = ps->gbase + (uint64_t)fn->parm_start * 4;
		len = (uint64_t)fn->locals * 4;
		if (at + len > m->s_len)
			return QC_SNAP_NONE;
		t->locals[t->numlocals++] = (qc_localsrange_t){(uint32_t)at, (uint32_t)len};
		total += len;
	}
	t->data = malloc (total ? (size_t)total : 1);
	if (!t->data)
		return QC_SNAP_NOMEM;
	memcpy (t->data, m->s.base + start, t->stack_len);
	t->len = t->stack_len;
	for (i = 0 ; i < t->numlocals ; i++)
	{
		memcpy (t->data + t->len, m->s.base + t->locals[i].at, t->locals[i].len);
		t->len += t->locals[i].len;
	}

	top = t->chain[t->numchain - 1].prnum;
	for (i = 0 ; i < 2 ; i++)
		if ((g = QC_NamedGlobal (vm, top, i ? "other" : "self")) >= 0)
		{
			e = QC_GetS (m, (uint64_t)g);
			t->selves[i] = (qc_selfslot_t){true, (uint32_t)g, e, e < m->num_edicts ? m->slots[e].serial : 0};
		}
	return QC_SNAP_OK;
}

bool QC_Suspend (qcvm_t *vm, float delay, const uint32_t resume[3], bool *suspended)
{
	qc_thread_t	t = {0};
	size_t		held;
	uint32_t	i;

	*suspended = false;
	if (vm->numthreads >= vm->config.limits.threads)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_THREADS, NULL);
	t.wake = QC_ThreadTime (vm) + delay;
	memcpy (t.resume, resume, sizeof(t.resume));
	switch (QC_Snapshot (vm, &t))
	{
	case QC_SNAP_NONE:
		QC_FreeThread (&t);
		return true;
	case QC_SNAP_NOMEM:
		QC_FreeThread (&t);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_THREADS, NULL);
	default:
		break;
	}
	held = t.len;
	for (i = 0 ; i < vm->numthreads ; i++)
		held = held + vm->threads[i].len < held ? SIZE_MAX : held + vm->threads[i].len;
	if (held > vm->config.limits.thread_bytes || !QC_Push (&vm->threads, &vm->numthreads, &vm->threadsize, &t))
	{
		QC_FreeThread (&t);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_THREADS, NULL);
	}
	*suspended = true;
	return true;
}

/*
==============================================================================

RESUMING

==============================================================================
*/

// f32::total_cmp: -0 before +0, NaNs at the ends by sign
static int QC_TotalCompare (float a, float b)
{
	int32_t	x = (int32_t)QC_FloatBits (a), y = (int32_t)QC_FloatBits (b);

	x ^= (int32_t)((uint32_t)(x >> 31) >> 1);
	y ^= (int32_t)((uint32_t)(y >> 31) >> 1);
	return x < y ? -1 : x > y;
}

static bool QC_Resume (qcvm_t *vm, const qc_thread_t *t)
{
	qc_mem_t				*m = &vm->mem;
	const qc_activation_t	*a;
	const qc_selfslot_t		*s;
	uint32_t				depth = vm->numframes, resume_pc = vm->x.pc, i, k;
	uint64_t				start, gbase;
	size_t					at;
	bool					alive, ok;

	for (i = 0 ; i < t->numchain ; i++)
	{
		a = &t->chain[i];
		if (!QC_Enter (vm, a->prnum, a->func, resume_pc))
		{
			QC_Unwind (vm, depth);
			return false;
		}
		vm->x.pushed = a->pushed;
		vm->x.switch_ref = a->switch_ref;
		vm->x.switch_kind = a->switch_kind;
		resume_pc = a->pc;
	}
	vm->x.pc = resume_pc;

	// the rebuilt frames have the captured layout: the captured words go back
	start = m->ls_base + (uint64_t)(depth < vm->numframes ? vm->frames[depth].locals_at : 0) * 4;
	if (start + t->stack_len <= m->s_len)
		memcpy (m->s.base + start, t->data, t->stack_len);
	at = t->stack_len;
	for (i = 0 ; i < t->numlocals ; i++)
	{
		if ((uint64_t)t->locals[i].at + t->locals[i].len <= m->s_len)
			memcpy (m->s.base + t->locals[i].at, t->data + at, t->locals[i].len);
		at += t->locals[i].len;
	}
	gbase = QC_GBase (vm);
	for (k = 0 ; k < 3 ; k++)
		QC_SetS (m, gbase + QC_OFS_RETURN + k * 4, t->resume[k]);
	for (i = 0 ; i < 2 ; i++)
	{
		s = &t->selves[i];
		if (!s->present)
			continue;
		alive = QC_InUse (m, s->ent) && m->slots[s->ent].serial == s->serial;
		QC_SetS (m, s->at, alive ? s->ent : 0);
	}
	ok = QC_Execute (vm, depth);
	// a resumed thread has no caller to return to: the result abort (or sleep
	// suspending it again) left for one is dropped, so the next host call reads its own
	vm->has_abort_ret = false;
	return ok;
}

bool QC_RunThreads (qcvm_t *vm, uint32_t *ran)
{
	qc_thread_t	*due = NULL, t;
	uint32_t	numdue = 0, duesize = 0, kept = 0, i, k;
	float		now;
	bool		ok = true;

	if (ran)
		*ran = 0;
	if (vm->nesting)
		return QC_Fail (vm, QC_ERR_HOST, 0, "QC_RunThreads called while QuakeC is running");

	// take out those due, the rest keeping their order
	now = QC_ThreadTime (vm);
	for (i = 0 ; i < vm->numthreads ; i++)
	{
		if (!(vm->threads[i].wake <= now))
			vm->threads[kept++] = vm->threads[i];
		else if (!QC_Push (&due, &numdue, &duesize, &vm->threads[i]))
		{
			// out of memory: leave the rest asleep
			for ( ; i < vm->numthreads ; i++)
				vm->threads[kept++] = vm->threads[i];
			break;
		}
	}
	vm->numthreads = kept;

	// in wake order (a stable sort)
	for (i = 1 ; i < numdue ; i++)
	{
		t = due[i];
		for (k = i ; k > 0 && QC_TotalCompare (due[k - 1].wake, t.wake) > 0 ; k--)
			due[k] = due[k - 1];
		due[k] = t;
	}

	for (i = 0 ; i < numdue ; i++)
	{
		// each is a host call of its own
		QC_StartBudgets (vm);
		vm->nesting++;
		ok = QC_Resume (vm, &due[i]);
		vm->nesting--;
		QC_FlushWarnings (vm);
		QC_FreeThread (&due[i]);
		if (!ok)
		{
			// those not yet resumed stay queued
			for (k = i + 1 ; k < numdue ; k++)
				if (!QC_Push (&vm->threads, &vm->numthreads, &vm->threadsize, &due[k]))
					QC_FreeThread (&due[k]);
			break;
		}
		if (ran)
			(*ran)++;
	}
	free (due);
	if (ok && QC_WantsCollection (&vm->strings))
		QC_CollectNow (vm);
	return ok;
}
