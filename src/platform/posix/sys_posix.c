// sys_posix.c -- the system services macOS and Linux share, for the windowed and
// console programs: files and the wait for a time (memory: sys_memory_posix.c)

#include "print.h"
#include "sys.h"
#include "posix_local.h"
#include "cpu_relax.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
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

bool Sys_ListDir (const char *path, void (*entry) (void *ctx, const char *name, bool isdir), void *ctx)
{
	DIR				*dir = opendir (path);
	struct dirent	*d;
	struct stat		st;
	char			full[1024];
	bool			isdir;

	if (!dir)
		return false;
	while ((d = readdir (dir)))
	{
		if (!strcmp (d->d_name, ".") || !strcmp (d->d_name, ".."))
			continue;
		// what the directory says, else what the entry is (links followed)
		if (d->d_type != DT_UNKNOWN && d->d_type != DT_LNK)
			isdir = d->d_type == DT_DIR;
		else
		{
			snprintf (full, sizeof(full), "%s/%s", path, d->d_name);
			isdir = !stat (full, &st) && S_ISDIR (st.st_mode);
		}
		entry (ctx, d->d_name, isdir);
	}
	closedir (dir);
	return true;
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

/*
================
Sys_WaitUntil

Waits on the program's events (Sys_WaitEvents: the registered fds, and the
window's). An exact wait spins the last quarter millisecond: the timer wakes
tens of microseconds late (macOS's critical timer about 60, at worst 130 on an
idle machine).
================
*/
void Sys_WaitUntil (double time, bool exact)
{
	double	wait;

	wait = time - Sys_DoubleTime ();
	if (wait <= 0)
		return;

	if (!exact)
	{
		Sys_WaitEvents (time);
		return;
	}

	if (wait > 0.0005 && Sys_WaitEvents (time - 0.00025))
		return;		// woken by input or a packet

	while (Sys_DoubleTime () < time)
		Sys_CpuRelax ();
}
