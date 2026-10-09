#pragma once
// loader.h -- work done on a thread of its own, while a level plays: models,
// sounds and maps loaded without the frames waiting for them.
//
// A job runs on the loader's thread, reading files through a chain of its
// own (fs.h), its prints kept; it then finishes on the main thread, at
// Load_Poll, which prints them. One thread runs the jobs in turn. Where there
// is none (the web, whose files the page's thread fetches anyway), a job runs
// as it is given, and still finishes at the next Load_Poll.

#include "fs.h"
#include "print.h"

typedef struct loadjob_s
{
	void			(*run) (struct loadjob_s *job);		// on the loader's thread; no Sys_Error
	void			(*finish) (struct loadjob_s *job);	// on the main thread; frees the job
	fs_chain_t		*chain;		// read through; the search path's as it is at Load_Submit if NULL

	// the loader's
	print_capture_t	prints;
	struct loadjob_s	*next;
} loadjob_t;

// a job to run, ahead of the others waiting if front; the job is the
// first member of the caller's own
void	Load_Submit (loadjob_t *job, bool front);

// the jobs run since: finished on the main thread (each frame)
void	Load_Poll (void);

// waits for every job given to run and finish
void	Load_Flush (void);

// whether jobs run apart from the main thread
bool	Load_Threaded (void);

// finishes the jobs and stops the thread
void	Load_Shutdown (void);
