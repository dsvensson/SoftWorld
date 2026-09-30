// sys_memory_win.c -- address space reserved up front and committed as it
// grows, on Windows

#include "sys.h"

#include <windows.h>

void *Sys_TryReserveMemory (size_t size)
{
	return VirtualAlloc (NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

// the pages already committed keep what they hold; new pages come zeroed
bool Sys_TryCommitMemory (void *base, size_t size)
{
	return VirtualAlloc (base, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void Sys_ReleaseMemory (void *base, size_t size)
{
	(void)size;
	if (base)
		VirtualFree (base, 0, MEM_RELEASE);
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
