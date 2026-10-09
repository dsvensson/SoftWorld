/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

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
// p_rain.c -- surfaces that make effects, as FTE's do: the world's faces of a
// texture with an effect tex_<name>, or the one the map's worldspawn names in
// a _texpart_<name> key, let it fall from them as much as their area, where
// the view can see them (rain from the sky). And DarkPlaces' weather (te_rain,
// te_snow) for games without a script of it.

#include "p_local.h"
#include "model.h"

// a triangle of a face making an effect
typedef struct
{
	vec3_t		org, x, y;		// a corner, and the edges from it
	vec3_t		normal;			// out of the face
	float		area;			// of the parallelogram of x and y, as FTE has it
	double		nexttime;		// when its next particle is due
	int			type;
	const mleaf_t	*leaf;		// in front of it: it makes its effect while that is in view
} p_skytri_t;

static p_skytri_t	*p_skytris;
static int			p_numskytris, p_maxskytris;
static double		p_skytime;
static struct model_s	*p_skyworld;	// what they are of
bool				p_skydirty = true;

// DarkPlaces' weather as FTE's weather script has it, for games without one
static const char	p_builtinweather[] =
	"r_part te_rain\n"
	"{\n"
	"	texture ball\n"
	"	scalefactor 1\n"
	"	count 1\n"
	"	alpha 0.1\n"
	"	rgb 255 255 255\n"
	"	die 2\n"
	"	veladd 1\n"
	"	scale 1\n"
	"	stretchfactor -40\n"
	"	type texturedspark\n"
	"	cliptype rainsplash\n"
	"	clipbounce 100\n"
	"	clipcount 5\n"
	"}\n"
	"r_part rainsplash\n"
	"{\n"
	"	randomvel 50 50\n"
	"	count 1\n"
	"	texture ball\n"
	"	scalefactor 1\n"
	"	alpha 0.1\n"
	"	rgb 255 255 255\n"
	"	die 0.4\n"
	"	scale 1\n"
	"	stretchfactor -2.5\n"
	"	veladd 1\n"
	"	type texturedspark\n"
	"	gravity 800\n"
	"}\n"
	"r_part te_snow\n"
	"{\n"
	"	texture ball\n"
	"	scalefactor 1\n"
	"	count 1\n"
	"	alpha 1\n"
	"	rgb 255 255 255\n"
	"	die 2\n"
	"	veladd 1\n"
	"	scale 5\n"
	"	flurry 40\n"
	"	gravity 400\n"
	"	friction 5\n"
	"	cliptype te_snow\n"		// settles where it lands
	"	clipbounce 0\n"
	"}\n";

/*
=================
P_WeatherType

te_<name>_<colour>, else te_<name>, else the built-in one of rain or snow
when no script has it; P_INVALID for none
=================
*/
int P_WeatherType (const char *name, int colour, bool *colored)
{
	int		type;

	*colored = true;
	if ((type = P_FindParticleType (va ("te_%s_%i", name, colour))) >= 0)
		return type;
	*colored = false;
	if ((type = P_FindParticleType (va ("te_%s", name))) >= 0)
		return type;
	if (strcmp (name, "rain") && strcmp (name, "snow"))
		return P_INVALID;
	P_LoadScriptText ("builtin", p_builtinweather);
	return P_FindParticleType (va ("builtin.te_%s", name));
}

// a triangle of a face, from a corner and its edges
static void P_AddSkyTri (const vec3_t a, const vec3_t b, const vec3_t c, const vec3_t normal, int type, model_t *world)
{
	p_skytri_t	*st;
	vec3_t		cross, mid;
	int			i;

	if (p_numskytris == p_maxskytris)
	{
		p_maxskytris = p_maxskytris ? p_maxskytris * 2 : 256;
		p_skytris = Mem_Realloc (p_skytris, sizeof(*p_skytris) * (size_t)p_maxskytris);
	}
	st = &p_skytris[p_numskytris];
	VectorCopy (a, st->org);
	VectorSubtract (b, a, st->x);
	VectorSubtract (c, a, st->y);
	P_CrossProduct (st->x, st->y, cross);
	st->area = sqrtf (DotProduct (cross, cross));
	if (st->area <= 0)
		return;
	VectorCopy (normal, st->normal);
	st->type = type;
	st->nexttime = p_skytime;
	for (i = 0 ; i < 3 ; i++)
		mid[i] = (a[i] + b[i] + c[i]) / 3 + normal[i];
	st->leaf = Mod_PointInLeaf (mid, world);
	p_numskytris++;
}

