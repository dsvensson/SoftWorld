// sys_workers_linux.c -- the worker threads' waits on Linux (sys_workers_posix.c),
// and the cores they run on

#include "sys.h"
#include "posix_local.h"

#include <limits.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_CPUS	1024

/*
================
Sys_ReadCpuList

A list of CPUs as sysfs writes them ("0-3,8,10-11") into set; false if the
file can't be read
================
*/
static bool Sys_ReadCpuList (const char *path, bool set[MAX_CPUS])
{
	char	text[4096], *p, *end;
	long	first, last, i;
	FILE	*f = fopen (path, "r");

	if (!f)
		return false;
	if (!fgets (text, sizeof(text), f))
	{
		fclose (f);
		return false;
	}
	fclose (f);

	memset (set, 0, MAX_CPUS * sizeof(bool));
	for (p = text ; *p && *p != '\n' ; p = *end == ',' ? end + 1 : end)
	{
		first = last = strtol (p, &end, 10);
		if (end == p)
			break;
		if (*end == '-')
		{
			p = end + 1;
			last = strtol (p, &end, 10);
			if (end == p)
				break;
		}
		for (i = first ; i <= last && i < MAX_CPUS ; i++)
			if (i >= 0)
				set[i] = true;
	}
	return true;
}

/*
================
Sys_NumCores

The cores, not the threads each runs: a CPU counts as the first thread of
its core. On a CPU with performance and efficiency cores only the
performance ones (cpu_core), as on macOS: a job left to an efficiency core
would hold up the frame.
================
*/
int Sys_NumCores (void)
{
	static bool	cpus[MAX_CPUS], siblings[MAX_CPUS];
	char		path[128];
	int			cores = 0, i, first;

	if (!Sys_ReadCpuList ("/sys/devices/cpu_core/cpus", cpus)
		&& !Sys_ReadCpuList ("/sys/devices/system/cpu/online", cpus))
	{
		long	n = sysconf (_SC_NPROCESSORS_ONLN);

		return n > 0 ? (int)n : 1;
	}

	for (i = 0 ; i < MAX_CPUS ; i++)
	{
		if (!cpus[i])
			continue;
		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_cpus_list", i);
		if (!Sys_ReadCpuList (path, siblings))
		{
			snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", i);
			if (!Sys_ReadCpuList (path, siblings))
			{
				cores++;
				continue;
			}
		}
		for (first = 0 ; first < MAX_CPUS && !siblings[first] ; first++)
			;
		if (first == i || first == MAX_CPUS)
			cores++;
	}
	return cores ? cores : 1;
}

void Sys_WorkerWait (_Atomic uint32_t *address, uint32_t value)
{
	syscall (SYS_futex, (uint32_t *)address, FUTEX_WAIT_PRIVATE, value, NULL, NULL, 0);
}

void Sys_WorkerWakeAll (_Atomic uint32_t *address)
{
	syscall (SYS_futex, (uint32_t *)address, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}

// the scheduler's defaults: no class of service to ask for
void Sys_WorkerThreadAttr (pthread_attr_t *attr)
{
	(void)attr;
}
