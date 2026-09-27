// print.h -- console output shared by the client and the server.
//
// Formatted text goes to the active redirect if there is one (the server uses
// that to send rcon replies back to the requester); otherwise it is echoed with
// Sys_Printf and handed to every registered sink (the client console, log files).
#pragma once

typedef void (*print_sink_t) (const char *text);

void	Con_Printf (char *fmt, ...);

// prints only when the developer cvar is set
void	Con_DPrintf (char *fmt, ...);

void	Con_AddPrintSink (print_sink_t sink);

// routes all output to redirect until called again with NULL
void	Con_SetPrintRedirect (print_sink_t redirect);

// registers the developer cvar
void	Con_PrintInit (void);

extern struct cvar_s	developer;
