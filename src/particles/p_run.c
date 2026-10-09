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
// p_run.c -- the particles run each frame: moved, changed by their ramps,
// emitting, hitting walls, dying; and what is left of them listed for the
// renderer (r_partscene_t) in batches of a look each, the darkening ones
// first, then the blending ones, then the adding ones, as QuakeSpasm-Spiked
// draws them.

#include "p_local.h"

#include <limits.h>

static r_part_t			*p_sceneparts;
static int				p_numsceneparts, p_maxsceneparts;
static r_partvert_t		*p_sceneverts;
static int				p_numsceneverts, p_maxsceneverts;
static r_partbatch_t	*p_batches, *p_ordered;
static int				p_numbatches, p_maxbatches;
static r_partscene_t	p_scene;

/*
==============================================================================

THE SCENE

==============================================================================
*/

static void *P_Grow (void *array, int *max, int need, size_t size)
{
	if (need <= *max)
		return array;
	*max = need > *max * 2 ? need : *max * 2;
	return Mem_Realloc (array, (size_t)*max * size);
}

// the batch for count more parts (or decal verts) of a look: the last one if
// it looks the same, as its parts are the last
static r_partbatch_t *P_Batch (const p_looks_t *looks, r_parttype_t type, int count)
{
	r_partbatch_t	*b;

	if (type == RPT_DECAL)
		p_sceneverts = P_Grow (p_sceneverts, &p_maxsceneverts, p_numsceneverts + count, sizeof(*p_sceneverts));
	else
		p_sceneparts = P_Grow (p_sceneparts, &p_maxsceneparts, p_numsceneparts + count, sizeof(*p_sceneparts));
	if (p_numbatches)
	{
		b = &p_batches[p_numbatches-1];
		if (b->type == type && b->blend == looks->blendmode && b->premul == looks->premul && b->image == looks->image
			&& b->scalefactor == looks->scalefactor && b->invscalefactor == looks->invscalefactor
			&& b->stretch == looks->stretch && b->minstretch == looks->minstretch)
			return b;
	}
	p_batches = P_Grow (p_batches, &p_maxbatches, p_numbatches + 1, sizeof(*p_batches));
	b = &p_batches[p_numbatches++];
	b->type = type;
	b->blend = looks->blendmode;
	b->premul = looks->premul;
	b->image = looks->image;
	b->scalefactor = looks->scalefactor;
	b->invscalefactor = looks->invscalefactor;
	b->stretch = looks->stretch;
	b->minstretch = looks->minstretch;
	b->first = type == RPT_DECAL ? p_numsceneverts : p_numsceneparts;
	b->count = 0;
	return b;
}

// a particle drawn as its type's shape
static void P_DrawParticle (const p_type_t *t, const p_particle_t *p)
{
	static const int	shapes[] = {RPT_SPRITE, RPT_SPARK, RPT_FAN, RPT_TSPARK, -1, -1, RPT_UDECAL, -1};
	r_partbatch_t		*b;
	r_part_t			*rp;

	if (shapes[t->looks.type] < 0)
		return;
	b = P_Batch (&t->looks, (r_parttype_t)shapes[t->looks.type], 1);
	rp = &p_sceneparts[p_numsceneparts++];
	b->count++;
	VectorCopy (p->org, rp->org);
	VectorCopy (p->vel, rp->vel);
	rp->scale = p->scale;
	rp->angle = p->angle;
	memcpy (rp->rgba, p->rgba, sizeof(rp->rgba));
	rp->st[0] = p->s1;
	rp->st[1] = p->t1;
	rp->st[2] = p->s2;
	rp->st[3] = p->t2;
}

// a quad from beam segment b's particle to the next's
static void P_DrawBeam (const p_type_t *t, const p_beamseg_t *b)
{
	const p_beamseg_t	*c = b->next;
	const p_particle_t	*p = b->p, *q = c->p, *ends[2];
	const p_beamseg_t	*segs[2];
	r_partbatch_t		*rb;
	r_part_t			*rp;
	int					i;

	if (!q || t->looks.type != PT_BEAM)
		return;
	ends[0] = q;
	ends[1] = p;
	segs[0] = c;
	segs[1] = b;
	rb = P_Batch (&t->looks, RPT_BEAM, 2);
	for (i = 0 ; i < 2 ; i++)
	{
		rp = &p_sceneparts[p_numsceneparts++];
		VectorCopy (ends[i]->org, rp->org);
		VectorCopy (segs[i]->dir, rp->vel);
		rp->scale = ends[i]->scale;
		rp->angle = 0;
		memcpy (rp->rgba, ends[i]->rgba, sizeof(rp->rgba));
		rp->st[0] = segs[i]->texture_s * ends[i]->angle + p_time * ends[i]->rotationspeed;
		rp->st[1] = p->t1;
		rp->st[2] = rp->st[0];
		rp->st[3] = p->t2;
	}
	rb->count += 2;
}

