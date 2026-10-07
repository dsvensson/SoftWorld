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
// list up to it, so a seek has nothing to put back (qualia's item board). It
// is shown as a list beside the view, and as qualia shows it in the world: a
// ring on the floor where an item is missing, lit as its return comes nearer,
// and a faint ghost of the item, where the item lies once it has dropped to
// the floor. Whether an item is missing is the server's word, not the
// clock's: a ring and a ghost stand while the item's entity isn't sent.

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

// a ring's color: the powerups their own, the armors and weapons the colors
// qualia outlines them in; 1.0 is white
static const float	item_colors[][3] = {
	{0, 0, 0}, {0.2f, 0.4f, 1.5f}, {1.5f, 0.1f, 0.1f}, {1.2f, 1.0f, 0.3f}, {0.2f, 1.2f, 0.4f}, {1.5f, 1.2f, 1.2f},
	{1.5f, 0, 0}, {1.5f, 1.5f, 0}, {0, 1.5f, 0}, {1.5f, 0, 0}, {1.5f, 0, 1.5f}
};

#define	MAX_AWAY		64
#define	RING_RADIUS		22			// just clear of an item's 32 wide box
#define	RING_SPIN		100			// degrees a second, as the items turn
#define	FLOOR_REACH		256			// how far below an item its floor is looked for

// the items' boxes, as id's items.qc sets their sizes, all 32 wide: the
// artifacts' hang from their middle, a megahealth's box from its corner
static const vec3_t	item_mins[] = {
	{0, 0, 0}, {-16, -16, -24}, {-16, -16, -24}, {-16, -16, -24}, {-16, -16, -24}, {0, 0, 0},
	{-16, -16, 0}, {-16, -16, 0}, {-16, -16, 0}, {-16, -16, 0}, {-16, -16, 0}
};

typedef struct
{
	int		entity;
	item_t	item;
	double	taken;		// when its clock started
	double	due;		// when it is back; -1 not counting yet
} away_t;

static mark_t	*marks;
static int		nummarks, maxmarks;

static r_ring_t	rings[MAX_AWAY];

static cvar_t	demo_itemtimers = {.name = "demo_itemtimers", .string = "quad pent", .archive = true,
	.description = "The items taken in a KTX MVD or QTV stream listed beside the view, with the seconds until each "
		"is back, by name: quad, pent, ring, suit, mega, ra, ya, ga, rl, lg. Empty for none."};
static cvar_t	demo_itemrings = {.name = "demo_itemrings", .string = "1", .archive = true,
	.description = "Marks where an item was taken in a KTX MVD or QTV stream with a ring, lit as it nears return, "
		"and a faint ghost of it (demo_itemghosts).",
	.values = (const cvar_value_t[]){{"0", "Not marked"}, {"1", "Marked"}, {0}}};
static cvar_t	demo_itemghosts = {.name = "demo_itemghosts", .string = "0.20", .archive = true,
	.description = "How solid the ghost of an item taken is, under its ring (demo_itemrings): 0 none, to 1 solid."};

void CL_ItemsClear (void)
{
	nummarks = 0;
	r_scene.numrings = 0;
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
Items_Away

The newest word about each timed item up to now, the soonest back first and
one still held (a megahealth) last; whether each is back yet is the caller's
to decide
==================
*/
static int Items_Away (away_t *away, double now)
{
	away_t	t;
	int		i, j, n;

	for (i=n=0 ; i<nummarks && marks[i].time <= now ; i++)
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
			if (n == MAX_AWAY)
				continue;
			away[n].entity = marks[i].entity;
			away[n].item = Items_Kind (marks[i].entity);
			n++;
		}
		away[j].taken = marks[i].time;		// a timer starts the clock over
		away[j].due = marks[i].delay > 0 ? marks[i].time + marks[i].delay : -1;
	}

	for (i=j=0 ; i<n ; i++)
		if (away[i].item != ITEM_NONE)
			away[j++] = away[i];
	n = j;
	for (i=1 ; i<n ; i++)
		for (j=i ; j>0 && Items_Order (away[j].due) < Items_Order (away[j-1].due) ; j--)
		{
			t = away[j];
			away[j] = away[j-1];
			away[j-1] = t;
		}
	return n;
}

// whether the server sends the entity: a taken item isn't, until it is back
static bool Items_Present (int entity)
{
	const packet_entities_t	*pack = &cl.frames[cl.validsequence & UPDATE_MASK].packet_entities;
	int						i;

	for (i=0 ; i<pack->num_entities ; i++)
		if (pack->entities[i].number == entity)
			return true;
	return false;
}

/*
==================
Items_Drop

Where an item lies: QuakeC drops items to the floor (PlaceItem's
droptofloor) after the baseline is made, so a baseline's origin is where the
map put the item, in the air on some maps. As the server drops it: from 6 up,
its box down at most FLOOR_REACH through the player's hull (an item is 32
wide), the trace moved by the hull's offset from the box; one with no floor
under it stays.
==================
*/
static void Items_Drop (const entity_state_t *base, item_t item, vec3_t origin)
{
	const hull_t	*hull;
	trace_t			trace;
	vec3_t			offset, start, end;

	VectorCopy (base->origin, origin);
	if (!cl.clipmodels[1])
		return;
	hull = &cl.clipmodels[1]->hulls[1];
	VectorSubtract (hull->clip_mins, item_mins[item], offset);
	VectorSubtract (origin, offset, start);
	start[2] += 6;
	VectorCopy (start, end);
	end[2] -= FLOOR_REACH;
	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.allsolid = true;
	VectorCopy (end, trace.endpos);
	CM_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, start, end, &trace);
	if (!trace.allsolid && trace.fraction < 1)
		VectorAdd (trace.endpos, offset, origin);
}

