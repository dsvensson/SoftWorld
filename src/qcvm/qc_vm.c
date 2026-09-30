// qc_vm.c -- virtual machines: creation, memory layout, and the host's access
// to globals, fields, entities and strings

#include "qc_builtins.h"

#include <stdlib.h>
#include <time.h>

// everything below 2^31, so pointers are never taken for tagged strings
#define ADDRESS_LIMIT	0x80000000ull

static double QC_SystemClock (void)
{
	struct timespec	ts;

	if (!timespec_get (&ts, TIME_UTC))
		return 0;
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double QC_Clock (const qcvm_t *vm)
{
	return vm->host.clock ? vm->host.clock (vm->ctx) : QC_SystemClock ();
}

double QC_Now (const qcvm_t *vm)
{
	return vm->time_set ? vm->time : QC_Clock (vm) - vm->started;
}

/*
==============================================================================

FIELDS

==============================================================================
*/

bool QC_AddFieldEntry (qc_fieldtable_t *t, const char *name, uint32_t type, uint32_t ofs)
{
	qc_fieldentry_t	*grown;
	size_t			len = strlen (name);
	uint32_t		n;
	char			*copy;

	if (t->count == t->size)
	{
		n = t->size ? t->size * 2 : 64;
		grown = realloc (t->entries, (size_t)n * sizeof(*grown));
		if (!grown)
			return false;
		t->entries = grown;
		t->size = n;
	}
	copy = malloc (len + 1);
	if (!copy)
		return false;
	memcpy (copy, name, len + 1);
	if (len && !QC_MapAdd (&t->by_name, copy, len, t->count))
	{
		free (copy);
		return false;
	}
	t->entries[t->count++] = (qc_fieldentry_t){copy, type, ofs};
	return true;
}

static void QC_FreeFieldTable (qc_fieldtable_t *t)
{
	uint32_t	i;

	for (i = 0 ; i < t->count ; i++)
		free (t->entries[i].name);
	free (t->entries);
	QC_MapClear (&t->by_name);
	*t = (qc_fieldtable_t){0};
}

const qc_fieldentry_t *QC_FieldEntry (const qcvm_t *vm, const char *name)
{
	uint32_t	i;

	if (!QC_MapGet (&vm->fields.by_name, name, strlen (name), &i))
		return NULL;
	return &vm->fields.entries[i];
}

// the words of a field of a type; unknown types take one
static uint32_t QC_FieldWordsOf (uint32_t type)
{
	int	words = QC_TypeWords (type);

	return words > 0 ? (uint32_t)words : 1;
}

void QC_ApplySpawnDefaults (qcvm_t *vm, uint32_t e)
{
	const qc_fieldfill_t	*d;
	uint8_t					*p;
	uint32_t				i, v;

	for (i = 0 ; i < vm->numspawn_defaults ; i++)
	{
		d = &vm->spawn_defaults[i];
		v = d->global >= 0 ? QC_GetS (&vm->mem, (uint64_t)d->global) : d->value;
		p = QC_FieldPtr (&vm->mem, e, d->ofs, 1);
		if (p)
			memcpy (p, &v, 4);
	}
}

/*
==============================================================================

PROGS

==============================================================================
*/

int64_t QC_GlobalOffset (const qcvm_t *vm, uint32_t prnum, uint32_t word)
{
	if (prnum >= vm->numprogs)
		return -1;
	return (int64_t)vm->progs[prnum].gbase + (int64_t)word * 4;
}

// the S offset of a global of a progs by name, or -1
static int64_t QC_GlobalAt (const qc_progs_t *p, uint32_t gbase, const char *name)
{
	const qc_def_t	*d = QC_GlobalDefRaw (p, name);

	return d ? (int64_t)gbase + (int64_t)d->ofs * 4 : -1;
}

static int64_t QC_FieldOfsOf (const qcvm_t *vm, const char *name)
{
	const qc_fieldentry_t	*f = QC_FieldEntry (vm, name);

	return f ? (int64_t)f->ofs : -1;
}

// binds each function record to a callee
static void QC_Bind (const qcvm_t *vm, const qc_progs_t *p, qc_callee_t *callees)
{
	const qc_function_t	*f;
	uint32_t			i, slot;

	for (i = 0 ; i < p->numfunctions ; i++)
	{
		f = &p->functions[i];
		switch (f->kind)
		{
		case QC_FUNC_NULL:
			callees[i] = (qc_callee_t){QC_CALLEE_NULL, 0};
			break;
		case QC_FUNC_QUAKEC:
			callees[i] = (qc_callee_t){QC_CALLEE_QC, 0};
			break;
		case QC_FUNC_BUILTIN:
			if (QC_BindNumber (vm->builtins, f->number, &slot))
				callees[i] = (qc_callee_t){QC_CALLEE_BUILTIN, slot};
			else
				callees[i] = (qc_callee_t){QC_CALLEE_MISSING, 0};
			break;
		case QC_FUNC_NAMED_BUILTIN:
			if (QC_BindName (vm->builtins, QC_Cstr (p, f->name), &slot))
				callees[i] = (qc_callee_t){QC_CALLEE_BUILTIN, slot};
			else
				callees[i] = (qc_callee_t){QC_CALLEE_MISSING, 0};
			break;
		case QC_FUNC_INVALID:
		default:
			callees[i] = (qc_callee_t){QC_CALLEE_INVALID, 0};
			break;
		}
	}
}

static void QC_FreeProgState (qc_progstate_t *ps)
{
	QC_ReleaseProgs (ps->progs);
	free (ps->code);
	free (ps->callees);
	free (ps->funcs);
	free (ps->copies);
	free (ps->shared);
	free (ps->profile);
	*ps = (qc_progstate_t){0};
}

// A progs loaded with its strings at sbase and its globals at gbase: its
// statements with their global operands relocated to absolute S offsets, its
// functions bound, and what entering one needs.
bool QC_InitProgState (qcvm_t *vm, qc_progstate_t *ps, qc_progs_t *p, uint32_t sbase, uint32_t gbase)
{
	const qc_opinfo_t	*info;
	const qc_function_t	*f;
	qc_stmt_t			*s;
	uint32_t			i, k, c, *operand[3];

	*ps = (qc_progstate_t){.progs = p, .sbase = sbase, .gbase = gbase};
	QC_RetainProgs (p);
	ps->code = malloc (((size_t)p->numstatements + 1) * sizeof(*ps->code));
	ps->callees = malloc (((size_t)p->numfunctions + 1) * sizeof(*ps->callees));
	ps->funcs = malloc (((size_t)p->numfunctions + 1) * sizeof(*ps->funcs));
	ps->copies = malloc (((size_t)p->numcopies + 1) * sizeof(*ps->copies));
	if (!ps->code || !ps->callees || !ps->funcs || !ps->copies)
	{
		QC_FreeProgState (ps);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS_AREA, NULL);
	}

	for (i = 0 ; i <= p->numstatements ; i++)
	{
		s = &ps->code[i];
		*s = p->statements[i];
		info = &qc_opinfo[s->op];
		operand[0] = &s->a;
		operand[1] = &s->b;
		operand[2] = &s->c;
		for (k = 0 ; k < 3 ; k++)
			if (info->operands[k] == QC_OPND_G || info->operands[k] == QC_OPND_U)
				*operand[k] += gbase;
	}

	QC_Bind (vm, p, ps->callees);

	for (i = 0 ; i < p->numfunctions ; i++)
	{
		f = &p->functions[i];
		if (f->kind != QC_FUNC_QUAKEC)
		{
			ps->funcs[i] = (qc_qcfunc_t){.entry = UINT32_MAX};
			continue;
		}
		ps->funcs[i] = (qc_qcfunc_t){.entry = f->entry, .locals_addr = gbase + f->parm_start * 4,
			.locals_words = f->locals, .copies_start = ps->numcopies};
		for (c = f->copies_start ; c < f->copies_end ; c++)
			ps->copies[ps->numcopies++] = (qc_paramcopy_t){p->copies[c].src + gbase, p->copies[c].dst + gbase};
		ps->funcs[i].copies_end = ps->numcopies;
	}

	ps->state = (qc_statehandles_t){
		.self_g = QC_GlobalAt (p, gbase, "self"),
		.time_g = QC_GlobalAt (p, gbase, "time"),
		.cycle_wrapped_g = QC_GlobalAt (p, gbase, "cycle_wrapped"),
		.frame_f = QC_FieldOfsOf (vm, "frame"),
		.think_f = QC_FieldOfsOf (vm, "think"),
		.nextthink_f = QC_FieldOfsOf (vm, "nextthink"),
		.weaponframe_f = QC_FieldOfsOf (vm, "weaponframe"),
	};
	return true;
}