// a face's triangles, a fan of its edges' corners
static void P_AddSkyFace (model_t *world, const msurface_t *surf, int type)
{
	vec3_t	first = {0, 0, 0}, prev = {0, 0, 0}, v, normal;
	int		i, e;

	if (surf->flags & SURF_PLANEBACK)
		VectorScale (surf->plane->normal, -1, normal);
	else
		VectorCopy (surf->plane->normal, normal);
	for (i = 0 ; i < surf->numedges ; i++)
	{
		e = world->surfedges[surf->firstedge + i];
		e = e >= 0 ? (int)world->edges[e].v[0] : (int)world->edges[-e].v[1];
		VectorCopy (world->vertexes[e].position, v);
		if (i >= 2)
			P_AddSkyTri (first, prev, v, normal, type, world);
		if (!i)
			VectorCopy (v, first);
		VectorCopy (v, prev);
	}
}

/*
=================
P_BuildSurfaceEffects

The world's faces making effects: the worldspawn's _texpart_<texture> keys
name a texture's effect, else tex_<texture> is it
=================
*/
static void P_BuildSurfaceEffects (model_t *world)
{
	char		key[128], *data;
	int			*types, t, i;
	msurface_t	*surf;

	p_numskytris = 0;
	p_skydirty = false;
	p_skyworld = world;
	if (!world || world->type != mod_brush || !world->numtextures)
		return;
	types = Mem_Alloc (sizeof(*types) * (size_t)world->numtextures);
	for (t = 0 ; t < world->numtextures ; t++)
		types[t] = P_INVALID;

	data = COM_Parse (world->entities);
	if (data && com_token[0] == '{')
		while ((data = COM_Parse (data)) && com_token[0] != '}')
		{
			Q_strncpyz (key, com_token[0] == '_' ? com_token + 1 : com_token, sizeof(key));
			if (!(data = COM_Parse (data)))
				break;
			if (Q_strncasecmp (key, "texpart_", 8))
				continue;
			for (t = 0 ; t < world->numtextures ; t++)
				if (world->textures[t] && !Q_strcasecmp (key + 8, world->textures[t]->name))
					types[t] = P_FindParticleType (com_token);
		}

	for (t = 0 ; t < world->numtextures ; t++)
	{
		if (types[t] == P_INVALID && world->textures[t])
			types[t] = P_FindParticleType (va ("tex_%s", world->textures[t]->name));
		if (types[t] == P_INVALID)
			continue;
		surf = world->surfaces + world->firstmodelsurface;
		for (i = 0 ; i < world->nummodelsurfaces ; i++, surf++)
			if (surf->texinfo->texture == world->textures[t])
				P_AddSkyFace (world, surf, types[t]);
	}
	Mem_Free (types);
}

/*
=================
P_RunSurfaceEffects

The faces' particles over ft more seconds, each face's at a rate by its
area, falling from where it is in view; the nearer the view, the more of
them, as FTE thins them out further off
=================
*/
void P_RunSurfaceEffects (const p_frame_t *frame, float ft)
{
	p_skytri_t	*st;
	p_type_t	*t;
	vec3_t		org, d;
	float		x, y;
	int			i;

	if (frame->worldmodel != p_skyworld || p_skydirty)
		P_BuildSurfaceEffects (frame->worldmodel);
	if (!r_part_rain.value || r_part_rain_quantity.value <= 0)
		return;
	p_skytime += ft;
	for (i = 0, st = p_skytris ; i < p_numskytris ; i++, st++)
	{
		if (!st->leaf || st->leaf->visframe != frame->visframe)
		{
			st->nexttime = p_skytime;
			continue;
		}
		if (st->type < 0 || st->type >= p_numtypes || !p_types[st->type].loaded)
			continue;
		t = &p_types[st->type];
		while (st->nexttime < p_skytime)
		{
			if (!p_freeparticles)
				return;
			st->nexttime += 10000 / (st->area * r_part_rain_quantity.value * t->rainfrequency);
			x = P_Random () * P_Random ();
			y = P_Random () * (1 - x);
			P_VectorMA (st->org, x, st->x, org);
			VectorMA (org, y, st->y, org);
			VectorSubtract (org, frame->vieworg, d);
			if (sqrtf (DotProduct (d, d)) > (1024 + 512) * P_Random ())
				continue;
			VectorMA (org, 0.5f, st->normal, org);
			if (!(P_PointContents (org) & P_CONT_SOLID))
				P_RunEffect (org, st->normal, 1, st->type, NULL);
		}
	}
}
