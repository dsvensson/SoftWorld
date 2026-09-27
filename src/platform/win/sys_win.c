// sys_win.c -- Windows system services shared by the windowed and console programs

#include "print.h"
#include "sys.h"
#include "win_local.h"

#include <direct.h>
#include <stdarg.h>
#include <stdio.h>

int Sys_FileTime (char *path)
{
	return GetFileAttributesA (path) == INVALID_FILE_ATTRIBUTES ? -1 : 1;
}

void Sys_mkdir (char *path)
{
	_mkdir (path);
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
		if (f)
			fclose (f);
		f = fopen (file, "a");
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

The performance counter, the process id and the tick count, mixed
(splitmix64's finalizer)
================
*/
unsigned Sys_Seed (void)
{
	LARGE_INTEGER	counter;
	uint64_t		x;

	QueryPerformanceCounter (&counter);
	x = (uint64_t)counter.QuadPart ^ ((uint64_t)GetCurrentProcessId () << 32) ^ GetTickCount64 ();
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

Seconds since the first call, from the performance counter.
================
*/
double Sys_DoubleTime (void)
{
	static LARGE_INTEGER	frequency, start;
	LARGE_INTEGER			now;

	if (!frequency.QuadPart)
	{
		QueryPerformanceFrequency (&frequency);
		QueryPerformanceCounter (&start);
	}
	QueryPerformanceCounter (&now);

	return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

const char *Sys_ExecutableDir (void)
{
	static char	dir[MAX_PATH];
	char		*p;
	DWORD		len;

	if (dir[0])
		return dir;

	len = GetModuleFileNameA (NULL, dir, sizeof(dir));
	if (!len || len >= sizeof(dir))
	{
		dir[0] = '.';
		dir[1] = 0;
		return dir;
	}
	for (p = dir ; *p ; p++)
		if (*p == '\\')
			*p = '/';
	p = strrchr (dir, '/');
	if (p)
		*p = 0;
	return dir;
}

void *Sys_ReserveMemory (size_t size)
{
	void	*base = VirtualAlloc (NULL, size, MEM_RESERVE, PAGE_NOACCESS);

	if (!base)
		Sys_Error ("Sys_ReserveMemory: couldn't reserve %zu bytes", size);
	return base;
}

void Sys_CommitMemory (void *base, size_t size)
{
	if (!VirtualAlloc (base, size, MEM_COMMIT, PAGE_READWRITE))
		Sys_Error ("Sys_CommitMemory: out of memory (%zu bytes)", size);
}

void Sys_ReleaseMemory (void *base, [[maybe_unused]] size_t size)
{
	VirtualFree (base, 0, MEM_RELEASE);
}

/*
===============================================================================

WAITING

===============================================================================
*/

#define MAX_WAIT_HANDLES	8

static HANDLE	sys_timer;
static HANDLE	sys_waithandles[MAX_WAIT_HANDLES];
static int		sys_numwaithandles;

void Sys_AddWaitHandle (HANDLE handle)
{
	if (sys_numwaithandles == MAX_WAIT_HANDLES)
		Sys_Error ("Sys_AddWaitHandle: too many handles");
	sys_waithandles[sys_numwaithandles++] = handle;
}

void Sys_RemoveWaitHandle (HANDLE handle)
{
	int		i;

	for (i=0 ; i<sys_numwaithandles ; i++)
	{
		if (sys_waithandles[i] == handle)
		{
			sys_waithandles[i] = sys_waithandles[--sys_numwaithandles];
			return;
		}
	}
}

/*
================
Sys_WaitUntil

Waits on a high-resolution waitable timer, window messages and the registered
handles. The last fraction of a millisecond is spun, since the timer can
overshoot by about that much.
================
*/
void Sys_WaitUntil (double time)
{
	HANDLE			handles[MAX_WAIT_HANDLES + 1];
	LARGE_INTEGER	due;
	double			wait;
	int				i;

	wait = time - Sys_DoubleTime ();
	if (wait <= 0)
		return;

	if (wait > 0.0008)
	{
		if (!sys_timer)
		{
			sys_timer = CreateWaitableTimerExW (NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!sys_timer)		// older systems lack high resolution timers
				sys_timer = CreateWaitableTimerExW (NULL, NULL, 0, TIMER_ALL_ACCESS);
			if (!sys_timer)
				Sys_Error ("Couldn't create a waitable timer");
		}
		due.QuadPart = -(LONGLONG)((wait - 0.0005) * 1e7);	// relative, in 100 ns
		SetWaitableTimer (sys_timer, &due, 0, NULL, NULL, FALSE);

		handles[0] = sys_timer;
		for (i=0 ; i<sys_numwaithandles ; i++)
			handles[i+1] = sys_waithandles[i];
		if (MsgWaitForMultipleObjectsEx ((DWORD)sys_numwaithandles + 1, handles, INFINITE,
			QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_OBJECT_0)
			return;		// woken by input or a packet
	}

	while (Sys_DoubleTime () < time)
		YieldProcessor ();
}
