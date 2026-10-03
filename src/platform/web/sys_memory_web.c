// sys_memory_web.c -- address space reserved up front and committed as it
// grows, in a page's WebAssembly memory
//
// WebAssembly reserves nothing: its memory is one block that only grows, and
// Emscripten's mmap allocates and clears the whole size at once. A reservation
// is a block of the heap instead, cleared as it is committed. The heap's new
// memory comes from the browser zeroed and is given pages by the system only
// where it is touched, so a reservation costs address space (2 GB of it) and
// the RAM of what is committed. A block of freed memory isn't zeroed, which is
// why commits clear what they add.

#include "sys.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
	size_t	size;			// reserved
	size_t	committed;		// cleared so far
	char	pad[64 - 2 * sizeof(size_t)];	// the base 64-byte aligned
} reservation_t;

static reservation_t *Sys_Reservation (void *base)
{
	return (reservation_t *)base - 1;
}

void *Sys_TryReserveMemory (size_t size)
{
	reservation_t	*r;

	if (size > SIZE_MAX - sizeof(*r) - 64)
		return NULL;
	r = aligned_alloc (64, (sizeof(*r) + size + 63) & ~(size_t)63);
	if (!r)
		return NULL;
	r->size = size;
	r->committed = 0;
	return r + 1;
}

// the bytes already committed keep what they hold; new ones come zeroed
bool Sys_TryCommitMemory (void *base, size_t size)
{
	reservation_t	*r = Sys_Reservation (base);

	if (size > r->size)
		return false;
	if (size > r->committed)
	{
		memset ((char *)base + r->committed, 0, size - r->committed);
		r->committed = size;
	}
	return true;
}

void Sys_ReleaseMemory (void *base, size_t size)
{
	(void)size;
	if (base)
		free (Sys_Reservation (base));
}

void *Sys_ReserveMemory (size_t size)
{
	void	*base = Sys_TryReserveMemory (size);

	if (!base)
		Sys_Error ("Sys_ReserveMemory: couldn't reserve %zu bytes", size);
	return base;
}

void Sys_CommitMemory (void *base, size_t size)
{
	if (!Sys_TryCommitMemory (base, size))
		Sys_Error ("Sys_CommitMemory: out of memory (%zu bytes)", size);
}
