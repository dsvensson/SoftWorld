// sys_win.c -- Windows system services shared by the windowed and console programs

#include "print.h"
#include "sys.h"
#include "win_local.h"

#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int Sys_FileTime (char *path)
{
	return GetFileAttributesA (path) == INVALID_FILE_ATTRIBUTES ? -1 : 1;
}

void Sys_mkdir (char *path)
{
	_mkdir (path);
}

bool Sys_ListDir (const char *path, void (*entry) (void *ctx, const char *name, bool isdir), void *ctx)
{
	WIN32_FIND_DATAA	data;
	HANDLE				find;
	char				pattern[MAX_PATH];

	snprintf (pattern, sizeof(pattern), "%s\\*", path);
	find = FindFirstFileA (pattern, &data);
	if (find == INVALID_HANDLE_VALUE)
		return false;
	do
	{
		if (!strcmp (data.cFileName, ".") || !strcmp (data.cFileName, ".."))
			continue;
		entry (ctx, data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
	} while (FindNextFileA (find, &data));
	FindClose (find);
	return true;
}

// the builds are x64's
const char *Sys_Platform (void)
{
	return "Win64";
}

// memory from the system, the CPU's name and clock from the registry's first core
void Sys_SystemInfo (sys_info_t *info)
{
	MEMORYSTATUSEX	memory = {.dwLength = sizeof(memory)};
	DWORD			mhz, size;

	memset (info, 0, sizeof(*info));
	if (GlobalMemoryStatusEx (&memory))
		info->memory = (unsigned)(memory.ullTotalPhys / (1024 * 1024));
	size = sizeof(info->cpu);
	if (RegGetValueA (HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
			"ProcessorNameString", RRF_RT_REG_SZ, NULL, info->cpu, &size) != ERROR_SUCCESS)
		info->cpu[0] = 0;
	size = sizeof(mhz);
	if (RegGetValueA (HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
			"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &size) == ERROR_SUCCESS)
		info->mhz = (int)mhz;
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

// how late the timer has woken lately, at worst: an exact wait sets it that
// much early and spins the rest. Windows wakes high-resolution timers on the
// system timer's ticks, a half millisecond late as a rule and a whole one at
// times; this rises at once to a wake later than it and sinks a thousandth a
// wait, settling just over what the timer does on this machine
static double	sys_timerlate = 0.0005;

/*
================
Sys_WaitUntil

Waits on a high-resolution waitable timer, window messages and the registered
handles; an exact wait sets the timer sys_timerlate early and spins the rest
================
*/
void Sys_WaitUntil (double time, bool exact)
{
	HANDLE			handles[MAX_WAIT_HANDLES + 1];
	LARGE_INTEGER	due;
	double			wait, early, late;
	int				i;

	wait = time - Sys_DoubleTime ();
	if (wait <= 0)
		return;

	// a timer for less than the timer's own lateness and a bit is all spun
	early = exact ? sys_timerlate : 0;
	if (wait > early + 0.0002)
	{
		if (!sys_timer)
		{
			sys_timer = CreateWaitableTimerExW (NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!sys_timer)		// older systems lack high resolution timers
				sys_timer = CreateWaitableTimerExW (NULL, NULL, 0, TIMER_ALL_ACCESS);
			if (!sys_timer)
				Sys_Error ("Couldn't create a waitable timer");
		}
		due.QuadPart = -(LONGLONG)((wait - early) * 1e7);	// relative, in 100 ns
		SetWaitableTimer (sys_timer, &due, 0, NULL, NULL, FALSE);

		handles[0] = sys_timer;
		for (i=0 ; i<sys_numwaithandles ; i++)
			handles[i+1] = sys_waithandles[i];
		if (MsgWaitForMultipleObjectsEx ((DWORD)sys_numwaithandles + 1, handles, INFINITE,
			QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_OBJECT_0)
			return;		// woken by input or a packet

		if (!exact)
			return;
		// a little over the latest wake, up to 2 ms (a busy machine's worst
		// isn't the timer's); never under a fifth of a millisecond
		late = Sys_DoubleTime () - (time - early);
		if (late > sys_timerlate)
			sys_timerlate = late + 0.00005 < 0.002 ? late + 0.00005 : 0.002;
		else if (sys_timerlate * 0.999 > 0.0002)
			sys_timerlate *= 0.999;
	}

	if (!exact)
		return;
	while (Sys_DoubleTime () < time)
		YieldProcessor ();
}
