// sys_mac.c -- macOS system services shared by the windowed and console programs

#include "print.h"
#include "sys.h"
#include "mac_local.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
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

/*
================
Sys_Seed

The monotonic clock, the process id and the time of day, mixed (splitmix64's
finalizer)
================
*/
unsigned Sys_Seed (void)
{
	uint64_t	x;

	x = clock_gettime_nsec_np (CLOCK_UPTIME_RAW) ^ ((uint64_t)getpid () << 32)
		^ clock_gettime_nsec_np (CLOCK_REALTIME);
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ull;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebull;
	x ^= x >> 31;
	return (unsigned)x;
}

/*
================
Sys_DoubleTime

Seconds since the first call, from the monotonic clock.
================
*/
double Sys_DoubleTime (void)
{
	static uint64_t	start;
	uint64_t		now = clock_gettime_nsec_np (CLOCK_UPTIME_RAW);

	if (!start)
		start = now;
	return (double)(now - start) * 1e-9;
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
===============================================================================

WAITING

===============================================================================
*/

static int	sys_kqueue = -1;

int Sys_WaitQueue (void)
{
	if (sys_kqueue < 0)
	{
		sys_kqueue = kqueue ();
		if (sys_kqueue < 0)
			Sys_Error ("Couldn't create a kqueue");
	}
	return sys_kqueue;
}

bool Sys_AddWaitFd (int fd)
{
	struct kevent	ev;

	EV_SET (&ev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	return kevent (Sys_WaitQueue (), &ev, 1, NULL, 0, NULL) == 0;
}

void Sys_RemoveWaitFd (int fd)
{
	struct kevent	ev;

	EV_SET (&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	kevent (Sys_WaitQueue (), &ev, 1, NULL, 0, NULL);
}

#define SYS_TIMER_IDENT	1

void Sys_SetWaitTimer (double until)
{
	struct kevent	ev;
	double			wait = until - Sys_DoubleTime ();

	EV_SET (&ev, SYS_TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL,
		wait > 0 ? (int64_t)(wait * 1e9) : 0, NULL);
	if (kevent (Sys_WaitQueue (), &ev, 1, NULL, 0, NULL) < 0)
		Sys_Error ("Sys_SetWaitTimer: couldn't set the timer");
}

// a timer that didn't fire; one that did is gone already
void Sys_ClearWaitTimer (void)
{
	struct kevent	ev;

	EV_SET (&ev, SYS_TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	kevent (Sys_WaitQueue (), &ev, 1, NULL, 0, NULL);
}

int Sys_ReadWaitQueue (bool block)
{
	struct kevent	events[16];
	struct timespec	none = {0};
	int				n, i, what = 0;

	n = kevent (Sys_WaitQueue (), NULL, 0, events, 16, block ? NULL : &none);
	if (n < 0)
		return errno == EINTR ? SYS_WAIT_SIGNAL : 0;
	for (i = 0 ; i < n ; i++)
		what |= events[i].filter == EVFILT_TIMER ? SYS_WAIT_TIMER : SYS_WAIT_FD;
	return what;
}

/*
================
Sys_WaitUntil

Waits on the program's events (Sys_WaitEvents: the registered fds, and the
window's). The last quarter millisecond is spun: the critical timer wakes
about 60 microseconds late, at worst 130 on an idle machine.
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
		__builtin_arm_yield ();
}
