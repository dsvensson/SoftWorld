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
// p_spawn.c -- effects started: at a point, along a trail, in a box, and id's
// of a palette color. Each type of an effect's chain makes its particles (or
// its decals, cut to the walls they land on), its light and its sound.

#include "p_local.h"

#define	NUMVERTEXNORMALS	162

static const float	p_vertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.inc"
};

p_particle_t	*p_particles, *p_freeparticles;
int				p_numparticles, p_particlerecycle;
p_beamseg_t		*p_beams, *p_freebeams;
int				p_numbeams;
p_decal_t		*p_decals, *p_freedecals;
int				p_numdecals, p_decalrecycle;
float			p_time;
double			p_clienttime, p_realtime;
float			p_frametime;

static p_trailstate_t	p_trailstates[P_MAXTRAILSTATES];
static int				p_tscycle;

/*
==============================================================================

PARTICLES AND TRAIL STATES

==============================================================================
*/

void P_AllocParticles (float maxparticles, float maxdecals)
{
	P_FreeParticles ();
	p_numparticles = maxparticles < 1 ? 1 : maxparticles > P_MAXPARTICLES ? P_MAXPARTICLES : (int)maxparticles;
	p_numdecals = maxdecals < 1 ? 1 : maxdecals > P_MAXDECALS ? P_MAXDECALS : (int)maxdecals;
	p_numbeams = P_MAXBEAMSEGS;
	p_particles = Mem_Calloc ((size_t)p_numparticles, sizeof(*p_particles));
	p_decals = Mem_Calloc ((size_t)p_numdecals, sizeof(*p_decals));
	p_beams = Mem_Calloc ((size_t)p_numbeams, sizeof(*p_beams));
	P_ClearParticles ();
}

void P_FreeParticles (void)
{
	Mem_Free (p_particles);
	Mem_Free (p_decals);
	Mem_Free (p_beams);
	p_particles = p_freeparticles = NULL;
	p_decals = p_freedecals = NULL;
	p_beams = p_freebeams = NULL;
	p_numparticles = p_numdecals = p_numbeams = 0;
}

/*
=================
P_ClearParticles

Every particle, beam and decal free, the run list empty and the trail states
forgotten; with r_part_maxparticles' and r_part_maxdecals' counts
=================
*/
void P_ClearParticles (void)
{
	int		i;

	if (p_particles && ((int)r_part_maxparticles.value != p_numparticles
		|| (int)r_part_maxdecals.value != p_numdecals))
	{
		P_AllocParticles (r_part_maxparticles.value, r_part_maxdecals.value);
		return;
	}
	if (!p_particles)
		return;
	for (i = 0 ; i < p_numparticles ; i++)
		p_particles[i].next = i + 1 < p_numparticles ? &p_particles[i+1] : NULL;
	p_freeparticles = p_particles;
	for (i = 0 ; i < p_numdecals ; i++)
		p_decals[i].next = i + 1 < p_numdecals ? &p_decals[i+1] : NULL;
	p_freedecals = p_decals;
	for (i = 0 ; i < p_numbeams ; i++)
	{
		p_beams[i].p = NULL;
		p_beams[i].flags = BS_DEAD;
		p_beams[i].next = i + 1 < p_numbeams ? &p_beams[i+1] : NULL;
	}
	p_freebeams = p_beams;
	p_particlerecycle = p_decalrecycle = 0;

	for (i = 0 ; i < p_numtypes ; i++)
	{
		p_types[i].particles = NULL;
		p_types[i].clippeddecals = NULL;
		p_types[i].beams = NULL;
		p_types[i].nexttorun = NULL;
		p_types[i].inrunlist = false;
	}
	p_runlist = NULL;
	P_ClearTrailStates ();
}

void P_ClearTrailStates (void)
{
	memset (p_trailstates, 0, sizeof(p_trailstates));
	p_tscycle = 0;
}

static void P_CleanTrailstate (p_trailstate_t *ts)
{
	// the trail's last segment may go now
	if (ts->lastbeam)
	{
		ts->lastbeam->flags &= ~BS_LASTSEG;
		ts->lastbeam->flags |= BS_NODRAW;
	}
	memset (ts, 0, sizeof(*ts));
}

// the trail state is let go, and its chain's
void P_DelinkTrailstate (p_trailstate_t **tsk)
{
	p_trailstate_t	*ts = *tsk, *next, **key;

	*tsk = NULL;
	if (!ts || ts->key != tsk)
		return;		// someone else's now
	while (ts)
	{
		next = ts->assoc;
		key = &ts->assoc;
		P_CleanTrailstate (ts);
		ts = next && next->key == key ? next : NULL;
	}
}

