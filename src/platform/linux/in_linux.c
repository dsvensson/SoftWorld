#include "cvar.h"
#include "keys.h"
#include <linux/input-event-codes.h>
#include "window.h"

cvar_t	_windowed_mouse = {.name = "_windowed_mouse", .string = "1", .archive = true,
	.description = "Captures the mouse in a window too, as fullscreen always does; the console and menus let it go.",
	.values = (const cvar_value_t[]){{"0", "Captured only in fullscreen"}, {"1", "Captured in a window too"}, {0}}};

void IN_Init (void)
{
	Cvar_RegisterVariable (&_windowed_mouse);
	window->InputInit ();
	IN_InitGamepad ();
}

void IN_Shutdown (void)
{
	if (window)
		window->InputShutdown ();
}

void IN_Commands (void)
{
	window->InputCommands ();
	IN_PollGamepad ();
}

void IN_WindowActivated (bool active)
{
	window->InputActivated (active);
}

double IN_NextRepeat (void)
{
	return window && window->NextRepeat ? window->NextRepeat () : 0;
}

void IN_Repeat (void)
{
	if (window && window->Repeat)
		window->Repeat ();
}

// the keys by evdev code, placed as the Windows scancodes are: the keypad is the
// navigation keys, as without Num Lock
static const byte	codetokey[128] =
{
	[KEY_ESC] = K_ESCAPE, [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
	[KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-',
	[KEY_EQUAL] = '=', [KEY_BACKSPACE] = K_BACKSPACE, [KEY_TAB] = K_TAB,
	[KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u',
	[KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
	[KEY_ENTER] = K_ENTER, [KEY_LEFTCTRL] = K_CTRL,
	[KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j',
	[KEY_K] = 'k', [KEY_L] = 'l', [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'', [KEY_GRAVE] = '`',
	[KEY_LEFTSHIFT] = K_SHIFT, [KEY_BACKSLASH] = '\\',
	[KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm',
	[KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/', [KEY_RIGHTSHIFT] = K_SHIFT,
	[KEY_KPASTERISK] = '*', [KEY_LEFTALT] = K_ALT, [KEY_SPACE] = K_SPACE,
	[KEY_F1] = K_F1, [KEY_F2] = K_F2, [KEY_F3] = K_F3, [KEY_F4] = K_F4, [KEY_F5] = K_F5, [KEY_F6] = K_F6,
	[KEY_F7] = K_F7, [KEY_F8] = K_F8, [KEY_F9] = K_F9, [KEY_F10] = K_F10, [KEY_F11] = K_F11, [KEY_F12] = K_F12,
	[KEY_KP7] = K_HOME, [KEY_KP8] = K_UPARROW, [KEY_KP9] = K_PGUP, [KEY_KPMINUS] = '-',
	[KEY_KP4] = K_LEFTARROW, [KEY_KP5] = '5', [KEY_KP6] = K_RIGHTARROW, [KEY_KPPLUS] = '+',
	[KEY_KP1] = K_END, [KEY_KP2] = K_DOWNARROW, [KEY_KP3] = K_PGDN, [KEY_KP0] = K_INS, [KEY_KPDOT] = K_DEL,
	[KEY_KPENTER] = K_ENTER, [KEY_RIGHTCTRL] = K_CTRL, [KEY_KPSLASH] = '/', [KEY_RIGHTALT] = K_ALT,
	[KEY_KPEQUAL] = '=',
	[KEY_HOME] = K_HOME, [KEY_UP] = K_UPARROW, [KEY_PAGEUP] = K_PGUP, [KEY_LEFT] = K_LEFTARROW,
	[KEY_RIGHT] = K_RIGHTARROW, [KEY_END] = K_END, [KEY_DOWN] = K_DOWNARROW, [KEY_PAGEDOWN] = K_PGDN,
	[KEY_INSERT] = K_INS, [KEY_DELETE] = K_DEL, [KEY_PAUSE] = K_PAUSE,
};

int IN_EvdevKey (unsigned code)
{
	return code < sizeof(codetokey) ? codetokey[code] : 0;
}
