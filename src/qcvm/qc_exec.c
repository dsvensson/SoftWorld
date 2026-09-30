// qc_exec.c -- the interpreter: its loop (qc_exec_loop.h), and entering and
// leaving QuakeC functions
//
// Semantics follow docs/qcvm/vm.md; the deliberate differences from FTE are in
// docs/qcvm/deviations.md.

#include "qc_local.h"

static inline qc_exit_t QC_ExitOf (qc_exitkind_t kind)
{
	return (qc_exit_t){.kind = kind};
}

// x86's shifts: the count masked, a signed shift arithmetic
static inline uint32_t QC_Sar32 (uint32_t x, uint32_t n)
{
	n &= 31;
	return (x & 0x80000000u) && n ? (x >> n) | ~(UINT32_MAX >> n) : x >> n;
}

static inline uint64_t QC_Sar64 (uint64_t x, uint32_t n)
{
	n &= 63;
	return (x >> 63) && n ? (x >> n) | ~(UINT64_MAX >> n) : x >> n;
}

// correctly rounded, whatever the compiler makes of an unsigned 64-bit conversion
static inline float QC_U64ToFloat (uint64_t x)
{
	if (x < ((uint64_t)1 << 63))
		return (float)(int64_t)x;
	return (float)(int64_t)((x >> 1) | (x & 1)) * 2.0f;
}

static inline double QC_U64ToDouble (uint64_t x)
{
	if (x < ((uint64_t)1 << 63))
		return (double)(int64_t)x;
	return (double)(int64_t)((x >> 1) | (x & 1)) * 2.0;
}