// the trail state *tsk has, or a new one when it has none or lost its one to
// someone else (the oldest are taken); NULL without tsk
static p_trailstate_t *P_TrailState (p_trailstate_t **tsk)
{
	p_trailstate_t	*ts;

	if (!tsk)
		return NULL;
	if (*tsk && (*tsk)->key == tsk)
		return *tsk;
	if (p_tscycle >= P_MAXTRAILSTATES)
		p_tscycle = 0;
	ts = &p_trailstates[p_tscycle++];
	P_CleanTrailstate (ts);
	ts->key = tsk;
	*tsk = ts;
	return ts;
}

void P_AddToRunList (p_type_t *t)
{
	if (t->inrunlist)
		return;
	t->nexttorun = p_runlist;
	p_runlist = t;
	t->inrunlist = true;
}

/*
=================
P_NewParticle

A particle of t: its color, size, angle, image and how long it lasts, as t
starts them; where it is and how it moves are the caller's. tracer counts a
tracer's particles, for id's colors (citracer).
=================
*/
static p_particle_t *P_NewParticle (p_type_t *t, int tracer)
{
	p_particle_t	*p = p_freeparticles;
	float			sync, offset;
	int				c, index;

	p_freeparticles = p->next;
	p->next = t->particles;
	t->particles = p;

	p->die = t->randdie * P_Random ();		// how much sooner it goes, for now
	p->scale = t->scale + t->scalerand * P_Random ();
	p->rgba[3] = (t->die ? t->alpha + p->die * t->alphachange : t->alpha) + t->alpharand * P_Random ();
	if (t->emittime < 0)
		p->state.trailstate = NULL;
	else
		p->state.nextemit = p_time + t->emitstart - p->die;
	p->rotationspeed = t->rotationmin + P_Random () * t->rotationrand;
	p->angle = t->rotationstartmin + P_Random () * t->rotationstartrand;

	p->s1 = t->s1;
	p->t1 = t->t1;
	p->s2 = t->s2;
	p->t2 = t->t2;
	if (t->randsmax != 1)
	{
		// one of randsmax images along, the next row past the end of one
		offset = t->texsstride * (rand () % t->randsmax);
		p->s1 += offset;
		p->s2 += offset;
		while (p->s1 >= 1)
		{
			p->s1 -= 1;
			p->s2 -= 1;
			p->t1 += t->texsstride;
			p->t2 += t->texsstride;
		}
	}

	if (t->colorindex >= 0)
	{
		index = t->colorindex + (t->colorrand > 0 ? rand () % t->colorrand : 0);
		if (t->flags & PT_CITRACER)
			index += (tracer & 4) << 1;
		if (index > 255)
			p->rgba[3] /= 2;		// Hexen II's translucency
		P_PaletteColor (index, p->rgba);
	}
	else
		VectorCopy (t->rgb, p->rgba);
	sync = P_Random ();
	for (c = 0 ; c < 3 ; c++)
		p->rgba[c] += (sync * t->rgbrandsync[c] + P_Random () * (1 - t->rgbrandsync[c])) * t->rgbrand[c]
			+ t->rgbchange[c] * p->die;
	return p;
}

// where it is from now: started, and how long it lasts
static void P_PlaceParticle (const p_type_t *t, p_particle_t *p)
{
	vec3_t	r;
	int		i;

	if (t->flags & PT_WORLDSPACERAND)
	{
		do
		{
			r[0] = P_CRandom ();
			r[1] = P_CRandom ();
			r[2] = P_CRandom ();
		} while (DotProduct (r, r) > 1);		// as DarkPlaces spreads them
		for (i = 0 ; i < 3 ; i++)
		{
			p->org[i] += r[i] * t->orgwrand[i];
			p->vel[i] += r[i] * t->velwrand[i] + t->velbias[i];
		}
	}
	VectorAdd (p->org, t->orgbias, p->org);
	p->die = p_time + t->die - p->die;
	VectorCopy (p->org, p->oldorg);
}

// a beam segment of p after *last (the first of them in *first), NULL if
// there are none free
static p_beamseg_t *P_NewBeamSeg (p_particle_t *p, p_beamseg_t **first, p_beamseg_t *last, float s)
{
	p_beamseg_t	*b = p_freebeams;

	p_freebeams = b->next;
	if (last)
		last->next = b;
	else
		*first = b;
	b->next = NULL;
	b->texture_s = s;
	b->flags = 0;
	b->p = p;
	VectorClear (b->dir);
	return b;
}

// whether the type makes nothing at org: out of its fluid, or in it
static bool P_FluidSkips (const p_type_t *t, const vec3_t org)
{
	unsigned	cont;

	if (!r_part_contentswitch.value || !(t->flags & (PT_TRUNDERWATER | PT_TROVERWATER)))
		return false;
	cont = P_PointContents (org);
	return ((t->flags & PT_TROVERWATER) && (cont & t->fluidmask))
		|| ((t->flags & PT_TRUNDERWATER) && !(cont & t->fluidmask));
}

