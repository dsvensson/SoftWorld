// qc_multiprogs.c -- several progs in one VM: FTE's multiprogs, as addprogs and
// CSQC add-ons use them
//
// Each progs keeps its own globals block (with its own PARM and RETURN slots)
// in region S, after its strings, so its string offsets become addresses once
// relocated. Function values carry the progs number in their top byte. Entity
// fields are unified by name: a later progs' fields map onto the layout there
// is, and new ones take words from the room each entity reserves, so entity
// data never moves (the main progs' map onto the host's fields the same way,
// when it lays them out). The shared globals (qc_vm.c) are copied between
// progs whenever execution passes from one to another.

#include "qc_local.h"

#include <stdlib.h>

// bytes after a progs' globals: FTE's three-word zero tail and two guard words,
// so vector reads at the last legal operand stay inside the block
#define GLOBALS_TAIL_BYTES	20

/*
==============================================================================

FIELD WORDS

A progs' field words, mapped to the VM's.

==============================================================================
*/

static uint32_t QC_WordHash (uint32_t k)
{
	k ^= k >> 16;
	k *= 0x7FEB352Du;
	k ^= k >> 15;
	k *= 0x846CA68Bu;
	return k ^ k >> 16;
}

static qc_wordslot_t *QC_WordSlot (const qc_wordmap_t *m, uint32_t key)
{
	uint32_t	i = QC_WordHash (key) & m->mask;

	while (m->slots[i].used && m->slots[i].key != key)
		i = (i + 1) & m->mask;
	return &m->slots[i];
}

static bool QC_WordGet (const qc_wordmap_t *m, uint32_t key, uint32_t *value)
{
	const qc_wordslot_t	*s;

	if (!m->slots)
		return false;
	s = QC_WordSlot (m, key);
	if (s->used)
		*value = s->value;
	return s->used;
}

// maps key to value unless it is mapped; false when out of memory
static bool QC_WordAdd (qc_wordmap_t *m, uint32_t key, uint32_t value)
{
	qc_wordslot_t	*old = m->slots, *s;
	uint32_t		oldsize = old ? m->mask + 1 : 0, size, i;

	if ((uint64_t)(m->count + 1) * 2 > oldsize)
	{
		size = oldsize ? oldsize * 2 : 64;
		if (size > (1u << 28) || !(m->slots = calloc (size, sizeof(*m->slots))))
		{
			m->slots = old;
			return false;
		}
		m->mask = size - 1;
		for (i = 0 ; i < oldsize ; i++)
			if (old[i].used)
				*QC_WordSlot (m, old[i].key) = old[i];
		free (old);
	}
	s = QC_WordSlot (m, key);
	if (!s->used)
	{
		*s = (qc_wordslot_t){key, value, true};
		m->count++;
	}
	return true;
}

/*
==============================================================================

FIELDS

==============================================================================
*/

typedef struct
{
	uint32_t	ofs, words, def;
} qc_fieldorder_t;

// by offset, the bigger first at equal offsets (so a vector maps before its
// components), then in file order
static int QC_CompareFieldOrder (const void *a, const void *b)
{
	const qc_fieldorder_t	*x = a, *y = b;

	if (x->ofs != y->ofs)
		return x->ofs < y->ofs ? -1 : 1;
	if (x->words != y->words)
		return x->words > y->words ? -1 : 1;
	return x->def < y->def ? -1 : x->def > y->def;
}

// words new field words, if they fit the room each entity has
static bool QC_TakeFieldWords (qcvm_t *vm, uint32_t words, uint32_t *at)
{
	uint64_t	next = (uint64_t)vm->fields.words + words;

	if (next > vm->mem.field_capacity / 4)
		return false;
	*at = vm->fields.words;
	vm->fields.words = (uint32_t)next;
	return true;
}

