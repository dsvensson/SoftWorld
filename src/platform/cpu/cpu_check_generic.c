// cpu_check_generic.c -- baseline build (x86-64, or arm64): every CPU of the target qualifies.

#include "../cpu_check.h"

#include <stddef.h>

const char *Cpu_CheckSupport (void)
{
	return NULL;
}