// a type's light and sound
static void P_EffectSpawned (const p_type_t *t, const vec3_t org, int dlkey)
{
	static int	flickertime, flicker;
	float		radius, w, total;
	int			i;

	if ((t->dl_radius[0] || t->dl_radius[1]) && p_host.dlight)
	{
		i = (int)(p_realtime * 20);
		if (flickertime != i)
		{
			flickertime = i;
			flicker = rand ();
		}
		radius = t->dl_radius[0] + t->dl_radius[1]
			* (r_lightflicker.value ? ((flicker + dlkey * 2000) & 0xffff) * (1.0f / 0xffff) : 0.5f);
		p_host.dlight (dlkey, org, radius, t->dl_time, t->dl_decay, t->dl_rgb);
	}
	if (t->numsounds && p_host.sound)
	{
		// one of them, by weight
		for (i = 0, total = 0 ; i < t->numsounds ; i++)
			total += t->sounds[i].weight;
		w = P_Random () * total;
		for (i = 0, total = 0 ; i < t->numsounds ; i++)
		{
			total += t->sounds[i].weight;
			if (w <= total)
			{
				if (*t->sounds[i].name && t->sounds[i].vol > 0)
					p_host.sound (org, t->sounds[i].name, t->sounds[i].vol, t->sounds[i].atten);
				break;
			}
		}
	}
}

/*
==============================================================================

DECALS

==============================================================================
*/

typedef struct
{
	p_type_t	*type;
	int			entity;
	vec3_t		center, normal, tangent1, tangent2;
	float		scale0, scale1, scale2, bias1, bias2;
} p_decalctx_t;

// R_ClipDecal's triangles as decals of the type
static void P_AddDecals (void *vctx, const vec3_t *points, int numtris)
{
	p_decalctx_t	*ctx = vctx;
	p_type_t		*t = ctx->type;
	p_decal_t		*d;
	vec3_t			v;
	float			sync;
	int				i, c, index;

	for ( ; numtris > 0 && p_freedecals ; numtris--, points += 3)
	{
		d = p_freedecals;
		p_freedecals = d->next;
		d->next = t->clippeddecals;
		t->clippeddecals = d;

		for (i = 0 ; i < 3 ; i++)
		{
			VectorCopy (points[i], d->vertex[i]);
			VectorSubtract (d->vertex[i], ctx->center, v);
			d->texcoords[i][0] = DotProduct (v, ctx->tangent1) * ctx->scale1 + ctx->bias1;
			d->texcoords[i][1] = DotProduct (v, ctx->tangent2) * ctx->scale2 + ctx->bias2;
			// faded away from the centre's plane, unless only faces it faces have it
			d->valpha[i] = r_decal_noperpendicular.value ? 1 : 1 - fabsf (DotProduct (v, ctx->normal) * ctx->scale0);
		}
		d->entity = ctx->entity;
		d->die = t->randdie * P_Random ();
		d->rgba[3] = (t->die ? t->alpha + d->die * t->alphachange : t->alpha) + t->alpharand * P_Random ();
		if (t->colorindex >= 0)
		{
			index = t->colorindex + (t->colorrand > 0 ? rand () % t->colorrand : 0);
			if (index > 255)
				d->rgba[3] /= 2;
			P_PaletteColor (index, d->rgba);
		}
		else
			VectorCopy (t->rgb, d->rgba);
		sync = P_Random ();
		for (c = 0 ; c < 3 ; c++)
			d->rgba[c] += (sync * t->rgbrandsync[c] + P_Random () * (1 - t->rgbrandsync[c])) * t->rgbrand[c]
				+ t->rgbchange[c] * d->die;
		d->die = p_time + t->die - d->die;
		if (t->looks.type != PT_CDECAL)
			d->die += 20;
		P_AddToRunList (t);
	}
}

/*
=================
P_Decal

The type's decal size across at org, on the faces of entity entnum (0 the
world) facing along facing, turned at random; offset, one of its random images
=================
*/
static void P_Decal (p_type_t *t, const vec3_t org, const vec3_t facing, int entnum, float size, bool randomimage)
{
	static const vec3_t	up = {0.5f, 0.5f, 0.431f};
	struct model_s		*model;
	p_decalctx_t		ctx;
	vec3_t				origin = {0, 0, 0}, ref;
	float				angle, c, s;
	int					i;

	if (!p_freedecals || !p_host.brushentity || size <= 0)
		return;
	if (!(model = p_host.brushentity (entnum, origin)))
	{
		entnum = 0;
		VectorClear (origin);
		if (!(model = p_host.brushentity (0, origin)))
			return;
	}
	ctx.type = t;
	ctx.entity = entnum;
	VectorSubtract (org, origin, ctx.center);
	VectorSet (ctx.normal, -facing[0], -facing[1], -facing[2]);
	if (!P_Normalize (ctx.normal))
		return;

	// tangents at a random angle round the normal
	VectorCopy (up, ref);
	P_Normalize (ref);
	P_CrossProduct (ctx.normal, ref, ref);
	if (!P_Normalize (ref))
		ref[0] = 1;
	P_CrossProduct (ctx.normal, ref, ctx.tangent2);
	angle = (float)(P_Random () * 2 * Q_PI);
	c = cosf (angle);
	s = sinf (angle);
	for (i = 0 ; i < 3 ; i++)
		ctx.tangent2[i] = ref[i] * c + ctx.tangent2[i] * s;
	P_CrossProduct (ctx.normal, ctx.tangent2, ctx.tangent1);
	P_Normalize (ctx.tangent1);
	P_Normalize (ctx.tangent2);

	ctx.scale1 = t->s2 - t->s1;
	ctx.bias1 = t->s1 + ctx.scale1 / 2;
	ctx.scale2 = t->t2 - t->t1;
	ctx.bias2 = t->t1 + ctx.scale2 / 2;
	ctx.scale0 = 2 / size;
	ctx.scale1 /= size;
	ctx.scale2 /= size;
	if (randomimage && t->randsmax != 1)
		ctx.bias1 += t->texsstride * (rand () % t->randsmax);
	R_ClipDecal (model, ctx.center, ctx.normal, ctx.tangent2, ctx.tangent1, size, r_decal_noperpendicular.value != 0,
		P_AddDecals, &ctx);
}

