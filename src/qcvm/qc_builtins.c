// qc_builtins.c -- builtin registries, and binding a progs' builtins to them

#include "qc_builtins.h"

#include <stdlib.h>

qc_builtins_t *QC_BuiltinsCreate (qc_numbering_t numbering)
{
	qc_builtins_t	*b = calloc (1, sizeof(*b));

	if (b)
		b->numbering = numbering;
	return b;
}

void QC_BuiltinsFree (qc_builtins_t *b)
{
	uint32_t	i;

	if (!b)
		return;
	for (i = 0 ; i < b->count ; i++)
	{
		free (b->entries[i]->name);
		free (b->entries[i]);
	}
	free (b->entries);
	QC_MapClear (&b->by_name);
	free (b->numbers);
	free (b);
}

qc_numbering_t QC_BuiltinsNumbering (const qc_builtins_t *b)
{
	return b->numbering;
}

// the index of the first binding at or after number
static uint32_t QC_NumberSearch (const qc_builtins_t *b, uint32_t number)
{
	uint32_t	lo = 0, hi = b->numnumbers, mid;

	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		if (b->numbers[mid].number < number)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static bool QC_NumberBound (const qc_builtins_t *b, uint32_t number, uint32_t *index)
{
	uint32_t	i = QC_NumberSearch (b, number);

	if (i == b->numnumbers || b->numbers[i].number != number)
		return false;
	*index = b->numbers[i].index;
	return true;
}

static void QC_NumberUnbind (qc_builtins_t *b, uint32_t number)
{
	uint32_t	i = QC_NumberSearch (b, number);

	if (i < b->numnumbers && b->numbers[i].number == number)
	{
		memmove (b->numbers + i, b->numbers + i + 1, (size_t)(b->numnumbers - i - 1) * sizeof(*b->numbers));
		b->numnumbers--;
	}
}

static bool QC_NumberBind (qc_builtins_t *b, uint32_t number, uint32_t index)
{
	uint32_t		i = QC_NumberSearch (b, number), n;
	qc_bnumber_t	*grown;

	if (i < b->numnumbers && b->numbers[i].number == number)
	{
		b->numbers[i].index = index;
		return true;
	}
	if (b->numnumbers == b->numbersize)
	{
		n = b->numbersize ? b->numbersize * 2 : 64;
		grown = realloc (b->numbers, (size_t)n * sizeof(*grown));
		if (!grown)
			return false;
		b->numbers = grown;
		b->numbersize = n;
	}
	memmove (b->numbers + i + 1, b->numbers + i, (size_t)(b->numnumbers - i) * sizeof(*b->numbers));
	b->numbers[i] = (qc_bnumber_t){number, index};
	b->numnumbers++;
	return true;
}

static bool QC_BuiltinsInsert (qc_builtins_t *b, const char *name, bool numbered, uint32_t number, qc_builtin_t func)
{
	qc_bentry_t		*e, **grown;
	uint32_t		index, old, n;
	size_t			len = strlen (name);

	if (QC_MapGet (&b->by_name, name, len, &index))
	{
		e = b->entries[index];
		// unbind the old number, unless another builtin has taken it since
		if (e->numbered && QC_NumberBound (b, e->number, &old) && old == index)
			QC_NumberUnbind (b, e->number);
		e->numbered = numbered;
		e->number = number;
		e->func = func;
	}
	else
	{
		if (b->count == b->size)
		{
			n = b->size ? b->size * 2 : 256;
			grown = realloc (b->entries, (size_t)n * sizeof(*grown));
			if (!grown)
				return false;
			b->entries = grown;
			b->size = n;
		}
		e = calloc (1, sizeof(*e));
		if (!e)
			return false;
		e->name = malloc (len + 1);
		if (!e->name)
		{
			free (e);
			return false;
		}
		memcpy (e->name, name, len + 1);
		e->numbered = numbered;
		e->number = number;
		e->func = func;
		index = b->count;
		if (!QC_MapAdd (&b->by_name, e->name, len, index))
		{
			free (e->name);
			free (e);
			return false;
		}
		b->entries[b->count++] = e;
	}
	return !numbered || QC_NumberBind (b, number, index);
}

bool QC_BuiltinsSetNumbered (qc_builtins_t *b, uint32_t number, const char *name, qc_builtin_t func)
{
	return QC_BuiltinsInsert (b, name, true, number, func);
}

bool QC_BuiltinsSet (qc_builtins_t *b, const char *name, qc_builtin_t func)
{
	uint32_t	number = 0;
	bool		numbered = QC_BuiltinNumber (b->numbering, name, &number);

	return QC_BuiltinsInsert (b, name, numbered, number, func);
}

bool QC_BuiltinsAlias (qc_builtins_t *b, const char *alias, const char *target)
{
	uint32_t	index;

	if (!QC_MapGet (&b->by_name, target, strlen (target), &index))
		return true;
	return QC_BuiltinsInsert (b, alias, false, 0, b->entries[index]->func);
}

void QC_BuiltinsRemove (qc_builtins_t *b, const char *name)
{
	uint32_t	index, i;

	if (!QC_MapGet (&b->by_name, name, strlen (name), &index))
		return;
	QC_MapRemove (&b->by_name, name, strlen (name));
	for (i = 0 ; i < b->numnumbers ; )
	{
		if (b->numbers[i].index == index)
		{
			memmove (b->numbers + i, b->numbers + i + 1, (size_t)(b->numnumbers - i - 1) * sizeof(*b->numbers));
			b->numnumbers--;
		}
		else
			i++;
	}
}

// the entry at slot index if it's still registered (removed ones keep their slot)
static const qc_bentry_t *QC_LiveEntry (const qc_builtins_t *b, uint32_t index)
{
	const qc_bentry_t	*e = b->entries[index];
	uint32_t			at;

	return QC_MapGet (&b->by_name, e->name, strlen (e->name), &at) && at == index ? e : NULL;
}

uint32_t QC_BuiltinsCount (const qc_builtins_t *b)
{
	uint32_t	i, n = 0;

	for (i = 0 ; i < b->count ; i++)
		n += QC_LiveEntry (b, i) != NULL;
	return n;
}

bool QC_BuiltinsAt (const qc_builtins_t *b, uint32_t i, const char **name, uint32_t *number, bool *numbered)
{
	const qc_bentry_t	*e = NULL;
	uint32_t			index, bound;

	for (index = 0 ; index < b->count ; index++)
		if ((e = QC_LiveEntry (b, index)) && !i--)
			break;
	if (index == b->count)
		return false;
	*name = e->name;
	// bound by number only if the number is still the entry's
	*numbered = e->numbered && QC_NumberBound (b, e->number, &bound) && bound == index;
	*number = *numbered ? e->number : 0;
	return true;
}

bool QC_BuiltinsContains (const qc_builtins_t *b, const char *name)
{
	return QC_MapGet (&b->by_name, name, strlen (name), NULL);
}

qc_builtin_t QC_BuiltinsFind (const qc_builtins_t *b, const char *name)
{
	uint32_t	index;

	if (!QC_MapGet (&b->by_name, name, strlen (name), &index))
		return NULL;
	return b->entries[index]->func;
}

bool QC_BindNumber (const qc_builtins_t *b, uint32_t number, uint32_t *slot)
{
	return b && QC_NumberBound (b, number, slot);
}

bool QC_BindName (const qc_builtins_t *b, const char *name, uint32_t *slot)
{
	return b && QC_MapGet (&b->by_name, name, strlen (name), slot);
}

qc_builtin_t QC_BuiltinSlot (const qc_builtins_t *b, uint32_t slot)
{
	return b && slot < b->count ? b->entries[slot]->func : NULL;
}

/*
==============================================================================

FTE'S NUMBERS

==============================================================================
*/

static void QC_NumberTable (qc_numbering_t numbering, const qc_builtinnumber_t **table, uint32_t *count,
	const char *const **named, uint32_t *namedcount)
{
	switch (numbering)
	{
	case QC_NUMBERING_CSQC:
		*table = qc_numbers_csqc; *count = qc_numbers_csqc_count;
		*named = qc_named_csqc; *namedcount = qc_named_csqc_count;
		break;
	case QC_NUMBERING_SSQC:
		*table = qc_numbers_ssqc; *count = qc_numbers_ssqc_count;
		*named = qc_named_ssqc; *namedcount = qc_named_ssqc_count;
		break;
	case QC_NUMBERING_MENU:
		*table = qc_numbers_menu; *count = qc_numbers_menu_count;
		*named = qc_named_menu; *namedcount = qc_named_menu_count;
		break;
	default:
		*table = NULL; *count = 0;
		*named = NULL; *namedcount = 0;
		break;
	}
}

bool QC_BuiltinNumber (qc_numbering_t numbering, const char *name, uint32_t *number)
{
	const qc_builtinnumber_t	*table;
	const char *const			*named;
	uint32_t					count, namedcount, lo, hi, mid;
	int							c;

	QC_NumberTable (numbering, &table, &count, &named, &namedcount);
	lo = 0;
	hi = count;
	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		c = strcmp (table[mid].name, name);
		if (!c)
		{
			*number = table[mid].number;
			return true;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	// builtins in FTE's tables that the platform dump doesn't declare for CSQC
	if (numbering == QC_NUMBERING_CSQC && !strcmp (name, "fork"))
	{
		*number = 210;
		return true;
	}
	if (numbering == QC_NUMBERING_CSQC && !strcmp (name, "sleep"))
	{
		*number = 212;
		return true;
	}
	return false;
}

uint32_t QC_NumKnownBuiltins (qc_numbering_t numbering)
{
	const qc_builtinnumber_t	*table;
	const char *const			*named;
	uint32_t					count, namedcount;

	QC_NumberTable (numbering, &table, &count, &named, &namedcount);
	return count + namedcount;
}

bool QC_KnownBuiltin (qc_numbering_t numbering, uint32_t i, const char **name, uint32_t *number, bool *numbered)
{
	const qc_builtinnumber_t	*table;
	const char *const			*named;
	uint32_t					count, namedcount;

	QC_NumberTable (numbering, &table, &count, &named, &namedcount);
	if (i < count)
	{
		*name = table[i].name;
		*number = table[i].number;
		*numbered = true;
		return true;
	}
	if (i - count < namedcount)
	{
		*name = named[i - count];
		*number = 0;
		*numbered = false;
		return true;
	}
	return false;
}