// the load-time fixups of a progs' globals: pointer relocations, thisprogs,
// and FTE's fast-track arrays
void QC_FixupGlobals (qcvm_t *vm, const qc_progs_t *p, uint32_t gbase, uint32_t prnum)
{
	const qc_def_t	*d;
	uint64_t		at;
	uint32_t		i, v;

	for (i = 0 ; i < p->numpointer_relocs ; i++)
	{
		at = gbase + (uint64_t)p->pointer_relocs[i] * 4;
		v = QC_GetS (&vm->mem, at);
		QC_SetS (&vm->mem, at, (v & 0x7FFFFFFFu) + gbase);
	}
	if (p->thisprogs >= 0)
		QC_SetS (&vm->mem, gbase + (uint64_t)p->thisprogs * 4, prnum ? QC_FloatBits ((float)prnum) : 0);
	if (p->fasttrackarrays >= 0)
	{
		d = QC_GlobalDefRaw (p, "__ext__fasttrackarrays");
		QC_SetS (&vm->mem, gbase + (uint64_t)p->fasttrackarrays * 4,
			d && d->type == QC_EV_FLOAT ? QC_FloatBits (1) : 1);
	}
}

/*
==============================================================================

SHARED GLOBALS

Globals the compiler flagged as shared, and those the configuration names,
are copied between progs whenever execution passes from one to another.

==============================================================================
*/

