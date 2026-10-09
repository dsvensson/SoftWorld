// print.h -- console output shared by the client and the server.
//
// Formatted text goes to the active redirect if there is one (the server uses
// that to send rcon replies back to the requester); otherwise it is echoed with
// Sys_Printf and handed to every registered sink (the client console, log files).
#pragma once

#include <stddef.h>

typedef void (*print_sink_t) (const char *text);

void	Con_Printf (char *fmt, ...);

// prints only when the developer cvar is set
void	Con_DPrintf (char *fmt, ...);

void	Con_AddPrintSink (print_sink_t sink);

// routes all output to redirect until called again with NULL
void	Con_SetPrintRedirect (print_sink_t redirect);

// what a thread other than the main one prints (a loader's), kept to be
// printed by the main thread: Con_CaptureThread sets the calling thread's
// (NULL: none), Con_FlushCapture prints and empties one
typedef struct print_capture_s
{
	char	*text;
	size_t	len, size;
} print_capture_t;
void	Con_CaptureThread (print_capture_t *capture);
void	Con_FlushCapture (print_capture_t *capture);

// registers the developer cvar
void	Con_PrintInit (void);

extern struct cvar_s	developer;
