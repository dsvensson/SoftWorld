/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// test_workers.c -- Sys_Parallel over the platform's worker threads: every job
// of a run runs once, sees what the caller wrote before the run, and has
// finished (its writes seen) when the run returns; with each number of workers.
// And threads of their own, joined and detached.

#include "sys.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define MAX_JOBS	256
#define ROUNDS		20000

typedef struct
{
	int		round;					// written by the caller before each run
	int		slots[MAX_JOBS][16];	// a cache line a job, written by it alone
} work_t;

static work_t	work;
static int		failures;

static void Job (void *ctx, int index)
{
	work_t	*w = ctx;

	for (int i = 0 ; i < 16 ; i++)
		w->slots[index][i] = w->round * 16 + i + index;
}

static void TestWorkers (int workers)
{
	uint32_t	rng = 0x9E3779B9u ^ (uint32_t)workers;
	int			count, round, index, i;

	Sys_SetWorkers (workers);
	for (round = 1 ; round <= ROUNDS ; round++)
	{
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		count = 1 + (int)(rng % MAX_JOBS);

		work.round = round;
		Sys_Parallel (count, Job, &work);
		for (index = 0 ; index < count ; index++)
			for (i = 0 ; i < 16 ; i++)
				if (work.slots[index][i] != round * 16 + i + index)
				{
					if (failures++ < 10)
						printf ("%d workers, round %d: job %d of %d wrote %d\n", workers, round, index,
							count, work.slots[index][i]);
					i = 16;
				}
	}
}

// a thread of its own: what it writes, seen after joining it; and one let run
// alone, seen by its flag
static volatile int	thread_out, thread_done;

static void ThreadFunc (void *arg)
{
	thread_out = *(int *)arg * 2;
	thread_done = 1;
}

static void TestThreads (void)
{
	systhread_t	*t;
	int			in = 21;
	time_t		until;

	t = Sys_StartThread ("test", ThreadFunc, &in);
	if (!t)
	{
		printf ("threads: none to start here\n");
		return;
	}
	Sys_JoinThread (t);
	if (thread_out != 42 && failures++ < 10)
		printf ("joined thread wrote %d\n", thread_out);

	thread_done = 0;
	in = 5;
	t = Sys_StartThread ("detached", ThreadFunc, &in);
	if (!t)
	{
		if (failures++ < 10)
			printf ("second thread didn't start\n");
		return;
	}
	Sys_DetachThread (t);
	for (until = time (NULL) + 5 ; !thread_done && time (NULL) < until ; )
		;
	if ((!thread_done || thread_out != 10) && failures++ < 10)
		printf ("detached thread: done %d, wrote %d\n", thread_done, thread_out);
}

int main (void)
{
	int		cores = Sys_NumCores ();

	TestWorkers (0);
	TestWorkers (1);
	TestWorkers (3);
	TestWorkers (cores > 1 ? cores - 1 : 1);
	TestWorkers (15);
	Sys_SetWorkers (0);
	TestThreads ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("workers: every job ran once and was seen, with %d cores\n", cores);
	return 0;
}
