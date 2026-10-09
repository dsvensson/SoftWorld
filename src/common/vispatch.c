// vispatch.c -- a map's visibility widened across its liquids, where its vis
// treated them as opaque; after qualia's patchvis (lib/bsp/src/patchvis.rs)
//
// qbsp's vis treats water as opaque: a leaf above the surface isn't told of
// the leafs below it. A liquid drawn see-through then shows nothing behind
// it. The sets are widened instead by what a line of sight crossing a
// surface once can reach. A leaf that sees an air leaf touching a liquid
// leaf (or is that liquid leaf) sees what the liquid leaf sees below the
// surface, the air leafs touching that, and what those see. That covers
// looking down into a pool, up out of it, and through it from high above
// into a room it hangs over. Every leaf told of another is told of back, as
// vis keeps it (A sees B, B sees A).
//
// Most maps need none of it: vis has long had a transparent-water mode and
// modern compilers default to it. Which map is which is what the renderer
// asks to draw liquids see-through (R_CheckLiquidVis): whether its liquids
// see open air (VP_LiquidsSeeAir). A map that does need it keeps its leafs
// (a vpsource_t) to be widened whenever it is asked to be, on the fly.
//
// Leafs touch when their boxes come within VP_EPS. Rows stay shared by the
// leafs whose compressed rows were the same (visofs): widening one widens
// its aliases, which errs the safe way for a PVS.
//
// qualia floods each body of liquid instead, and widens the body and the
// leafs touching it to all they see. A leaf seeing the body from further
// off is told of the body and its edge, not of what lies past it: start's
// pool shows the room under it from its edge but not from higher up. A line
// crossing two surfaces (through a pocket of air under the water) still
// isn't covered.

#include "arena.h"
#include "mem.h"
#include "print.h"
#include "q_endian.h"
#include "sys.h"
#include "vispatch.h"

#include <stdlib.h>
#include <string.h>

#define	VP_EPS		1.0f	// leafs whose boxes come this near touch

static thread_local int	vp_reasons;		// VP_NOVIS, VP_ATTRACT

void BSP_WantVisPatch (int reason, bool want)
{
	vp_reasons = want ? vp_reasons | reason : vp_reasons & ~reason;
}

bool BSP_VisPatchWanted (void)
{
	return vp_reasons != 0;
}

// what of a leaf it takes
typedef struct
{
	int			contents;
	int			visofs;
	float		mins[3], maxs[3];
} vpleaf_t;

struct vpsource_s
{
	char		*name;
	vpleaf_t	*leafs;
	int			numleafs;		// leaf 0 too
	int			visleafs;		// the world's with visibility: 1 .. visleafs
};

// a map's leafs with its visibility
typedef struct
{
	const vpleaf_t	*leafs;
	int				numleafs, visleafs;
	const byte		*vis;
	int				vissize;
} vpmap_t;

static bool VP_IsLiquid (int contents)
{
	return contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA;
}

// the compressed row at visofs into out (zeroed)
static void VP_Decompress (const vpmap_t *m, int visofs, byte *out)
{
	int		row = (m->visleafs + 7) >> 3, at = 0, i = visofs, run;

	while (at < row && i < m->vissize)
	{
		if (m->vis[i])
		{
			out[at++] = m->vis[i++];
			continue;
		}
		if (i + 1 >= m->vissize)
			break;
		run = m->vis[i + 1];
		at += run < row - at ? run : row - at;
		i += 2;
	}
}

/*
===============================================================================

WHETHER THE MAP NEEDS IT

===============================================================================
*/

static int VP_LiquidKind (int contents)
{
	return contents == CONTENTS_WATER ? 1 : contents == CONTENTS_SLIME ? 2 : contents == CONTENTS_LAVA ? 4 : 0;
}