static bool QC_SharedSlot (qcvm_t *vm, const char *name)
{
	qc_sharedtable_t	*t = &vm->shared;
	size_t				len = strlen (name);
	char				*copy, **grown;
	uint32_t			n;

	if (QC_MapGet (&t->by_name, name, len, NULL))
		return true;
	if (t->count == t->size)
	{
		n = t->size ? t->size * 2 : 16;
		grown = realloc (t->names, (size_t)n * sizeof(*grown));
		if (!grown)
			return false;
		t->names = grown;
		t->size = n;
	}
	copy = malloc (len + 1);
	if (!copy)
		return false;
	memcpy (copy, name, len + 1);
	if (!QC_MapAdd (&t->by_name, copy, len, t->count))
	{
		free (copy);
		return false;
	}
	t->names[t->count++] = copy;
	return true;
}

static bool QC_IsConfiguredShared (const qcvm_t *vm, const char *name)
{
	char	**n;

	for (n = vm->shared_names ; n && *n ; n++)
		if (!strcmp (*n, name))
			return true;
	return false;
}

// registers the shared globals of progs pr, and refreshes every progs' view
// of every slot
bool QC_RegisterShared (qcvm_t *vm, uint32_t pr)
{
	const qc_progs_t	*p = vm->progs[pr].progs;
	const qc_def_t		*d;
	qc_progstate_t		*ps;
	const char			*name;
	uint32_t			i, k;

	for (i = 0 ; i < p->numglobaldefs ; i++)
	{
		name = QC_Cstr (p, p->globaldefs[i].name);
		if (!*name || !(p->globaldefs[i].shared || QC_IsConfiguredShared (vm, name)))
			continue;
		if (!QC_SharedSlot (vm, name))
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS, NULL);
	}
	for (k = 0 ; k < vm->numprogs ; k++)
	{
		ps = &vm->progs[k];
		free (ps->shared);
		ps->shared = calloc (vm->shared.count + 1, sizeof(*ps->shared));
		ps->numshared = 0;
		if (!ps->shared)
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS, NULL);
		ps->numshared = vm->shared.count;
		for (i = 0 ; i < vm->shared.count ; i++)
		{
			d = QC_GlobalDefRaw (ps->progs, vm->shared.names[i]);
			if (!d || (uint64_t)ps->gbase + (uint64_t)d->ofs * 4 > UINT32_MAX)
				continue;
			ps->shared[i] = (qc_sharedglobal_t){true, ps->gbase + d->ofs * 4, QC_FieldWordsOf (d->type)};
		}
	}
	return true;
}

/*
==============================================================================

CREATION

==============================================================================
*/

static uint32_t QC_NextPow2 (uint64_t v)
{
	uint64_t	p = 16;

	while (p < v)
		p <<= 1;
	return p > 0x80000000ull ? 0 : (uint32_t)p;
}

static void QC_FreeCore (qcvm_t *vm)
{
	uint32_t	i;

	for (i = 0 ; i < vm->numprogs ; i++)
		QC_FreeProgState (&vm->progs[i]);
	free (vm->progs);
	vm->progs = NULL;
	vm->numprogs = 0;
	QC_RegionFree (&vm->mem.s);
	QC_RegionFree (&vm->mem.e);
	QC_HeapFree (&vm->mem.heap);
	free (vm->mem.slots);
	vm->mem = (qc_mem_t){0};
	QC_StringsFree (&vm->strings);
	QC_FreeFieldTable (&vm->fields);
	free (vm->frames);
	vm->frames = NULL;
	vm->numframes = 0;
	free (vm->remove_clears);
	vm->remove_clears = NULL;
	vm->numremove_clears = 0;
	free (vm->spawn_defaults);
	vm->spawn_defaults = NULL;
	vm->numspawn_defaults = 0;
	for (i = 0 ; i < vm->shared.count ; i++)
		free (vm->shared.names[i]);
	free (vm->shared.names);
	QC_MapClear (&vm->shared.by_name);
	vm->shared = (qc_sharedtable_t){0};
	QC_FlushWarnings (vm);
	for (i = 0 ; i < QC_STRING_COPIES ; i++)
	{
		free (vm->copies[i]);
		vm->copies[i] = NULL;
	}
}

static bool QC_OutOfMemory (qcvm_t *vm, qc_resource_t r)
{
	return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, r, NULL);
}

