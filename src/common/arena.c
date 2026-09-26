// arena.c -- growable bump allocators

#include "sys.h"
#include "arena.h"
#include "mem.h"
#include "print.h"

#include <stdint.h>
#include <string.h>

#define ARENA_FIRST_CHUNK	(64 * 1024)
#define ARENA_MAX_CHUNK		(16 * 1024 * 1024)

struct arena_chunk_s
{
	arena_chunk_t	*next;
	size_t			size;		// usable bytes after the header
	size_t			used;
};

// the chunk header is padded so the first block is ARENA_ALIGN aligned
#define CHUNK_HEADER	((sizeof (arena_chunk_t) + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1))

static arena_t	*arena_list;

static unsigned char *ChunkData (arena_chunk_t *chunk)
{
	return (unsigned char *)chunk + CHUNK_HEADER;
}

static arena_chunk_t *Arena_NewChunk (arena_t *arena, size_t size)
{
	arena_chunk_t	*chunk = Mem_AllocAligned (CHUNK_HEADER + size, ARENA_ALIGN);

	chunk->size = size;
	chunk->used = 0;
	chunk->next = arena->chunks;
	arena->chunks = chunk;
	arena->held += size;
	return chunk;
}

void Arena_Init (arena_t *arena, const char *name)
{
	memset (arena, 0, sizeof (*arena));
	arena->name = name;
	arena->next_size = ARENA_FIRST_CHUNK;
	arena->next_arena = arena_list;
	arena_list = arena;
}

void *Arena_Alloc (arena_t *arena, size_t size)
{
	arena_chunk_t	*chunk = arena->chunks;
	void			*p;

	size = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
	if (!size)
		size = ARENA_ALIGN;

	if (!chunk || chunk->size - chunk->used < size)
	{
		if (size > arena->next_size)
		{
			// oversized: give it a dedicated chunk, but keep filling the current one
			arena_chunk_t	*big = Mem_AllocAligned (CHUNK_HEADER + size, ARENA_ALIGN);

			big->size = big->used = size;
			arena->held += size;
			arena->used += size;
			if (chunk)
			{
				big->next = chunk->next;
				chunk->next = big;
			}
			else
			{
				big->next = NULL;
				arena->chunks = big;
			}
			return ChunkData (big);
		}

		chunk = Arena_NewChunk (arena, arena->next_size);
		if (arena->next_size < ARENA_MAX_CHUNK)
			arena->next_size *= 2;
	}

	p = ChunkData (chunk) + chunk->used;
	chunk->used += size;
	arena->used += size;
	return p;
}

char *Arena_StrDup (arena_t *arena, const char *s)
{
	size_t	len = strlen (s) + 1;
	char	*copy = Arena_Alloc (arena, len);

	memcpy (copy, s, len);
	return copy;
}

void Arena_Reset (arena_t *arena)
{
	arena_chunk_t	*chunk, *next, *keep = NULL;

	for (chunk = arena->chunks ; chunk ; chunk = chunk->next)
		if (!keep || chunk->size > keep->size)
			keep = chunk;

	for (chunk = arena->chunks ; chunk ; chunk = next)
	{
		next = chunk->next;
		if (chunk != keep)
			Mem_FreeAligned (chunk);
	}

	arena->chunks = keep;
	arena->used = 0;
	arena->held = 0;
	if (keep)
	{
		keep->next = NULL;
		keep->used = 0;
		memset (ChunkData (keep), 0, keep->size);
		arena->held = keep->size;
	}
}

void Arena_Free (arena_t *arena)
{
	arena_t			**link;
	arena_chunk_t	*chunk, *next;

	for (chunk = arena->chunks ; chunk ; chunk = next)
	{
		next = chunk->next;
		Mem_FreeAligned (chunk);
	}

	for (link = &arena_list ; *link ; link = &(*link)->next_arena)
	{
		if (*link == arena)
		{
			*link = arena->next_arena;
			break;
		}
	}

	memset (arena, 0, sizeof (*arena));
}

void Arena_PrintStats (void)
{
	arena_t	*arena;
	size_t	used = 0, held = 0;

	for (arena = arena_list ; arena ; arena = arena->next_arena)
	{
		Con_Printf ("%-20s %9zu KB used %9zu KB held\n", arena->name, arena->used / 1024, arena->held / 1024);
		used += arena->used;
		held += arena->held;
	}
	Con_Printf ("%-20s %9zu KB used %9zu KB held\n", "total", used / 1024, held / 1024);
}
