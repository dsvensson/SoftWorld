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
// d_batch.c -- models kept to be drawn later, on the worker threads
//
// Between D_BeginBatch and D_EndBatch the models are put together as ever, in
// order, on this thread, but what they draw isn't drawn: each span or pixel of
// an alias model, and each span of a sprite, is kept in the strip of lines it
// falls in. D_FillBatch draws the strips on the workers, each what it was
// given in the order it was given it, and no pixel is in two strips, so every
// pixel ends as it would have if drawn at once: the depth tests, the overdraw
// and the ties alike. Nothing kept reads the frame until it is filled, so the
// models can be put together before what they're drawn over is.

#include "r_local.h"
#include "d_local.h"

// thin, as a view model is in few of the view's lines and Sys_Parallel hands
// the strips out as threads come free
#define D_STRIP_LINES	8

typedef enum
{
	DK_ALIASSPAN,
	DK_ALIASPIXEL,
	DK_SPRITESPAN
} dkind_t;

typedef struct
{
	pixel_t		*pdest;
	float		*pz;
	dkind_t		kind;
	union
	{
		struct
		{
			const byte	*ptex;
			int			sfrac, tfrac, light, zi, count;
			int			map;		// of the batch's alias maps
		} alias;
		struct
		{
			pixel_t		color;
			float		z;
		} pixel;
		struct
		{
			int			u, v, count;
			int			map;		// of the batch's sprite maps
		} sprite;
	};
} dkept_t;

typedef struct
{
	dkept_t		*kept;
	int			numkept, maxkept;
} dstrip_t;

typedef struct
{
	dstrip_t			*strips;
	int					*busy;			// the strips with something to draw
	int					numbusy;
	simd_aliasmap_t		*aliasmaps;
	int					numaliasmaps, maxaliasmaps;
	d_spritemap_t		*spritemaps;
	int					numspritemaps, maxspritemaps;
} dbatch_t;

static dbatch_t	d_batches[D_NUMBATCHES];
static int		d_numstrips;
static dbatch_t	*d_keeping;		// the batch being kept, NULL drawing at once

/*
================
D_SetBatchSize

The strips of the frame buffer's lines, height of them, empty
================
*/
void D_SetBatchSize (int height)
{
	dbatch_t	*b;
	int			i;

	for (b = d_batches ; b<d_batches + D_NUMBATCHES ; b++)
	{
		for (i=0 ; i<d_numstrips ; i++)
			Mem_Free (b->strips[i].kept);
		Mem_Free (b->strips);
		Mem_Free (b->busy);
	}
	d_numstrips = (height + D_STRIP_LINES - 1) / D_STRIP_LINES;
	for (b = d_batches ; b<d_batches + D_NUMBATCHES ; b++)
	{
		b->strips = Mem_Calloc ((size_t)d_numstrips, sizeof(*b->strips));
		b->busy = Mem_Alloc ((size_t)d_numstrips * sizeof(*b->busy));
		b->numbusy = 0;
		b->numaliasmaps = 0;
		b->numspritemaps = 0;
	}
}

/*
================
D_BeginBatch / D_EndBatch

What the models draw from here is kept in the batch, to its D_FillBatch
================
*/
void D_BeginBatch (dbatchid_t batch)
{
	d_keeping = d_numstrips ? &d_batches[batch] : NULL;
}

void D_EndBatch (void)
{
	d_keeping = NULL;
}

bool D_Keeping (void)
{
	return d_keeping != NULL;
}

/*
================
D_Keep

Room for what's kept on line v, at the end of its strip's
================
*/
static dkept_t *D_Keep (int v, dkind_t kind)
{
	dstrip_t	*s = &d_keeping->strips[v / D_STRIP_LINES];
	dkept_t		*k;

	if (!s->numkept)
		d_keeping->busy[d_keeping->numbusy++] = (int)(s - d_keeping->strips);
	if (s->numkept == s->maxkept)
	{
		s->maxkept = s->maxkept ? s->maxkept * 2 : 256;
		s->kept = Mem_Realloc (s->kept, (size_t)s->maxkept * sizeof(*s->kept));
	}
	k = &s->kept[s->numkept++];
	k->kind = kind;
	return k;
}

