// sys_win_workers.c -- the worker threads of Sys_Parallel
//
// A run publishes its jobs and bumps a generation the workers wait on; the
// jobs are taken by an index each thread increments. The caller starts on the
// jobs at once and the workers join as they wake. A worker out of jobs sleeps
// (WaitOnAddress) at once: spinning for the next run cost more than waking,
// as spinning cores take clock speed and cycles from the thread drawing the
// rest of the frame.

#include "sys.h"
#include "win_local.h"

#include <stdlib.h>

#define	MAX_WORKERS		63

static struct
{
	void			(*job) (void *ctx, int index);
	void			*ctx;
	volatile LONG	count;			// the jobs of the run
	volatile LONG	next;			// the next job to take
	volatile LONG	done;			// jobs finished
	volatile LONG	running;		// a run is on; the workers join only then
	volatile LONG	active;			// workers inside a run
	volatile LONG	generation;		// bumped by each run and by stopping; the workers wait on it
	volatile LONG	sleepers;		// workers waiting in WaitOnAddress
	volatile LONG	quit;
	int				numworkers;
	HANDLE			threads[MAX_WORKERS];
} pool;

int Sys_NumCores (void)
{
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX	*info, *p;
	DWORD	size = 0;
	int		cores = 0;

	GetLogicalProcessorInformationEx (RelationProcessorCore, NULL, &size);
	info = size ? malloc (size) : NULL;
	if (info && GetLogicalProcessorInformationEx (RelationProcessorCore, info, &size))
		for (p = info ; (char *)p < (char *)info + size ; p = (void *)((char *)p + p->Size))
			cores++;
	free (info);
	return cores ? cores : 1;
}

// takes and runs the run's jobs until none are left
static void Sys_RunJobs (void)
{
	LONG	i;

	while ((i = InterlockedIncrement (&pool.next) - 1) < pool.count)
	{
		pool.job (pool.ctx, (int)i);
		InterlockedIncrement (&pool.done);
	}
}

static DWORD WINAPI Sys_WorkerMain (void *unused)
{
	LONG	seen = 0, now;

	(void)unused;
	for (;;)
	{
		// sleep until the next run
		InterlockedIncrement (&pool.sleepers);
		while ((now = pool.generation) == seen)
			WaitOnAddress (&pool.generation, &seen, sizeof(seen), INFINITE);
		InterlockedDecrement (&pool.sleepers);
		seen = now;
		if (pool.quit)
			return 0;

		// a run: joined only if it is still the one that woke us
		InterlockedIncrement (&pool.active);
		if (pool.running && pool.generation == now)
			Sys_RunJobs ();
		InterlockedDecrement (&pool.active);
	}
}

// bumps the generation and wakes the sleeping workers
static void Sys_WakeWorkers (void)
{
	InterlockedIncrement (&pool.generation);
	if (pool.sleepers)
		WakeByAddressAll ((void *)&pool.generation);
}

void Sys_SetWorkers (int workers)
{
	int		i;

	if (workers < 0)
		workers = 0;
	if (workers > MAX_WORKERS)
		workers = MAX_WORKERS;
	if (workers == pool.numworkers)
		return;

	if (pool.numworkers)
	{
		InterlockedExchange (&pool.quit, 1);
		Sys_WakeWorkers ();
		WaitForMultipleObjects ((DWORD)pool.numworkers, pool.threads, TRUE, INFINITE);
		for (i = 0 ; i < pool.numworkers ; i++)
			CloseHandle (pool.threads[i]);
		pool.numworkers = 0;
		InterlockedExchange (&pool.quit, 0);
	}

	InterlockedExchange (&pool.generation, 0);
	for (i = 0 ; i < workers ; i++)
	{
		pool.threads[i] = CreateThread (NULL, 0, Sys_WorkerMain, NULL, 0, NULL);
		if (!pool.threads[i])
			break;
	}
	pool.numworkers = i;
}

void Sys_Parallel (int count, void (*job) (void *ctx, int index), void *ctx)
{
	int		i;

	if (count <= 0)
		return;
	if (!pool.numworkers || count == 1)
	{
		for (i = 0 ; i < count ; i++)
			job (ctx, i);
		return;
	}

	// the run, published before the workers are let in
	pool.job = job;
	pool.ctx = ctx;
	InterlockedExchange (&pool.count, count);
	InterlockedExchange (&pool.done, 0);
	InterlockedExchange (&pool.next, 0);
	InterlockedExchange (&pool.running, 1);
	Sys_WakeWorkers ();

	Sys_RunJobs ();
	while (pool.done < count)
		YieldProcessor ();

	// no worker may still be taking jobs when the next run is published
	InterlockedExchange (&pool.running, 0);
	while (pool.active)
		YieldProcessor ();
}