// a decal's triangle, where its entity is now; false if that has gone or turns
static bool P_DrawDecal (const p_type_t *t, const p_decal_t *d)
{
	r_partbatch_t	*b;
	r_partvert_t	*v;
	vec3_t			origin = {0, 0, 0};
	int				i;

	if (d->entity && (!p_host.brushentity || !p_host.brushentity (d->entity, origin)))
		return false;
	b = P_Batch (&t->looks, RPT_DECAL, 3);
	for (i = 0 ; i < 3 ; i++)
	{
		v = &p_sceneverts[p_numsceneverts++];
		VectorAdd (d->vertex[i], origin, v->xyz);
		v->st[0] = d->texcoords[i][0];
		v->st[1] = d->texcoords[i][1];
		memcpy (v->rgba, d->rgba, sizeof(v->rgba));
		v->rgba[3] *= d->valpha[i];
	}
	b->count += 3;
	return true;
}

// the batches in the order they blend: darkening, blending, adding
static void P_OrderBatches (void)
{
	static const int	pass[] =
	{
		[RPB_BLEND] = 1, [RPB_BLENDCOLOR] = 1, [RPB_ADDA] = 2, [RPB_ADDC] = 2,
		[RPB_SUBTRACT] = 0, [RPB_INVMODA] = 0, [RPB_INVMODC] = 0, [RPB_PREMUL] = 2,
	};
	int		o, i, n = 0;

	p_ordered = Mem_Realloc (p_ordered, sizeof(*p_ordered) * (size_t)(p_maxbatches > 1 ? p_maxbatches : 1));
	for (o = 0 ; o < 3 ; o++)
		for (i = 0 ; i < p_numbatches ; i++)
			if (pass[p_batches[i].blend] == o && p_batches[i].count)
				p_ordered[n++] = p_batches[i];
	p_scene.batches = p_ordered;
	p_scene.numbatches = n;
	p_scene.parts = p_sceneparts;
	p_scene.verts = p_sceneverts;
}

/*
==============================================================================

RUNNING

==============================================================================
*/

// a particle's or decal's color, alpha and size after ft more seconds, by
// the type's ramp, or its changes a second; age is how long it has lasted
static void P_Ramp (const p_type_t *t, float age, float ft, float *rgba, float *scale)
{
	const p_ramp_t	*a, *b;
	float			at = t->rampindexes * (t->die > 0 ? age / t->die : 1), frac;
	int				i, c;

	i = (int)at;
	frac = at - (float)i;
	if (i < 0)
	{
		i = 0;
		frac = 0;
	}
	if (i >= t->rampindexes)
		i = t->rampindexes - 1;
	switch (t->rampmode)
	{
	case RAMP_NEAREST:
		a = &t->ramp[i];
		VectorCopy (a->rgb, rgba);
		rgba[3] = a->alpha;
		if (scale)
			*scale = a->scale;
		break;
	case RAMP_LERP:
		a = &t->ramp[i];
		b = &t->ramp[i + 1 < t->rampindexes ? i + 1 : i];
		for (c = 0 ; c < 3 ; c++)
			rgba[c] = a->rgb[c] + (b->rgb[c] - a->rgb[c]) * frac;
		rgba[3] = a->alpha + (b->alpha - a->alpha) * frac;
		if (scale)
			*scale = a->scale + (b->scale - a->scale) * frac;
		break;
	case RAMP_DELTA:
		a = &t->ramp[i];
		for (c = 0 ; c < 3 ; c++)
			rgba[c] += ft * a->rgb[c];
		rgba[3] -= ft * a->alpha;
		if (scale)
			*scale += ft * a->scale;
		break;
	case RAMP_NONE:
		if (age < t->rgbchangetime)
			for (c = 0 ; c < 3 ; c++)
				rgba[c] += ft * t->rgbchange[c];
		rgba[3] += ft * t->alphachange;
		if (scale)
			*scale += ft * t->scaledelta;
		break;
	}
}

// the type's decals that are left, changed and drawn
static void P_RunDecals (p_type_t *t, float ft)
{
	p_decal_t	**link, *d;

	for (link = &t->clippeddecals ; (d = *link) ; )
	{
		if (d->die < p_time || !P_DrawDecal (t, d))
		{
			*link = d->next;
			d->next = p_freedecals;
			p_freedecals = d;
			continue;
		}
		if (d->die - p_time <= t->die)
			P_Ramp (t, t->die - (d->die - p_time), ft, d->rgba, NULL);
		link = &d->next;
	}
}

