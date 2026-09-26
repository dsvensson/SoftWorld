// entry_gui.c -- process entry point for the windowed executables. Compiled without
// architecture flags and outside link-time code generation, so the CPU check runs
// before any code that may use instructions the CPU lacks.

#include "../cpu_check.h"
#include "entry_win.h"

int WINAPI WinMain (HINSTANCE hInstance, [[maybe_unused]] HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	const char *problem = Cpu_CheckSupport ();
	if (problem)
	{
		MessageBoxA (NULL, problem, "SoftWorld", MB_OK | MB_ICONERROR);
		return 1;
	}
	return Sys_WinMain (hInstance, lpCmdLine, nCmdShow);
}
