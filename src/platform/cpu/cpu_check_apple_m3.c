// cpu_check_apple_m3.c -- verifies the instruction set -mcpu=apple-m3 builds for
// (that of the M2 and later: NEON with FP16, dot products, BF16 and I8MM, LSE
// atomics, and the rest), before any code compiled for it runs.

#include "../cpu_check.h"

#include <stddef.h>
#include <stdio.h>
#include <sys/sysctl.h>

// what clang's apple-m3 target enables that code can use
static const char *const	features[] =
{
	"AdvSIMD", "FEAT_FP16", "FEAT_FHM", "FEAT_DotProd", "FEAT_RDM", "FEAT_BF16", "FEAT_I8MM",
	"FEAT_LSE", "FEAT_LSE2", "FEAT_CRC32", "FEAT_AES", "FEAT_PMULL", "FEAT_SHA1", "FEAT_SHA256",
	"FEAT_SHA512", "FEAT_SHA3", "FEAT_FCMA", "FEAT_JSCVT", "FEAT_FRINTTS", "FEAT_LRCPC",
	"FEAT_LRCPC2", "FEAT_FlagM", "FEAT_FlagM2", "FEAT_SB", "FEAT_BTI", "FEAT_PAuth",
};

const char *Cpu_CheckSupport (void)
{
	static char	problem[160];
	char		name[64];
	int			value;
	size_t		size, i;

	for (i = 0 ; i < sizeof(features) / sizeof(features[0]) ; i++)
	{
		snprintf (name, sizeof(name), "hw.optional.arm.%s", features[i]);
		value = 0;
		size = sizeof(value);
		if (sysctlbyname (name, &value, &size, NULL, 0) || !value)
		{
			snprintf (problem, sizeof(problem),
				"This build needs an Apple M2 or later: the CPU lacks %s.", features[i]);
			return problem;
		}
	}
	return NULL;
}
