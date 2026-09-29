// sys_workers_posix.c -- the worker threads of Sys_Parallel, on macOS and Linux
//
// As on Windows (sys_win_workers.c): a run publishes its jobs and bumps a
// generation the workers wait on; the jobs are taken by an index each thread
// increments. The caller starts on the jobs at once and the workers join as
// they wake. A worker out of jobs sleeps (Sys_WorkerWait: macOS's
// os_sync_wait_on_address, Linux's futex) at once: spinning for the next run
// cost more than waking, as spinning cores take clock speed and cycles from
// the thread drawing the rest of the frame.
//
// x86 kept the Windows version's plain loads in order; arm64 doesn't, so the
// shared fields are atomics, sequentially consistent as Interlocked* are. The
// run's job, context and count are written before the generation that
// publishes them, and a worker reads them only after seeing it.

#include "sys.h"
#include "posix_local.h"
#include "cpu_relax.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define	MAX_WORKERS		63
#define	WORKER_STACK	(1 << 20)	// Windows' default; macOS gives threads 512 KB

static struct
{
	void			(*job) (void *ctx, int index);
	void			*ctx;
	int				count;			// the jobs of the run
	atomic_int		next;			// the next job to take
	atomic_int		done;			// jobs finished
	atomic_int		running;		// a run is on; the workers join only then
	atomic_int		active;			// workers inside a run
	_Atomic uint32_t	generation;	// bumped by each run and by stopping; the workers wait on it
	atomic_int		sleepers;		// workers waiting on the generation
	atomic_int		quit;
	int				numworkers;
	pthread_t		threads[MAX_WORKERS];
} pool;

// takes and runs the run's jobs until none are left
static void Sys_RunJobs (void)
{
	int		i;

	while ((i = atomic_fetch_add (&pool.next, 1)) < pool.count)
	{
		pool.job (pool.ctx, i);
		atomic_fetch_add (&pool.done, 1);
	}
}

static void *Sys_WorkerMain (void *unused)
{
	uint32_t	seen = 0, now;

	(void)unused;
	for (;;)
	{
		// sleep until the next run
		atomic_fetch_add (&pool.sleepers, 1);
		while ((now = atomic_load (&pool.generation)) == seen)
			Sys_WorkerWait (&pool.generation, seen);
		atomic_fetch_sub (&pool.sleepers, 1);
		seen = now;
		if (atomic_load (&pool.quit))
			return NULL;

		// a run: joined only if it is still the one that woke us
		atomic_fetch_add (&pool.active, 1);
		if (atomic_load (&pool.running) && atomic_load (&pool.generation) == now)
			Sys_RunJobs ();
		atomic_fetch_sub (&pool.active, 1);
	}
}

// bumps the generation and wakes the sleeping workers
static void Sys_WakeWorkers (void)
{
	atomic_fetch_add (&pool.generation, 1);
	if (atomic_load (&pool.sleepers))
		Sys_WorkerWakeAll (&pool.generation);
}

void Sys_SetWorkers (int workers)
{
	pthread_attr_t	attr;
	int				i;

	if (workers < 0)
		workers = 0;
	if (workers > MAX_WORKERS)
		workers = MAX_WORKERS;
	if (workers == pool.numworkers)
		return;

	if (pool.numworkers)
	{
		atomic_store (&pool.quit, 1);
		Sys_WakeWorkers ();
		for (i = 0 ; i < pool.numworkers ; i++)
			pthread_join (pool.threads[i], NULL);
		pool.numworkers = 0;
		atomic_store (&pool.quit, 0);
	}

	atomic_store (&pool.generation, 0);
	pthread_attr_init (&attr);
	pthread_attr_setstacksize (&attr, WORKER_STACK);
	Sys_WorkerThreadAttr (&attr);
	for (i = 0 ; i < workers ; i++)
		if (pthread_create (&pool.threads[i], &attr, Sys_WorkerMain, NULL))
			break;
	pthread_attr_destroy (&attr);
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
	pool.count = count;
	atomic_store (&pool.done, 0);
	atomic_store (&pool.next, 0);
	atomic_store (&pool.running, 1);
	Sys_WakeWorkers ();

	Sys_RunJobs ();
	while (atomic_load (&pool.done) < count)
		Sys_CpuRelax ();

	// no worker may still be taking jobs when the next run is published
	atomic_store (&pool.running, 0);
	while (atomic_load (&pool.active))
		Sys_CpuRelax ();
}
