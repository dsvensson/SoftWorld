// cpu_relax.h -- the hint a spinning loop gives the CPU, x86-64's (the arm64
// one is in cpu/arm64; the build puts the target's on the include path)
#pragma once

static inline void Sys_CpuRelax (void)
{
	__builtin_ia32_pause ();
}