// lays out memory for the main progs and initializes it
static bool QC_BuildCore (qcvm_t *vm)
{
	const qc_limits_t	*limits = &vm->config.limits;
	qc_progs_t			*p = vm->main;
	qc_mem_t			*m = &vm->mem;
	uint64_t			gbase, ls_base, s_len, s_reserve, field_bytes, reserve, stride, fit, maxe, h_base;
	uint32_t			ls_words, header = vm->config.entity_header_bytes, i, n;
	const char			*const *name;
	const qc_fieldentry_t	*f;
	const qc_def_t		*d;

	if (header % 4)
		return QC_Fail (vm, QC_ERR_HOST, 0, "entity_header_bytes %u is not a multiple of 4", header);

	// region S: strings | globals and their three-word tail | the local stack
	// (and four bytes, as FTE)
	gbase = ((uint64_t)p->numstrings + 1 + 3) & ~3ull;
	ls_base = gbase + ((uint64_t)p->numglobals + 3) * 4;
	ls_words = limits->local_stack_words > 16 ? limits->local_stack_words : 16;
	s_len = ls_base + (uint64_t)ls_words * 4 + 4;
	s_reserve = ((s_len + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1)) + limits->progs_area_bytes;
	if (s_reserve >= ADDRESS_LIMIT)
		return QC_OutOfMemory (vm, QC_RES_PROGS_AREA);

	// region E: blocks of a power-of-two stride, the host's header first, with
	// room for fields added later and a zero word after the most there can be
	field_bytes = (uint64_t)p->entityfields * 4;
	reserve = vm->config.field_reserve_bytes > field_bytes / 2 ? vm->config.field_reserve_bytes : field_bytes / 2;
	stride = QC_NextPow2 (header + field_bytes + reserve);
	while (stride && header + field_bytes + 4 > stride)
		stride = stride < 0x80000000ull ? stride * 2 : 0;
	if (!stride)
		return QC_OutOfMemory (vm, QC_RES_FIELDS);
	for (m->shift = 0 ; (1ull << m->shift) < stride ; m->shift++)
		;

	// region H after the most entities there can be
	fit = (ADDRESS_LIMIT - s_reserve);
	fit = fit > (uint64_t)limits->heap_bytes + 16 ? (fit - limits->heap_bytes - 16) >> m->shift : 0;
	maxe = limits->max_edicts < fit ? limits->max_edicts : fit;
	if (maxe < 1)
		return QC_OutOfMemory (vm, QC_RES_ENTITIES);
	h_base = (s_reserve + (maxe << m->shift) + 15) & ~15ull;

	if (!QC_RegionReserve (&m->s, (size_t)s_reserve + QC_S_SLACK)
		|| !QC_RegionCommit (&m->s, (size_t)s_len + QC_S_SLACK))
		return QC_OutOfMemory (vm, QC_RES_PROGS_AREA);
	m->s_len = (uint32_t)s_len;
	m->ls_base = (uint32_t)ls_base;
	m->ls_words = ls_words;
	memcpy (m->s.base, p->strings, p->numstrings);
	for (i = 0 ; i < p->numglobals ; i++)
		memcpy (m->s.base + gbase + (size_t)i * 4, &p->globals[i], 4);

	if (!QC_RegionReserve (&m->e, (size_t)maxe << m->shift))
		return QC_OutOfMemory (vm, QC_RES_ENTITIES);
	m->header = header;
	m->fields = m->e.base + header;
	m->e_base = (uint32_t)s_reserve;
	m->field_bytes = (uint32_t)field_bytes;
	m->field_capacity = (uint32_t)stride - header - 4;
	m->max_edicts = (uint32_t)maxe;
	if (!QC_HeapInit (&m->heap, limits->heap_bytes))
		return QC_OutOfMemory (vm, QC_RES_HEAP);
	m->h_base = (uint32_t)h_base;

	// the world
	if (!QC_GrowEdicts (m, 0))
		return QC_OutOfMemory (vm, QC_RES_ENTITIES);
	QC_ClearEntity (m, 0);
	m->slots[0] = (qc_entslot_t){.in_use = true, .serial = 1};

	vm->frames = malloc (((size_t)limits->call_depth + 1) * sizeof(*vm->frames));
	if (!vm->frames || !QC_StringsInit (&vm->strings, limits->temp_strings, limits->temp_string_bytes))
		return QC_OutOfMemory (vm, QC_RES_PROGS);

	QC_FixupGlobals (vm, p, (uint32_t)gbase, 0);

	vm->fields.words = p->entityfields;
	for (i = 0 ; i < p->numfielddefs ; i++)
		if (!QC_AddFieldEntry (&vm->fields, QC_Cstr (p, p->fielddefs[i].name), p->fielddefs[i].type,
			p->fielddefs[i].ofs))
			return QC_OutOfMemory (vm, QC_RES_FIELDS);

	vm->progs = calloc (vm->config.limits.progs ? vm->config.limits.progs : 1, sizeof(*vm->progs));
	if (!vm->progs)
		return QC_OutOfMemory (vm, QC_RES_PROGS);
	if (!QC_InitProgState (vm, &vm->progs[0], p, 0, (uint32_t)gbase))
		return false;
	vm->numprogs = 1;

	// the fields remove zeroes, and those spawn fills
	for (name = vm->config.remove_clears, n = 0 ; name && *name ; name++)
		n++;
	vm->remove_clears = calloc (n + 1, sizeof(*vm->remove_clears));
	vm->spawn_defaults = calloc (vm->config.num_spawn_defaults + 1, sizeof(*vm->spawn_defaults));
	if (!vm->remove_clears || !vm->spawn_defaults)
		return QC_OutOfMemory (vm, QC_RES_FIELDS);
	for (name = vm->config.remove_clears ; name && *name ; name++)
		if ((f = QC_FieldEntry (vm, *name)))
			vm->remove_clears[vm->numremove_clears++] = (qc_fieldfill_t){f->ofs, QC_FieldWordsOf (f->type), -1, 0};
	for (i = 0 ; i < vm->config.num_spawn_defaults ; i++)
	{
		const qc_spawndefault_t	*sd = &vm->config.spawn_defaults[i];

		if (!sd->field || !(f = QC_FieldEntry (vm, sd->field)))
			continue;
		d = sd->global ? QC_GlobalDefRaw (p, sd->global) : NULL;
		vm->spawn_defaults[vm->numspawn_defaults++] = (qc_fieldfill_t){f->ofs, 1,
			d ? (int64_t)gbase + (int64_t)d->ofs * 4 : -1, QC_FloatBits (sd->value)};
	}

	vm->x = (qc_exec_t){.func = QC_NO_FUNCTION};
	vm->argc = 0;
	vm->builtin = 0;
	vm->nesting = 0;
	vm->rng = vm->config.seed;
	vm->warnings_this_call = vm->suppressed = 0;
	vm->trace = vm->traced = false;
	vm->has_abort_ret = false;
	vm->entry_depth = 0;
	vm->budget = 0;
	vm->has_deadline = false;

	if (!QC_RegisterShared (vm, 0))
		return false;
	QC_ApplySpawnDefaults (vm, 0);
	return true;
}

