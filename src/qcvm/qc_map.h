// qc_map.h -- hash maps from byte strings to 32-bit values
//
// Open addressing with linear probing; removal shifts the following entries
// back, so there are no tombstones. Keys are borrowed: whoever inserts one keeps
// its bytes alive and unchanged while it is in the map.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	const char	*key;		// NULL: an empty slot
	uint32_t	len;
	uint32_t	hash;
	uint32_t	value;
} qc_mapslot_t;

typedef struct
{
	qc_mapslot_t	*slots;
	uint32_t		mask;		// slots - 1 (the count of slots is a power of two), 0 when empty
	uint32_t		count;
} qc_map_t;

// FNV-1a
uint32_t	QC_Hash (const void *key, size_t len);

bool	QC_MapGet (const qc_map_t *map, const void *key, size_t len, uint32_t *value);

// inserts unless the key is there already (which keeps its value); false only
// when out of memory
bool	QC_MapAdd (qc_map_t *map, const void *key, size_t len, uint32_t value);

// inserts or replaces; false only when out of memory
bool	QC_MapSet (qc_map_t *map, const void *key, size_t len, uint32_t value);

// false if the key was not there
bool	QC_MapRemove (qc_map_t *map, const void *key, size_t len);

void	QC_MapClear (qc_map_t *map);		// frees the slots too
