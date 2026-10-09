// print.c -- console output shared by the client and the server

#include "cvar.h"
#include "mem.h"
#include "print.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define	MAXPRINTMSG		4096
#define	MAX_PRINT_SINKS	8

cvar_t	developer = {.name = "developer", .string = "0",	// show extra messages
	.description = "Prints developer messages to the console, and warns of unknown commands.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "Developer messages printed"}, {0}}};

static print_sink_t	print_sinks[MAX_PRINT_SINKS];
static int			num_print_sinks;
static print_sink_t	print_redirect;
static thread_local print_capture_t	*print_capture;	// Con_CaptureThread's

void Con_PrintInit (void)
{
	Cvar_RegisterVariable (&developer);
}

void Con_AddPrintSink (print_sink_t sink)
{
	if (num_print_sinks == MAX_PRINT_SINKS)
		Sys_Error ("Con_AddPrintSink: too many sinks");
	print_sinks[num_print_sinks++] = sink;
}

void Con_SetPrintRedirect (print_sink_t redirect)
{
	print_redirect = redirect;
}

void Con_CaptureThread (print_capture_t *capture)
{
	print_capture = capture;
}

static void Con_Output (const char *msg);

void Con_FlushCapture (print_capture_t *capture)
{
	char	*text = capture->text;

	*capture = (print_capture_t){0};
	if (!text)
		return;
	Con_Output (text);
	Mem_Free (text);
}

static void Con_Output (const char *msg)
{
	int		i;
	size_t	len;

	if (print_capture)
	{	// a loader's: the sinks are the main thread's
		len = strlen (msg);
		if (print_capture->len + len + 1 > print_capture->size)
		{
			print_capture->size = (print_capture->len + len + 1) * 2;
			print_capture->text = Mem_Realloc (print_capture->text, print_capture->size);
		}
		memcpy (print_capture->text + print_capture->len, msg, len + 1);
		print_capture->len += len;
		return;
	}

	if (print_redirect)
	{
		print_redirect (msg);
		return;
	}

	Sys_Printf ("%s", msg);	// also echo to debugging console
	for (i = 0 ; i < num_print_sinks ; i++)
		print_sinks[i] (msg);
}

/*
================
Con_Printf
================
*/
void Con_Printf (char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_Output (msg);
}

/*
================
Con_DPrintf

A Con_Printf that only shows up if the "developer" cvar is set
================
*/
void Con_DPrintf (char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if (!developer.value)
		return;

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_Output (msg);
}