/*
================
D_KeepAliasMap / D_KeepSpriteMap

A triangle's or a sprite's mapping kept for its spans, by its index
================
*/
int D_KeepAliasMap (const simd_aliasmap_t *map)
{
	dbatch_t	*b = d_keeping;

	if (b->numaliasmaps == b->maxaliasmaps)
	{
		b->maxaliasmaps = b->maxaliasmaps ? b->maxaliasmaps * 2 : 256;
		b->aliasmaps = Mem_Realloc (b->aliasmaps, (size_t)b->maxaliasmaps * sizeof(*b->aliasmaps));
	}
	b->aliasmaps[b->numaliasmaps] = *map;
	return b->numaliasmaps++;
}

int D_KeepSpriteMap (const d_spritemap_t *map)
{
	dbatch_t	*b = d_keeping;

	if (b->numspritemaps == b->maxspritemaps)
	{
		b->maxspritemaps = b->maxspritemaps ? b->maxspritemaps * 2 : 16;
		b->spritemaps = Mem_Realloc (b->spritemaps, (size_t)b->maxspritemaps * sizeof(*b->spritemaps));
	}
	b->spritemaps[b->numspritemaps] = *map;
	return b->numspritemaps++;
}

/*
================
D_KeepAliasSpan / D_KeepAliasPixel / D_KeepSpriteSpan

What an alias model or a sprite would have drawn on line v
================
*/
void D_KeepAliasSpan (int v, pixel_t *pdest, float *pz, const byte *ptex, int sfrac, int tfrac, int light, int zi,
	int count, int map)
{
	dkept_t	*k = D_Keep (v, DK_ALIASSPAN);

	k->pdest = pdest;
	k->pz = pz;
	k->alias.ptex = ptex;
	k->alias.sfrac = sfrac;
	k->alias.tfrac = tfrac;
	k->alias.light = light;
	k->alias.zi = zi;
	k->alias.count = count;
	k->alias.map = map;
}

void D_KeepAliasPixel (int v, pixel_t *pdest, float *pz, pixel_t color, float z)
{
	dkept_t	*k = D_Keep (v, DK_ALIASPIXEL);

	k->pdest = pdest;
	k->pz = pz;
	k->pixel.color = color;
	k->pixel.z = z;
}

void D_KeepSpriteSpan (int u, int v, int count, int map)
{
	dkept_t	*k = D_Keep (v, DK_SPRITESPAN);

	k->sprite.u = u;
	k->sprite.v = v;
	k->sprite.count = count;
	k->sprite.map = map;
}

/*
================
D_FillStrip

A strip of a batch, on a worker thread, in the order it was kept
================
*/
static void D_FillStrip (void *ctx, int index)
{
	dbatch_t	*b = ctx;
	dstrip_t	*s = &b->strips[b->busy[index]];
	dkept_t		*k;
	int			i;

	for (i=0, k=s->kept ; i<s->numkept ; i++, k++)
	{
		switch (k->kind)
		{
		case DK_ALIASSPAN:
			simd_aliasspan (k->pdest, k->pz, k->alias.ptex, k->alias.sfrac, k->alias.tfrac, k->alias.light,
				k->alias.zi, k->alias.count, &b->aliasmaps[k->alias.map]);
			break;
		case DK_ALIASPIXEL:
			if (k->pixel.z >= *k->pz)
			{
				*k->pz = k->pixel.z;
				*k->pdest = k->pixel.color;
			}
			break;
		case DK_SPRITESPAN:
			D_SpriteSpan (&b->spritemaps[k->sprite.map], k->sprite.u, k->sprite.v, k->sprite.count);
			break;
		}
	}
	s->numkept = 0;
}

/*
================
D_FillBatch

What the batch has kept, drawn on the worker threads; it is empty after
================
*/
void D_FillBatch (dbatchid_t batch)
{
	dbatch_t	*b = &d_batches[batch];

	Sys_Parallel (b->numbusy, D_FillStrip, b);
	b->numbusy = 0;
	b->numaliasmaps = 0;
	b->numspritemaps = 0;
}