// a decal type's effect at org: on the nearest wall within 16 units, or on
// what dir points into
static void P_DecalEffect (p_type_t *t, const vec3_t org, const vec3_t dir)
{
	vec3_t	start, end, hit, impact, normal, best = {0, 0.73f, 0.73f};
	float	frac, bestfrac = 1;
	int		i, entnum, bestent = 0;

	if (!p_host.trace)
		return;
	VectorCopy (org, impact);
	if (!dir || (!dir[0] && !dir[1] && !dir[2]))
	{
		P_Normalize (best);
		for (i = 0 ; i < 6 ; i++)
		{
			VectorClear (end);
			end[i % 3] = i < 3 ? -16.0f : 16.0f;
			VectorSubtract (org, end, start);
			VectorAdd (org, end, end);
			frac = p_host.trace (start, end, hit, normal, &entnum);
			if (frac < bestfrac)
			{
				bestfrac = frac;
				VectorCopy (normal, best);
				VectorCopy (hit, impact);
				bestent = entnum;
			}
		}
	}
	else
	{
		// onto the plane, as the network and collision leave it off it
		VectorSubtract (org, dir, start);
		VectorAdd (org, dir, end);
		p_host.trace (start, end, impact, normal, &bestent);
		VectorCopy (dir, best);
	}
	P_Decal (t, impact, best, bestent, t->scale + P_Random () * t->scalerand, true);
}

// a particle's decal where it hit a wall (clipbounce -2), as DarkPlaces' blood
void P_SplatDecal (p_type_t *t, const vec3_t org, const vec3_t normal, int entnum, float size)
{
	P_Decal (t, org, normal, entnum, size * (1.5f + P_Random () * 0.5f) * 0.5f, false);
}

/*
==============================================================================

EFFECTS AT A POINT

==============================================================================
*/

// a vector at right angles to the unit vector src
static void P_Perpendicular (const vec3_t src, vec3_t dst)
{
	float	least = 1, d;
	int		i, axis = 0;

	for (i = 0 ; i < 3 ; i++)
		if (fabsf (src[i]) < least)
		{
			least = fabsf (src[i]);
			axis = i;
		}
	VectorClear (dst);
	dst[axis] = 1;
	d = DotProduct (dst, src);
	for (i = 0 ; i < 3 ; i++)
		dst[i] -= d * src[i];
	P_Normalize (dst);
}

