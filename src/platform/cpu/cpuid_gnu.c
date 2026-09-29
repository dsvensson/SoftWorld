// cpuid_gnu.c -- CPUID and XGETBV as GCC and clang have them (cpu_cpuid.h). XGETBV
// is written out: its intrinsic needs -mxsave, which the check isn't built with.

#include "cpu_cpuid.h"

#include <cpuid.h>

void Cpu_Cpuid (uint32_t leaf, uint32_t subleaf, uint32_t regs[4])
{
	unsigned	a, b, c, d;

	__cpuid_count (leaf, subleaf, a, b, c, d);
	regs[0] = a;
	regs[1] = b;
	regs[2] = c;
	regs[3] = d;
}

uint64_t Cpu_Xgetbv (uint32_t index)
{
	uint32_t	lo, hi;

	__asm__ volatile ("xgetbv" : "=a" (lo), "=d" (hi) : "c" (index));
	return ((uint64_t)hi << 32) | lo;
}
