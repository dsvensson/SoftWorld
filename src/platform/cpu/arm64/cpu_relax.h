// cpu_relax.h -- the hint a spinning loop gives the CPU, arm64's (the x86-64
// one is in cpu/x86_64; the build puts the target's on the include path)
#pragma once

static inline void Sys_CpuRelax (void)
{
	__builtin_arm_yield ();
}
