// entry_gui.c -- process entry point for the windowed programs on macOS. Compiled
// without architecture flags and outside link-time code generation, so the CPU
// check runs before any code that may use instructions the CPU lacks.

#include "../cpu_check.h"
#include "../entry.h"

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

int main (int argc, char **argv)
{
	const char *problem = Cpu_CheckSupport ();
	if (problem)
	{
		CFStringRef	text = CFStringCreateWithCString (NULL, problem, kCFStringEncodingUTF8);

		fprintf (stderr, "%s\n", problem);
		CFUserNotificationDisplayAlert (0, kCFUserNotificationStopAlertLevel, NULL, NULL, NULL,
			CFSTR ("SoftWorld"), text, NULL, NULL, NULL, NULL);
		CFRelease (text);
		return 1;
	}
	return Sys_MacMain (argc, argv);
}
