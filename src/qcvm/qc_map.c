// qc_map.c -- hash maps from byte strings to 32-bit values

#include "qc_map.h"

#include <stdlib.h>
#include <string.h>

uint32_t QC_Hash (const void *key, size_t len)
{
	const unsigned char	*p = key;
	uint32_t			h = 0x811C9DC5u;
	size_t				i;

	for (i = 0 ; i < len ; i++)
		h = (h ^ p[i]) * 0x01000193u;
	return h;
}

// the slot holding the key, or the empty slot where it would go
static qc_mapslot_t *QC_MapSlot (const qc_map_t *map, const void *key, size_t len, uint32_t hash)
{
	uint32_t		i = hash & map->mask;
	qc_mapslot_t	*s;

	for ( ; ; i = (i + 1) & map->mask)
	{
		s = &map->slots[i];
		if (!s->key)
			return s;
		if (s->hash == hash && s->len == len && !memcmp (s->key, key, len))
			return s;
	}
}

bool QC_MapGet (const qc_map_t *map, const void *key, size_t len, uint32_t *value)
{
	const qc_mapslot_t	*s;

	if (!map->count || len > UINT32_MAX)
		return false;
	s = QC_MapSlot (map, key, len, QC_Hash (key, len));
	if (!s->key)
		return false;
	if (value)
		*value = s->value;
	return true;
}

// keeps the load at most 1/2
static bool QC_MapGrow (qc_map_t *map)
{
	qc_mapslot_t	*old = map->slots, *s;
	uint32_t		oldsize = old ? map->mask + 1 : 0;
	uint32_t		size = oldsize ? oldsize * 2 : 16;
	uint32_t		i;

	if (oldsize > UINT32_MAX / 4)
		return false;
	map->slots = calloc (size, sizeof(*map->slots));
	if (!map->slots)
	{
		map->slots = old;
		return false;
	}
	map->mask = size - 1;
	for (i = 0 ; i < oldsize ; i++)
	{
		if (!old[i].key)
			continue;
		s = QC_MapSlot (map, old[i].key, old[i].len, old[i].hash);
		*s = old[i];
	}
	free (old);
	return true;
}

static bool QC_MapPut (qc_map_t *map, const void *key, size_t len, uint32_t value, bool replace)
{
	qc_mapslot_t	*s;
	uint32_t		hash;

	if (len > UINT32_MAX)
		return false;
	if ((map->count + 1) * 2 > (map->slots ? map->mask + 1 : 0) && !QC_MapGrow (map))
		return false;
	hash = QC_Hash (key, len);
	s = QC_MapSlot (map, key, len, hash);
	if (s->key)
	{
		if (replace)
			s->value = value;
		return true;
	}
	s->key = key;
	s->len = (uint32_t)len;
	s->hash = hash;
	s->value = value;
	map->count++;
	return true;
}

bool QC_MapAdd (qc_map_t *map, const void *key, size_t len, uint32_t value)
{
	return QC_MapPut (map, key, len, value, false);
}

bool QC_MapSet (qc_map_t *map, const void *key, size_t len, uint32_t value)
{
	return QC_MapPut (map, key, len, value, true);
}

bool QC_MapRemove (qc_map_t *map, const void *key, size_t len)
{
	qc_mapslot_t	*s;
	uint32_t		i, j, home;

	if (!map->count || len > UINT32_MAX)
		return false;
	s = QC_MapSlot (map, key, len, QC_Hash (key, len));
	if (!s->key)
		return false;

	// shift back the entries of the run after it that would no longer be found
	i = (uint32_t)(s - map->slots);
	for (j = (i + 1) & map->mask ; map->slots[j].key ; j = (j + 1) & map->mask)
	{
		home = map->slots[j].hash & map->mask;
		// j's entry may move to i if its home is not in (i, j], cyclically
		if (((j - home) & map->mask) >= ((j - i) & map->mask))
		{
			map->slots[i] = map->slots[j];
			i = j;
		}
	}
	map->slots[i].key = NULL;
	map->count--;
	return true;
}

void QC_MapClear (qc_map_t *map)
{
	free (map->slots);
	map->slots = NULL;
	map->mask = 0;
	map->count = 0;
}
