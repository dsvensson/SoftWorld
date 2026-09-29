// entry_gui.c -- process entry point for the windowed programs on Linux. Compiled
// without architecture flags and outside link-time optimization, so the CPU
// check runs before any code that may use instructions the CPU lacks.

#include "../cpu_check.h"
#include "../entry.h"

#include <stdio.h>

int main (int argc, char **argv)
{
	const char *problem = Cpu_CheckSupport ();
	if (problem)
	{
		fprintf (stderr, "%s\n", problem);
		return 1;
	}
	return Sys_LinuxMain (argc, argv);
}
