// cpu_check_wasm_simd128.c -- WebAssembly with SIMD: a browser without it can't
// compile the game at all, so the page checks before it loads it (shell.html).

#include "../cpu_check.h"

#include <stddef.h>

const char *Cpu_CheckSupport (void)
{
	return NULL;
}
