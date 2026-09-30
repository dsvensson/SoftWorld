// qc_lib_threads.c -- QuakeC threads: sleep and fork (FTE_MULTITHREADED); abort
// is in qc_lib_introspect.c
//
// The host resumes sleeping threads with QC_RunThreads.

#include "qc_lib.h"

// void sleep(float sleeptime): suspends the QuakeC thread for sleeptime seconds
// of the time global. Execution returns to the engine at once (the function it
// called returning 0); the thread carries on after the call when it wakes.
static bool QC_Sleep (qcvm_t *vm)
{
	static const uint32_t	zero[3] = {0, 0, 0};
	bool					suspended;

	if (!QC_Suspend (vm, QC_ArgFloat (vm, 0), zero, &suspended))
		return false;
	if (suspended)
		return QC_Abort (vm, QC_ValWord (0));
	QC_Warning (vm, "sleep called outside QuakeC");
	return true;
}

// float fork(optional float sleeptime): returns twice, now with 0 and after
// sleeptime seconds in a copy of the thread with 1; the copy should end with
// abort()
static bool QC_Fork (qcvm_t *vm)
{
	const uint32_t	one[3] = {QC_FloatBits (1), 0, 0};
	bool			suspended;

	if (!QC_Suspend (vm, QC_Argc (vm) > 0 ? QC_ArgFloat (vm, 0) : 0, one, &suspended))
		return false;
	QC_ReturnFloat (vm, 0);
	return true;
}

static const qc_libentry_t	qc_threads[] = {
	{"sleep", QC_Sleep, NULL, 0},
	{"fork", QC_Fork, NULL, 0},
};

bool QC_RegisterThreads (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_threads, sizeof(qc_threads) / sizeof(qc_threads[0]));
}