// the dead of a chain of beam segments freed, from *link on
static void P_FreeDeadBeams (p_beamseg_t **link, bool lastsegs)
{
	p_beamseg_t	*b;

	while ((b = *link) && (b->flags & BS_DEAD) && (lastsegs || !(b->flags & BS_LASTSEG)))
	{
		*link = b->next;
		b->next = p_freebeams;
		p_freebeams = b;
	}
}

// a type whose particles last a frame: drawn, emitting once, and gone
static void P_RunInstant (p_type_t *t, p_particle_t **kill, p_particle_t **killfirst)
{
	p_particle_t	*p;
	p_beamseg_t		*b;
	vec3_t			d;

	while ((p = t->particles))
	{
		P_DrawParticle (t, p);
		if (t->emit >= 0 && t->emitstart <= 0)
			P_RunEffect (p->org, p->vel, 1, t->emit, NULL);
		t->particles = p->next;
		p->next = *kill;
		*kill = p;
		if (!*killfirst)
			*killfirst = p;
	}

	P_FreeDeadBeams (&t->beams, true);
	for (b = t->beams ; b ; b = b->next)
	{
		if (!(b->flags & BS_NODRAW))
		{
			// BS_NODRAW's absence means a next
			VectorSubtract (b->next->p->org, b->p->org, d);
			VectorCopy (d, b->next->dir);
			P_Normalize (b->next->dir);
			P_DrawBeam (t, b);
		}
		P_FreeDeadBeams (&b->next, true);
		b->flags |= BS_DEAD;
	}
}

/*
=================
P_RunParticles

A type's particles a frame on: dead ones to the kill list, the rest moved,
changed, emitting and hitting walls, and drawn; traces counts down what may
still look for walls
=================
*/
static void P_RunParticles (p_type_t *t, float ft, bool flurry, int *traces, p_particle_t **kill,
	p_particle_t **killfirst)
{
	p_particle_t	**link, *p;
	vec3_t			oldorg, stop, normal, friction;
	float			grav = t->gravity * ft, dist, count;
	int				i, e;

	for (i = 0 ; i < 3 ; i++)
		friction[i] = 1 - t->friction[i] * ft;
	for (link = &t->particles ; (p = *link) ; )
	{
		if (p->die < p_time)
		{
			if (t->emittime < 0)
				P_DelinkTrailstate (&p->state.trailstate);
			*link = p->next;
			p->next = *kill;
			*kill = p;
			if (!*killfirst)
				*killfirst = p;
			continue;
		}
		link = &p->next;

		VectorCopy (p->org, oldorg);
		if (t->flags & PT_VELOCITY)
		{
			VectorMA (p->org, ft, p->vel, p->org);
			p->vel[2] -= grav;
			if (t->flags & PT_FRICTION)
				for (i = 0 ; i < 3 ; i++)
					p->vel[i] *= friction[i];
			if (t->flurry && flurry)
			{
				p->vel[0] += P_CRandom () * t->flurry;
				p->vel[1] += P_CRandom () * t->flurry;
			}
		}
		p->angle += p->rotationspeed * ft;
		P_Ramp (t, t->die - (p->die - p_time), ft, p->rgba, &p->scale);

		if (t->emit >= 0)
		{
			if (t->emittime < 0)
				P_Trail (oldorg, p->org, t->emit, ft, 0, NULL, &p->state.trailstate);
			else if (p->state.nextemit < p_time)
			{
				p->state.nextemit = p_time + t->emittime + P_Random () * t->emitrand;
				P_RunEffect (p->org, p->vel, 1, t->emit, NULL);
			}
		}

		if (t->cliptype >= 0 && r_bouncysparks.value && p_host.trace)
		{
			VectorSubtract (p->org, p->oldorg, stop);
			if (!t->clipbounce || DotProduct (stop, stop) > 10 * 10)
			{
				if ((*traces)-- > 0 && p_host.trace (p->oldorg, p->org, stop, normal, &e) < 1)
				{
					if (t->clipbounce < 0)
					{
						// gone, as a mark where it hit with -2
						p->die = -1;
						if (t->clipbounce == -2)
							P_SplatDecal (t, stop, normal, e, p->scale);
						continue;
					}
					if (&p_types[t->cliptype] == t)
					{
						// bounces, until it is too slow to
						dist = -DotProduct (p->vel, normal) * t->clipbounce;
						VectorMA (p->vel, dist, normal, p->vel);
						VectorCopy (stop, p->org);
						if (!*t->texname && DotProduct (p->vel, p->vel) < 1000 * ft * 1000 * ft
							&& t->looks.type == PT_NORMAL)
						{
							p->die = -1;
							continue;
						}
					}
					else
					{
						// becomes the clip type's effect there
						p->die = -1;
						P_Normalize (p->vel);
						count = p_types[t->cliptype].count ? t->clipcount / p_types[t->cliptype].count : t->clipcount;
						if (t->clipbounce)
						{
							VectorScale (normal, t->clipbounce, normal);
							P_RunEffect (stop, normal, count, t->cliptype, NULL);
						}
						else
							P_RunEffect (stop, p->vel, count, t->cliptype, NULL);
						continue;
					}
				}
				VectorCopy (p->org, p->oldorg);
			}
		}
		P_DrawParticle (t, p);
	}
}

