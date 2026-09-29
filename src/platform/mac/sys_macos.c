// sys_macos.c -- macOS's own system services: the clock, the seed, and the
// kqueue the waits are on (posix_local.h)

#include "sys.h"
#include "posix_local.h"

#include <errno.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

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
