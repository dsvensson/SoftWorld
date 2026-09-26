// cpu_check.h -- verifies that the CPU supports the instruction set the engine was
// compiled for. Implementations are selected by the build (SW_ARCH), and are compiled
// without architecture-specific code generation flags.
#pragma once

// Returns NULL when the CPU can run this build, otherwise a human-readable reason.
const char *Cpu_CheckSupport (void);
