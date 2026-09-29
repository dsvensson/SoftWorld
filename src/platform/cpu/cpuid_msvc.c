// cpuid_msvc.c -- CPUID and XGETBV as MSVC and clang-cl have them (cpu_cpuid.h)

#include "cpu_cpuid.h"

#include <intrin.h>

void Cpu_Cpuid (uint32_t leaf, uint32_t subleaf, uint32_t regs[4])
{
	int		r[4];
	int		i;

	__cpuidex (r, (int)leaf, (int)subleaf);
	for (i = 0 ; i < 4 ; i++)
		regs[i] = (uint32_t)r[i];
}

uint64_t Cpu_Xgetbv (uint32_t index)
{
	return _xgetbv (index);
}
