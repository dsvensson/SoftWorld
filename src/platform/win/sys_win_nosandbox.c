// sys_win_nosandbox.c -- the programs with a client without the AppContainer
// (Debug builds, and SW_SANDBOX off): the program started is the game

#include "win_local.h"

bool Sys_SandboxLaunch ([[maybe_unused]] const char *cmdline, [[maybe_unused]] int *code)
{
	return false;
}

void Sys_SandboxInit (void)
{
}

void Sys_ClipCursor (const RECT *rect)
{
	ClipCursor (rect);
}
