// mem.c -- general-purpose heap allocation

#include "mem.h"
#include "sys.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *Mem_Alloc (size_t size)
{
	void	*p = malloc (size ? size : 1);

	if (!p)
		Sys_Error ("Mem_Alloc: out of memory (%zu bytes)", size);
	return p;
}

void *Mem_Calloc (size_t count, size_t size)
{
	void	*p;

	if (size && count > SIZE_MAX / size)
		Sys_Error ("Mem_Calloc: %zu * %zu bytes overflows", count, size);
	p = calloc (count ? count : 1, size ? size : 1);
	if (!p)
		Sys_Error ("Mem_Calloc: out of memory (%zu * %zu bytes)", count, size);
	return p;
}

void *Mem_Realloc (void *ptr, size_t size)
{
	void	*p = realloc (ptr, size ? size : 1);

	if (!p)
		Sys_Error ("Mem_Realloc: out of memory (%zu bytes)", size);
	return p;
}

void Mem_Free (void *ptr)
{
	free (ptr);
}

// The block returned by Mem_AllocAligned is preceded by the pointer malloc returned.
void *Mem_AllocAligned (size_t size, size_t alignment)
{
	uintptr_t	raw, aligned;

	if (alignment < sizeof (void *) || (alignment & (alignment - 1)))
		Sys_Error ("Mem_AllocAligned: bad alignment %zu", alignment);
	if (size > SIZE_MAX - alignment - sizeof (void *))
		Sys_Error ("Mem_AllocAligned: %zu bytes overflows", size);

	raw = (uintptr_t)Mem_Calloc (1, size + alignment + sizeof (void *));
	aligned = (raw + sizeof (void *) + alignment - 1) & ~(uintptr_t)(alignment - 1);
	((void **)aligned)[-1] = (void *)raw;
	return (void *)aligned;
}

void Mem_FreeAligned (void *ptr)
{
	if (ptr)
		free (((void **)ptr)[-1]);
}
