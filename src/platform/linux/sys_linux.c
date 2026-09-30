// sys_linux.c -- Linux's own system services: the clock, the seed, and the
// epoll the waits are on (posix_local.h)
//
// The clock is CLOCK_MONOTONIC, the one the wait's timerfd counts in and the
// compositor stamps presented frames with. The timer is as precise as the
// kernel's high-resolution timers: the process asks for no timer slack (50
// microseconds by default, by which a wakeup may be put off to fire with
// others).

#include "sys.h"
#include "posix_local.h"
#include "linux_local.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static uint64_t Sys_MonotonicNs (void)
{
	struct timespec	ts;

	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
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
	struct timespec	now;
	uint64_t		x;

	clock_gettime (CLOCK_REALTIME, &now);
	x = Sys_MonotonicNs () ^ ((uint64_t)getpid () << 32)
		^ ((uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec);
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
static uint64_t	sys_start;

double Sys_DoubleTime (void)
{
	uint64_t	now = Sys_MonotonicNs ();

	if (!sys_start)
		sys_start = now;
	return (double)(now - sys_start) * 1e-9;
}

double Sys_MonotonicToTime (uint64_t ns)
{
	if (!sys_start)
		Sys_DoubleTime ();
	return ((double)ns - (double)sys_start) * 1e-9;
}

/*
===============================================================================

THE MACHINE

===============================================================================
*/

const char *Sys_Platform (void)
{
	return sizeof(void *) == 8 ? "Linux64" : "Linux32";
}

// memory from /proc/meminfo, the CPU's name and clock from /proc/cpuinfo (the
// first core's; the name is "model name" on x86, and missing on some ARM)
void Sys_SystemInfo (sys_info_t *info)
{
	char		line[512], *value, *end;
	unsigned long long	kb;
	FILE		*f;

	memset (info, 0, sizeof(*info));
	if ((f = fopen ("/proc/meminfo", "r")))
	{
		while (fgets (line, sizeof(line), f))
			if (sscanf (line, "MemTotal: %llu kB", &kb) == 1)
			{
				info->memory = (unsigned)(kb / 1024);
				break;
			}
		fclose (f);
	}
	if ((f = fopen ("/proc/cpuinfo", "r")))
	{
		while (fgets (line, sizeof(line), f) && (!info->cpu[0] || !info->mhz))
		{
			value = strchr (line, ':');
			if (!value)
				continue;
			for (value++ ; *value == ' ' || *value == '\t' ; value++)
				;
			if ((end = strchr (value, '\n')))
				*end = 0;
			if (!strncmp (line, "model name", 10) && !info->cpu[0])
				snprintf (info->cpu, sizeof(info->cpu), "%s", value);
			else if (!strncmp (line, "cpu MHz", 7) && !info->mhz)
				info->mhz = (int)(atof (value) + 0.5);
		}
		fclose (f);
	}
}

/*
===============================================================================

WAITING

===============================================================================
*/

static int	sys_epoll = -1;
static int	sys_timer = -1;		// a timerfd, level-triggered in the epoll
static int	sys_windowfd = -1;	// level-triggered too (Sys_AddWindowFd)

int Sys_WaitQueue (void)
{
	struct epoll_event	ev = {.events = EPOLLIN};

	if (sys_epoll >= 0)
		return sys_epoll;

	sys_epoll = epoll_create1 (EPOLL_CLOEXEC);
	if (sys_epoll < 0)
		Sys_Error ("Couldn't create an epoll");
	sys_timer = timerfd_create (CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (sys_timer < 0)
		Sys_Error ("Couldn't create a timerfd");
	ev.data.fd = sys_timer;
	if (epoll_ctl (sys_epoll, EPOLL_CTL_ADD, sys_timer, &ev) < 0)
		Sys_Error ("Couldn't wait on the timerfd");

	// the wait's timer fires when it is due, not up to 50 us later
	prctl (PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
	return sys_epoll;
}

bool Sys_AddWaitFd (int fd)
{
	struct epoll_event	ev = {.events = EPOLLIN | EPOLLET, .data.fd = fd};

	return epoll_ctl (Sys_WaitQueue (), EPOLL_CTL_ADD, fd, &ev) == 0;
}

void Sys_RemoveWaitFd (int fd)
{
	epoll_ctl (Sys_WaitQueue (), EPOLL_CTL_DEL, fd, NULL);
}

// the display's: readable for as long as there is something to read, as the
// Vulkan driver reads the same socket, and may leave our events queued
bool Sys_AddWindowFd (int fd)
{
	struct epoll_event	ev = {.events = EPOLLIN, .data.fd = fd};

	if (epoll_ctl (Sys_WaitQueue (), EPOLL_CTL_ADD, fd, &ev) < 0)
		return false;
	sys_windowfd = fd;
	return true;
}

void Sys_SetWaitTimer (double until)
{
	struct itimerspec	it = {0};
	uint64_t			ns;

	Sys_WaitQueue ();
	Sys_DoubleTime ();		// sys_start set
	// the time on the monotonic clock; 0 would disarm the timer, a time past fires it
	ns = until > 0 ? sys_start + (uint64_t)(until * 1e9) : sys_start;
	it.it_value.tv_sec = (time_t)(ns / 1000000000u);
	it.it_value.tv_nsec = (long)(ns % 1000000000u);
	if (timerfd_settime (sys_timer, TFD_TIMER_ABSTIME, &it, NULL) < 0)
		Sys_Error ("Sys_SetWaitTimer: couldn't set the timer");
}

// disarmed, and a firing not yet read forgotten
void Sys_ClearWaitTimer (void)
{
	struct itimerspec	it = {0};
	uint64_t			expirations;

	Sys_WaitQueue ();
	timerfd_settime (sys_timer, 0, &it, NULL);
	while (read (sys_timer, &expirations, sizeof(expirations)) > 0)
		;
}

int Sys_ReadWaitQueue (bool block)
{
	struct epoll_event	events[16];
	uint64_t			expirations;
	int					n, i, what = 0;

	n = epoll_wait (Sys_WaitQueue (), events, 16, block ? -1 : 0);
	if (n < 0)
		return errno == EINTR ? SYS_WAIT_SIGNAL : 0;
	for (i = 0 ; i < n ; i++)
	{
		if (events[i].data.fd == sys_timer)
		{
			while (read (sys_timer, &expirations, sizeof(expirations)) > 0)
				;
			what |= SYS_WAIT_TIMER;
		}
		else if (events[i].data.fd == sys_windowfd)
			what |= SYS_WAIT_WINDOW;
		else
			what |= SYS_WAIT_FD;
	}
	return what;
}
