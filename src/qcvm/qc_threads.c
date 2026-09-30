// qc_threads.c -- QuakeC threads: FTE's sleep and fork

#include "qc_local.h"

struct qc_thread_s
{
	uint8_t		*data;			// captured local stack and locals
	size_t		len;
};

void QC_MarkThreads (qcvm_t *vm, uint8_t *marks)
{
	uint32_t	i;

	for (i = 0 ; i < vm->numthreads ; i++)
		QC_GCMark (&vm->strings, marks, vm->threads[i].data, vm->threads[i].len);
}
