// cpu_check_generic.c -- baseline build (x86-64, arm64 or WebAssembly): every CPU of the target qualifies.

#include "../cpu_check.h"

#include <stddef.h>

const char *Cpu_CheckSupport (void)
{
	return NULL;
}