// Maps a progs' field words onto the VM's layout (a later progs', or the main
// progs' onto the host's fields), adding the fields it brings. False if they
// don't fit the room the fields have; the room is checked as words are taken,
// so a huge field count in the header costs no more than the room before it
// is refused.
bool QC_MapFields (qcvm_t *vm, const qc_progs_t *p, qc_wordmap_t *map)
{
	const qc_fieldentry_t	*existing;
	const qc_def_t			*d;
	qc_fieldorder_t			*order;
	const char				*name;
	uint32_t				i, w, u;
	bool					ok = true;

	order = malloc (((size_t)p->numfielddefs + 1) * sizeof(*order));
	if (!order)
		return false;
	for (i = 0 ; i < p->numfielddefs ; i++)
		order[i] = (qc_fieldorder_t){p->fielddefs[i].ofs, QC_FieldWordsOf (p->fielddefs[i].type), i};
	qsort (order, p->numfielddefs, sizeof(*order), QC_CompareFieldOrder);

	for (i = 0 ; ok && i < p->numfielddefs ; i++)
	{
		d = &p->fielddefs[order[i].def];
		name = QC_Cstr (p, d->name);
		existing = QC_FieldEntry (vm, name);
		if (existing && QC_TypeWords (existing->type) == QC_TypeWords (d->type))
			u = existing->ofs;
		else
		{
			// a new field, or one of another size: where its words already
			// went (an alias), else new words
			if (!QC_WordGet (map, d->ofs, &u))
				ok = QC_TakeFieldWords (vm, order[i].words, &u);
			ok = ok && QC_AddFieldEntry (&vm->fields, name, d->type, u);
		}
		for (w = 0 ; ok && w < order[i].words ; w++)
			ok = (uint64_t)d->ofs + w <= UINT32_MAX && (uint64_t)u + w <= UINT32_MAX
				&& QC_WordAdd (map, d->ofs + w, u + w);
	}
	free (order);

	// words no definition covers still need a home
	for (w = 0 ; ok && w < p->entityfields ; w++)
		if (!QC_WordGet (map, w, &u))
			ok = QC_TakeFieldWords (vm, 1, &u) && QC_WordAdd (map, w, u);
	if (ok)
		vm->mem.field_bytes = vm->fields.words * 4;
	return ok;
}

// drops the fields added after the first count, and the words they took
static void QC_RollbackFields (qcvm_t *vm, uint32_t count, uint32_t words, uint32_t field_bytes)
{
	qc_fieldtable_t	*t = &vm->fields;
	qc_fieldentry_t	*f;
	uint32_t		index;
	size_t			len;

	while (t->count > count)
	{
		f = &t->entries[--t->count];
		len = strlen (f->name);
		if (len && QC_MapGet (&t->by_name, f->name, len, &index) && index == t->count)
			QC_MapRemove (&t->by_name, f->name, len);
		free (f->name);
	}
	t->words = words;
	vm->mem.field_bytes = field_bytes;
}

/*
==============================================================================

LINKING

==============================================================================
*/

// writes function references into the globals of the bodyless (extern)
// functions that another loaded progs defines
static void QC_LinkExterns (qcvm_t *vm)
{
	const qc_progstate_t	*ps;
	const qc_progs_t		*p, *other;
	const qc_def_t			*d;
	const char				*name;
	uint32_t				pr, i, o, index;
	uint64_t				at;
	qc_funckind_t			kind;

	for (pr = 0 ; pr < vm->numprogs ; pr++)
	{
		ps = &vm->progs[pr];
		p = ps->progs;
		for (i = 0 ; i < p->numbodyless ; i++)
		{
			name = p->bodyless[i];
			d = QC_GlobalDefRaw (p, name);
			if (!d || d->type != QC_EV_FUNCTION)
				continue;
			at = ps->gbase + (uint64_t)d->ofs * 4;
			if (QC_FUNC_INDEX (QC_GetS (&vm->mem, at)))
				continue;
			for (o = 0 ; o < vm->numprogs ; o++)
			{
				other = vm->progs[o].progs;
				if (o == pr || !QC_MapGet (&other->functions_by_name, name, strlen (name), &index))
					continue;
				kind = other->functions[index].kind;
				if (kind == QC_FUNC_NULL || kind == QC_FUNC_INVALID)
					continue;
				QC_SetS (&vm->mem, at, QC_FUNC (o, index));
				break;
			}
		}
	}
}

/*
==============================================================================

ADDING PROGS

==============================================================================
*/

