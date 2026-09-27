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
// cl_items.c  -- when the items of a recorded or streamed game are back
//
// KTX writes into its recordings and QTV streams, never to players, a line
// when an item is taken ("//ktx took <entity> <seconds> <player>"; 0 seconds
// for a megahealth, whose clock waits for the overheal to run out), when a
// held item's clock starts ("//ktx timer <entity> <seconds>"), and when the
// match starts and every item is put back ("//ktx matchstart"). They are kept
// as a list of what was said when; what is away at a moment follows from the
// list up to it, so a seek has nothing to put back (qualia's item board, as a
// list beside the view).

#include "cl_local.h"

typedef enum { MARK_TOOK, MARK_TIMER, MARK_MATCHSTART } markkind_t;

typedef struct
{
	double		time;		// demo seconds
	markkind_t	kind;
	int			entity;
	float		delay;		// seconds until it is back; 0: not counting yet
} mark_t;

typedef enum { ITEM_NONE, ITEM_QUAD, ITEM_PENT, ITEM_RING, ITEM_SUIT, ITEM_MEGA, ITEM_RA, ITEM_YA, ITEM_GA,
	ITEM_RL, ITEM_LG } item_t;

static const char *const	item_names[] = {"", "quad", "pent", "ring", "suit", "mega", "ra", "ya", "ga", "rl", "lg"};

static mark_t	*marks;
static int		nummarks, maxmarks;

static cvar_t	demo_itemtimers = {.name = "demo_itemtimers", .string = "1", .archive = true};

void CL_ItemsClear (void)
{
	nummarks = 0;
}

static void Items_Add (double time, markkind_t kind, int entity, float delay)
{
	if (nummarks == maxmarks)
	{
		maxmarks = maxmarks ? maxmarks * 2 : 256;
		marks = Mem_Realloc (marks, (size_t)maxmarks * sizeof(*marks));
	}
	marks[nummarks++] = (mark_t){.time = time, .kind = kind, .entity = entity, .delay = delay};
}

/*
==================
CL_ItemsMarker

A stufftext of an MVD's said at time: its "//ktx" lines about items
==================
*/
void CL_ItemsMarker (const char *text, double time)
{
	char		line[256];
	const char	*end;
	size_t		len;
	int			entity;
	float		delay;

	for ( ; *text ; text = *end ? end + 1 : end)
	{
		end = strchr (text, '\n');
		if (!end)
			end = text + strlen (text);
		len = (size_t)(end - text) < sizeof(line) - 1 ? (size_t)(end - text) : sizeof(line) - 1;
		memcpy (line, text, len);
		line[len] = 0;

		if (sscanf (line, "//ktx took %d %f", &entity, &delay) == 2)
			Items_Add (time, MARK_TOOK, entity, delay);
		else if (sscanf (line, "//ktx timer %d %f", &entity, &delay) == 2)
			Items_Add (time, MARK_TIMER, entity, delay);
		else if (!strcmp (line, "//ktx matchstart"))
			Items_Add (time, MARK_MATCHSTART, 0, 0);
	}
}

/*
==================
Items_Kind

Which item an entity is, by the model it spawned with; only those worth
timing
==================
*/
static item_t Items_Kind (int entity)
{
	static const struct { const char *model; item_t item; }	models[] = {
		{"quaddama.mdl", ITEM_QUAD}, {"invulner.mdl", ITEM_PENT}, {"invisibl.mdl", ITEM_RING},
		{"suit.mdl", ITEM_SUIT}, {"b_bh100.bsp", ITEM_MEGA}, {"g_rock2.mdl", ITEM_RL}, {"g_light.mdl", ITEM_LG}
	};
	const entity_state_t	*base;
	const char				*model, *slash;
	int						i;

	if (entity <= 0 || entity >= MAX_EDICTS)
		return ITEM_NONE;
	base = &cl.baselines[entity];
	if (base->modelindex <= 0 || base->modelindex >= MAX_MODELS)
		return ITEM_NONE;
	model = cl.model_name[base->modelindex];
	slash = strrchr (model, '/');
	if (slash)
		model = slash + 1;
	if (!strcmp (model, "armor.mdl"))
		return base->skinnum == 2 ? ITEM_RA : base->skinnum == 1 ? ITEM_YA : ITEM_GA;
	for (i=0 ; i<(int)(sizeof(models)/sizeof(models[0])) ; i++)
		if (!strcmp (model, models[i].model))
			return models[i].item;
	return ITEM_NONE;
}

// the order they are listed in: one still held after all that count
static double Items_Order (double due)
{
	return due < 0 ? 1e30 : due;
}

/*
==================
CL_DrawItemTimers

Beside the view, the items away at the moment played, the soonest back
first; one still held (a megahealth) last
==================
*/
void CL_DrawItemTimers (void)
{
	struct { int entity; item_t item; double due; }	away[64], t;
	char		num[16];
	double		now, left;
	int			i, j, n, x, y;

	if (!cls.mvdplayback || !demo_itemtimers.value || cls.state != ca_active || cl.intermission)
		return;

	// the newest word about each item, up to now
	now = cl.time;
	n = 0;
	for (i=0 ; i<nummarks && marks[i].time <= now ; i++)
	{
		if (marks[i].kind == MARK_MATCHSTART)
		{
			n = 0;
			continue;
		}
		for (j=0 ; j<n && away[j].entity != marks[i].entity ; j++)
			;
		if (j == n)
		{
			if (n == (int)(sizeof(away)/sizeof(away[0])))
				continue;
			away[n].entity = marks[i].entity;
			away[n].item = Items_Kind (marks[i].entity);
			n++;
		}
		away[j].due = marks[i].delay > 0 ? marks[i].time + marks[i].delay : -1;
	}

	// the ones not back yet, soonest first
	for (i=j=0 ; i<n ; i++)
		if (away[i].item != ITEM_NONE && (away[i].due < 0 || away[i].due > now))
			away[j++] = away[i];
	n = j;
	for (i=1 ; i<n ; i++)
		for (j=i ; j>0 && Items_Order (away[j].due) < Items_Order (away[j-1].due) ; j--)
		{
			t = away[j];
			away[j] = away[j-1];
			away[j-1] = t;
		}

	x = scr.vrect.x + 8;
	y = scr.vrect.y + scr.vrect.height / 3;
	for (i=0 ; i<n && y + 8 <= scr.vrect.y + scr.vrect.height ; i++, y += 8)
	{
		if (away[i].due < 0)
			snprintf (num, sizeof(num), "%5s", "held");
		else
		{
			left = ceil (away[i].due - now);
			if (left >= 60)
				snprintf (num, sizeof(num), "%2i:%02i", (int)left / 60, (int)left % 60);
			else
				snprintf (num, sizeof(num), "%5i", (int)left);
		}
		Draw_Alt_String (x, y, (char *)item_names[away[i].item]);
		Draw_String (x + 40, y, num);
	}
}

void CL_InitItems (void)
{
	Cvar_RegisterVariable (&demo_itemtimers);
}