static char *QC_CopyString (const char *s)
{
	size_t	len = strlen (s);
	char	*copy = malloc (len + 1);

	if (copy)
		memcpy (copy, s, len + 1);
	return copy;
}

// a copy of a NULL-terminated list of names
static char **QC_CopyNames (const char *const *names)
{
	const char	*const *name;
	uint32_t	n = 0, i;
	char		**copy;

	for (name = names ; name && *name ; name++)
		n++;
	copy = calloc (n + 1, sizeof(*copy));
	if (!copy)
		return NULL;
	for (i = 0 ; i < n ; i++)
		if (!(copy[i] = QC_CopyString (names[i])))
			return copy;		// freed with the VM; the list is short by the rest
	return copy;
}

static void QC_FreeNames (char **names)
{
	char	**n;

	for (n = names ; n && *n ; n++)
		free (*n);
	free (names);
}

// the VM's own copies of the configuration's lists, which a reset resolves again
static bool QC_CopyConfig (qcvm_t *vm, const qc_config_t *config)
{
	qc_spawndefault_t	*d;
	uint32_t			i;

	vm->config = *config;
	vm->shared_names = QC_CopyNames (config->shared_globals);
	vm->clear_names = QC_CopyNames (config->remove_clears);
	vm->spawn_list = calloc (config->num_spawn_defaults + 1, sizeof(*vm->spawn_list));
	if (!vm->shared_names || !vm->clear_names || !vm->spawn_list)
		return false;
	vm->config.shared_globals = (const char *const *)vm->shared_names;
	vm->config.remove_clears = (const char *const *)vm->clear_names;
	vm->config.spawn_defaults = vm->spawn_list;
	for (i = 0 ; i < config->num_spawn_defaults ; i++)
	{
		d = &vm->spawn_list[i];
		d->value = config->spawn_defaults[i].value;
		d->field = config->spawn_defaults[i].field ? QC_CopyString (config->spawn_defaults[i].field) : NULL;
		d->global = config->spawn_defaults[i].global ? QC_CopyString (config->spawn_defaults[i].global) : NULL;
	}
	return true;
}

static void QC_FreeConfig (qcvm_t *vm)
{
	uint32_t	i;

	QC_FreeNames (vm->shared_names);
	QC_FreeNames (vm->clear_names);
	for (i = 0 ; vm->spawn_list && i < vm->config.num_spawn_defaults ; i++)
	{
		free ((char *)vm->spawn_list[i].field);
		free ((char *)vm->spawn_list[i].global);
	}
	free (vm->spawn_list);
	vm->shared_names = vm->clear_names = NULL;
	vm->spawn_list = NULL;
}

qcvm_t *QC_Create (qc_progs_t *progs, const qc_builtins_t *builtins, const qc_config_t *config,
	const qc_host_t *host, void *ctx, qc_error_t *error)
{
	qcvm_t		*vm = calloc (1, sizeof(*vm));
	qc_config_t	defaults;

	if (error)
		*error = (qc_error_t){0};
	if (!vm)
	{
		if (error)
			*error = (qc_error_t){.kind = QC_ERR_OUT_OF_MEMORY, .value = QC_RES_PROGS};
		return NULL;
	}
	if (!config)
	{
		QC_DefaultConfig (&defaults, QC_CSQC);
		config = &defaults;
	}
	vm->main = progs;
	QC_RetainProgs (progs);
	vm->builtins = builtins;
	if (host)
		vm->host = *host;
	vm->ctx = ctx;
	vm->started = QC_Clock (vm);
	if (!QC_CopyConfig (vm, config))
		QC_OutOfMemory (vm, QC_RES_PROGS);
	else if (QC_BuildCore (vm))
		return vm;
	if (error)
	{
		*error = vm->error;
		vm->error = (qc_error_t){0};
	}
	QC_Destroy (vm);
	return NULL;
}