// relocates the typed globals of a progs laid out at sbase and gbase: strings
// by where its strings are, functions by its number, fields by the field map;
// each word once, whatever definitions share it
bool QC_RelocateGlobals (qcvm_t *vm, const qc_progs_t *p, uint32_t sbase, uint32_t gbase, uint32_t pr,
	const qc_wordmap_t *map)
{
	const qc_def_t	*d;
	uint8_t			*done = calloc ((size_t)p->numglobals / 8 + 1, 1);
	uint64_t		at;
	uint32_t		i, v, u;

	if (!done)
		return false;
	for (i = 0 ; i < p->numglobaldefs ; i++)
	{
		d = &p->globaldefs[i];
		if (d->ofs >= p->numglobals || done[d->ofs / 8] & (1 << (d->ofs % 8)))
			continue;
		at = gbase + (uint64_t)d->ofs * 4;
		v = QC_GetS (&vm->mem, at);
		switch (d->type)
		{
		case QC_EV_STRING:
			if (v && v < p->numstrings)
				QC_SetS (&vm->mem, at, v + sbase);
			break;
		case QC_EV_FUNCTION:
			if (v && v < p->numfunctions)
				QC_SetS (&vm->mem, at, QC_FUNC (pr, v));
			break;
		case QC_EV_FIELD:
			if (QC_WordGet (map, v, &u))
				QC_SetS (&vm->mem, at, u);
			break;
		default:
			continue;
		}
		done[d->ofs / 8] |= (uint8_t)(1 << (d->ofs % 8));
	}
	free (done);
	return true;
}

bool QC_AddProgs (qcvm_t *vm, qc_progs_t *p, uint32_t *prnum)
{
	qc_mem_t		*m = &vm->mem;
	uint32_t		pr = vm->numprogs, limit = vm->config.limits.progs < 256 ? vm->config.limits.progs : 256;
	uint32_t		fieldcount = vm->fields.count, fieldwords = vm->fields.words;
	uint32_t		field_bytes = m->field_bytes, i, index;
	uint64_t		sbase, gbase, end;
	qc_wordmap_t	map = {0};
	qc_progstate_t	*ps;
	qc_value_t		arg;
	bool			ok;

	if (pr >= limit)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS, NULL);

	// its strings and globals at the end of region S
	sbase = ((uint64_t)m->s_len + 15) & ~15ull;
	gbase = (sbase + p->numstrings + 4) & ~3ull;
	end = gbase + (uint64_t)p->numglobals * 4 + GLOBALS_TAIL_BYTES;
	if (end > m->e_base)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS_AREA, NULL);
	if (!QC_MapFields (vm, p, &map))
	{
		QC_RollbackFields (vm, fieldcount, fieldwords, field_bytes);
		free (map.slots);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_FIELDS, NULL);
	}
	if (!QC_RegionCommit (&m->s, (size_t)end + QC_S_SLACK))
	{
		QC_RollbackFields (vm, fieldcount, fieldwords, field_bytes);
		free (map.slots);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS_AREA, NULL);
	}
	memset (m->s.base + m->s_len, 0, (size_t)(end + QC_S_SLACK - m->s_len));
	memcpy (m->s.base + sbase, p->strings, p->numstrings);
	for (i = 0 ; i < p->numglobals ; i++)
		memcpy (m->s.base + gbase + (size_t)i * 4, &p->globals[i], 4);
	m->s_len = (uint32_t)end;

	ok = QC_RelocateGlobals (vm, p, (uint32_t)sbase, (uint32_t)gbase, pr, &map);
	free (map.slots);
	if (!ok)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_PROGS, NULL);
	QC_FixupGlobals (vm, p, (uint32_t)gbase, pr);

	ps = &vm->progs[pr];
	if (!QC_InitProgState (vm, ps, p, (uint32_t)sbase, (uint32_t)gbase))
		return false;
	if (vm->profiling)
		ps->profile = calloc ((size_t)p->numfunctions + 1, sizeof(uint64_t));
	vm->numprogs++;
	if (!QC_RegisterShared (vm, pr))
		return false;
	QC_LinkExterns (vm);
	if (prnum)
		*prnum = pr;

	if (!QC_MapGet (&p->functions_by_name, "init", 4, &index))
		return true;
	arg = QC_ValFloat ((float)(pr - 1));
	return QC_Call (vm, QC_FUNC (pr, index), 1, &arg, NULL);
}

uint32_t QC_NumProgs (const qcvm_t *vm)
{
	return vm->numprogs;
}

qc_progs_t *QC_LoadedProgs (const qcvm_t *vm, uint32_t pr)
{
	return pr < vm->numprogs ? vm->progs[pr].progs : NULL;
}