// a type's beam segments: the dead freed, the rest drawn to the next
static void P_RunBeams (p_type_t *t)
{
	p_beamseg_t	*b, *next;
	vec3_t		d;

	// leading dead ones, but not a trail's last
	while ((b = t->beams) && ((b->flags & BS_DEAD) || b->p->die < p_time) && !(b->flags & BS_LASTSEG))
	{
		t->beams = b->next;
		b->next = p_freebeams;
		p_freebeams = b;
	}
	for (b = t->beams ; b ; b = b->next)
	{
		if (!(next = b->next))
		{
			if (b->p->die < p_time)
				b->flags |= BS_DEAD;
			break;
		}
		if (b->flags & (BS_LASTSEG | BS_DEAD | BS_NODRAW))
		{
			P_FreeDeadBeams (&b->next, false);
			if (!b->next)
				continue;
		}
		else if (!(next->flags & BS_DEAD))
		{
			VectorSubtract (next->p->org, b->p->org, d);
			VectorCopy (d, next->dir);
			P_Normalize (next->dir);
			P_DrawBeam (t, b);
		}
		if (b->p->die < p_time)
			b->flags |= BS_DEAD;
	}
}

/*
=================
P_RunFrame

The particles a frame on, as far as the client's time has gone, and the list
of what to draw
=================
*/
const r_partscene_t *P_RunFrame (const p_frame_t *frame)
{
	static double	oldtime;
	static float	flurrytime;
	p_particle_t	*kill = NULL, *killfirst = NULL;
	p_type_t		**link, *t;
	float			ft;
	bool			flurry;
	int				traces, i;

	ft = (float)(frame->time - oldtime);
	ft = ft < 0 ? 0 : ft > 1 ? 1 : ft;
	oldtime = frame->time;
	p_clienttime = frame->time;
	p_realtime = frame->realtime;
	p_frametime = frame->frametime;
	p_numsceneparts = p_numsceneverts = p_numbatches = 0;
	if (!p_particles)
		return NULL;
	if (p_looksdirty)
		P_UpdateLooks ();
	P_RunSurfaceEffects (frame, ft);

	traces = r_particle_tracelimit.value >= (float)INT_MAX ? INT_MAX : (int)r_particle_tracelimit.value;
	flurrytime -= ft;
	flurry = flurrytime < 0;
	if (flurry)
		flurrytime = 0.1f + P_Random () * 0.3f;

	// none free: some of the oldest go, for new ones next frame
	if (!p_freedecals)
		for (i = 0 ; i < 256 ; i++)
		{
			p_decals[p_decalrecycle].die = -1;
			p_decalrecycle = (p_decalrecycle + 1) % p_numdecals;
		}
	if (!p_freeparticles)
		for (i = 0 ; i < 256 ; i++)
		{
			p_particles[p_particlerecycle].die = -1;
			p_particlerecycle = (p_particlerecycle + 1) % p_numparticles;
		}

	for (link = &p_runlist ; (t = *link) ; )
	{
		P_RunDecals (t, ft);
		if (!t->die)
			P_RunInstant (t, &kill, &killfirst);
		else
		{
			P_RunParticles (t, ft, flurry, &traces, &kill, &killfirst);
			P_RunBeams (t);
		}

		if (t->particles || t->beams || t->clippeddecals)
		{
			link = &t->nexttorun;
			continue;
		}
		// out of the list: types started while it ran are ahead of it
		while (*link != t)
			link = &(*link)->nexttorun;
		*link = t->nexttorun;
		t->nexttorun = NULL;
		t->inrunlist = false;
	}

	// freed only now, as beams look at their particles while they run
	if (kill)
	{
		killfirst->next = p_freeparticles;
		p_freeparticles = kill;
	}
	p_time += ft;

	P_OrderBatches ();
	return p_scene.numbatches ? &p_scene : NULL;
}
