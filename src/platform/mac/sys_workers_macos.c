// sys_workers_macos.c -- the worker threads' waits and attributes on macOS
// (sys_workers_posix.c), and the cores they run on

#include "sys.h"
#include "posix_local.h"

#include <os/os_sync_wait_on_address.h>
#include <sys/sysctl.h>

// the performance cores: a job left to an efficiency core would hold up the frame
int Sys_NumCores (void)
{
	int		cores = 0;
	size_t	size = sizeof(cores);

	if (sysctlbyname ("hw.perflevel0.physicalcpu", &cores, &size, NULL, 0) || cores < 1)
	{
		size = sizeof(cores);
		if (sysctlbyname ("hw.physicalcpu", &cores, &size, NULL, 0) || cores < 1)
			cores = 1;
	}
	return cores;
}

void Sys_WorkerWait (_Atomic uint32_t *address, uint32_t value)
{
	os_sync_wait_on_address ((void *)address, value, sizeof(value), OS_SYNC_WAIT_ON_ADDRESS_NONE);
}

void Sys_WorkerWakeAll (_Atomic uint32_t *address)
{
	os_sync_wake_by_address_all ((void *)address, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

// run as the frame is drawn: on the performance cores
void Sys_WorkerThreadAttr (pthread_attr_t *attr)
{
	pthread_attr_set_qos_class_np (attr, QOS_CLASS_USER_INTERACTIVE, 0);
}