// an item's ring: on the floor its box rests on, under its middle (a brush
// model's, the megahealth's, is the middle of its bounds)
static void Items_RingCentre (const entity_state_t *base, item_t item, const vec3_t origin, vec3_t centre)
{
	const model_t	*model = CL_Model (base->modelindex);

	VectorCopy (origin, centre);
	if (model && model->type == mod_brush)
	{
		centre[0] += (model->mins[0] + model->maxs[0]) * 0.5f;
		centre[1] += (model->mins[1] + model->maxs[1]) * 0.5f;
	}
	centre[2] += item_mins[item][2];
}

/*
==================
CL_LinkItems

Where an item is missing at the moment played: a ring on the floor, lit as
its return comes nearer and turning as the items do, and a ghost of the item
==================
*/
void CL_LinkItems (void)
{
	const entity_state_t	*base;
	away_t		away[MAX_AWAY];
	entity_t	*ent;
	r_ring_t	*ring;
	model_t		*model;
	vec3_t		origin;
	double		now = cl.time;
	float		ghost = fminf (fmaxf (demo_itemghosts.value, 0), 1);
	int			i, n;

	r_scene.rings = rings;
	r_scene.numrings = 0;
	if (!cls.mvdplayback || !demo_itemrings.value || cl.intermission)
		return;

	n = Items_Away (away, now);
	for (i=0 ; i<n ; i++)
	{
		if (Items_Present (away[i].entity))
			continue;
		base = &cl.baselines[away[i].entity];
		model = CL_Model (base->modelindex);
		if (!model)
			continue;

		Items_Drop (base, away[i].item, origin);
		ring = &rings[r_scene.numrings++];
		Items_RingCentre (base, away[i].item, origin, ring->centre);
		ring->radius = RING_RADIUS;
		ring->fill = away[i].due < 0 ? 0 : away[i].due <= away[i].taken ? 1
			: (float)((now - away[i].taken) / (away[i].due - away[i].taken));
		ring->phase = (float)(fmod (RING_SPIN * now, 360.0) * Q_PI / 180);
		VectorCopy (item_colors[away[i].item], ring->color);

		if (!ghost || cl.numvisedicts == MAX_VISEDICTS)
			continue;
		ent = &cl.visedicts[cl.numvisedicts++];
		memset (ent, 0, sizeof(*ent));
		ent->keynum = away[i].entity;
		ent->model = model;
		ent->alpha = (byte)fmaxf (fminf (ghost * 254, 254), 1);		// 0 is solid
		ent->skinnum = base->skinnum;
		ent->frame = base->frame;
		VectorCopy (origin, ent->origin);
		if (model->flags & EF_ROTATE)
			ent->angles[1] = anglemod ((float)(RING_SPIN * now));
		else
			VectorCopy (base->angles, ent->angles);
	}
}

/*
==================
Items_Listed

The items demo_itemtimers names, a bit each; "1", the switch it was before it
took names (and which config.cfg keeps), the ones it lists by default
==================
*/
static unsigned Items_Listed (void)
{
	const char	*s = demo_itemtimers.string;
	unsigned	listed = 0;
	size_t		len;
	int			i;

	if (!strcmp (s, "1"))
		s = demo_itemtimers.defaultstring;
	for ( ; *s ; s += len)
	{
		while (*s && (byte)*s <= ' ')
			s++;
		for (len = 0 ; (byte)s[len] > ' ' ; len++)
			;
		for (i=ITEM_QUAD ; i<(int)(sizeof(item_names)/sizeof(item_names[0])) ; i++)
			if (len == strlen (item_names[i]) && !Q_strncasecmp (s, item_names[i], len))
				listed |= 1u << i;
	}
	return listed;
}

/*
==================
CL_DrawItemTimers

Beside the view, the items demo_itemtimers names that are away at the moment
played, the soonest back first; one still held (a megahealth) last
==================
*/
void CL_DrawItemTimers (void)
{
	away_t		away[MAX_AWAY];
	char		num[16];
	double		now, left;
	unsigned	listed;
	int			i, j, n, x, y;

	if (!cls.mvdplayback || cls.state != ca_active || cl.intermission)
		return;
	listed = Items_Listed ();
	if (!listed)
		return;

	// the ones named, not back yet
	now = cl.time;
	n = Items_Away (away, now);
	for (i=j=0 ; i<n ; i++)
		if ((listed & (1u << away[i].item)) && (away[i].due < 0 || away[i].due > now))
			away[j++] = away[i];
	n = j;

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
	Cvar_RegisterVariable (&demo_itemrings);
	Cvar_RegisterVariable (&demo_itemghosts);
}
