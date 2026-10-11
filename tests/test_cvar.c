/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// test_cvar.c -- variables set with and without a range: a value in it kept
// as it was given, one past it set to its nearer end and the console told

#include "cvar.h"
#include "print.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;
static char	printed[256];		// what the console was told last

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

// the file system the commands' exec would read: none
int Sys_FileTime (char *path) { (void)path; return -1; }
void Sys_mkdir (char *path) { (void)path; }
bool Sys_ListDir (const char *path, void (*entry) (void *ctx, const char *name, bool isdir), void *ctx)
{
	(void)path;
	(void)entry;
	(void)ctx;
	return false;
}

static void Printed (const char *msg)
{
	snprintf (printed, sizeof(printed), "%s", msg);
}

static cvar_t	ranged = {.name = "test_ranged", .string = "1", .min = 0, .max = 4};
static cvar_t	negative = {.name = "test_negative", .string = "-0.5", .min = -1, .max = 0};
static cvar_t	unranged = {.name = "test_unranged", .string = "1"};

// set to value, var is want and the console told told ("" for nothing)
static void Check (cvar_t *var, char *value, const char *want, const char *told)
{
	printed[0] = 0;
	Cvar_Set (var->name, value);
	if (strcmp (var->string, want) || var->value != (float)atof (want) || strcmp (printed, told))
	{
		printf ("FAIL %s %s: \"%s\" (%g), told \"%s\"; want \"%s\", told \"%s\"\n", var->name, value,
			var->string, var->value, printed, want, told);
		failures++;
	}
}

int main (void)
{
	Con_SetPrintRedirect (Printed);
	Cvar_RegisterVariable (&ranged);
	Cvar_RegisterVariable (&negative);
	Cvar_RegisterVariable (&unranged);

	Check (&ranged, "2", "2", "");
	Check (&ranged, "2.5", "2.5", "");
	Check (&ranged, "0", "0", "");
	Check (&ranged, "4", "4", "");
	Check (&ranged, "9", "4", "test_ranged is 0 to 4: set to 4\n");
	Check (&ranged, "-1", "0", "test_ranged is 0 to 4: set to 0\n");
	Check (&ranged, "4.01", "4", "test_ranged is 0 to 4: set to 4\n");
	Check (&negative, "-2", "-1", "test_negative is -1 to 0: set to -1\n");
	Check (&negative, "0.25", "0", "test_negative is -1 to 0: set to 0\n");
	Check (&unranged, "9", "9", "");
	Check (&unranged, "-1", "-1", "");

	// as C code sets one
	printed[0] = 0;
	Cvar_SetValue (ranged.name, 7);
	if (strcmp (ranged.string, "4") || ranged.value != 4)
	{
		printf ("FAIL Cvar_SetValue: \"%s\"\n", ranged.string);
		failures++;
	}

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("all variables set as they should be\n");
	return 0;
}
