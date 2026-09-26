// entry_win.h -- hand-off from the process entry points (built without architecture
// flags) to the engine's main functions (built with them).
#pragma once

#include <windows.h>

int Sys_WinMain (HINSTANCE hInstance, LPSTR lpCmdLine, int nCmdShow);
int Sys_ConsoleMain (int argc, char **argv);
