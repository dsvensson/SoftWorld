// cpu_check_x86_64_v3.c -- verifies x86-64-v3 support (AVX2, FMA, BMI1/2, F16C, LZCNT,
// MOVBE on top of x86-64-v2) before any code compiled for that level runs.

#include "../cpu_check.h"
#include "cpu_cpuid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool Cpu_HasBits (uint32_t reg, uint32_t mask)
{
	return (reg & mask) == mask;
}

const char *Cpu_CheckSupport (void)
{
	uint32_t regs[4];

	Cpu_Cpuid (0, 0, regs);
	if (regs[0] < 7)
		return "This build requires an x86-64-v3 CPU (AVX2), but CPUID leaf 7 is unavailable.";

	// leaf 1 ECX: SSE3, SSSE3, FMA, CX16, SSE4.1, SSE4.2, MOVBE, POPCNT, OSXSAVE, AVX, F16C
	Cpu_Cpuid (1, 0, regs);
	const uint32_t leaf1_ecx = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 13) | (1u << 19) |
		(1u << 20) | (1u << 22) | (1u << 23) | (1u << 27) | (1u << 28) | (1u << 29);
	if (!Cpu_HasBits (regs[2], leaf1_ecx))
		return "This build requires an x86-64-v3 CPU, but AVX/FMA/SSE4.2-level features are missing.";

	// leaf 7 EBX: BMI1, AVX2, BMI2
	Cpu_Cpuid (7, 0, regs);
	if (!Cpu_HasBits (regs[1], (1u << 3) | (1u << 5) | (1u << 8)))
		return "This build requires an x86-64-v3 CPU with AVX2, BMI1 and BMI2.";

	// the CPU has it; XCR0: SSE and AVX state must be enabled by the OS
	if ((Cpu_Xgetbv (0) & 0x6) != 0x6)
		return "This build requires an x86-64-v3 CPU, but the OS has not enabled AVX state.";

	// extended leaf 0x80000001 ECX: LAHF/SAHF, LZCNT
	Cpu_Cpuid (0x80000001, 0, regs);
	if (!Cpu_HasBits (regs[2], (1u << 0) | (1u << 5)))
		return "This build requires an x86-64-v3 CPU, but LZCNT is missing.";

	return NULL;
}
