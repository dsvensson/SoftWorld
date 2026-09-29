// cpu_cpuid.h -- the CPUID and XGETBV instructions, as each compiler has them
// (cpuid_msvc.c, cpuid_gnu.c); for the x86-64-v4 check, built as it is
// without architecture flags
#pragma once

#include <stdint.h>

// eax, ebx, ecx and edx of CPUID leaf, subleaf
void		Cpu_Cpuid (uint32_t leaf, uint32_t subleaf, uint32_t regs[4]);

// the extended control register index, which says what state the OS saves
uint64_t	Cpu_Xgetbv (uint32_t index);
