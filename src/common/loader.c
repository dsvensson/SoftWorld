// loader.c -- work done on a thread of its own (loader.h)

#include "loader.h"
#include "sys.h"

#include <stdatomic.h>

static struct
{
	systhread_t			*thread;		// NULL: jobs run as they are given
	bool				started;		// the thread was tried
	atomic_flag			lock;			// the two lists
	loadjob_t			*pending, *pending_tail;	// to run
	loadjob_t			*done, *done_tail;			// to finish
	_Atomic uint32_t	signal;			// bumped by each job given and by stopping; the thread waits on it
	atomic_bool			quit;
	int					outstanding;	// given and not finished (main thread)
} load = {.lock = ATOMIC_FLAG_INIT};

static void Load_Lock (void)
{
	while (atomic_flag_test_and_set_explicit (&load.lock, memory_order_acquire))
		;
}

static void Load_Unlock (void)
{
	atomic_flag_clear_explicit (&load.lock, memory_order_release);
}

// a job run, its prints kept and its files read through its chain
static void Load_Run (loadjob_t *job)
{
	Con_CaptureThread (&job->prints);
	FS_UseChain (job->chain);
	job->run (job);
	FS_UseChain (NULL);
	Con_CaptureThread (NULL);

	Load_Lock ();
	job->next = NULL;
	if (load.done_tail)
		load.done_tail->next = job;
	else
		load.done = job;
	load.done_tail = job;
	Load_Unlock ();
}

#ifndef __EMSCRIPTEN__
static void Load_Thread (void *unused)
{
	loadjob_t	*job;
	uint32_t	seen;

	(void)unused;
	for (;;)
	{
		// read before the list is: a job given after is a change seen
		seen = atomic_load (&load.signal);
		Load_Lock ();
		job = load.pending;
		if (job)
		{
			load.pending = job->next;
			if (!load.pending)
				load.pending_tail = NULL;
		}
		Load_Unlock ();

		if (job)
			Load_Run (job);
		else if (atomic_load (&load.quit))
			return;
		else
			Sys_WaitAddress ((void *)&load.signal, seen);
	}
}
#endif

static void Load_Start (void)
{
	load.started = true;
#ifndef __EMSCRIPTEN__
	load.thread = Sys_StartThread ("loader", Load_Thread, NULL);
#endif
}

bool Load_Threaded (void)
{
	if (!load.started)
		Load_Start ();
	return load.thread != NULL;
}

void Load_Submit (loadjob_t *job, bool front)
{
	job->next = NULL;
	job->prints = (print_capture_t){0};
	if (!job->chain)
		job->chain = FS_RetainChain (NULL);
	load.outstanding++;

	if (!Load_Threaded ())
	{
		Load_Run (job);
		return;
	}

	Load_Lock ();
	if (front)
	{
		job->next = load.pending;
		load.pending = job;
		if (!load.pending_tail)
			load.pending_tail = job;
	}
	else
	{
		if (load.pending_tail)
			load.pending_tail->next = job;
		else
			load.pending = job;
		load.pending_tail = job;
	}
	Load_Unlock ();
	atomic_fetch_add (&load.signal, 1);
	Sys_WakeAddress ((void *)&load.signal);
}

void Load_Poll (void)
{
	loadjob_t	*job, *next;

	if (!load.outstanding)
		return;

	Load_Lock ();
	job = load.done;
	load.done = load.done_tail = NULL;
	Load_Unlock ();

	for ( ; job ; job = next)
	{
		next = job->next;
		load.outstanding--;
		Con_FlushCapture (&job->prints);
		FS_ReleaseChain (job->chain);
		job->chain = NULL;
		job->finish (job);
	}
}

void Load_Flush (void)
{
	// rare (shutting down): the thread runs a job in a moment, so a wait for
	// it is a spin
	while (load.outstanding)
		Load_Poll ();
}

void Load_Shutdown (void)
{
	Load_Flush ();
	if (!load.thread)
		return;
	atomic_store (&load.quit, true);
	atomic_fetch_add (&load.signal, 1);
	Sys_WakeAddress ((void *)&load.signal);
	Sys_JoinThread (load.thread);
	load.thread = NULL;
	load.started = false;
	atomic_store (&load.quit, false);
}
