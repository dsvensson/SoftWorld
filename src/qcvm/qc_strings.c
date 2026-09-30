// qc_strings.c -- temp strings, collected when nothing refers to them, and static
// strings that live as long as the VM
//
// Temp strings live outside VM memory, in a slot table. A conservative
// mark-and-sweep treats every aligned word of VM memory whose top bits are 10 as
// a reference, and runs only when no QuakeC is executing, so a temp string lives
// exactly as long as something QuakeC can see refers to it.

#include "qc_local.h"

#include <stdlib.h>

#define INITIAL_SLOTS	1024u

bool QC_StringsInit (qc_strings_t *s, uint32_t max_slots, size_t max_bytes)
{
	*s = (qc_strings_t){0};
	s->max_slots = max_slots < QC_INDEX_MASK ? max_slots : QC_INDEX_MASK;
	s->capacity = INITIAL_SLOTS < (max_slots ? max_slots : 1) ? INITIAL_SLOTS : (max_slots ? max_slots : 1);
	s->max_bytes = max_bytes;
	return true;
}

void QC_StringsFree (qc_strings_t *s)
{
	uint32_t	i;

	for (i = 0 ; i < s->numslots ; i++)
		free (s->slots[i].data);
	free (s->slots);
	for (i = 0 ; i < s->numstatics ; i++)
		if (s->statics[i].owned)
			free ((char *)s->statics[i].text);
	free (s->statics);
	QC_MapClear (&s->static_text);
	free (s->pins);
	*s = (qc_strings_t){0};
}

bool QC_StringsFit (const qc_strings_t *s, size_t len)
{
	size_t	room = s->max_bytes > s->bytes ? s->max_bytes - s->bytes : 0;

	return len <= SIZE_MAX - 4 && ((len + 4) & ~(size_t)3) <= room;
}

// a free slot, the table grown as FTE grows it (2 × max + 1024) within the limit
static bool QC_FreeSlot (qc_strings_t *s, uint32_t *slot)
{
	uint64_t		grown;
	qc_tempslot_t	*slots;
	uint32_t		i, start;

	if (s->live >= s->capacity)
	{
		grown = (uint64_t)s->capacity * 2 + INITIAL_SLOTS;
		if (grown > s->max_slots)
			grown = s->max_slots;
		if (grown <= s->capacity)
			return false;
		s->capacity = (uint32_t)grown;
	}
	if (s->numslots < s->capacity)
	{
		slots = realloc (s->slots, (size_t)s->capacity * sizeof(*slots));
		if (!slots)
			return false;
		memset (slots + s->numslots, 0, (size_t)(s->capacity - s->numslots) * sizeof(*slots));
		s->slots = slots;
		s->numslots = s->capacity;
	}
	start = s->cursor % s->capacity;
	for (i = start ; i < s->capacity ; i++)
		if (!s->slots[i].data)
		{
			*slot = i;
			return true;
		}
	for (i = 0 ; i < start ; i++)
		if (!s->slots[i].data)
		{
			*slot = i;
			return true;
		}
	return false;
}

bool QC_TempAlloc (qc_strings_t *s, const void *text, size_t len, uint32_t *ref)
{
	uint32_t	slot;
	size_t		padded;
	uint8_t		*data;

	if (!QC_StringsFit (s, len))
		return false;
	padded = (len + 4) & ~(size_t)3;
	if (!QC_FreeSlot (s, &slot))
		return false;
	data = malloc (padded + 1);
	if (!data)
		return false;
	if (len)
		memcpy (data, text, len);
	memset (data + len, 0, padded + 1 - len);
	s->slots[slot] = (qc_tempslot_t){data, (uint32_t)padded};
	s->live++;
	s->bytes += padded;
	s->cursor = slot + 1;
	*ref = slot | QC_TEMP_TAG;
	return true;
}

uint8_t *QC_TempData (const qc_strings_t *s, uint32_t slot, uint32_t *size)
{
	if (slot >= s->numslots || !s->slots[slot].data)
		return NULL;
	if (size)
		*size = s->slots[slot].size;
	return s->slots[slot].data;
}

uint8_t *QC_TempGrow (qc_strings_t *s, uint32_t slot, size_t len)
{
	qc_tempslot_t	*t;
	size_t			grow, extra;
	uint8_t			*data;

	if (slot >= s->numslots || !s->slots[slot].data)
		return NULL;
	t = &s->slots[slot];
	if (t->size >= len)
		return t->data;
	if (len > QC_MAX_TEMP_GROWTH)
		return NULL;
	grow = (len + 3) & ~(size_t)3;
	extra = grow - t->size;
	if (s->bytes + extra > s->max_bytes)
		return NULL;
	data = realloc (t->data, grow + 1);
	if (!data)
		return NULL;
	memset (data + t->size, 0, grow + 1 - t->size);
	t->data = data;
	t->size = (uint32_t)grow;
	s->bytes += extra;
	return data;
}

