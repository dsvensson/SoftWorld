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
// sv_null.c -- the server of the client-only program: there is none

#include "cmd.h"
#include "print.h"
#include "sv_public.h"

static void SV_NoServer_f (void)
{
	Con_Printf ("This program has no server; connect to one instead.\n");
}

void SV_Init (void)
{
	Cmd_AddCommand ("map", SV_NoServer_f, "Says this program has no server to load a map on; connect to one instead.");
}

void SV_Shutdown (void)
{
}

bool SV_Active (void)
{
	return false;
}

void SV_Kill (void)
{
}

void SV_Frame (double time, bool away)
{
	(void)time;
	(void)away;
}

double SV_NextFrameWait (void)
{
	return 1;
}

bool SV_AttractLevel (const sv_attract_t *level)
{
	(void)level;
	return false;
}

void SV_AttractEnd (void)
{
}

bool SV_Attracting (void)
{
	return false;
}

struct cmap_s *SV_ShareMap (const char *name, unsigned *checksum2)
{
	(void)name;
	(void)checksum2;
	return NULL;
}

void SV_SetAttractStop (void (*stop) (void))
{
	(void)stop;
}

const char *SV_CanSave (void)
{
	return "This program has no server to save a game of.";
}

bool SV_CanLoad (void)
{
	return false;
}