void QC_Destroy (qcvm_t *vm)
{
	if (!vm)
		return;
	vm->host.warning = NULL;		// nothing more to tell
	QC_FreeCore (vm);
	QC_ReleaseProgs (vm->main);
	QC_FreeConfig (vm);
	QC_ClearError (&vm->error);
	free (vm);
}

bool QC_Reset (qcvm_t *vm)
{
	QC_FreeCore (vm);
	return QC_BuildCore (vm);
}

qc_progs_t *QC_MainProgs (const qcvm_t *vm)
{
	return vm->main;
}

void *QC_HostContext (const qcvm_t *vm)
{
	return vm->ctx;
}

const qc_config_t *QC_Config (const qcvm_t *vm)
{
	return &vm->config;
}

void QC_SetDeveloper (qcvm_t *vm, bool on)
{
	vm->config.developer = on;
}

void QC_SetTime (qcvm_t *vm, double seconds)
{
	vm->time = seconds;
	vm->time_set = true;
}

double QC_Time (const qcvm_t *vm)
{
	return QC_Now (vm);
}

const qc_error_t *QC_LastError (const qcvm_t *vm)
{
	return &vm->error;
}

/*
==============================================================================

GLOBALS AND FIELDS

==============================================================================
*/

bool QC_FindGlobal (const qcvm_t *vm, const char *name, uint32_t *word, uint32_t *type)
{
	const qc_def_t	*d = QC_GlobalDefRaw (vm->main, name);

	if (!d)
		return false;
	if (word)
		*word = d->ofs;
	if (type)
		*type = d->type;
	return true;
}

qc_word_t *QC_Globals (qcvm_t *vm)
{
	return (qc_word_t *)(vm->mem.s.base + vm->progs[0].gbase);
}

uint32_t QC_NumGlobals (const qcvm_t *vm)
{
	return vm->main->numglobals;
}

bool QC_FindField (const qcvm_t *vm, const char *name, uint32_t *ofs, uint32_t *type)
{
	const qc_fieldentry_t	*f = QC_FieldEntry (vm, name);

	if (!f)
		return false;
	if (ofs)
		*ofs = f->ofs;
	if (type)
		*type = f->type;
	return true;
}

// whether a field of type have holds a value of type want (as qcvm-rs's typed handles)
static bool QC_TypeAccepts (uint32_t want, uint32_t have)
{
	switch (want)
	{
	case QC_EV_INTEGER:
		return have == QC_EV_INTEGER || have == QC_EV_FLOAT || have == QC_EV_ENTITY || have == QC_EV_FIELD
			|| have == QC_EV_FUNCTION || have == QC_EV_POINTER || have == QC_EV_STRING;
	case QC_EV_UINT:
		return have == QC_EV_UINT || QC_TypeAccepts (QC_EV_INTEGER, have);
	case QC_EV_INT64:
	case QC_EV_UINT64:
		return have == QC_EV_INT64 || have == QC_EV_UINT64;
	default:
		return want == have;
	}
}

bool QC_EnsureField (qcvm_t *vm, const char *name, uint32_t type, uint32_t *ofs)
{
	const qc_fieldentry_t	*f = QC_FieldEntry (vm, name);
	uint32_t				words = QC_FieldWordsOf (type), at;
	uint64_t				bytes;

	if (f)
	{
		if (!QC_TypeAccepts (type, f->type))
			return false;
		*ofs = f->ofs;
		return true;
	}
	at = vm->fields.words;
	bytes = ((uint64_t)at + words) * 4;
	if (bytes > vm->mem.field_capacity || !QC_AddFieldEntry (&vm->fields, name, type, at))
		return false;
	vm->fields.words = at + words;
	vm->mem.field_bytes = (uint32_t)bytes;
	*ofs = at;
	return true;
}

uint32_t QC_NumFields (const qcvm_t *vm)
{
	return vm->fields.count;
}

bool QC_FieldAt (const qcvm_t *vm, uint32_t i, const char **name, uint32_t *type, uint32_t *ofs)
{
	if (i >= vm->fields.count)
		return false;
	if (name)
		*name = vm->fields.entries[i].name;
	if (type)
		*type = vm->fields.entries[i].type;
	if (ofs)
		*ofs = vm->fields.entries[i].ofs;
	return true;
}

uint32_t QC_FieldWords (const qcvm_t *vm)
{
	return vm->fields.words;
}

bool QC_GetField (const qcvm_t *vm, qc_ent_t e, uint32_t ofs, uint32_t words, uint32_t *out)
{
	const uint8_t	*p = QC_FieldPtr (&vm->mem, e, ofs, words);

	if (!p)
		return false;
	memcpy (out, p, (size_t)words * 4);
	return true;
}

bool QC_SetField (qcvm_t *vm, qc_ent_t e, uint32_t ofs, uint32_t words, const uint32_t *in)
{
	uint8_t	*p = QC_FieldPtr (&vm->mem, e, ofs, words);

	if (!p)
		return false;
	memcpy (p, in, (size_t)words * 4);
	return true;
}

