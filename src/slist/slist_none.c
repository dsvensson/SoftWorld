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
// slist_none.c -- the list where there is none to make: a page has no UDP to
// ask servers with. Its sources are listed, its servers none, and a scan ends
// as it starts.

#include "slist.h"

#include <stdlib.h>
#include <string.h>

static bool			sl_started;
static unsigned		sl_generation;
static slsnapshot_t	*sl_published;

// an empty list, its sources as they are
static void SL_PublishEmpty (int numsources)
{
	slsnapshot_t	*snap = calloc (1, sizeof(*snap));
	int				i;

	if (!snap)
		return;
	snap->generation = ++sl_generation;
	for (i = 0 ; i < numsources && i < SL_MAXSOURCES ; i++)
		snap->sources[i].state = SLSRC_IDLE;
	free (sl_published);
	sl_published = snap;
}

bool SL_Start (const char *dir, const char *cachepath, const slsource_t *sources, int numsources)
{
	(void)dir;
	(void)cachepath;
	(void)sources;
	if (!sl_started)
		SL_PublishEmpty (numsources);
	sl_started = true;
	return true;
}

bool SL_Running (void)
{
	return sl_started;
}

void SL_Refresh (const slsource_t *sources, int numsources, const slconfig_t *c)
{
	(void)sources;
	(void)c;
	if (sl_started)
		SL_PublishEmpty (numsources);
}

void SL_Describe (netadr_t a)
{
	(void)a;
}

void SL_Pause (bool paused)
{
	(void)paused;
}

void SL_Shutdown (void)
{
	free (sl_published);
	sl_published = NULL;
	sl_started = false;
}

slsnapshot_t *SL_TakeSnapshot (void)
{
	slsnapshot_t	*snap = sl_published;

	sl_published = NULL;
	return snap;
}

void SL_FreeSnapshot (slsnapshot_t *s)
{
	free (s);
}

bool SL_TakeMessage (char *buf, size_t size)
{
	(void)buf;
	(void)size;
	return false;
}
