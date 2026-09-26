// print.c -- console output shared by the client and the server

#include "cvar.h"
#include "print.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>

#define	MAXPRINTMSG		4096
#define	MAX_PRINT_SINKS	8

cvar_t	developer = {.name = "developer", .string = "0"};	// show extra messages

static print_sink_t	print_sinks[MAX_PRINT_SINKS];
static int			num_print_sinks;
static print_sink_t	print_redirect;

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

void Con_RemovePrintSink (print_sink_t sink)
{
	int		i;

	for (i = 0 ; i < num_print_sinks ; i++)
	{
		if (print_sinks[i] == sink)
		{
			print_sinks[i] = print_sinks[--num_print_sinks];
			return;
		}
	}
}

void Con_SetPrintRedirect (print_sink_t redirect)
{
	print_redirect = redirect;
}

static void Con_Output (const char *msg)
{
	int		i;

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