bool QC_ReadMemory (const qcvm_t *vm, qc_ptr_t p, void *out, size_t len)
{
	uint8_t		*o = out;
	size_t		i;
	qc_loc_t	loc;

	for (i = 0 ; i < len ; i++)
	{
		if ((uint64_t)p + i > UINT32_MAX)
			return false;
		loc = QC_Locate (&vm->mem, (uint32_t)(p + i), 1);
		if (loc.type == QC_LOC_NONE)
			return false;
		o[i] = *loc.p;
	}
	return true;
}

bool QC_WriteMemory (qcvm_t *vm, qc_ptr_t p, const void *in, size_t len)
{
	return len <= UINT32_MAX && QC_WriteBytes (&vm->mem, p, in, (uint32_t)len, NULL) == QC_WRITE_OK;
}

/*
==============================================================================

ENTITIES

==============================================================================
*/

uint8_t *QC_Edicts (qcvm_t *vm)
{
	return vm->mem.e.base;
}

uint32_t QC_EdictShift (const qcvm_t *vm)
{
	return vm->mem.shift;
}

uint32_t QC_NumEdicts (const qcvm_t *vm)
{
	return vm->mem.num_edicts;
}

uint32_t QC_MaxEdicts (const qcvm_t *vm)
{
	return vm->mem.max_edicts;
}

bool QC_CommitEdicts (qcvm_t *vm, uint32_t count)
{
	if (count > vm->mem.max_edicts)
		count = vm->mem.max_edicts;
	return QC_RegionCommit (&vm->mem.e, (size_t)count << vm->mem.shift);
}

// a slot as FTE picks one: the first free one freed more than half a second
// ago (or in the first two seconds), else a new one, else any free one
static bool QC_PickSlot (qcvm_t *vm, uint32_t *out)
{
	qc_mem_t	*m = &vm->mem;
	double		now = QC_Now (vm);
	uint32_t	first = vm->config.first_spawnable, start, grow, e;

	start = first < m->num_edicts ? first : m->num_edicts;
	for (e = start ; e < m->num_edicts ; e++)
		if (!m->slots[e].in_use && (m->slots[e].freetime < 2 || now - m->slots[e].freetime > 0.5))
		{
			*out = e;
			return true;
		}
	// growing starts at the first spawnable slot: reserved ones are never handed out
	grow = m->num_edicts > first ? m->num_edicts : first;
	if (grow < m->max_edicts)
	{
		if (!QC_GrowEdicts (m, grow))
			return QC_OutOfMemory (vm, QC_RES_ENTITIES);
		*out = grow;
		return true;
	}
	for (e = start ; e < m->num_edicts ; e++)
		if (!m->slots[e].in_use)
		{
			*out = e;
			return true;
		}
	return QC_Fail (vm, QC_ERR_NO_FREE_EDICTS, 0, NULL);
}

// a slot taken: fields zeroed, in use, a new serial
static void QC_TakeSlot (qcvm_t *vm, uint32_t e)
{
	qc_entslot_t	*slot = &vm->mem.slots[e];

	QC_ClearEntity (&vm->mem, e);
	slot->in_use = true;
	slot->protected = false;
	slot->serial++;
	QC_ApplySpawnDefaults (vm, e);
}

bool QC_Spawn (qcvm_t *vm, qc_ent_t *e)
{
	uint32_t	slot;

	if (!QC_PickSlot (vm, &slot))
		return false;
	QC_TakeSlot (vm, slot);
	if (vm->host.on_spawn)
		vm->host.on_spawn (vm->ctx, vm, slot);
	*e = slot;
	return true;
}

bool QC_ClaimEdict (qcvm_t *vm, qc_ent_t e)
{
	if (e >= vm->mem.max_edicts)
		return QC_Fail (vm, QC_ERR_NO_FREE_EDICTS, 0, NULL);
	if (!QC_GrowEdicts (&vm->mem, e))
		return QC_OutOfMemory (vm, QC_RES_ENTITIES);
	if (vm->mem.slots[e].in_use)
		return true;
	QC_TakeSlot (vm, e);
	if (vm->host.on_spawn)
		vm->host.on_spawn (vm->ctx, vm, e);
	return true;
}

bool QC_Remove (qcvm_t *vm, qc_ent_t e, bool instant)
{
	qc_mem_t				*m = &vm->mem;
	const qc_fieldfill_t	*c;
	uint8_t					*p;
	uint32_t				i;

	if (!e)
	{
		QC_Warn (vm, QC_WARN_BUILTIN, 0, "Unable to remove the world");
		return false;
	}
	if (e >= m->num_edicts)
	{
		QC_Warn (vm, QC_WARN_BAD_ENTITY, e, NULL);
		return false;
	}
	if (!m->slots[e].in_use)
	{
		QC_Warn (vm, QC_WARN_BUILTIN, 0, "entity %u is already free", e);
		return false;
	}
	if (m->slots[e].protected)
	{
		QC_Warn (vm, QC_WARN_READONLY_ENTITY, e, NULL);
		return false;
	}
	if (vm->host.on_remove && !vm->host.on_remove (vm->ctx, vm, e))
		return false;
	for (i = 0 ; i < vm->numremove_clears ; i++)
	{
		c = &vm->remove_clears[i];
		if ((p = QC_FieldPtr (m, e, c->ofs, c->words)))
			memset (p, 0, (size_t)c->words * 4);
	}
	m->slots[e].in_use = false;
	m->slots[e].freetime = instant ? 0 : QC_Now (vm);
	return true;
}