static bool QC_AddStatic (qc_strings_t *s, const char *text, bool owned, uint32_t *index)
{
	qc_static_t	*grown;
	uint32_t	n;

	if (s->numstatics >= QC_INDEX_MASK)
		return false;
	if (s->numstatics == s->staticsize)
	{
		n = s->staticsize ? s->staticsize * 2 : 64;
		grown = realloc (s->statics, (size_t)n * sizeof(*grown));
		if (!grown)
			return false;
		s->statics = grown;
		s->staticsize = n;
	}
	*index = s->numstatics;
	s->statics[s->numstatics++] = (qc_static_t){text, owned};
	return true;
}

bool QC_StaticIntern (qc_strings_t *s, const char *text, size_t len, uint32_t *ref)
{
	uint32_t	index;
	char		*copy;

	if (QC_MapGet (&s->static_text, text, len, &index))
	{
		*ref = index | QC_STATIC_TAG;
		return true;
	}
	copy = malloc (len + 1);
	if (!copy)
		return false;
	memcpy (copy, text, len);
	copy[len] = 0;
	if (!QC_AddStatic (s, copy, true, &index))
	{
		free (copy);
		return false;
	}
	if (!QC_MapAdd (&s->static_text, copy, len, index))
	{
		// keep it, found by reference only
	}
	*ref = index | QC_STATIC_TAG;
	return true;
}

// the host makes few of these (names and the like), so they are found by a scan
bool QC_StaticBorrow (qc_strings_t *s, const char *text, uint32_t *ref)
{
	uint32_t	i;

	for (i = 0 ; i < s->numstatics ; i++)
		if (!s->statics[i].owned && s->statics[i].text == text)
		{
			*ref = i | QC_STATIC_TAG;
			return true;
		}
	if (!QC_AddStatic (s, text, false, &i))
		return false;
	*ref = i | QC_STATIC_TAG;
	return true;
}

const char *QC_StaticText (const qc_strings_t *s, uint32_t index)
{
	return index < s->numstatics ? s->statics[index].text : NULL;
}

void QC_StringsPin (qc_strings_t *s, uint32_t ref)
{
	uint32_t	slot = ref & QC_INDEX_MASK, i, n;
	qc_pin_t	*grown;

	if ((ref & QC_TAG_MASK) != QC_TEMP_TAG)
		return;
	for (i = 0 ; i < s->numpins ; i++)
		if (s->pins[i].slot == slot)
		{
			if (s->pins[i].count < UINT32_MAX)
				s->pins[i].count++;
			return;
		}
	if (s->numpins == s->pinsize)
	{
		n = s->pinsize ? s->pinsize * 2 : 16;
		grown = realloc (s->pins, (size_t)n * sizeof(*grown));
		if (!grown)
			return;
		s->pins = grown;
		s->pinsize = n;
	}
	s->pins[s->numpins++] = (qc_pin_t){slot, 1};
}

void QC_StringsUnpin (qc_strings_t *s, uint32_t ref)
{
	uint32_t	slot = ref & QC_INDEX_MASK, i;

	if ((ref & QC_TAG_MASK) != QC_TEMP_TAG)
		return;
	for (i = 0 ; i < s->numpins ; i++)
		if (s->pins[i].slot == slot)
		{
			if (--s->pins[i].count == 0)
				s->pins[i] = s->pins[--s->numpins];
			return;
		}
}

// FTE's rule: half the table live and the cursor past half
bool QC_WantsCollection (const qc_strings_t *s)
{
	uint32_t	half = s->capacity / 2;

	return s->live >= half && s->cursor >= half;
}

bool QC_GCBegin (qc_strings_t *s, uint8_t **marks)
{
	*marks = calloc (s->numslots ? s->numslots : 1, 1);
	return *marks != NULL;
}

void QC_GCMark (const qc_strings_t *s, uint8_t *marks, const uint8_t *root, size_t len)
{
	const uint8_t	*end = root + (len & ~(size_t)3);
	uint32_t		w;

	for ( ; root < end ; root += 4)
	{
		memcpy (&w, root, 4);
		if ((w & QC_TAG_MASK) == QC_TEMP_TAG && (w & QC_INDEX_MASK) < s->numslots)
			marks[w & QC_INDEX_MASK] = 1;
	}
}

uint32_t QC_GCSweep (qc_strings_t *s, uint8_t *marks)
{
	uint32_t	i, freed = 0;
	uint64_t	grown;

	for (i = 0 ; i < s->numpins ; i++)
		if (s->pins[i].slot < s->numslots)
			marks[s->pins[i].slot] = 1;
	for (i = 0 ; i < s->numslots ; i++)
	{
		if (marks[i] || !s->slots[i].data)
			continue;
		s->bytes -= s->slots[i].size;
		free (s->slots[i].data);
		s->slots[i] = (qc_tempslot_t){0};
		freed++;
	}
	free (marks);
	s->live -= freed;
	s->cursor = 0;
	if (s->live >= s->capacity / 2)
	{
		grown = (uint64_t)s->capacity * 2;
		if (grown > (s->max_slots ? s->max_slots : 1))
			grown = s->max_slots ? s->max_slots : 1;
		s->capacity = (uint32_t)grown;
	}
	return freed;
}
