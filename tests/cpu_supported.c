// cpu_supported.c -- whether this machine's CPU runs the build: the check the
// programs make as they start (cpu_check_<arch>.c), as the exit code, 0 if it
// does. Built without the architecture's flags, as the check is, so it runs on
// any CPU; CI runs the tests only where it says 0.

#include "cpu_check.h"

#include <stdio.h>

int main (void)
{
	const char	*problem = Cpu_CheckSupport ();

	if (problem)
	{
		printf ("%s\n", problem);
		return 1;
	}
	printf ("This CPU runs the build.\n");
	return 0;
}
