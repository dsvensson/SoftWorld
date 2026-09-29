#pragma once
// posix_local.h -- shared by the platform files of macOS and Linux: what the
// posix/ files use, and each system provides (sys_macos.c, sys_linux.c)

#include "q_types.h"

#include <pthread.h>
#include <stdint.h>

// the file descriptors Sys_WaitUntil wakes up for (console input, sockets), in
// a kqueue or an epoll. Edge-triggered: an fd wakes it when something new
// arrives, not again while unread data waits (the client leaves packets unread
// until its frame is due). False for what can't be waited on, such as
// /dev/null.
bool	Sys_AddWaitFd (int fd);
void	Sys_RemoveWaitFd (int fd);
int		Sys_WaitQueue (void);		// the kqueue or epoll itself, created on first use

// a timer in the queue for when the wait ends, as precise as the system makes
// one
void	Sys_SetWaitTimer (double until);
void	Sys_ClearWaitTimer (void);

// what the queue has: its timer fired, an fd has something, or a signal
// interrupted the wait; waits for one of them if block
enum { SYS_WAIT_TIMER = 1, SYS_WAIT_FD = 2, SYS_WAIT_SIGNAL = 4 };
int		Sys_ReadWaitQueue (bool block);

// each program's own: sleeps until Sys_DoubleTime () reaches until, or until
// input or a packet arrives; true if woken early
bool	Sys_WaitEvents (double until);

//
// the worker threads (sys_workers_posix.c), as each system has them
//

// sleeps while *address holds value; may return early
void	Sys_WorkerWait (_Atomic uint32_t *address, uint32_t value);

// wakes every thread sleeping on address
void	Sys_WorkerWakeAll (_Atomic uint32_t *address);

// a worker thread's attributes, besides its stack size
void	Sys_WorkerThreadAttr (pthread_attr_t *attr);
