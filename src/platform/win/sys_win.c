// sys_win.c -- Windows system services shared by the windowed and console programs

#include "print.h"
#include "sys.h"

#include <windows.h>
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

void Sys_DebugLog (char *file, char *fmt, ...)
{
	va_list	argptr;
	FILE	*f;

	f = fopen (file, "a");
	if (!f)
		return;
	va_start (argptr, fmt);
	vfprintf (f, fmt, argptr);
	va_end (argptr);
	fclose (f);
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
