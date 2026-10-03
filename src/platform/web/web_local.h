#pragma once
// web_local.h -- shared by the web's platform files; the worker threads are
// posix_local.h's. The page's side of each is a JavaScript library next to its
// C file (sys_web.js, fs_web.js, vid_webgl.js, in_web.js, snd_webaudio.js,
// net_ws_web.js), whose functions are declared here.

#include "posix_local.h"

//
// sys_web_gui.c, sys_web.js
//

// the page has the focus; the page can't be seen (another tab, minimized)
extern	bool	ActiveApp, Minimized;

// a notice over the game, by its slot ("capture", "sound", "error"); NULL text
// takes it down
void	web_notice (const char *slot, const char *text);

// the page's loop stops for good: the text over the game, mouse and fullscreen let go
void	web_stop (const char *text, int error);

// starts the page's loop (Sys_WebFrame), and when the page hides, Sys_WebHidden
void	web_start_loop (void);

// a frame comes now rather than when due: input or a packet arrived
void	web_wake (void);

//
// vid_webgl.c
//

// the focus came or went, the page was hidden or shown, fullscreen began or ended
void	VID_AppActivate (bool active);
void	VID_WindowSuspended (bool suspended);
void	VID_SetFullscreenState (bool fullscreen);
void	VID_ToggleFullscreen (void);

//
// in_web.c
//

// the page's events (keys, buttons, the focus) are queued as they come, and sent
// to the game here, in Sys_SendKeyEvents
void	IN_StartEvents (void);
void	IN_PumpEvents (void);

// the keyboard's and mouse's focus went or came
void	IN_WindowActivated (bool active);

// the text pasted last (Ctrl+V lets the browser paste it), malloc'd; NULL for none
char	*IN_PastedText (void);

// in_gamepad_web.c
void	IN_InitGamepad (void);
void	IN_PollGamepad (void);
