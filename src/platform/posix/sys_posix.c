// sys_posix.c -- the system services macOS and Linux share, for the windowed and
// console programs: files, memory, and the wait for a time

#include "print.h"
#include "sys.h"
#include "posix_local.h"
#include "cpu_relax.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int Sys_FileTime (char *path)
{
	struct stat	st;

	return stat (path, &st) ? -1 : 1;
}

void Sys_mkdir (char *path)
{
	mkdir (path, 0777);
}

// the file stays open, flushed after each write: opened for every print, a
// map whose entities print thousands of lines took seconds to load
void Sys_DebugLog (char *file, char *fmt, ...)
{
	static FILE	*f;
	static char	name[1024];		// the file f is open on
	va_list	argptr;

	if (!f || strcmp (name, file))
	{
		char	dir[1024], *slash;

		if (f)
			fclose (f);
		f = fopen (file, "a");
		if (!f)
		{	// a game directory the recording or server named, not made yet
			snprintf (dir, sizeof(dir), "%s", file);
			slash = strrchr (dir, '/');
			if (slash)
			{
				*slash = 0;
				Sys_mkdir (dir);
				f = fopen (file, "a");
			}
		}
		if (!f)
			return;
		snprintf (name, sizeof(name), "%s", file);
	}
	va_start (argptr, fmt);
	vfprintf (f, fmt, argptr);
	va_end (argptr);
	fflush (f);
}

void *Sys_ReserveMemory (size_t size)
{
	void	*base = mmap (NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

	if (base == MAP_FAILED)
		Sys_Error ("Sys_ReserveMemory: couldn't reserve %zu bytes", size);
	return base;
}

// the pages already committed keep what they hold; new pages come zeroed
void Sys_CommitMemory (void *base, size_t size)
{
	size_t	page = (size_t)getpagesize ();

	size = (size + page - 1) & ~(page - 1);
	if (mprotect (base, size, PROT_READ | PROT_WRITE))
		Sys_Error ("Sys_CommitMemory: out of memory (%zu bytes)", size);
}

/*
================
Sys_WaitUntil

Waits on the program's events (Sys_WaitEvents: the registered fds, and the
window's). The last quarter millisecond is spun: the timer wakes tens of
microseconds late (macOS's critical timer about 60, at worst 130 on an idle
machine).
================
*/
void Sys_WaitUntil (double time)
{
	double	wait;

	wait = time - Sys_DoubleTime ();
	if (wait <= 0)
		return;

	if (wait > 0.0005 && Sys_WaitEvents (time - 0.00025))
		return;		// woken by input or a packet

	while (Sys_DoubleTime () < time)
		Sys_CpuRelax ();
}