// SplitMix64, 15 bits of it as C's rand () & 0x7fff
uint32_t QC_Rand15 (qcvm_t *vm)
{
	uint64_t	z = (vm->rng += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	return (uint32_t)((z >> 33) & 0x7FFF);
}

/*
==============================================================================

CALLS AND RETURNS

==============================================================================
*/

// copies words within region S in increasing order, each checked, as FTE's
// copies overlap
static void QC_CopyS (qc_mem_t *m, uint64_t src, uint64_t dst, uint32_t words)
{
	uint32_t	k;

	for (k = 0 ; k < words ; k++)
		QC_SetS (m, dst + (uint64_t)k * 4, QC_GetS (m, src + (uint64_t)k * 4));
}

// a call passing from one progs to another: PARM0-7 and the shared globals
static void QC_SwitchIn (qcvm_t *vm, uint32_t from, uint32_t to)
{
	const qc_progstate_t	*x = &vm->progs[from], *y = &vm->progs[to];
	uint32_t				i, n;

	QC_CopyS (&vm->mem, x->gbase + QC_OFS_PARM0, y->gbase + QC_OFS_PARM0, 24);
	n = x->numshared < y->numshared ? x->numshared : y->numshared;
	for (i = 0 ; i < n ; i++)
		if (x->shared[i].present && y->shared[i].present)
			QC_CopyS (&vm->mem, x->shared[i].offset, y->shared[i].offset,
				x->shared[i].words < y->shared[i].words ? x->shared[i].words : y->shared[i].words);
}

// and a return: RETURN and the shared globals
static void QC_SwitchOut (qcvm_t *vm, uint32_t from, uint32_t to)
{
	const qc_progstate_t	*x = &vm->progs[from], *y = &vm->progs[to];
	uint32_t				i, n;

	QC_CopyS (&vm->mem, x->gbase + QC_OFS_RETURN, y->gbase + QC_OFS_RETURN, 3);
	n = x->numshared < y->numshared ? x->numshared : y->numshared;
	for (i = 0 ; i < n ; i++)
		if (x->shared[i].present && y->shared[i].present)
			QC_CopyS (&vm->mem, x->shared[i].offset, y->shared[i].offset,
				x->shared[i].words < y->shared[i].words ? x->shared[i].words : y->shared[i].words);
}

void QC_SwitchProgs (qcvm_t *vm, uint32_t from, uint32_t to, bool in)
{
	if (from < vm->numprogs && to < vm->numprogs)
		(in ? QC_SwitchIn : QC_SwitchOut) (vm, from, to);
}

// memmove within S, false if out of range
static bool QC_MoveS (qc_mem_t *m, uint64_t src, uint64_t dst, uint64_t len)
{
	if (src + len > m->s_len || dst + len > m->s_len)
		return false;
	memmove (m->s.base + dst, m->s.base + src, (size_t)len);
	return true;
}

// Enters QuakeC function index of progs prnum, resuming the current context at
// resume_pc when it returns: the caller's pushed memory is kept, the callee's
// locals saved on the local stack, and its parameters copied in.
bool QC_Enter (qcvm_t *vm, uint32_t prnum, uint32_t index, uint32_t resume_pc)
{
	const qc_progstate_t	*ps;
	const qc_qcfunc_t		*f;
	uint64_t				ls_top, new_top;
	uint32_t				c, v;

	if (vm->numframes >= vm->config.limits.call_depth)
		return QC_Fail (vm, QC_ERR_CALL_DEPTH, 0, NULL);
	if (prnum != vm->x.prnum && vm->numprogs > 1)
		QC_SwitchProgs (vm, vm->x.prnum, prnum, true);
	if (prnum >= vm->numprogs || index >= vm->progs[prnum].progs->numfunctions
		|| vm->progs[prnum].funcs[index].entry == UINT32_MAX)
		return QC_Fail (vm, QC_ERR_INVALID_FUNCTION, QC_FUNC (prnum, index), NULL);
	ps = &vm->progs[prnum];
	f = &ps->funcs[index];

	ls_top = (uint64_t)vm->x.ls_top + vm->x.pushed;
	new_top = ls_top + f->locals_words;
	if (new_top > vm->mem.ls_words)
		return QC_Fail (vm, QC_ERR_LOCAL_STACK, 0, NULL);
	if (!QC_MoveS (&vm->mem, f->locals_addr, vm->mem.ls_base + ls_top * 4, (uint64_t)f->locals_words * 4))
		return QC_Fail (vm, QC_ERR_LOCAL_STACK, 0, NULL);
	for (c = f->copies_start ; c < f->copies_end ; c++)
	{
		v = QC_GetS (&vm->mem, ps->copies[c].src);
		QC_SetS (&vm->mem, ps->copies[c].dst, v);
	}

	vm->frames[vm->numframes++] = (qc_frame_t){
		.resume_pc = resume_pc,
		.func = vm->x.func,
		.prnum = vm->x.prnum,
		.pushed = vm->x.pushed,
		.switch_ref = vm->x.switch_ref,
		.switch_kind = vm->x.switch_kind,
		.locals_at = (uint32_t)ls_top,
		.locals_addr = vm->x.locals_addr,
		.locals_words = vm->x.locals_words,
	};
	vm->x = (qc_exec_t){
		.pc = f->entry,
		.func = index,
		.prnum = prnum,
		.ls_top = (uint32_t)new_top,
		.switch_kind = QC_SWITCH_FLOAT,
		.locals_addr = f->locals_addr,
		.locals_words = f->locals_words,
	};
	return true;
}

// returns from the current function: its caller's locals and context back
void QC_Leave (qcvm_t *vm)
{
	uint32_t	callee = vm->x.prnum;
	uint32_t	top = vm->x.ls_top > vm->x.locals_words ? vm->x.ls_top - vm->x.locals_words : 0;
	qc_frame_t	*frame;

	QC_MoveS (&vm->mem, vm->mem.ls_base + (uint64_t)top * 4, vm->x.locals_addr, (uint64_t)vm->x.locals_words * 4);
	vm->x.ls_top = top;
	if (!vm->numframes)
		return;
	frame = &vm->frames[--vm->numframes];
	vm->x.ls_top = vm->x.ls_top > frame->pushed ? vm->x.ls_top - frame->pushed : 0;
	vm->x.pc = frame->resume_pc;
	vm->x.func = frame->func;
	vm->x.prnum = frame->prnum;
	vm->x.pushed = frame->pushed;
	vm->x.locals_addr = frame->locals_addr;
	vm->x.locals_words = frame->locals_words;
	if (vm->config.compat.switch_reset_on_call)
	{
		vm->x.switch_ref = 0;
		vm->x.switch_kind = QC_SWITCH_FLOAT;
	}
	else
	{
		vm->x.switch_ref = frame->switch_ref;
		vm->x.switch_kind = frame->switch_kind;
	}
	if (frame->prnum != callee)
		QC_SwitchProgs (vm, callee, frame->prnum, false);
}

void QC_Unwind (qcvm_t *vm, uint32_t depth)
{
	while (vm->numframes > depth)
		QC_Leave (vm);
}

/*
==============================================================================

THE LOOP'S HELPERS

==============================================================================
*/

// a load from a bad entity or field
static void QC_BadFieldAccess (qcvm_t *vm, uint32_t e, uint32_t f)
{
	if (e >= vm->mem.num_edicts)
		QC_Warn (vm, QC_WARN_BAD_ENTITY, e, NULL);
	else
		QC_Warn (vm, QC_WARN_BAD_FIELD, (int32_t)f, NULL);
}

// string equality with FTE's null rules: identical references are equal; null
// equals any string that resolves empty; else the contents are compared
bool QC_StringsEqual (qcvm_t *vm, uint32_t a, uint32_t b)
{
	const char	*x, *y;

	if (a == b)
		return true;
	if (!a || !b)
		return !*QC_StrOrWarn (vm, a ? a : b);
	x = QC_StrOrWarn (vm, a);
	y = QC_StrOrWarn (vm, b);
	return !strcmp (x, y);
}

// the word EQ_S or NE_S stores
static uint32_t QC_StringCompare (qcvm_t *vm, uint32_t op, uint32_t a, uint32_t b)
{
	bool			eq = QC_StringsEqual (vm, a, b);
	const uint8_t	*x, *y;

	if (op == QOP_EQ_S)
		return QC_FBool (eq);
	if (eq || !vm->config.compat.ne_s_raw_strcmp)
		return QC_FBool (!eq);
	// FTE: the raw strcmp result as a float
	x = (const uint8_t *)QC_String (vm, a);
	y = (const uint8_t *)QC_String (vm, b);
	while (*x && *x == *y)
		x++, y++;
	return QC_FloatBits ((float)((int)*x - (int)*y));
}

// Reads n bytes through a pointer: base + offset. Falls back as FTE does: the
// 0xFFFFFFFF sentinel (ADDRESS on a protected entity) reads zeros, and a temp
// or static string handle as the base reads inside that string (zeros past it).
bool QC_PtrRead (qcvm_t *vm, uint32_t base, uint32_t offset, void *out, uint32_t n)
{
	uint32_t		addr = base + offset, size, i;
	qc_loc_t		loc = QC_Locate (&vm->mem, addr, n);
	const uint8_t	*data = NULL;
	uint8_t			*o = out;
	uint64_t		at;

	if (loc.type != QC_LOC_NONE)
	{
		memcpy (out, loc.p, n);
		return true;
	}
	if (addr == UINT32_MAX)
	{
		memset (out, 0, n);
		return true;
	}
	if ((base & QC_TAG_MASK) == QC_TEMP_TAG)
		data = QC_TempData (&vm->strings, base & QC_INDEX_MASK, &size);
	else if ((base & QC_TAG_MASK) == QC_STATIC_TAG
		&& (data = (const uint8_t *)QC_StaticText (&vm->strings, base & QC_INDEX_MASK)))
		size = (uint32_t)strlen ((const char *)data);
	if (!data)
		return QC_Fail (vm, QC_ERR_BAD_POINTER_READ, addr, NULL);
	for (i = 0 ; i < n ; i++)
	{
		at = (uint64_t)offset + i;
		o[i] = at < size ? data[at] : 0;
	}
	return true;
}

// Writes bytes through a pointer: base + offset, with FTE's fallbacks (the
// sentinel skipped, temp strings grown when written past their end); a
// protected entity is skipped with a warning.
bool QC_PtrWrite (qcvm_t *vm, uint32_t base, uint32_t offset, const void *bytes, uint32_t n)
{
	uint32_t			addr = base + offset, ent = 0;
	qc_writeresult_t	r = QC_WriteBytes (&vm->mem, addr, bytes, n, &ent);
	uint8_t				*data;

	if (r == QC_WRITE_OK)
		return true;
	if (r == QC_WRITE_PROTECTED)
	{
		QC_Warn (vm, QC_WARN_READONLY_ENTITY, ent, NULL);
		return true;
	}
	if (addr == UINT32_MAX)
		return true;
	if ((base & QC_TAG_MASK) == QC_TEMP_TAG)
	{
		data = QC_TempGrow (&vm->strings, base & QC_INDEX_MASK, (size_t)offset + n);
		if (!data)
			return QC_Fail (vm, QC_ERR_BAD_POINTER_WRITE, addr, NULL);
		memcpy (data + offset, bytes, n);
		return true;
	}
	if (r == QC_WRITE_NULL)
		return QC_Fail (vm, QC_ERR_NULL_POINTER_WRITE, 0, NULL);
	return QC_Fail (vm, QC_ERR_BAD_POINTER_WRITE, addr, NULL);
}

static float QC_ReadFloat (const qc_mem_t *m, uint32_t at)
{
	uint32_t	w = 0;

	QC_ReadBytes (m, at, &w, 4);
	return QC_BitsFloat (w);
}

static void QC_WriteFloat (qc_mem_t *m, uint32_t at, float v)
{
	uint32_t	w = QC_FloatBits (v);

	QC_WriteBytes (m, at, &w, 4, NULL);
}

// the Hexen 2 read-modify-write opcodes through a pointer in B
static bool QC_CompoundPointerStore (qcvm_t *vm, uint32_t op, uint32_t a, uint32_t b, uint32_t c)
{
	qc_mem_t			*m = &vm->mem;
	uint32_t			p = QC_GetS (m, b), k, at;
	bool				vector = op == QOP_MULSTOREP_VF || op == QOP_ADDSTOREP_V || op == QOP_SUBSTOREP_V;
	qc_loc_t			loc;
	float				f, x, y, v;
	int32_t				i, j;

	switch (QC_CheckWrite (m, p, vector ? 12 : 4, &loc))
	{
	case QC_WRITE_OK:
		break;
	case QC_WRITE_PROTECTED:
		QC_Warn (vm, QC_WARN_READONLY_ENTITY, loc.ent, NULL);
		return true;
	case QC_WRITE_NULL:
		return QC_Fail (vm, QC_ERR_NULL_POINTER_WRITE, 0, NULL);
	default:
		return QC_Fail (vm, QC_ERR_BAD_POINTER_WRITE, p, NULL);
	}
	switch (op)
	{
	case QOP_MULSTOREP_VF:
		f = QC_BitsFloat (QC_GetS (m, a));
		for (k = 0 ; k < 3 ; k++)
		{
			at = p + k * 4;
			v = QC_ReadFloat (m, at) * f;
			QC_WriteFloat (m, at, v);
			QC_SetS (m, c + (uint64_t)k * 4, QC_FloatBits (v));
		}
		break;
	case QOP_ADDSTOREP_V:
	case QOP_SUBSTOREP_V:
		for (k = 0 ; k < 3 ; k++)
		{
			at = p + k * 4;
			x = QC_BitsFloat (QC_GetS (m, a + (uint64_t)k * 4));
			y = QC_ReadFloat (m, at);
			v = op == QOP_ADDSTOREP_V ? y + x : y - x;
			QC_WriteFloat (m, at, v);
			QC_SetS (m, c + (uint64_t)k * 4, QC_FloatBits (v));
		}
		break;
	case QOP_BITSETSTOREP_F:
	case QOP_BITCLRSTOREP_F:
		i = QC_F2I (QC_ReadFloat (m, p));
		j = QC_F2I (QC_BitsFloat (QC_GetS (m, a)));
		QC_WriteFloat (m, p, (float)(op == QOP_BITSETSTOREP_F ? i | j : i & ~j));
		break;
	default:
		y = QC_ReadFloat (m, p);
		x = QC_BitsFloat (QC_GetS (m, a));
		v = op == QOP_MULSTOREP_F ? y * x : op == QOP_DIVSTOREP_F ? y / x : op == QOP_ADDSTOREP_F ? y + x : y - x;
		QC_WriteFloat (m, p, v);
		QC_SetS (m, c, QC_FloatBits (v));
		break;
	}
	return true;
}

// the error for calling a builtin that isn't bound
bool QC_MissingBuiltin (qcvm_t *vm, qc_func_t f)
{
	uint32_t			pr = QC_FUNC_PROGS (f), index = QC_FUNC_INDEX (f);
	const qc_function_t	*fn;

	if (pr >= vm->numprogs || index >= vm->progs[pr].progs->numfunctions)
		return QC_Fail (vm, QC_ERR_INVALID_FUNCTION, f, NULL);
	fn = &vm->progs[pr].progs->functions[index];
	return QC_Fail (vm, QC_ERR_BUILTIN_NOT_IMPLEMENTED, fn->kind == QC_FUNC_BUILTIN ? fn->number : 0,
		"%s", QC_Cstr (vm->progs[pr].progs, fn->name));
}

/*
==============================================================================

THE LOOP

==============================================================================
*/

#define QC_LOOP_NAME	QC_RunPlain
#define QC_LOOP_TRACED	0
#include "qc_exec_loop.h"
#undef QC_LOOP_NAME
#undef QC_LOOP_TRACED

#define QC_LOOP_NAME	QC_RunTraced
#define QC_LOOP_TRACED	1
#include "qc_exec_loop.h"
#undef QC_LOOP_NAME
#undef QC_LOOP_TRACED

qc_exit_t QC_Run (qcvm_t *vm, uint32_t exit_depth, uint32_t *budget)
{
	if (vm->trace)
		return QC_RunTraced (vm, exit_depth, budget);
	vm->traced = false;
	return QC_RunPlain (vm, exit_depth, budget);
}
