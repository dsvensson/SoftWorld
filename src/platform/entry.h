// entry.h -- hand-off from the process entry points (built without architecture
// flags) to the engine's main functions (built with them).
#pragma once

int Sys_ConsoleMain (int argc, char **argv);
int Sys_MacMain (int argc, char **argv);		// the windowed programs on macOS
int Sys_LinuxMain (int argc, char **argv);		// and on Linux
int Sys_WebMain (int argc, char **argv);		// and in a browser
