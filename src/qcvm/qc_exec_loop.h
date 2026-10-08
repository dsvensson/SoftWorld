// qc_exec_loop.h -- the interpreter loop, included by qc_exec.c once plain and
// once traced (QC_LOOP_NAME, QC_LOOP_TRACED)
//
// It runs QuakeC-to-QuakeC calls itself and leaves only for a builtin, an
// animation opcode, a statement to trace, a spent budget, a fault, or the
// return to the host. All it keeps across that is in the VM, so builtins may
// call back into QuakeC.
//
// Global operands were relocated to byte offsets in region S when the progs
// was loaded, and the loader keeps them below the globals' three-word tail;
// every globals block is followed by committed memory, so they are read and
// written without checks. pc never passes the sentinel after the statements.

static qc_exit_t QC_LOOP_NAME (qcvm_t *vm, uint32_t exit_depth, uint32_t *budget)
{
	uint8_t *const			S = vm->mem.s.base;
	const qc_progstate_t	*ps;
	const qc_stmt_t			*code, *st;
	uint32_t				gb, ng, count, pc, a, b, c;
	uint8_t					*fields;
	uint32_t				shift, field_bytes, num_edicts;
#if QC_LOOP_TRACED
	uint64_t				*profile;
#endif

#define U32(o)	(*(uint32_t *)(S + (o)))
#define I32(o)	(*(int32_t *)(S + (o)))
#define FLT(o)	(*(float *)(S + (o)))
#define GET64(o)	((uint64_t)U32 (o) | (uint64_t)U32 ((o) + 4) << 32)
#define SET64(o, v)	do { uint64_t v_ = (v); U32 (o) = (uint32_t)v_; U32 ((o) + 4) = (uint32_t)(v_ >> 32); } while (0)
#define GETD(o)		QC_BitsDouble (GET64 (o))
#define SETD(o, v)	SET64 ((o), QC_DoubleBits (v))

// a counted instruction: a jump, call or return; when the budget runs out, the
// statement has had no effect and runs again when the caller resumes
#define TICK() \
	do { \
		if (*budget <= 1) \
		{ \
			*budget = 0; \
			vm->x.pc = pc; \
			vm->traced = QC_LOOP_TRACED; \
			return QC_ExitOf (QC_EXIT_BUDGET); \
		} \
		(*budget)--; \
	} while (0)
#define JUMP(target)	do { TICK (); pc = (target); goto next; } while (0)
#define FAULT(kind, value) \
	do { \
		vm->x.pc = pc; \
		QC_Fail (vm, (kind), (value), NULL); \
		return QC_ExitOf (QC_EXIT_FAULT); \
	} while (0)
// a helper failed, having set the error
#define FAULTED() \
	do { \
		vm->x.pc = pc; \
		return QC_ExitOf (QC_EXIT_FAULT); \
	} while (0)
#define COPY(src, dst, words) \
	do { \
		uint32_t	s_ = (src), d_ = (dst), k_; \
		for (k_ = 0 ; k_ < (words) ; k_++) \
			U32 (d_ + k_ * 4) = U32 (s_ + k_ * 4); \
	} while (0)
#define FIELD(e, f, n) \
	((e) < num_edicts && (uint64_t)(f) * 4 + (n) <= field_bytes \
		? fields + ((size_t)(e) << shift) + (size_t)(f) * 4 : NULL)

reload:
	ps = &vm->progs[vm->x.prnum];
	code = ps->code;
	gb = ps->gbase;
	ng = ps->progs->numglobals;
	count = ps->progs->numstatements;
	pc = vm->x.pc;
	fields = vm->mem.fields;
	shift = vm->mem.shift;
	field_bytes = vm->mem.field_bytes;
	num_edicts = vm->mem.num_edicts;
#if QC_LOOP_TRACED
	profile = vm->profiling ? ps->profile : NULL;
#endif

	for ( ; ; pc++)
	{
#if QC_LOOP_TRACED
		if (vm->watch.name && QC_WatchChanged (vm))
		{
			vm->x.pc = pc;
			return QC_ExitOf (QC_EXIT_WATCH);
		}
		if (vm->trace && !vm->traced)
		{
			vm->traced = true;
			vm->x.pc = pc;
			return QC_ExitOf (QC_EXIT_TRACE);
		}
		vm->traced = false;
		if (profile)
			profile[vm->x.func]++;
#endif
		if (pc > count)
			FAULT (QC_ERR_JUMP_OUT_OF_RANGE, 0);
		st = &code[pc];
		a = st->a;
		b = st->b;
		c = st->c;

		switch (st->op)
		{
		// ---- v6 arithmetic --------------------------------------------------
		case QOP_MUL_F:	{ float v = FLT (a) * FLT (b); FLT (c) = v; break; }
		case QOP_DIV_F:	{ float v = FLT (a) / FLT (b); FLT (c) = v; break; }
		case QOP_ADD_F:	{ float v = FLT (a) + FLT (b); FLT (c) = v; break; }
		case QOP_SUB_F:	{ float v = FLT (a) - FLT (b); FLT (c) = v; break; }
		case QOP_MUL_V:
		{
			float	v = FLT (a) * FLT (b) + FLT (a + 4) * FLT (b + 4) + FLT (a + 8) * FLT (b + 8);

			FLT (c) = v;
			break;
		}
		case QOP_MUL_FV:
		{
			float	f = FLT (a), v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				v = f * FLT (b + k);
				FLT (c + k) = v;
			}
			break;
		}
		case QOP_MUL_VF:
		case QOP_DIV_VF:
		{
			float	f = FLT (b), v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				v = st->op == QOP_MUL_VF ? FLT (a + k) * f : FLT (a + k) / f;
				FLT (c + k) = v;
			}
			break;
		}
		case QOP_ADD_V:
		case QOP_SUB_V:
		{
			float	v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				v = st->op == QOP_ADD_V ? FLT (a + k) + FLT (b + k) : FLT (a + k) - FLT (b + k);
				FLT (c + k) = v;
			}
			break;
		}

		// ---- v6 comparisons -------------------------------------------------
		case QOP_EQ_F:	{ uint32_t v = QC_FBool (FLT (a) == FLT (b)); U32 (c) = v; break; }
		case QOP_NE_F:	{ uint32_t v = QC_FBool (FLT (a) != FLT (b)); U32 (c) = v; break; }
		case QOP_LE_F:	{ uint32_t v = QC_FBool (FLT (a) <= FLT (b)); U32 (c) = v; break; }
		case QOP_GE_F:	{ uint32_t v = QC_FBool (FLT (a) >= FLT (b)); U32 (c) = v; break; }
		case QOP_LT_F:	{ uint32_t v = QC_FBool (FLT (a) < FLT (b)); U32 (c) = v; break; }
		case QOP_GT_F:	{ uint32_t v = QC_FBool (FLT (a) > FLT (b)); U32 (c) = v; break; }
		case QOP_EQ_V:
		case QOP_NE_V:
		{
			float		x0 = FLT (a), x1 = FLT (a + 4), x2 = FLT (a + 8);
			float		y0 = FLT (b), y1 = FLT (b + 4), y2 = FLT (b + 8);
			bool		eq = st->op == QOP_EQ_V ? x0 == y0 && x1 == y1 && x2 == y2
				: x0 != y0 || x1 != y1 || x2 != y2;

			U32 (c) = QC_FBool (eq);
			break;
		}
		case QOP_EQ_E:
		case QOP_EQ_FNC:	{ uint32_t v = QC_FBool (U32 (a) == U32 (b)); U32 (c) = v; break; }
		case QOP_NE_E:
		case QOP_NE_FNC:	{ uint32_t v = QC_FBool (U32 (a) != U32 (b)); U32 (c) = v; break; }
		case QOP_EQ_S:
		case QOP_NE_S:
		{
			uint32_t	x = U32 (a), y = U32 (b), v;

			vm->x.pc = pc;
			v = QC_StringCompare (vm, st->op, x, y);
			U32 (c) = v;
			break;
		}

		// ---- logic -----------------------------------------------------------
		case QOP_NOT_F:		{ uint32_t v = QC_FBool (!QC_FloatTrue (U32 (a))); U32 (c) = v; break; }
		case QOP_NOT_V:
		{
			uint32_t	v = QC_FBool (FLT (a) == 0 && FLT (a + 4) == 0 && FLT (a + 8) == 0);

			U32 (c) = v;
			break;
		}
		case QOP_NOT_S:
		{
			uint32_t	r = U32 (a);
			bool		empty;

			vm->x.pc = pc;
			empty = !r || !*QC_StrOrWarn (vm, r);
			U32 (c) = QC_FBool (empty);
			break;
		}
		case QOP_NOT_ENT:	{ uint32_t v = QC_FBool (!U32 (a)); U32 (c) = v; break; }
		case QOP_NOT_FNC:	{ uint32_t v = QC_FBool (!(U32 (a) & 0x00FFFFFFu)); U32 (c) = v; break; }
		case QOP_NOT_I:		{ uint32_t v = !U32 (a); U32 (c) = v; break; }
		case QOP_AND_F:
		{
			uint32_t	v = QC_FBool (QC_FloatTrue (U32 (a)) && QC_FloatTrue (U32 (b)));

			U32 (c) = v;
			break;
		}
		case QOP_OR_F:
		{
			uint32_t	v = QC_FBool (QC_FloatTrue (U32 (a)) || QC_FloatTrue (U32 (b)));

			U32 (c) = v;
			break;
		}
		case QOP_BITAND_F:	{ float v = (float)(QC_F2I (FLT (a)) & QC_F2I (FLT (b))); FLT (c) = v; break; }
		case QOP_BITOR_F:	{ float v = (float)(QC_F2I (FLT (a)) | QC_F2I (FLT (b))); FLT (c) = v; break; }

		// ---- branches --------------------------------------------------------
		case QOP_IF_I:
		case QOP_IF_S:
			if (U32 (a))
				JUMP (b);
			TICK ();
			break;
		case QOP_IFNOT_I:
		case QOP_IFNOT_S:
			if (!U32 (a))
				JUMP (b);
			TICK ();
			break;
		case QOP_IF_F:
			if (QC_FloatTrue (U32 (a)))
				JUMP (b);
			TICK ();
			break;
		case QOP_IFNOT_F:
			if (!QC_FloatTrue (U32 (a)))
				JUMP (b);
			TICK ();
			break;
		case QOP_GOTO:
			JUMP (a);

		// ---- entity fields ---------------------------------------------------
		case QOP_LOAD_F:
		case QOP_LOAD_S:
		case QOP_LOAD_ENT:
		case QOP_LOAD_FLD:
		case QOP_LOAD_FNC:
		case QOP_LOAD_I:
		case QOP_LOAD_P:
		{
			uint32_t		e = U32 (a), f = U32 (b), v;
			const uint8_t	*p = FIELD (e, f, 4);

			if (p)
			{
				memcpy (&v, p, 4);
				U32 (c) = v;
				break;
			}
			vm->x.pc = pc;
			QC_BadFieldAccess (vm, e, f);
			U32 (c) = 0;
			break;
		}
		case QOP_LOAD_V:
		{
			uint32_t		e = U32 (a), f = U32 (b), v[3];
			const uint8_t	*p = FIELD (e, f, 12);

			if (p)
			{
				memcpy (v, p, 12);
				U32 (c) = v[0];
				U32 (c + 4) = v[1];
				U32 (c + 8) = v[2];
				break;
			}
			// FTE: a bad entity zeroes the vector, a bad field only its first word
			vm->x.pc = pc;
			QC_BadFieldAccess (vm, e, f);
			U32 (c) = 0;
			if (e >= num_edicts)
				U32 (c + 4) = U32 (c + 8) = 0;
			break;
		}
		case QOP_LOAD_I64:
		{
			uint32_t		e = U32 (a), f = U32 (b), v[2];
			const uint8_t	*p = FIELD (e, f, 8);

			if (p)
			{
				memcpy (v, p, 8);
				U32 (c) = v[0];
				U32 (c + 4) = v[1];
				break;
			}
			vm->x.pc = pc;
			QC_BadFieldAccess (vm, e, f);
			U32 (c) = 0;
			if (e >= num_edicts)
			{
				U32 (c + 4) = 0;
				if (vm->config.compat.load_i64_zero3)
					U32 (c + 8) = 0;
			}
			break;
		}
		case QOP_ADDRESS:
		{
			uint32_t	e = U32 (a), f = U32 (b);

			if (e < num_edicts && !vm->mem.slots[e].protected)
				U32 (c) = vm->mem.e_base + (e << shift) + f * 4;
			else if (e >= num_edicts)
			{
				vm->x.pc = pc;
				QC_Warn (vm, QC_WARN_BAD_ENTITY, e, NULL);
			}
			else
			{
				U32 (c) = UINT32_MAX;
				vm->x.pc = pc;
				QC_Warn (vm, QC_WARN_READONLY_ENTITY, e, NULL);
			}
			break;
		}
		case QOP_STOREF_F:
		case QOP_STOREF_S:
		case QOP_STOREF_I:
		case QOP_STOREF_V:
		case QOP_STOREF_I64:
		{
			uint32_t	words = st->op == QOP_STOREF_V ? 3 : st->op == QOP_STOREF_I64 ? 2 : 1;
			uint32_t	e = U32 (a), f = U32 (b), v[3];
			uint8_t		*p = FIELD (e, f, words * 4);

			if (p && !vm->mem.slots[e].protected)
			{
				v[0] = U32 (c);
				v[1] = words > 1 ? U32 (c + 4) : 0;
				v[2] = words > 2 ? U32 (c + 8) : 0;
				memcpy (p, v, words * 4);
				break;
			}
			vm->x.pc = pc;
			if (e >= num_edicts)
				QC_Warn (vm, QC_WARN_BAD_ENTITY, e, NULL);
			else if (!p)
				QC_Warn (vm, QC_WARN_BAD_FIELD, (int32_t)f, NULL);
			else
				QC_Warn (vm, QC_WARN_READONLY_ENTITY, e, NULL);
			break;
		}

		// ---- global stores ---------------------------------------------------
		case QOP_STORE_F:
		case QOP_STORE_S:
		case QOP_STORE_ENT:
		case QOP_STORE_FLD:
		case QOP_STORE_FNC:
		case QOP_STORE_I:
		case QOP_STORE_P:
			U32 (b) = U32 (a);
			break;
		case QOP_STORE_V:
			COPY (a, b, 3);
			break;
		case QOP_STORE_I64:
			COPY (a, b, 2);
			break;
		case QOP_STORE_IF:	{ float v = (float)I32 (a); FLT (b) = v; break; }
		case QOP_STORE_FI:	{ uint32_t v = (uint32_t)QC_F2I (FLT (a)); U32 (b) = v; break; }

		// ---- stores through pointers -----------------------------------------
		case QOP_STOREP_F:
		case QOP_STOREP_S:
		case QOP_STOREP_ENT:
		case QOP_STOREP_FLD:
		case QOP_STOREP_FNC:
		case QOP_STOREP_I:
		case QOP_STOREP_IF:
		case QOP_STOREP_FI:
		{
			uint32_t	v = U32 (a), base = U32 (b), idx = U32 (c);

			if (st->op == QOP_STOREP_IF)
				v = QC_FloatBits ((float)(int32_t)v);
			else if (st->op == QOP_STOREP_FI)
				v = (uint32_t)QC_F2I (QC_BitsFloat (v));
			vm->x.pc = pc;
			if (!QC_PtrWrite (vm, base, idx * 4, &v, 4))
				FAULTED ();
			break;
		}
		case QOP_STOREP_V:
		case QOP_STOREP_I64:
		{
			uint32_t	v[3] = {U32 (a), U32 (a + 4), 0}, base = U32 (b), idx = U32 (c);

			if (st->op == QOP_STOREP_V)
				v[2] = U32 (a + 8);
			vm->x.pc = pc;
			if (!QC_PtrWrite (vm, base, idx * 4, v, st->op == QOP_STOREP_V ? 12 : 8))
				FAULTED ();
			break;
		}
		case QOP_STOREP_C:
		case QOP_STOREP_I8:
		{
			uint8_t		v = st->op == QOP_STOREP_C ? (uint8_t)QC_F2I (FLT (a)) : (uint8_t)U32 (a);
			uint32_t	base = U32 (b), idx = U32 (c);

			vm->x.pc = pc;
			if (!QC_PtrWrite (vm, base, idx, &v, 1))
				FAULTED ();
			break;
		}
		case QOP_STOREP_I16:
		{
			uint16_t	v = (uint16_t)U32 (a);
			uint32_t	base = U32 (b), idx = U32 (c);

			vm->x.pc = pc;
			if (!QC_PtrWrite (vm, base, idx * 2, &v, 2))
				FAULTED ();
			break;
		}

		// ---- loads through pointers ------------------------------------------
		case QOP_LOADP_F:
		case QOP_LOADP_S:
		case QOP_LOADP_ENT:
		case QOP_LOADP_FLD:
		case QOP_LOADP_FNC:
		case QOP_LOADP_I:
		{
			uint32_t	base = U32 (a), idx = U32 (b), v;

			vm->x.pc = pc;
			if (!QC_PtrRead (vm, base, idx * 4, &v, 4))
				FAULTED ();
			U32 (c) = v;
			break;
		}
		case QOP_LOADP_V:
		{
			uint32_t	base = U32 (a), idx = U32 (b), v[3];

			vm->x.pc = pc;
			if (!QC_PtrRead (vm, base, idx * 4, v, 12))
				FAULTED ();
			U32 (c) = v[0];
			U32 (c + 4) = v[1];
			U32 (c + 8) = v[2];
			break;
		}
		case QOP_LOADP_I64:
		{
			uint32_t	base = U32 (a), idx = U32 (b), v[2];

			vm->x.pc = pc;
			if (!QC_PtrRead (vm, base, idx * 4, v, 8))
				FAULTED ();
			U32 (c) = v[0];
			U32 (c + 4) = v[1];
			break;
		}
		case QOP_LOADP_C:
		{
			uint32_t	base = U32 (a), idx = (uint32_t)QC_F2I (FLT (b));
			uint8_t		v;

			vm->x.pc = pc;
			if (!QC_PtrRead (vm, base, idx, &v, 1))
				FAULTED ();
			FLT (c) = (float)v;
			break;
		}
		case QOP_LOADP_U8:
		case QOP_LOADP_I8:
		{
			uint32_t	base = U32 (a), idx = U32 (b);
			uint8_t		v;

			vm->x.pc = pc;
			if (!QC_PtrRead (vm, base, idx, &v, 1))
				FAULTED ();
			U32 (c) = st->op == QOP_LOADP_I8 ? (uint32_t)(int32_t)(int8_t)v : v;
			break;
		}
		case QOP_LOADP_U16:
		case QOP_LOADP_I16:
		{
			uint32_t	base = U32 (a), idx = U32 (b);
			uint16_t	v;

			vm->x.pc = pc;
			if (!QC_PtrRead (vm, base, idx * 2, &v, 2))
				FAULTED ();
			U32 (c) = st->op == QOP_LOADP_I16 ? (uint32_t)(int32_t)(int16_t)v : v;
			break;
		}
		case QOP_LOADP_ITOF:
		case QOP_LOADP_FTOI:
		{
			// FTE: operand B is ignored, and there is no fallback
			uint32_t	p = U32 (a), w;

			if (!QC_ReadBytes (&vm->mem, p, &w, 4))
				FAULT (QC_ERR_BAD_POINTER_READ, p);
			U32 (c) = st->op == QOP_LOADP_ITOF ? QC_FloatBits ((float)(int32_t)w) : (uint32_t)QC_F2I (QC_BitsFloat (w));
			break;
		}

		// ---- Hexen 2 compound stores -----------------------------------------
		case QOP_MULSTORE_F:	{ float v = FLT (b) * FLT (a); FLT (b) = v; break; }
		case QOP_DIVSTORE_F:	{ float v = FLT (b) / FLT (a); FLT (b) = v; break; }
		case QOP_ADDSTORE_F:	{ float v = FLT (b) + FLT (a); FLT (b) = v; break; }
		case QOP_SUBSTORE_F:	{ float v = FLT (b) - FLT (a); FLT (b) = v; break; }
		case QOP_MULSTORE_VF:
		{
			float	f = FLT (a), v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				v = FLT (b + k) * f;
				FLT (b + k) = v;
			}
			break;
		}
		case QOP_ADDSTORE_V:
		case QOP_SUBSTORE_V:
		{
			float	x, y, v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				y = FLT (b + k);
				x = FLT (a + k);
				v = st->op == QOP_ADDSTORE_V ? y + x : y - x;
				FLT (b + k) = v;
			}
			break;
		}
		case QOP_BITSETSTORE_F:	{ float v = (float)(QC_F2I (FLT (b)) | QC_F2I (FLT (a))); FLT (b) = v; break; }
		case QOP_BITCLRSTORE_F:	{ float v = (float)(QC_F2I (FLT (b)) & ~QC_F2I (FLT (a))); FLT (b) = v; break; }
		case QOP_MULSTOREP_F:
		case QOP_DIVSTOREP_F:
		case QOP_ADDSTOREP_F:
		case QOP_SUBSTOREP_F:
		case QOP_MULSTOREP_VF:
		case QOP_ADDSTOREP_V:
		case QOP_SUBSTOREP_V:
		case QOP_BITSETSTOREP_F:
		case QOP_BITCLRSTOREP_F:
			vm->x.pc = pc;
			if (!QC_CompoundPointerStore (vm, st->op, a, b, c))
				FAULTED ();
			break;

		// ---- Hexen 2 global arrays -------------------------------------------
		case QOP_FETCH_GBL_F:
		case QOP_FETCH_GBL_S:
		case QOP_FETCH_GBL_E:
		case QOP_FETCH_GBL_FNC:
		case QOP_FETCH_GBL_V:
		{
			// the index comes from memory: read anywhere in region S, checked
			int32_t		i = QC_F2I (FLT (b));
			uint32_t	stride = st->op == QOP_FETCH_GBL_V ? 3 : 1, prefix, k;
			uint64_t	word, src;

			if (!a)
				FAULT (QC_ERR_ARRAY_INDEX, i);
			prefix = QC_GetS (&vm->mem, gb + (uint64_t)(a - 1) * 4);
			if ((uint32_t)i > prefix)
				FAULT (QC_ERR_ARRAY_INDEX, i);
			word = (uint32_t)(a + (uint32_t)i * stride);
			src = gb + word * 4;
			for (k = 0 ; k < stride ; k++)
				U32 (c + k * 4) = QC_GetS (&vm->mem, src + k * 4);
			break;
		}

		// ---- animation -------------------------------------------------------
		case QOP_STATE:
		{
			qc_exit_t	exit = QC_ExitOf (QC_EXIT_STATEOP);

			exit.op = (qc_stateop_t){.kind = QC_STATE_STATE, .frame = FLT (a), .func = U32 (b)};
			vm->x.pc = pc + 1;
			return exit;
		}
		case QOP_CSTATE:
		case QOP_CWSTATE:
		{
			qc_exit_t	exit = QC_ExitOf (QC_EXIT_STATEOP);

			exit.op = (qc_stateop_t){.kind = st->op == QOP_CSTATE ? QC_STATE_CSTATE : QC_STATE_CWSTATE,
				.first = FLT (a), .last = FLT (b), .func = QC_FUNC (vm->x.prnum, vm->x.func)};
			vm->x.pc = pc + 1;
			return exit;
		}
		case QOP_THINKTIME:
		{
			qc_exit_t	exit = QC_ExitOf (QC_EXIT_STATEOP);

			exit.op = (qc_stateop_t){.kind = QC_STATE_THINKTIME, .ent = U32 (a), .delay = FLT (b)};
			vm->x.pc = pc + 1;
			return exit;
		}

		// ---- random ----------------------------------------------------------
		case QOP_RAND0:
		case QOP_RAND1:
		case QOP_RAND2:
		{
			float	r = (float)QC_Rand15 (vm) / 32768.0f, v;

			if (st->op == QOP_RAND0)
				v = r;
			else if (st->op == QOP_RAND1)
				v = r * FLT (a);
			else
			{
				float	lo = FLT (a), hi = FLT (b);

				v = lo + r * (hi - lo);
			}
			FLT (c) = v;
			break;
		}
		case QOP_RANDV0:
		case QOP_RANDV1:
		case QOP_RANDV2:
		{
			float	r, v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				r = (float)QC_Rand15 (vm) / 32767.0f;
				if (st->op == QOP_RANDV0)
					v = r;
				else if (st->op == QOP_RANDV1)
					v = r * FLT (a + k);
				else
				{
					float	lo = FLT (a + k), hi = FLT (b + k);

					v = lo + r * (hi - lo);
				}
				FLT (c + k) = v;
			}
			break;
		}

		// ---- switch ----------------------------------------------------------
		case QOP_SWITCH_F:
		case QOP_SWITCH_V:
		case QOP_SWITCH_S:
		case QOP_SWITCH_E:
		case QOP_SWITCH_FNC:
		case QOP_SWITCH_I:
			vm->x.switch_ref = a;
			vm->x.switch_kind = st->op == QOP_SWITCH_F ? QC_SWITCH_FLOAT : st->op == QOP_SWITCH_V ? QC_SWITCH_VECTOR
				: st->op == QOP_SWITCH_S ? QC_SWITCH_STRING : QC_SWITCH_INT;
			JUMP (b);
		case QOP_CASE:
		{
			uint32_t	r = vm->x.switch_ref;
			bool		hit;

			switch (vm->x.switch_kind)
			{
			case QC_SWITCH_FLOAT:
				hit = FLT (r) == FLT (a);
				break;
			case QC_SWITCH_VECTOR:
				hit = FLT (r) == FLT (a) && FLT (r + 4) == FLT (a + 4) && FLT (r + 8) == FLT (a + 8);
				break;
			case QC_SWITCH_INT:
				hit = U32 (r) == U32 (a);
				break;
			case QC_SWITCH_STRING:
			default:
				vm->x.pc = pc;
				hit = QC_StringsEqual (vm, U32 (r), U32 (a));
				break;
			}
			if (hit)
				JUMP (b);
			break;
		}
		case QOP_CASERANGE:
		{
			uint32_t	r = vm->x.switch_ref;
			bool		hit;
			int			k;

			switch (vm->x.switch_kind)
			{
			case QC_SWITCH_FLOAT:
				hit = FLT (a) <= FLT (r) && FLT (r) <= FLT (b);
				break;
			case QC_SWITCH_VECTOR:
				hit = true;
				for (k = 0 ; k < 12 ; k += 4)
					hit = hit && FLT (a + k) <= FLT (r + k) && FLT (r + k) <= FLT (b + k);
				break;
			case QC_SWITCH_INT:
				hit = I32 (a) <= I32 (r) && I32 (r) <= I32 (b);
				break;
			case QC_SWITCH_STRING:
			default:
				FAULT (QC_ERR_STRING_CASE_RANGE, 0);
			}
			if (hit)
				JUMP (c);
			break;
		}

		// ---- calls and returns -----------------------------------------------
		case QOP_CALL0: case QOP_CALL1: case QOP_CALL2: case QOP_CALL3: case QOP_CALL4:
		case QOP_CALL5: case QOP_CALL6: case QOP_CALL7: case QOP_CALL8:
		case QOP_CALL1H: case QOP_CALL2H: case QOP_CALL3H: case QOP_CALL4H:
		case QOP_CALL5H: case QOP_CALL6H: case QOP_CALL7H: case QOP_CALL8H:
		{
			int					argc = QC_CallArgc (st->op);
			uint32_t			fv, tp, index;
			const qc_callee_t	*callee;

			// counted first: a statement the budget stops must have had no effect
			TICK ();
			if (st->op >= QOP_CALL1H)
			{
				if (argc >= 2)
					COPY (c, gb + QC_OFS_PARM1, 3);
				COPY (b, gb + QC_OFS_PARM0, 3);
			}
			fv = U32 (a);
			vm->argc = (uint32_t)argc;
			tp = QC_FUNC_PROGS (fv);
			index = QC_FUNC_INDEX (fv);
			if (tp >= vm->numprogs || index >= vm->progs[tp].progs->numfunctions)
				FAULT (QC_ERR_INVALID_FUNCTION, fv);
			callee = &vm->progs[tp].callees[index];
			switch (callee->kind)
			{
			case QC_CALLEE_QC:
				vm->x.pc = pc;
				if (!QC_Enter (vm, tp, index, pc + 1))
					FAULTED ();
				goto reload;
			case QC_CALLEE_BUILTIN:
			{
				qc_exit_t	exit = QC_ExitOf (QC_EXIT_BUILTIN);

				exit.slot = callee->slot;
				exit.func = fv;
				vm->x.pc = pc + 1;
				return exit;
			}
			case QC_CALLEE_NULL:
				FAULT (QC_ERR_NULL_FUNCTION, 0);
			case QC_CALLEE_MISSING:
				vm->x.pc = pc;
				QC_MissingBuiltin (vm, fv);
				return QC_ExitOf (QC_EXIT_FAULT);
			case QC_CALLEE_INVALID:
			default:
				FAULT (QC_ERR_INVALID_FUNCTION, fv);
			}
		}
		case QOP_RETURN:
		case QOP_DONE:
			TICK ();
			COPY (a, gb + QC_OFS_RETURN, 3);
			vm->x.pc = pc;
			QC_Leave (vm);
			if (vm->numframes <= exit_depth)
				return QC_ExitOf (QC_EXIT_RETURNED);
			goto reload;

		// ---- integers --------------------------------------------------------
		case QOP_ADD_I:	{ uint32_t v = U32 (a) + U32 (b); U32 (c) = v; break; }
		case QOP_SUB_I:	{ uint32_t v = U32 (a) - U32 (b); U32 (c) = v; break; }
		case QOP_MUL_I:	{ uint32_t v = U32 (a) * U32 (b); U32 (c) = v; break; }
		case QOP_DIV_I:
		{
			int32_t	x = I32 (a), y = I32 (b), v;

			v = !y ? 0 : x == INT32_MIN && y == -1 ? INT32_MAX : x / y;
			I32 (c) = v;
			break;
		}
		case QOP_BITAND_I:	{ uint32_t v = U32 (a) & U32 (b); U32 (c) = v; break; }
		case QOP_BITOR_I:	{ uint32_t v = U32 (a) | U32 (b); U32 (c) = v; break; }
		case QOP_BITXOR_I:	{ uint32_t v = U32 (a) ^ U32 (b); U32 (c) = v; break; }
		case QOP_RSHIFT_I:	{ uint32_t v = QC_Sar32 (U32 (a), U32 (b)); U32 (c) = v; break; }
		case QOP_LSHIFT_I:	{ uint32_t v = U32 (a) << (U32 (b) & 31); U32 (c) = v; break; }
		case QOP_EQ_I:	{ uint32_t v = I32 (a) == I32 (b); U32 (c) = v; break; }
		case QOP_NE_I:	{ uint32_t v = I32 (a) != I32 (b); U32 (c) = v; break; }
		case QOP_LE_I:	{ uint32_t v = I32 (a) <= I32 (b); U32 (c) = v; break; }
		case QOP_GE_I:	{ uint32_t v = I32 (a) >= I32 (b); U32 (c) = v; break; }
		case QOP_LT_I:	{ uint32_t v = I32 (a) < I32 (b); U32 (c) = v; break; }
		case QOP_GT_I:	{ uint32_t v = I32 (a) > I32 (b); U32 (c) = v; break; }
		case QOP_AND_I:	{ uint32_t v = U32 (a) && U32 (b); U32 (c) = v; break; }
		case QOP_OR_I:	{ uint32_t v = U32 (a) || U32 (b); U32 (c) = v; break; }
		case QOP_CONV_ITOF:	{ float v = (float)I32 (a); FLT (c) = v; break; }
		case QOP_CONV_FTOI:	{ uint32_t v = (uint32_t)QC_F2I (FLT (a)); U32 (c) = v; break; }

		// ---- mixed integers and floats ---------------------------------------
		case QOP_ADD_FI:	{ float v = FLT (a) + (float)I32 (b); FLT (c) = v; break; }
		case QOP_ADD_IF:	{ float v = (float)I32 (a) + FLT (b); FLT (c) = v; break; }
		case QOP_SUB_FI:	{ float v = FLT (a) - (float)I32 (b); FLT (c) = v; break; }
		case QOP_SUB_IF:	{ float v = (float)I32 (a) - FLT (b); FLT (c) = v; break; }
		case QOP_MUL_IF:	{ float v = (float)I32 (a) * FLT (b); FLT (c) = v; break; }
		case QOP_MUL_FI:	{ float v = FLT (a) * (float)I32 (b); FLT (c) = v; break; }
		case QOP_DIV_IF:	{ float v = (float)I32 (a) / FLT (b); FLT (c) = v; break; }
		case QOP_DIV_FI:	{ float v = FLT (a) / (float)I32 (b); FLT (c) = v; break; }
		case QOP_MUL_VI:
		{
			float	i = (float)I32 (b), v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				v = FLT (a + k) * i;
				FLT (c + k) = v;
			}
			break;
		}
		case QOP_MUL_IV:
		{
			float	i = (float)I32 (a), v;
			int		k;

			for (k = 0 ; k < 12 ; k += 4)
			{
				v = i * FLT (b + k);
				FLT (c + k) = v;
			}
			break;
		}
		case QOP_LE_IF:	{ uint32_t v = (float)I32 (a) <= FLT (b); U32 (c) = v; break; }
		case QOP_GE_IF:	{ uint32_t v = (float)I32 (a) >= FLT (b); U32 (c) = v; break; }
		case QOP_LT_IF:	{ uint32_t v = (float)I32 (a) < FLT (b); U32 (c) = v; break; }
		case QOP_GT_IF:	{ uint32_t v = (float)I32 (a) > FLT (b); U32 (c) = v; break; }
		case QOP_EQ_IF:	{ uint32_t v = (float)I32 (a) == FLT (b); U32 (c) = v; break; }
		case QOP_NE_IF:	{ uint32_t v = (float)I32 (a) != FLT (b); U32 (c) = v; break; }
		case QOP_LE_FI:	{ uint32_t v = FLT (a) <= (float)I32 (b); U32 (c) = v; break; }
		case QOP_GE_FI:	{ uint32_t v = FLT (a) >= (float)I32 (b); U32 (c) = v; break; }
		case QOP_LT_FI:	{ uint32_t v = FLT (a) < (float)I32 (b); U32 (c) = v; break; }
		case QOP_GT_FI:	{ uint32_t v = FLT (a) > (float)I32 (b); U32 (c) = v; break; }
		case QOP_EQ_FI:	{ uint32_t v = FLT (a) == (float)I32 (b); U32 (c) = v; break; }
		case QOP_NE_FI:	{ uint32_t v = FLT (a) != (float)I32 (b); U32 (c) = v; break; }
		case QOP_BITAND_IF:	{ int32_t v = I32 (a) & QC_F2I (FLT (b)); I32 (c) = v; break; }
		case QOP_BITOR_IF:	{ int32_t v = I32 (a) | QC_F2I (FLT (b)); I32 (c) = v; break; }
		case QOP_BITAND_FI:	{ int32_t v = QC_F2I (FLT (a)) & I32 (b); I32 (c) = v; break; }
		case QOP_BITOR_FI:	{ int32_t v = QC_F2I (FLT (a)) | I32 (b); I32 (c) = v; break; }
		case QOP_AND_IF:	{ uint32_t v = U32 (a) && FLT (b) != 0; U32 (c) = v; break; }
		case QOP_OR_IF:		{ uint32_t v = U32 (a) || FLT (b) != 0; U32 (c) = v; break; }
		case QOP_AND_FI:	{ uint32_t v = FLT (a) != 0 && U32 (b); U32 (c) = v; break; }
		case QOP_OR_FI:		{ uint32_t v = FLT (a) != 0 || U32 (b); U32 (c) = v; break; }

		// ---- pointers, strings, indexed globals ------------------------------
		case QOP_GLOBALADDRESS:	{ uint32_t v = gb + (a + U32 (b)) * 4; U32 (c) = v; break; }
		case QOP_ADD_PIW:		{ uint32_t v = U32 (a) + U32 (b) * 4; U32 (c) = v; break; }
		case QOP_ADD_SF:		{ uint32_t v = U32 (a) + (uint32_t)QC_F2I (FLT (b)); U32 (c) = v; break; }
		case QOP_SUB_S:			{ uint32_t v = U32 (a) - U32 (b); U32 (c) = v; break; }
		case QOP_LOADA_F:
		case QOP_LOADA_S:
		case QOP_LOADA_ENT:
		case QOP_LOADA_FLD:
		case QOP_LOADA_FNC:
		case QOP_LOADA_I:
		case QOP_LOADA_V:
		case QOP_LOADA_I64:
		{
			uint32_t	words = st->op == QOP_LOADA_V ? 3 : st->op == QOP_LOADA_I64 ? 2 : 1;
			int64_t		i = (int64_t)a + I32 (b);

			if (i < 0 || i + words > ng)
				FAULT (QC_ERR_ARRAY_INDEX, i);
			COPY (gb + (uint32_t)i * 4, c, words);
			break;
		}
		case QOP_GLOAD_I:
		case QOP_GLOAD_F:
		case QOP_GLOAD_FLD:
		case QOP_GLOAD_ENT:
		case QOP_GLOAD_S:
		case QOP_GLOAD_FNC:
		case QOP_GLOAD_V:
		{
			uint32_t	words = st->op == QOP_GLOAD_V ? 3 : 1;
			int32_t		i = I32 (a);

			if (i < 0 || (int64_t)i + words > ng)
				FAULT (QC_ERR_ARRAY_INDEX, i);
			COPY (gb + (uint32_t)i * 4, c, words);
			break;
		}
		case QOP_GSTOREP_I:
		case QOP_GSTOREP_F:
		case QOP_GSTOREP_ENT:
		case QOP_GSTOREP_FLD:
		case QOP_GSTOREP_S:
		case QOP_GSTOREP_FNC:
		case QOP_GSTOREP_V:
		{
			uint32_t	words = st->op == QOP_GSTOREP_V ? 3 : 1;
			int32_t		i = I32 (b);

			if (i < 0 || (int64_t)i + words > ng)
				FAULT (QC_ERR_ARRAY_INDEX, i);
			COPY (a, gb + (uint32_t)i * 4, words);
			break;
		}
		case QOP_BOUNDCHECK:
		{
			uint32_t	v = U32 (a);

			if (v < c || v >= b)
			{
				vm->x.pc = pc;
				QC_Fail (vm, QC_ERR_BOUND_CHECK, (int32_t)v, NULL);
				vm->error.low = c;
				vm->error.high = b;
				return QC_ExitOf (QC_EXIT_FAULT);
			}
			break;
		}
		case QOP_PUSH:
		{
			uint32_t	words = U32 (a);
			uint32_t	word = vm->x.ls_top + vm->x.pushed;
			uint32_t	addr = vm->mem.ls_base + word * 4;
			uint64_t	pushed = (uint64_t)vm->x.pushed + words;

			if (pushed > UINT32_MAX)
				pushed = UINT32_MAX;
			if ((uint64_t)vm->x.ls_top + pushed >= vm->mem.ls_words)
				FAULT (QC_ERR_PUSHED_TOO_MUCH, 0);
			vm->x.pushed = (uint32_t)pushed;
			U32 (c) = addr;
			break;
		}
		case QOP_GADDRESS:
			FAULT (QC_ERR_GADDRESS, 0);
		case QOP_UNUSED:
		case QOP_POP:
		case QOP_BAD:
			FAULT (QC_ERR_BAD_OPCODE, st->op);
		case QOP_JUMP_OUT_OF_RANGE:
			FAULT (QC_ERR_JUMP_OUT_OF_RANGE, 0);

		// ---- unsigned --------------------------------------------------------
		case QOP_LE_U:		{ uint32_t v = U32 (a) <= U32 (b); U32 (c) = v; break; }
		case QOP_LT_U:		{ uint32_t v = U32 (a) < U32 (b); U32 (c) = v; break; }
		case QOP_DIV_U:		{ uint32_t y = U32 (b), v = y ? U32 (a) / y : 0; U32 (c) = v; break; }
		case QOP_RSHIFT_U:	{ uint32_t v = U32 (a) >> (U32 (b) & 31); U32 (c) = v; break; }
		case QOP_CONV_UF:	{ float v = (float)U32 (a); FLT (c) = v; break; }
		case QOP_CONV_FU:	{ uint32_t v = QC_F2U (FLT (a)); U32 (c) = v; break; }

		// ---- 64-bit integers -------------------------------------------------
		case QOP_ADD_I64:		{ uint64_t v = GET64 (a) + GET64 (b); SET64 (c, v); break; }
		case QOP_SUB_I64:		{ uint64_t v = GET64 (a) - GET64 (b); SET64 (c, v); break; }
		case QOP_MUL_I64:		{ uint64_t v = GET64 (a) * GET64 (b); SET64 (c, v); break; }
		case QOP_DIV_I64:
		{
			int64_t	x = (int64_t)GET64 (a), y = (int64_t)GET64 (b), v;

			v = !y ? 0 : x == INT64_MIN && y == -1 ? INT64_MIN : x / y;
			SET64 (c, (uint64_t)v);
			break;
		}
		case QOP_BITAND_I64:	{ uint64_t v = GET64 (a) & GET64 (b); SET64 (c, v); break; }
		case QOP_BITOR_I64:		{ uint64_t v = GET64 (a) | GET64 (b); SET64 (c, v); break; }
		case QOP_BITXOR_I64:	{ uint64_t v = GET64 (a) ^ GET64 (b); SET64 (c, v); break; }
		case QOP_LSHIFT_I64I:	{ uint64_t v = GET64 (a) << (U32 (b) & 63); SET64 (c, v); break; }
		case QOP_RSHIFT_I64I:	{ uint64_t v = QC_Sar64 (GET64 (a), U32 (b)); SET64 (c, v); break; }
		case QOP_RSHIFT_U64I:	{ uint64_t v = GET64 (a) >> (U32 (b) & 63); SET64 (c, v); break; }
		case QOP_LE_I64:	{ uint32_t v = (int64_t)GET64 (a) <= (int64_t)GET64 (b); U32 (c) = v; break; }
		case QOP_LT_I64:	{ uint32_t v = (int64_t)GET64 (a) < (int64_t)GET64 (b); U32 (c) = v; break; }
		case QOP_EQ_I64:	{ uint32_t v = GET64 (a) == GET64 (b); U32 (c) = v; break; }
		case QOP_NE_I64:	{ uint32_t v = GET64 (a) != GET64 (b); U32 (c) = v; break; }
		case QOP_LE_U64:	{ uint32_t v = GET64 (a) <= GET64 (b); U32 (c) = v; break; }
		case QOP_LT_U64:	{ uint32_t v = GET64 (a) < GET64 (b); U32 (c) = v; break; }
		case QOP_DIV_U64:
		{
			uint64_t	y = GET64 (b), v = y ? GET64 (a) / y : 0;

			SET64 (c, v);
			break;
		}
		case QOP_CONV_UI64:	{ uint64_t v = U32 (a); SET64 (c, v); break; }
		case QOP_CONV_II64:	{ uint64_t v = (uint64_t)(int64_t)I32 (a); SET64 (c, v); break; }
		case QOP_CONV_I64I:	{ uint32_t v = U32 (a); U32 (c) = v; break; }
		case QOP_CONV_I64F:	{ float v = (float)(int64_t)GET64 (a); FLT (c) = v; break; }
		case QOP_CONV_U64F:	{ float v = QC_U64ToFloat (GET64 (a)); FLT (c) = v; break; }
		case QOP_CONV_FI64:	{ uint64_t v = (uint64_t)QC_D2I64 ((double)FLT (a)); SET64 (c, v); break; }
		case QOP_CONV_FU64:	{ uint64_t v = QC_D2U64 ((double)FLT (a)); SET64 (c, v); break; }

		// ---- doubles ---------------------------------------------------------
		case QOP_ADD_D:	{ double v = GETD (a) + GETD (b); SETD (c, v); break; }
		case QOP_SUB_D:	{ double v = GETD (a) - GETD (b); SETD (c, v); break; }
		case QOP_MUL_D:	{ double v = GETD (a) * GETD (b); SETD (c, v); break; }
		case QOP_DIV_D:	{ double v = GETD (a) / GETD (b); SETD (c, v); break; }
		case QOP_LE_D:	{ uint32_t v = GETD (a) <= GETD (b); U32 (c) = v; break; }
		case QOP_LT_D:	{ uint32_t v = GETD (a) < GETD (b); U32 (c) = v; break; }
		case QOP_EQ_D:	{ uint32_t v = GETD (a) == GETD (b); U32 (c) = v; break; }
		case QOP_NE_D:	{ uint32_t v = GETD (a) != GETD (b); U32 (c) = v; break; }
		case QOP_CONV_FD:	{ double v = (double)FLT (a); SETD (c, v); break; }
		case QOP_CONV_DF:	{ float v = (float)GETD (a); FLT (c) = v; break; }
		case QOP_CONV_I64D:	{ double v = (double)(int64_t)GET64 (a); SETD (c, v); break; }
		case QOP_CONV_U64D:	{ double v = QC_U64ToDouble (GET64 (a)); SETD (c, v); break; }
		case QOP_CONV_DI64:	{ uint64_t v = (uint64_t)QC_D2I64 (GETD (a)); SET64 (c, v); break; }
		case QOP_CONV_DU64:	{ uint64_t v = QC_D2U64 (GETD (a)); SET64 (c, v); break; }

		// ---- bitfields -------------------------------------------------------
		case QOP_BITEXTEND_I:
		case QOP_BITEXTEND_U:
		{
			uint32_t	x = U32 (a), d = U32 (b), width = d & 0xFF, p = d >> 8, up, down, v;

			if (!width)
				v = 0;
			else
			{
				up = x << ((32u - width - p) & 31);
				down = (32u - width) & 31;
				v = st->op == QOP_BITEXTEND_I ? QC_Sar32 (up, down) : up >> down;
			}
			U32 (c) = v;
			break;
		}
		case QOP_BITCOPY_I:
		{
			uint32_t	x = U32 (a), d = U32 (b), width = d & 0xFF, p = (d >> 8) & 31, mask, v;

			mask = width >= 32 ? UINT32_MAX : (1u << width) - 1;
			v = (U32 (c) & ~(mask << p)) | ((x & mask) << p);
			U32 (c) = v;
			break;
		}

		default:
			FAULT (QC_ERR_BAD_OPCODE, st->op);
		}
		continue;
next:
		pc--;		// the loop's increment lands on the target
	}

#undef U32
#undef I32
#undef FLT
#undef GET64
#undef SET64
#undef GETD
#undef SETD
#undef TICK
#undef JUMP
#undef FAULT
#undef FAULTED
#undef COPY
#undef FIELD
}
