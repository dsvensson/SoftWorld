// sys_memory_posix.c -- address space reserved up front and committed as it
// grows, on macOS and Linux

#include "sys.h"

#include <sys/mman.h>
#include <unistd.h>

void *Sys_TryReserveMemory (size_t size)
{
	void	*base = mmap (NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

	return base == MAP_FAILED ? NULL : base;
}

// the pages already committed keep what they hold; new pages come zeroed
bool Sys_TryCommitMemory (void *base, size_t size)
{
	size_t	page = (size_t)getpagesize ();

	size = (size + page - 1) & ~(page - 1);
	return !mprotect (base, size, PROT_READ | PROT_WRITE);
}

void Sys_ReleaseMemory (void *base, size_t size)
{
	if (base)
		munmap (base, size);
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
