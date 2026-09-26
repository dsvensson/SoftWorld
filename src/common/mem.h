// mem.h -- general-purpose heap allocation.
//
// Every allocation function either succeeds or terminates the program through
// Sys_Error, so callers never need to check for NULL.
#pragma once

#include <stddef.h>

// uninitialized memory
void	*Mem_Alloc (size_t size);

// zero-initialized memory for count elements; the multiplication is overflow-checked
void	*Mem_Calloc (size_t count, size_t size);

// ptr may be NULL; newly added bytes are uninitialized
void	*Mem_Realloc (void *ptr, size_t size);

// ptr may be NULL
void	Mem_Free (void *ptr);

char	*Mem_StrDup (const char *s);

// zero-initialized memory aligned to alignment (a power of two); release with Mem_FreeAligned
void	*Mem_AllocAligned (size_t size, size_t alignment);
void	Mem_FreeAligned (void *ptr);