/*
=================
P_SpawnPoint

A type's particles at org, along the axes: count times its count, and what
the trail state owes; by its spawn mode
=================
*/
static void P_SpawnPoint (p_type_t *t, const vec3_t org, const vec3_t dir, vec3_t axis[3], float count,
	p_trailstate_t *ts)
{
	static float	avelocities[NUMVERTEXNORMALS][2];
	p_particle_t	*p;
	p_beamseg_t		*b = NULL, *bfirst = NULL;
	vec3_t			ofs, ars;
	float			pcount, m = 0, orgadd, veladd, rdist;
	int				i, j = 0, k = 0, l = 0, spawnspc = 8;

	pcount = t->countextra + r_part_density.value * count * (t->count + t->countrand * P_Random ());
	if ((t->flags & PT_INVFRAMETIME) && p_frametime > 0)
		pcount /= p_frametime;
	if (ts)
		pcount += ts->state2.emittime;

	switch (t->spawnmode)
	{
	case SM_UNICIRCLE:
		m = t->looks.type == PT_BEAM ? pcount - 1 : pcount;
		m = m < 1 ? 0 : (float)(2 * Q_PI) / m;
		if (t->spawnparam1)		// for odd shapes
			m *= t->spawnparam1;
		break;
	case SM_TELEBOX:
		spawnspc = 4;
		l = (int)-t->areaspreadvert;
		// fall through
	case SM_LAVASPLASH:
		j = k = (int)-t->areaspread;
		m = t->spawnparam1 ? t->spawnparam1 : 0.55752f;		// id's
		if (t->spawnparam2)
			spawnspc = (int)t->spawnparam2;
		spawnspc = spawnspc < 1 ? 1 : spawnspc;
		break;
	case SM_FIELD:
		if (!avelocities[0][0])
			for (i = 0 ; i < NUMVERTEXNORMALS ; i++)
			{
				avelocities[i][0] = (rand () & 255) * 0.01f;
				avelocities[i][1] = (rand () & 255) * 0.01f;
			}
		break;
	default:
		break;
	}

	if (t->spawntime && ts)
	{
		if (ts->state1.statetime > p_time)
			return;
		ts->state1.statetime = p_time + t->spawntime;
	}
	if (t->spawnchance < P_Random ())
		return;
	if (!t->die && t->count == 1 && t->countrand == 0 && pcount < 1)
		pcount = 1;		// countextra 1 and count 0, as they were meant

	for (i = 0 ; i < pcount && p_freeparticles ; i++)
	{
		if (t->looks.type == PT_BEAM && !p_freebeams)
			break;
		p = P_NewParticle (t, 0);
		if (t->looks.type == PT_BEAM)
			b = P_NewBeamSeg (p, &bfirst, b, (float)i);
		VectorClear (p->vel);

		switch (t->spawnmode)
		{
		case SM_BOX:
			ofs[0] = P_CRandom ();
			ofs[1] = P_CRandom ();
			ofs[2] = P_CRandom ();
			VectorSet (ars, ofs[0] * t->areaspread, ofs[1] * t->areaspread, ofs[2] * t->areaspreadvert);
			break;
		case SM_TELEBOX:
			VectorSet (ofs, (float)k, (float)j, (float)(l + 4));
			P_Normalize (ofs);
			VectorScale (ofs, 1 - P_Random () * m, ofs);
			VectorSet (ars, (float)(j + rand () % spawnspc), (float)(k + rand () % spawnspc),
				(float)(l + rand () % spawnspc));
			j += spawnspc;
			if (j >= t->areaspread)
			{
				j = (int)-t->areaspread;
				k += spawnspc;
				if (k >= t->areaspread)
				{
					k = (int)-t->areaspread;
					l += spawnspc;
					if (l >= t->areaspreadvert)
						l = (int)-t->areaspreadvert;
				}
			}
			break;
		case SM_LAVASPLASH:
			VectorSet (ofs, (float)(k + rand () % spawnspc), (float)(j + rand () % spawnspc), 256);
			VectorSet (ars, ofs[0], ofs[1], P_Random () * t->areaspreadvert);
			P_Normalize (ofs);
			VectorScale (ofs, 1 - P_Random () * m, ofs);
			j += spawnspc;
			if (j >= t->areaspread)
			{
				j = (int)-t->areaspread;
				k += spawnspc;
				if (k >= t->areaspread)
					k = (int)-t->areaspread;
			}
			break;
		case SM_UNICIRCLE:
			VectorSet (ofs, cosf (m * i), sinf (m * i), 0);
			VectorScale (ofs, t->areaspread, ars);
			break;
		case SM_FIELD:
			// id's bright field: a normal each, turning
			ars[0] = (float)(p_clienttime * avelocities[j][0]) + m;
			ars[1] = (float)(p_clienttime * avelocities[j][1]) + m;
			ars[2] = cosf (ars[1]);
			VectorSet (ofs, ars[2] * cosf (ars[0]), ars[2] * sinf (ars[0]), -sinf (ars[1]));
			orgadd = t->spawnparam2 * sinf ((float)p_clienttime + j + m);
			ars[0] = p_vertexnormals[j][0] * (t->areaspread + orgadd) + ofs[0] * t->spawnparam1;
			ars[1] = p_vertexnormals[j][1] * (t->areaspread + orgadd) + ofs[1] * t->spawnparam1;
			ars[2] = p_vertexnormals[j][2] * (t->areaspreadvert + orgadd) + ofs[2] * t->spawnparam1;
			P_Normalize (ofs);
			if (++j >= NUMVERTEXNORMALS)
			{
				j = 0;
				m += 0.1762891f;	// another round, elsewhere
			}
			break;
		case SM_DISTBALL:
			// a random direction: most of them near the middle
			rdist = t->spawnparam2 - P_CRandom () * (1 - P_CRandom () * t->spawnparam1);
			VectorSet (ofs, P_HRandom (), P_HRandom (), t->areaspreadvert ? P_HRandom () : 0);
			P_Normalize (ofs);
			VectorScale (ofs, rdist, ofs);
			VectorSet (ars, ofs[0] * t->areaspread, ofs[1] * t->areaspread, ofs[2] * t->areaspreadvert);
			break;
		default:		// SM_BALL, SM_CIRCLE
			VectorSet (ofs, P_HRandom (), P_HRandom (), t->areaspreadvert ? P_HRandom () : 0);
			P_Normalize (ofs);
			if (t->spawnmode != SM_CIRCLE)
				VectorScale (ofs, P_Random (), ofs);
			VectorSet (ars, ofs[0] * t->areaspread, ofs[1] * t->areaspread, ofs[2] * t->areaspreadvert);
			break;
		}

		orgadd = t->orgadd + P_Random () * t->randomorgadd;
		veladd = t->veladd + P_Random () * t->randomveladd;
		if (dir)
			veladd *= sqrtf (DotProduct (dir, dir));
		VectorScale (axis[0], ofs[0] * t->spawnvel, p->vel);
		VectorMA (p->vel, ofs[1] * t->spawnvel, axis[1], p->vel);
		VectorMA (p->vel, veladd + ofs[2] * t->spawnvelvert, axis[2], p->vel);
		P_VectorMA (org, ars[0], axis[0], p->org);
		VectorMA (p->org, ars[1], axis[1], p->org);
		VectorMA (p->org, orgadd + ars[2], axis[2], p->org);
		P_PlaceParticle (t, p);
	}

	if (b)
	{
		// a circle's first segment points from its last
		if (t->spawnmode == SM_UNICIRCLE)
		{
			VectorSet (ars, cosf (m * (i - 2)), sinf (m * (i - 2)), 0);
			VectorSubtract (b->p->org, ars, bfirst->dir);
			P_Normalize (bfirst->dir);
		}
		b->flags |= BS_NODRAW;
		b->next = t->beams;
		t->beams = bfirst;
	}
	if (ts)
		ts->state2.emittime = pcount - i;
	if (t->particles || t->clippeddecals)
		P_AddToRunList (t);
}

