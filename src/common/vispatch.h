#pragma once
// vispatch.h -- a map's visibility widened across its liquids, where its vis
// treated them as opaque (after qualia's patchvis)

#include "bspfile.h"

struct arena_s;

// why the calling thread's maps are widened: the main thread's as r_novis 2
// and attract mode say, which its maps follow on the fly; a loader's as its
// job does (attract mode's)
#define	VP_NOVIS	1
#define	VP_ATTRACT	2

void	BSP_WantVisPatch (int reason, bool want);
bool	BSP_VisPatchWanted (void);

// what widening a map takes of it, kept while it is loaded: from arena, NULL
// where there is nothing to do (no visibility, no liquid, or vis that already
// sees across every liquid surface, as most maps' since vis had a mode for it)
typedef struct vpsource_s	vpsource_t;

vpsource_t	*BSP_VisPatchSource (bspfile_t *bsp, struct arena_s *arena);

// The map's PVS decompressed from vis (its visibility lump as loaded),
// rowbytes a row, and widened so that every leaf sees what a line of sight
// crossing one liquid surface reaches; rows from arena, *leafrow the byte
// offset of each leaf's row in *rows, -1 for a leaf without one (it sees
// everything). False, with nothing allocated, if rowbytes is too few.
bool	BSP_PatchVis (const vpsource_t *src, const byte *vis, int vissize, struct arena_s *arena, int rowbytes,
	byte **rows, int **leafrow);