/*
=================
VP_LiquidsSeeAir

Whether vis already sees across the map's liquids, as R_CheckLiquidVis asks
it: a liquid built opaque to vis sees none of the open air, one built
see-through does from its surface. So every kind of liquid the map has
(water, slime, lava) must have a leaf that sees an empty one; a leaf without
a row sees everything. The first leaf of a kind that does settles that kind.
=================
*/
static bool VP_LiquidsSeeAir (const vpmap_t *m)
{
	byte	*air, *row;
	int		l, k, kind, present = 0, seen = 0, rowbytes = (m->visleafs + 7) >> 3;

	air = Mem_Calloc ((size_t)rowbytes, 1);
	row = Mem_Alloc ((size_t)rowbytes);
	for (l = 1 ; l <= m->visleafs ; l++)
		if (m->leafs[l].contents == CONTENTS_EMPTY)
			air[(l - 1) >> 3] |= (byte)(1 << ((l - 1) & 7));

	for (l = 1 ; l <= m->visleafs ; l++)
	{
		kind = VP_LiquidKind (m->leafs[l].contents);
		present |= kind;
		if (!kind || (seen & kind))
			continue;
		if (m->leafs[l].visofs < 0 || m->leafs[l].visofs >= m->vissize)
		{
			seen |= kind;
			continue;
		}
		memset (row, 0, (size_t)rowbytes);
		VP_Decompress (m, m->leafs[l].visofs, row);
		for (k = 0 ; k < rowbytes ; k++)
			if (row[k] & air[k])
			{
				seen |= kind;
				break;
			}
	}
	Mem_Free (air);
	Mem_Free (row);
	return !(present & ~seen);
}

/*
===============================================================================

THE PATCH

===============================================================================
*/

typedef struct
{
	int		leaf;
	float	mins[3], maxs[3];
} vpproxy_t;

static int VP_CompareProxies (const void *a, const void *b)
{
	const vpproxy_t	*x = a, *y = b;

	return x->mins[0] < y->mins[0] ? -1 : x->mins[0] > y->mins[0];
}

static int VP_CompareInts (const void *a, const void *b)
{
	int		x = *(const int *)a, y = *(const int *)b;

	return x < y ? -1 : x > y;
}

// a growable list of ints
typedef struct
{
	int		*v;
	int		n, max;
} vplist_t;

static void VP_Add (vplist_t *list, int value)
{
	if (list->n == list->max)
	{
		list->max = list->max ? list->max * 2 : 64;
		list->v = Mem_Realloc (list->v, (size_t)list->max * sizeof(*list->v));
	}
	list->v[list->n++] = value;
}

// pairs (a, b) grouped by a (key 0) or by b (key 1): key k's partners are
// (*partners)[start[k] .. start[k + 1]], start returned
static int *VP_Group (const vplist_t *pairs, int key, int numkeys, int **partners)
{
	int		*start, *fill, i;

	start = Mem_Calloc ((size_t)numkeys + 1, sizeof(*start));
	for (i = 0 ; i < pairs->n ; i += 2)
		start[pairs->v[i + key] + 1]++;
	for (i = 0 ; i < numkeys ; i++)
		start[i + 1] += start[i];
	fill = Mem_Alloc (((size_t)numkeys + 1) * sizeof(*fill));
	memcpy (fill, start, ((size_t)numkeys + 1) * sizeof(*fill));
	*partners = Mem_Alloc (((size_t)pairs->n / 2 + 1) * sizeof(**partners));
	for (i = 0 ; i < pairs->n ; i += 2)
		(*partners)[fill[pairs->v[i + key]]++] = pairs->v[i + 1 - key];
	Mem_Free (fill);
	return start;
}

static bool VP_Bit (const byte *row, int leaf)
{
	return (row[(leaf - 1) >> 3] >> ((leaf - 1) & 7)) & 1;
}

static void VP_SetBit (byte *row, int leaf, int visleafs)
{
	if (leaf >= 1 && leaf <= visleafs)
		row[(leaf - 1) >> 3] |= (byte)(1 << ((leaf - 1) & 7));
}