/*
=================
P_RunEffect

An effect at org, count times: each type of its chain in turn, in water
the type's inwater instead; dir points it (along the third axis, down
without), and its length scales veladd
=================
*/
bool P_RunEffect (const vec3_t org, const vec3_t dir, float count, int type, p_trailstate_t **tsk)
{
	p_type_t		*t;
	p_trailstate_t	*ts;
	vec3_t			axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};

	if (type < 0 || type >= p_numtypes || !p_types[type].loaded)
		return false;
	t = &p_types[type];
	if (r_part_contentswitch.value && t->inwater >= 0 && (P_PointContents (org) & P_CONT_FLUID))
		t = &p_types[t->inwater];
	if (dir && (dir[0] || dir[1] || dir[2]))
	{
		VectorCopy (dir, axis[2]);
		P_Normalize (axis[2]);
		P_Perpendicular (axis[2], axis[0]);
		P_CrossProduct (axis[2], axis[0], axis[1]);
		P_Normalize (axis[1]);
	}

	ts = t->flags & PT_NOSTATE ? NULL : P_TrailState (tsk);
	for (;;)
	{
		if (!P_FluidSkips (t, org))
		{
			P_EffectSpawned (t, org, 0);
			if (t->looks.type == PT_CDECAL)
				P_DecalEffect (t, org, dir);
			else
				P_SpawnPoint (t, org, dir, axis, count, ts);
		}
		if (t->assoc < 0)
			break;
		if (ts)
			ts = P_TrailState (&ts->assoc);
		t = &p_types[t->assoc];
	}
	return true;
}

bool P_RunEffectName (const vec3_t org, const vec3_t dir, float count, const char *name)
{
	return P_RunEffect (org, dir, count, P_FindParticleType (name), NULL);
}

// pe_<color>, else the type for count in the color's row of the palette
bool P_RunPaletteEffect (const vec3_t org, const vec3_t dir, int color, int count)
{
	int		type;

	if (P_RunEffect (org, dir, (float)count, P_FindParticleType (va ("pe_%i", color)), NULL))
		return true;
	type = count > 130 && pe_size3 >= 0 ? pe_size3 : count > 20 && pe_size2 >= 0 ? pe_size2 : pe_default;
	if (type < 0)
		return false;
	p_types[type].colorindex = color & ~7;
	p_types[type].colorrand = 8;
	return P_RunEffect (org, dir, (float)count, type, NULL);
}

/*
=================
P_RunWeather

count particles of te_<name>_<colour>, else te_<name> (in the colour) or
PE_DEFAULT, each somewhere in the box
=================
*/
void P_RunWeather (const vec3_t mins, const vec3_t maxs, const vec3_t dir, float count, int colour, const char *name)
{
	vec3_t	org;
	float	each;
	int		type, i, k;

	type = P_FindParticleType (va ("te_%s_%i", name, colour));
	if (type < 0)
	{
		if ((type = P_FindParticleType (va ("te_%s", name))) < 0)
			type = pe_default;
		if (type < 0)
			return;
		p_types[type].colorindex = colour;
	}
	if (!p_types[type].count)
		return;
	each = 1 / p_types[type].count;		// a particle each
	count *= p_types[type].count;
	for (i = 0 ; i < count && p_freeparticles ; i++)
	{
		for (k = 0 ; k < 3 ; k++)
			org[k] = mins[k] + P_Random () * (maxs[k] - mins[k]);
		P_RunEffect (org, dir, each, type, NULL);
	}
}

