// sys_workers_linux.c -- the worker threads' waits on Linux (sys_workers_posix.c),
// and the cores they run on

#include "sys.h"
#include "posix_local.h"

#include <limits.h>
#include <linux/futex.h>
#include <sched.h>
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
Sys_FirstOfCore

Whether CPU cpu is the first thread of its core; true where sysfs can't tell
================
*/
static bool Sys_FirstOfCore (int cpu)
{
	static bool	siblings[MAX_CPUS];
	char		path[128];
	int			first;

	snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_cpus_list", cpu);
	if (!Sys_ReadCpuList (path, siblings))
	{
		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
		if (!Sys_ReadCpuList (path, siblings))
			return true;
	}
	for (first = 0 ; first < MAX_CPUS && !siblings[first] ; first++)
		;
	return first == cpu || first == MAX_CPUS;
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
	static bool	cpus[MAX_CPUS];
	int			cores = 0, i;

	if (!Sys_ReadCpuList ("/sys/devices/cpu_core/cpus", cpus)
		&& !Sys_ReadCpuList ("/sys/devices/system/cpu/online", cpus))
	{
		long	n = sysconf (_SC_NPROCESSORS_ONLN);

		return n > 0 ? (int)n : 1;
	}

	for (i = 0 ; i < MAX_CPUS ; i++)
		if (cpus[i] && Sys_FirstOfCore (i))
			cores++;
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

/*
================
Sys_WorkerThreadAttr

The frame's threads, threads of them with the calling one, on the cores that
share its last level cache, a thread each, where there are as many there:
on a CPU of more than one such cache (a Ryzen of two dies) the scheduler
spreads them over all of them otherwise, and a frame drawn on two caches
took a third longer than on one, with siblings of a core taking a twentieth
more. Only the performance cores where there are others; never outside the
CPUs the program was given. Where there's no room, the calling thread goes
back to those.
================
*/
static cpu_set_t	sys_given;			// the calling thread's CPUs, before any placing
static bool			sys_havegiven;

void Sys_WorkerThreadAttr (pthread_attr_t *attr, int threads)
{
	static bool	shared[MAX_CPUS], performance[MAX_CPUS];
	char		path[128];
	cpu_set_t	set;
	bool		hybrid;
	int			cpu, i, count;

	if (!sys_havegiven)
	{
		if (sched_getaffinity (0, sizeof(sys_given), &sys_given))
			return;
		sys_havegiven = true;
	}

	CPU_ZERO (&set);
	count = 0;
	cpu = sched_getcpu ();
	snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index3/shared_cpu_list", cpu);
	if (threads > 1 && cpu >= 0 && Sys_ReadCpuList (path, shared))
	{
		hybrid = Sys_ReadCpuList ("/sys/devices/cpu_core/cpus", performance);
		for (i = 0 ; i < MAX_CPUS && i < CPU_SETSIZE ; i++)
			if (shared[i] && CPU_ISSET (i, &sys_given) && (!hybrid || performance[i]) && Sys_FirstOfCore (i))
			{
				CPU_SET (i, &set);
				count++;
			}
	}

	if (count < threads)
	{
		pthread_setaffinity_np (pthread_self (), sizeof(sys_given), &sys_given);
		return;
	}
	pthread_setaffinity_np (pthread_self (), sizeof(set), &set);
	pthread_attr_setaffinity_np (attr, sizeof(set), &set);
}

void Sys_NameThread (const char *name)
{
	pthread_setname_np (pthread_self (), name);
}
