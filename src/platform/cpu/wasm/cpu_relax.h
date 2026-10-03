// cpu_relax.h -- the hint a spinning loop gives the CPU, WebAssembly's, which
// has none (the x86-64 one is in cpu/x86_64; the build puts the target's on the
// include path). The page's own thread spins instead on what the workers hand
// it: a worker's print goes through the file system there, and would wait on
// the thread waiting on it.
#pragma once

#include <emscripten/threading.h>

static inline void Sys_CpuRelax (void)
{
	if (emscripten_is_main_runtime_thread ())
		emscripten_main_thread_process_queued_calls ();
}