/*
==============================================================================

TRAILS

==============================================================================
*/

static void P_RightUp (const vec3_t forward, vec3_t right, vec3_t up)
{
	if (!forward[0] && !forward[1])
	{
		VectorSet (right, 0, forward[2] ? -1.0f : 0, 0);
	}
	else
	{
		VectorSet (right, forward[1], -forward[0], 0);
		P_Normalize (right);
	}
	P_CrossProduct (right, forward, up);
}

/*
=================
P_TrailSpawn

A type's particles from start to end: a particle each countspacing units,
else count a second of frametime; the trail state carries the spacing over
from where the last of the trail left off
=================
*/
static void P_TrailSpawn (const vec3_t startpos, const vec3_t end, p_type_t *t, float frametime,
	p_trailstate_t **tsk, int dlkey)
{
	p_trailstate_t	*ts;
	p_particle_t	*p;
	p_beamseg_t		*b = NULL, *bfirst = NULL;
	vec3_t			vec, vstep, right, up, start, ofs;
	float			len, step, stop, count = 0, veladd = -t->veladd, tdegree = (float)(2 * Q_PI / 256), sdegree = 0;
	float			nrfirst, nrlast, tavg, tc, ts1, rstep;
	int				tcount;
	bool			spread;

	VectorCopy (startpos, start);
	ts = t->flags & PT_NOSTATE ? NULL : P_TrailState (tsk);
	P_EffectSpawned (t, start, dlkey);
	if (t->assoc >= 0)
		P_Trail (start, end, t->assoc, frametime, dlkey, NULL, ts ? &ts->assoc : NULL);
	if (P_FluidSkips (t, startpos))
		return;

	if (t->spawntime && ts)
	{
		if (ts->state1.statetime > p_time)
			return;
		ts->state1.statetime = p_time + t->spawntime;
		ts = NULL;		// nor the length kept
	}
	if (t->spawnchance < P_Random ())
		return;
	if (!t->die)
		ts = NULL;

	VectorSubtract (end, start, vec);
	len = P_Normalize (vec);
	if (t->countspacing)
	{
		step = t->countspacing / r_part_density.value;
		if (t->countextra)
		{
			count = t->countextra + (step > 0 ? len / step : 0);
			step = len / count;
		}
	}
	else
	{
		// count a second, the part of a particle left over kept for the next
		step = t->count * r_part_density.value * frametime + t->countextra + t->countoverflow;
		count = (float)(int)step;
		t->countoverflow = step - count;
		if (count <= 0)
			return;
		step = len / count;
	}
	if (t->flags & PT_AVERAGETRAIL)
	{
		// the last at the end
		tavg = len / step;
		tavg /= ceilf (tavg);
		step *= tavg;
		len += step;
	}
	VectorScale (vec, step, vstep);

	if (t->spawnmode == SM_SPIRAL)
	{
		P_RightUp (vec, right, up);
		if (t->spawnparam1)
			tdegree = (float)(2 * Q_PI) / t->spawnparam1;	// units a turn
		sdegree = (float)(t->spawnparam2 * Q_PI / 180);
	}
	else if (t->spawnmode == SM_CIRCLE)
		P_RightUp (vec, right, up);

	if (ts)
	{
		ts->state2.laststop = stop = ts->state2.laststop + len;
		len = ts->state1.lastdist;
	}
	else
	{
		stop = len;
		len = 0;
	}
	nrfirst = t->flags & PT_NOSPREADFIRST ? len + step * 1.5f : len;
	nrlast = t->flags & PT_NOSPREADLAST ? stop : stop + step;

	if (len < stop && step > 0)
		count = (stop - len) / step;
	else
	{
		count = 0;
		step = 0;
		VectorClear (vstep);
	}

	while (count-- > 0)
	{
		len += step;
		if (!p_freeparticles || (t->looks.type == PT_BEAM && !p_freebeams))
		{
			len = stop;
			break;
		}
		tcount = (int)(len * t->count / (t->spawnparam1 ? t->spawnparam1 : 1));
		p = P_NewParticle (t, tcount);
		if (t->looks.type == PT_BEAM)
		{
			b = P_NewBeamSeg (p, &bfirst, b, len);
			VectorCopy (vec, b->dir);
		}
		VectorClear (p->vel);

		if (len < nrfirst || len >= nrlast)
		{
			// no spread for these
			VectorScale (vec, veladd, p->vel);
			VectorCopy (start, p->org);
		}
		else
		{
			spread = false;
			switch (t->spawnmode)
			{
			case SM_TRACER:
				// out to either side by turns
				tc = tcount & 1 ? 1.0f : -1.0f;
				VectorSet (p->vel, tc * vec[1] * t->spawnvel + vec[0] * veladd,
					-tc * vec[0] * t->spawnvel + vec[1] * veladd, vec[2] * veladd);
				VectorSet (p->org, start[0] + tc * vec[1] * t->areaspread, start[1] - tc * vec[0] * t->areaspread, start[2]);
				break;
			case SM_SPIRAL:
				tc = cosf (len * tdegree + sdegree);
				ts1 = sinf (len * tdegree + sdegree);
				P_VectorMA (start, tc * t->areaspread, right, p->org);
				VectorMA (p->org, ts1 * t->areaspread, up, p->org);
				VectorScale (vec, veladd, p->vel);
				VectorMA (p->vel, tc * t->spawnvel, right, p->vel);
				VectorMA (p->vel, ts1 * t->spawnvel, up, p->vel);
				break;
			case SM_BALL:
				VectorSet (ofs, P_CRandom (), P_CRandom (), P_CRandom ());
				P_Normalize (ofs);
				VectorScale (ofs, P_Random (), ofs);
				spread = true;
				break;
			case SM_CIRCLE:
				tc = cosf (len * tdegree);
				ts1 = sinf (len * tdegree);
				P_VectorMA (start, tc * t->areaspread, right, p->org);
				VectorMA (p->org, ts1 * t->areaspread, up, p->org);
				p->org[0] += vstep[0] * len * tdegree;
				p->org[1] += vstep[1] * len * tdegree;
				p->org[2] += vstep[2] * len * tdegree * 50;
				VectorScale (vec, veladd, p->vel);
				VectorMA (p->vel, tc * t->spawnvel, right, p->vel);
				VectorMA (p->vel, ts1 * t->spawnvel, up, p->vel);
				break;
			case SM_DISTBALL:
				// a random direction: most of them near the middle
				rstep = t->spawnparam2 - P_CRandom () * (1 - P_CRandom () * t->spawnparam1);
				VectorSet (ofs, P_CRandom (), P_CRandom (), P_CRandom ());
				P_Normalize (ofs);
				VectorScale (ofs, rstep, ofs);
				spread = true;
				break;
			default:
				VectorSet (ofs, P_CRandom (), P_CRandom (), P_CRandom ());
				spread = true;
				break;
			}
			if (spread)
			{
				// out from the trail along ofs
				VectorSet (p->vel, vec[0] * veladd + ofs[0] * t->spawnvel, vec[1] * veladd + ofs[1] * t->spawnvel,
					vec[2] * veladd + ofs[2] * t->spawnvelvert);
				VectorSet (p->org, start[0] + ofs[0] * t->areaspread, start[1] + ofs[1] * t->areaspread,
					start[2] + ofs[2] * t->areaspreadvert);
			}
			if (t->orgadd)
				VectorMA (p->org, t->orgadd, vec, p->org);
		}
		P_PlaceParticle (t, p);

		VectorAdd (start, vstep, start);
		if (t->countrand)
		{
			rstep = P_Random () / t->countrand;
			VectorMA (start, rstep, vec, start);
			step += rstep;
		}
	}

	if (ts)
	{
		ts->state1.lastdist = len;
		if (t->looks.type == PT_BEAM)
		{
			// the segments after the trail's last, to go on from
			if (b)
			{
				if (t->beams && ts->lastbeam)
				{
					b->next = ts->lastbeam->next;
					ts->lastbeam->next = bfirst;
					ts->lastbeam->flags &= ~BS_LASTSEG;
				}
				else
				{
					b->next = t->beams;
					t->beams = bfirst;
				}
				b->flags |= BS_LASTSEG;
				ts->lastbeam = b;
			}
			if ((!p_freeparticles || !p_freebeams) && ts->lastbeam)
			{
				ts->lastbeam->flags &= ~BS_LASTSEG;
				ts->lastbeam->flags |= BS_NODRAW;
				ts->lastbeam = NULL;
			}
		}
	}
	else if (b)
	{
		b->flags |= BS_NODRAW;
		b->next = t->beams;
		t->beams = bfirst;
	}
	P_AddToRunList (t);
}

/*
=================
P_Trail

An effect along a trail from start to end over frametime seconds; in water
the type's inwater instead. dlkey keys its lights; axis is unused, as models
aren't spawned.
=================
*/
bool P_Trail (const vec3_t start, const vec3_t end, int type, float frametime, int dlkey, const vec3_t axis[3],
	p_trailstate_t **tsk)
{
	p_type_t	*t;

	(void)axis;
	if (type < 0 || type >= p_numtypes || !p_types[type].loaded)
		return false;
	t = &p_types[type];
	if (r_part_contentswitch.value && t->inwater >= 0 && (P_PointContents (start) & P_CONT_FLUID))
		t = &p_types[t->inwater];
	P_TrailSpawn (start, end, t, frametime, tsk, dlkey);
	return true;
}
