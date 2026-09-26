// arena.h -- growable bump allocators for data that shares one lifetime.
//
// An arena hands out zeroed, 64-byte aligned blocks from chunks it gets from the
// heap. It never runs out: when a chunk is full a new one is added, doubling in
// size (64 KB up to 16 MB), and requests larger than a chunk get a chunk of their
// own. Everything is released at once with Arena_Reset or Arena_Free.
#pragma once

#include <stddef.h>

#define ARENA_ALIGN		64

typedef struct arena_chunk_s arena_chunk_t;

typedef struct arena_s
{
	const char		*name;			// for memstats
	arena_chunk_t	*chunks;		// the chunk being filled first
	size_t			next_size;		// size of the next regular chunk
	size_t			used;			// bytes handed out since the last reset
	size_t			held;			// bytes held in chunks
	struct arena_s	*next_arena;	// all live arenas, for memstats
} arena_t;

void	Arena_Init (arena_t *arena, const char *name);

// zeroed, ARENA_ALIGN aligned
void	*Arena_Alloc (arena_t *arena, size_t size);
char	*Arena_StrDup (arena_t *arena, const char *s);

// releases every block; the largest chunk is kept for reuse
void	Arena_Reset (arena_t *arena);

// releases every block and chunk; the arena must be re-initialized before reuse
void	Arena_Free (arena_t *arena);

// prints usage of every live arena through Con_Printf
void	Arena_PrintStats (void);