static void VP_Or (byte *dst, const byte *src, int bytes)
{
	int		i;

	for (i = 0 ; i < bytes ; i++)
		dst[i] |= src[i];
}

vpsource_t *BSP_VisPatchSource (bspfile_t *bsp, arena_t *arena)
{
	vpsource_t	*src = NULL;
	vpmap_t		m = {0};
	bspleaf_t	*in;
	vpleaf_t	*leafs = NULL;
	const byte	*models;
	dmodel_t	world;
	int			nummodels, l, numliquid = 0;
	size_t		namesize;

	in = BSP_Leafs (bsp, &m.numleafs);
	if (!in || !BSP_Lump (bsp, LUMP_MODELS, sizeof(dmodel_t), &models, &nummodels) || nummodels < 1
		|| !BSP_Lump (bsp, LUMP_VISIBILITY, 1, &m.vis, &m.vissize) || !m.vissize)
		goto done;
	memcpy (&world, models, sizeof(world));
	m.visleafs = LittleLong (world.visleafs);
	if (m.visleafs > m.numleafs - 1)
		m.visleafs = m.numleafs - 1;
	if (m.visleafs <= 0)
		goto done;
	for (l = 1 ; l <= m.visleafs ; l++)
		numliquid += VP_IsLiquid (in[l].contents);
	if (!numliquid)
		goto done;

	leafs = Mem_Alloc ((size_t)m.numleafs * sizeof(*leafs));
	for (l = 0 ; l < m.numleafs ; l++)
	{
		leafs[l].contents = in[l].contents;
		leafs[l].visofs = in[l].visofs;
		memcpy (leafs[l].mins, in[l].mins, sizeof(leafs[l].mins));
		memcpy (leafs[l].maxs, in[l].maxs, sizeof(leafs[l].maxs));
	}
	m.leafs = leafs;
	if (VP_LiquidsSeeAir (&m))
		goto done;

	// kept with the map
	src = Arena_Alloc (arena, sizeof(*src));
	namesize = strlen (bsp->name) + 1;
	src->name = Arena_Alloc (arena, namesize);
	memcpy (src->name, bsp->name, namesize);
	src->leafs = Arena_Alloc (arena, (size_t)m.numleafs * sizeof(*src->leafs));
	memcpy (src->leafs, leafs, (size_t)m.numleafs * sizeof(*src->leafs));
	src->numleafs = m.numleafs;
	src->visleafs = m.visleafs;

done:
	Mem_Free (in);
	Mem_Free (leafs);
	return src;
}

