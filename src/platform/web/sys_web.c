// sys_web.c -- the web's own system services under the shared POSIX ones
// (posix_local.h): the clock, the seed, the machine as far as a page sees it,
// and the worker threads' waits (pthreads in Web Workers, over shared memory)
//
// Nothing here waits for a time: the page's own loop does (sys_web.js), so
// Sys_WaitEvents never sleeps, and the workers wait on futexes no main thread
// ever waits on (the browser's main thread may only spin).

#include "sys.h"
#include "posix_local.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/*
================
Sys_DoubleTime

Seconds since the first call, from the page's monotonic clock
(performance.now, the same on every thread)
================
*/
static double	sys_start = -1;

double Sys_DoubleTime (void)
{
	double	now = emscripten_get_now ();

	if (sys_start < 0)
		sys_start = now;
	return (now - sys_start) * 0.001;
}

/*
================
Sys_Seed

The browser's random numbers and the time, mixed (splitmix64's finalizer)
================
*/
unsigned Sys_Seed (void)
{
	uint64_t	x = 0;

	getentropy (&x, sizeof(x));
	x ^= (uint64_t)(emscripten_get_now () * 1000.0);
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ull;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebull;
	x ^= x >> 31;
	return (unsigned)x;
}

/*
================
Sys_WaitEvents

The page waits between frames, never a call: nothing to wait for here
================
*/
bool Sys_WaitEvents (double until)
{
	(void)until;
	return false;
}

/*
===============================================================================

THE MACHINE

===============================================================================
*/

const char *Sys_Platform (void)
{
	return "Web";
}

// the memory the browser tells (navigator.deviceMemory: GB, rounded and capped;
// 0 where it tells none), and no CPU's name or clock
EM_JS (int, Sys_WebDeviceMemory, (void), {
	return navigator.deviceMemory ? Math.round (navigator.deviceMemory * 1024) : 0;
});

void Sys_SystemInfo (sys_info_t *info)
{
	memset (info, 0, sizeof(*info));
	info->memory = (unsigned)Sys_WebDeviceMemory ();
}

/*
===============================================================================

WORKER THREADS

===============================================================================
*/

/*
================
Sys_NumCores

The browser tells the threads the CPU runs at once, not its cores: half of
them, as most desktop CPUs run two a core
================
*/
int Sys_NumCores (void)
{
	int		threads = emscripten_num_logical_cores ();

	return threads > 1 ? threads / 2 : 1;
}

// only the workers wait: memory.atomic.wait32, which the page's thread may not
void Sys_WorkerWait (_Atomic uint32_t *address, uint32_t value)
{
	emscripten_futex_wait ((volatile void *)address, value, INFINITY);
}

void Sys_WorkerWakeAll (_Atomic uint32_t *address)
{
	emscripten_futex_wake ((volatile void *)address, INT_MAX);
}

// a Web Worker has no class of service to ask for
void Sys_WorkerThreadAttr (pthread_attr_t *attr)
{
	(void)attr;
}