bool QC_IsFree (const qcvm_t *vm, qc_ent_t e)
{
	return !QC_InUse (&vm->mem, e);
}

bool QC_SetProtected (qcvm_t *vm, qc_ent_t e, bool on)
{
	bool	old;

	if (e >= vm->mem.num_edicts)
		return false;
	old = vm->mem.slots[e].protected;
	vm->mem.slots[e].protected = on;
	return old;
}

bool QC_IsProtected (const qcvm_t *vm, qc_ent_t e)
{
	return QC_Protected (&vm->mem, e);
}

uint32_t QC_Serial (const qcvm_t *vm, qc_ent_t e)
{
	return e < vm->mem.num_edicts ? vm->mem.slots[e].serial : 0;
}

/*
==============================================================================

STRINGS

==============================================================================
*/

bool QC_IsTempString (qc_str_t s)
{
	return (s & QC_TAG_MASK) == QC_TEMP_TAG;
}

// a copy of text QuakeC left unterminated at the end of a region, with a NUL
static const char *QC_TerminatedCopy (qcvm_t *vm, const uint8_t *p, size_t len)
{
	char	*copy = malloc (len + 1);

	if (!copy)
		return NULL;
	memcpy (copy, p, len);
	copy[len] = 0;
	free (vm->copies[vm->nextcopy]);
	vm->copies[vm->nextcopy] = copy;
	vm->nextcopy = (vm->nextcopy + 1) % QC_STRING_COPIES;
	return copy;
}

const char *QC_Str (qcvm_t *vm, uint32_t ref)
{
	qc_loc_t	loc;

	switch (ref & QC_TAG_MASK)
	{
	case QC_TEMP_TAG:
		return (const char *)QC_TempData (&vm->strings, ref & QC_INDEX_MASK, NULL);
	case QC_STATIC_TAG:
		return QC_StaticText (&vm->strings, ref & QC_INDEX_MASK);
	default:
		loc = QC_Locate (&vm->mem, ref, 1);
		if (loc.type == QC_LOC_NONE)
			return NULL;
		if (memchr (loc.p, 0, loc.avail))
			return (const char *)loc.p;
		return QC_TerminatedCopy (vm, loc.p, loc.avail);
	}
}

const char *QC_StrOrWarn (qcvm_t *vm, uint32_t ref)
{
	const char	*s = QC_Str (vm, ref);

	if (s)
		return s;
	QC_Warn (vm, QC_WARN_BAD_STRING, ref, NULL);
	return "";
}

const char *QC_String (qcvm_t *vm, qc_str_t s)
{
	const char	*text = QC_Str (vm, s);

	return text ? text : "";
}

bool QC_IsValidString (qcvm_t *vm, qc_str_t s)
{
	return QC_Str (vm, s) != NULL;
}

uint32_t QC_NewTemp (qcvm_t *vm, const char *text, size_t len)
{
	uint32_t	ref;

	if (!QC_TempAlloc (&vm->strings, text, len, &ref))
	{
		QC_OutOfMemory (vm, QC_RES_TEMP_STRINGS);
		return 0;
	}
	return ref;
}

qc_str_t QC_TempString (qcvm_t *vm, const char *text, size_t len)
{
	return QC_NewTemp (vm, text, len);
}

qc_str_t QC_Intern (qcvm_t *vm, const char *text, size_t len)
{
	uint32_t	ref;

	return QC_StaticIntern (&vm->strings, text, len, &ref) ? ref : 0;
}

qc_str_t QC_HostString (qcvm_t *vm, const char *text)
{
	uint32_t	ref;

	return QC_StaticBorrow (&vm->strings, text, &ref) ? ref : 0;
}

void QC_Pin (qcvm_t *vm, qc_str_t s)
{
	QC_StringsPin (&vm->strings, s);
}

void QC_Unpin (qcvm_t *vm, qc_str_t s)
{
	QC_StringsUnpin (&vm->strings, s);
}

uint32_t QC_NumTempStrings (const qcvm_t *vm)
{
	return vm->strings.live;
}

// Marks from every aligned word of VM memory: region S, each entity's fields,
// the heap, and the threads' snapshots.
uint32_t QC_CollectNow (qcvm_t *vm)
{
	qc_mem_t	*m = &vm->mem;
	uint8_t		*marks;
	uint32_t	e;

	if (!QC_GCBegin (&vm->strings, &marks))
		return 0;
	QC_GCMark (&vm->strings, marks, m->s.base, m->s_len);
	for (e = 0 ; e < m->num_edicts ; e++)
		QC_GCMark (&vm->strings, marks, m->fields + ((size_t)e << m->shift), m->field_bytes);
	QC_GCMark (&vm->strings, marks, m->heap.region.base, m->heap.len);
	QC_MarkThreads (vm, marks);
	return QC_GCSweep (&vm->strings, marks);
}

uint32_t QC_CollectGarbage (qcvm_t *vm)
{
	if (vm->nesting)
		return 0;
	return QC_CollectNow (vm);
}