bool BSP_PatchVis (const vpsource_t *src, const byte *vis, int vissize, arena_t *arena, int rowbytes, byte **rows,
	int **leafrow)
{
	vpmap_t		m = {.leafs = src->leafs, .numleafs = src->numleafs, .visleafs = src->visleafs, .vis = vis,
		.vissize = vissize};
	int			i, j, k, l, n, t, numrows, visbytes, numliquid = 0, numtouching = 0, numproxies = 0;
	int			*order = NULL, *slot = NULL, *liquid = NULL, *airstart = NULL, *air = NULL;
	int			*liquidstart = NULL, *liquids = NULL;
	vpproxy_t	*proxies = NULL;
	vplist_t	pairs = {0};
	bool		patched = false;
	byte		*gain = NULL, *seers = NULL, *touching = NULL, *row;
	double		start = 0;

	*rows = NULL;
	*leafrow = NULL;
	visbytes = (m.visleafs + 7) >> 3;
	if (rowbytes < visbytes)
		goto done;
	for (l = 1 ; l <= m.visleafs ; l++)
		numliquid += VP_IsLiquid (m.leafs[l].contents);

	// the rows, decompressed, one for each compressed row
	order = Mem_Alloc ((size_t)m.numleafs * sizeof(*order));
	for (l = 0, n = 0 ; l < m.numleafs ; l++)
		if (l > 0 && m.leafs[l].visofs >= 0 && m.leafs[l].visofs < m.vissize)
			order[n++] = m.leafs[l].visofs;
	qsort (order, (size_t)n, sizeof(*order), VP_CompareInts);
	for (i = 0, numrows = 0 ; i < n ; i++)
		if (!i || order[i] != order[i - 1])
			order[numrows++] = order[i];
	*rows = Arena_Alloc (arena, (size_t)(numrows ? numrows : 1) * (size_t)rowbytes);
	*leafrow = Arena_Alloc (arena, (size_t)m.numleafs * sizeof(**leafrow));
	for (i = 0 ; i < numrows ; i++)
		VP_Decompress (&m, order[i], *rows + (size_t)i * rowbytes);
	for (l = 0 ; l < m.numleafs ; l++)
	{
		(*leafrow)[l] = -1;
		if (l > 0 && m.leafs[l].visofs >= 0 && m.leafs[l].visofs < m.vissize)
		{
			i = (int)((int *)bsearch (&m.leafs[l].visofs, order, (size_t)numrows, sizeof(*order), VP_CompareInts) - order);
			(*leafrow)[l] = i * rowbytes;
		}
	}
	patched = true;
	start = Sys_DoubleTime ();

	// the air leafs each liquid leaf touches, by their boxes: a sweep along x
	// over the leafs that aren't solid (most of the map is, and can never
	// touch); air is all that isn't liquid, sky too
	slot = Mem_Alloc ((size_t)m.numleafs * sizeof(*slot));
	liquid = Mem_Alloc ((size_t)numliquid * sizeof(*liquid));
	proxies = Mem_Alloc ((size_t)m.numleafs * sizeof(*proxies));
	for (l = 0, n = 0 ; l < m.numleafs ; l++)
	{
		slot[l] = -1;
		if (l > 0 && l <= m.visleafs && VP_IsLiquid (m.leafs[l].contents))
		{
			liquid[n] = l;
			slot[l] = n++;
		}
		if (l > 0 && l <= m.visleafs && m.leafs[l].contents != CONTENTS_SOLID)
		{
			proxies[numproxies].leaf = l;
			memcpy (proxies[numproxies].mins, m.leafs[l].mins, sizeof(proxies[numproxies].mins));
			memcpy (proxies[numproxies].maxs, m.leafs[l].maxs, sizeof(proxies[numproxies].maxs));
			numproxies++;
		}
	}
	qsort (proxies, (size_t)numproxies, sizeof(*proxies), VP_CompareProxies);
	for (i = 0 ; i < numproxies ; i++)
	{
		const vpproxy_t	*a = &proxies[i];

		for (j = i + 1 ; j < numproxies ; j++)
		{
			const vpproxy_t	*b = &proxies[j];

			if (b->mins[0] > a->maxs[0] + VP_EPS)
				break;		// and so are all after it
			if ((slot[a->leaf] < 0) == (slot[b->leaf] < 0))
				continue;	// both liquid, or both air
			if (a->mins[1] - VP_EPS < b->maxs[1] && a->maxs[1] + VP_EPS > b->mins[1]
				&& a->mins[2] - VP_EPS < b->maxs[2] && a->maxs[2] + VP_EPS > b->mins[2])
			{
				VP_Add (&pairs, slot[a->leaf] >= 0 ? slot[a->leaf] : slot[b->leaf]);
				VP_Add (&pairs, slot[a->leaf] >= 0 ? b->leaf : a->leaf);
			}
		}
	}
	airstart = VP_Group (&pairs, 0, numliquid, &air);			// by liquid slot
	liquidstart = VP_Group (&pairs, 1, m.numleafs, &liquids);	// by air leaf

	// what is seen across each liquid leaf: what it sees below the surface,
	// the air leafs touching that, and what they see. Each liquid leaf's own
	// air leafs and what they see go in seers first, unused until later.
	gain = Mem_Calloc ((size_t)numliquid, (size_t)visbytes);
	seers = Mem_Calloc ((size_t)numliquid, (size_t)visbytes);
	for (i = 0 ; i < numliquid ; i++)
	{
		byte	*own = seers + (size_t)i * visbytes;

		VP_SetBit (own, liquid[i], m.visleafs);
		for (k = airstart[i] ; k < airstart[i + 1] ; k++)
		{
			VP_SetBit (own, air[k], m.visleafs);
			if ((row = (*leafrow)[air[k]] >= 0 ? *rows + (*leafrow)[air[k]] : NULL))
				VP_Or (own, row, visbytes);
		}
	}
	for (i = 0 ; i < numliquid ; i++)
	{
		byte	*across = gain + (size_t)i * visbytes;

		// a liquid leaf without a row is taken to see only itself
		row = (*leafrow)[liquid[i]] >= 0 ? *rows + (*leafrow)[liquid[i]] : NULL;
		if (row)
			VP_Or (across, row, visbytes);
		for (j = 0 ; j < numliquid ; j++)
			if (j == i || (row && VP_Bit (row, liquid[j])))
				VP_Or (across, seers + (size_t)j * visbytes, visbytes);
	}
	memset (seers, 0, (size_t)numliquid * visbytes);

	// who sees across each: the leafs seeing an air leaf touching it, or
	// being one, and the liquid leaf itself; a leaf without a row already
	// sees everything
	touching = Mem_Calloc ((size_t)visbytes, 1);
	for (l = 1 ; l <= m.visleafs ; l++)
		if (liquidstart[l] < liquidstart[l + 1])
		{
			VP_SetBit (touching, l, m.visleafs);
			numtouching++;
		}
	for (j = 1 ; j <= m.visleafs ; j++)
	{
		if ((*leafrow)[j] < 0)
			continue;
		row = *rows + (*leafrow)[j];
		for (k = 0 ; k < visbytes ; k++)
		{
			n = row[k] & touching[k];
			if (k == (j - 1) >> 3)
				n |= touching[k] & (1 << ((j - 1) & 7));
			for (l = k * 8 + 1 ; n ; l++, n >>= 1)
				if (n & 1)
					for (t = liquidstart[l] ; t < liquidstart[l + 1] ; t++)
						VP_SetBit (seers + (size_t)liquids[t] * visbytes, j, m.visleafs);
		}
		if (slot[j] >= 0)
			VP_SetBit (seers + (size_t)slot[j] * visbytes, j, m.visleafs);
	}

	// they see it, and are seen back by all it reaches
	for (i = 0 ; i < numliquid ; i++)
	{
		const byte	*across = gain + (size_t)i * visbytes, *seen = seers + (size_t)i * visbytes;

		for (l = 1 ; l <= m.visleafs ; l++)
		{
			if ((*leafrow)[l] < 0)
				continue;
			row = *rows + (*leafrow)[l];
			if (VP_Bit (seen, l))
				VP_Or (row, across, visbytes);
			if (VP_Bit (across, l))
				VP_Or (row, seen, visbytes);
		}
	}
	Con_DPrintf ("%s: vis doesn't see across its liquids: widened across %i liquid leafs and %i touching them "
		"(%.1f ms)\n", src->name, numliquid, numtouching, (Sys_DoubleTime () - start) * 1000);

done:
	Mem_Free (order);
	Mem_Free (slot);
	Mem_Free (liquid);
	Mem_Free (proxies);
	Mem_Free (pairs.v);
	Mem_Free (airstart);
	Mem_Free (air);
	Mem_Free (liquidstart);
	Mem_Free (liquids);
	Mem_Free (gain);
	Mem_Free (seers);
	Mem_Free (touching);
	return patched;
}
