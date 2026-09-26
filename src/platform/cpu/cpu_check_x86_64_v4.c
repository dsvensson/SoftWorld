// cpu_check_x86_64_v4.c -- verifies x86-64-v4 support (AVX-512 F/BW/CD/DQ/VL on top of
// x86-64-v3) before any code compiled for that level runs.

#include "../cpu_check.h"

#include <intrin.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool Cpu_HasBits (uint32_t reg, uint32_t mask)
{
	return (reg & mask) == mask;
}

const char *Cpu_CheckSupport (void)
{
	int regs[4];

	__cpuid (regs, 0);
	if (regs[0] < 7)
		return "This build requires an x86-64-v4 CPU (AVX-512), but CPUID leaf 7 is unavailable.";

	// leaf 1 ECX: SSE3, SSSE3, FMA, CX16, SSE4.1, SSE4.2, MOVBE, POPCNT, OSXSAVE, AVX, F16C
	__cpuid (regs, 1);
	const uint32_t leaf1_ecx = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 13) | (1u << 19) |
		(1u << 20) | (1u << 22) | (1u << 23) | (1u << 27) | (1u << 28) | (1u << 29);
	if (!Cpu_HasBits ((uint32_t)regs[2], leaf1_ecx))
		return "This build requires an x86-64-v4 CPU, but AVX/FMA/SSE4.2-level features are missing.";

	// XCR0: SSE, AVX, opmask, ZMM_Hi256 and Hi16_ZMM state must be enabled by the OS
	if ((_xgetbv (0) & 0xE6) != 0xE6)
		return "This build requires an x86-64-v4 CPU, but the OS has not enabled AVX-512 state.";

	// leaf 7 EBX: BMI1, AVX2, BMI2, AVX512F, AVX512DQ, AVX512CD, AVX512BW, AVX512VL
	__cpuidex (regs, 7, 0);
	const uint32_t leaf7_ebx = (1u << 3) | (1u << 5) | (1u << 8) | (1u << 16) | (1u << 17) |
		(1u << 28) | (1u << 30) | (1u << 31);
	if (!Cpu_HasBits ((uint32_t)regs[1], leaf7_ebx))
		return "This build requires an x86-64-v4 CPU with AVX-512 (F, DQ, CD, BW, VL).";

	// extended leaf 0x80000001 ECX: LAHF/SAHF, LZCNT
	__cpuid (regs, (int)0x80000001);
	if (!Cpu_HasBits ((uint32_t)regs[2], (1u << 0) | (1u << 5)))
		return "This build requires an x86-64-v4 CPU, but LZCNT is missing.";

	return NULL;
}
