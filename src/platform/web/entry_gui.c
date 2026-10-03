// entry_gui.c -- process entry point for the programs with a client in a
// browser. Compiled as the other systems' entry points are (without
// architecture flags, outside link-time optimization); every browser that
// runs the page runs the build, so the CPU check passes.

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
	return Sys_WebMain (argc, argv);
}
