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

#pragma once
// bspfile.h -- the map file formats: BSP29, BSP2 with 32 bit indices and
// float bounds, and the BSPX lumps that may follow either. bspfile.c reads a
// map and hands out the lumps whose layout differs between the two in one wide
// layout. (The rare 2PSB variant is not read.)

#include "q_types.h"

#define	MAX_MAP_HULLS		4

#define	BSPVERSION			29
#define	BSPVERSION_BSP2		(('B') | ('S' << 8) | ('P' << 16) | ('2' << 24))

typedef struct
{
	int		fileofs, filelen;
} lump_t;

#define	LUMP_ENTITIES	0
#define	LUMP_PLANES		1
#define	LUMP_TEXTURES	2
#define	LUMP_VERTEXES	3
#define	LUMP_VISIBILITY	4
#define	LUMP_NODES		5
#define	LUMP_TEXINFO	6
#define	LUMP_FACES		7
#define	LUMP_LIGHTING	8
#define	LUMP_CLIPNODES	9
#define	LUMP_LEAFS		10
#define	LUMP_MARKSURFACES 11
#define	LUMP_EDGES		12
#define	LUMP_SURFEDGES	13
#define	LUMP_MODELS		14

#define	HEADER_LUMPS	15

typedef struct
{
	int			version;
	lump_t		lumps[HEADER_LUMPS];
} dheader_t;

//
// on disk, the same in every version
//

typedef struct
{
	float		mins[3], maxs[3];
	float		origin[3];
	int			headnode[MAX_MAP_HULLS];
	int			visleafs;		// not including the solid leaf 0
	int			firstface, numfaces;
} dmodel_t;

typedef struct
{
	int			nummiptex;
	int			dataofs[4];		// [nummiptex]
} dmiptexlump_t;

#define	MIPLEVELS	4
typedef struct miptex_s
{
	char		name[16];
	unsigned	width, height;
	unsigned	offsets[MIPLEVELS];		// four mip maps stored
} miptex_t;

typedef struct
{
	float	point[3];
} dvertex_t;

// 0-2 are axial planes
#define	PLANE_X			0
#define	PLANE_Y			1
#define	PLANE_Z			2

// 3-5 are non-axial planes snapped to the nearest
#define	PLANE_ANYX		3
#define	PLANE_ANYY		4
#define	PLANE_ANYZ		5

typedef struct
{
	float	normal[3];
	float	dist;
	int		type;		// PLANE_X - PLANE_ANYZ
} dplane_t;

#define	CONTENTS_EMPTY		-1
#define	CONTENTS_SOLID		-2
#define	CONTENTS_WATER		-3
#define	CONTENTS_SLIME		-4
#define	CONTENTS_LAVA		-5
#define	CONTENTS_SKY		-6

typedef struct texinfo_s
{
	float		vecs[2][4];		// [s/t][xyz offset]
	int			miptex;
	int			flags;
} texinfo_t;
#define	TEX_SPECIAL		1		// sky or slime, no lightmap or 256 subdivision

#define	MAXLIGHTMAPS	4

#define	AMBIENT_WATER	0
#define	AMBIENT_SKY		1
#define	AMBIENT_SLIME	2
#define	AMBIENT_LAVA	3

#define	NUM_AMBIENTS			4		// automatic ambient sounds

//
// on disk, BSP29: 16 bit indices and bounds
//

typedef struct
{
	int				planenum;
	short			children[2];	// negative numbers are -(leafs+1), not nodes
	short			mins[3];		// for sphere culling
	short			maxs[3];
	unsigned short	firstface;
	unsigned short	numfaces;		// counting both sides
} dnode_t;

typedef struct
{
	int			planenum;
	short		children[2];	// negative numbers are contents
} dclipnode_t;

// note that edge 0 is never used, because negative edge nums are used for
// counterclockwise use of the edge in a face
typedef struct
{
	unsigned short	v[2];		// vertex numbers
} dedge_t;

typedef struct
{
	short		planenum;
	short		side;
	int			firstedge;
	short		numedges;
	short		texinfo;
	byte		styles[MAXLIGHTMAPS];
	int			lightofs;		// start of [numstyles*surfsize] samples
} dface_t;

// leaf 0 is the generic CONTENTS_SOLID leaf, used for all solid areas
// all other leafs need visibility info
typedef struct
{
	int				contents;
	int				visofs;			// -1 = no visibility info
	short			mins[3];		// for frustum culling
	short			maxs[3];
	unsigned short	firstmarksurface;
	unsigned short	nummarksurfaces;
	byte			ambient_level[NUM_AMBIENTS];
} dleaf_t;

//
// on disk, BSP2: 32 bit indices, float bounds
//

typedef struct
{
	int			planenum;
	int			children[2];
	float		mins[3];
	float		maxs[3];
	unsigned	firstface;
	unsigned	numfaces;
} dnode_bsp2_t;

typedef struct
{
	int			planenum;
	int			children[2];
} dclipnode_bsp2_t;

typedef struct
{
	unsigned	v[2];
} dedge_bsp2_t;

typedef struct
{
	int			planenum;
	int			side;
	int			firstedge;
	int			numedges;
	int			texinfo;
	byte		styles[MAXLIGHTMAPS];
	int			lightofs;
} dface_bsp2_t;

typedef struct
{
	int			contents;
	int			visofs;
	float		mins[3];
	float		maxs[3];
	unsigned	firstmarksurface;
	unsigned	nummarksurfaces;
	byte		ambient_level[NUM_AMBIENTS];
} dleaf_bsp2_t;

//
// BSPX: a directory after the last lump, 4 byte aligned: "BSPX", a count, then
// 24 byte names with offsets and lengths
//

// DECOUPLED_LM: for every face, its lightmap's size in luxels, its first
// sample, and the projection from world space to luxels (luxel i at i)
typedef struct
{
	unsigned short	lmwidth, lmheight;
	int				lightofs;
	float			vecs[2][4];
} dlminfo_t;

static_assert (sizeof(dheader_t) == 124, "dheader_t");
static_assert (sizeof(dmodel_t) == 64, "dmodel_t");
static_assert (sizeof(miptex_t) == 40, "miptex_t");
static_assert (sizeof(dplane_t) == 20, "dplane_t");
static_assert (sizeof(texinfo_t) == 40, "texinfo_t");
static_assert (sizeof(dnode_t) == 24 && sizeof(dnode_bsp2_t) == 44, "dnode");
static_assert (sizeof(dclipnode_t) == 8 && sizeof(dclipnode_bsp2_t) == 12, "dclipnode");
static_assert (sizeof(dedge_t) == 4 && sizeof(dedge_bsp2_t) == 8, "dedge");
static_assert (sizeof(dface_t) == 20 && sizeof(dface_bsp2_t) == 28, "dface");
static_assert (sizeof(dleaf_t) == 28 && sizeof(dleaf_bsp2_t) == 44, "dleaf");
static_assert (sizeof(dlminfo_t) == 40, "dlminfo_t");

//
// the lumps that differ between versions, in the widest layout
//

typedef struct
{
	int			planenum;
	int			children[2];	// nodes, or -(leaf+1)
	float		mins[3], maxs[3];
	unsigned	firstface, numfaces;
} bspnode_t;

typedef struct
{
	int			planenum;
	int			children[2];	// clipnodes, or negative contents
} bspclipnode_t;

typedef struct
{
	unsigned	v[2];
} bspedge_t;

typedef struct
{
	int			planenum;
	int			side;
	int			firstedge;
	int			numedges;
	int			texinfo;
	byte		styles[MAXLIGHTMAPS];
	int			lightofs;
} bspface_t;

typedef struct
{
	int			contents;
	int			visofs;
	float		mins[3], maxs[3];
	unsigned	firstmarksurface, nummarksurfaces;
	byte		ambient_level[NUM_AMBIENTS];
} bspleaf_t;

// a map file being read; data stays owned by the caller
typedef struct
{
	char		name[64];
	const byte	*data;
	int			size;
	int			version;		// BSPVERSION or BSPVERSION_BSP2
	lump_t		lumps[HEADER_LUMPS];
	char		error[160];		// why the last call failed
} bspfile_t;

// checks the header and that every lump lies within the file
bool	BSP_Open (bspfile_t *bsp, const char *name, const byte *data, int size);

// a lump of whole elements of elemsize bytes, read in place (possibly unaligned)
bool	BSP_Lump (bspfile_t *bsp, int lump, size_t elemsize, const byte **data, int *count);

// the version-dependent lumps in the wide layout, with Mem_Free'd arrays;
// NULL (with bsp->error) if malformed. Node and clipnode children are checked:
// a child always comes after its parent, which also rules out cycles.
bspnode_t		*BSP_Nodes (bspfile_t *bsp, int numleafs, int *count);
bspclipnode_t	*BSP_Clipnodes (bspfile_t *bsp, int *count);
bspedge_t		*BSP_Edges (bspfile_t *bsp, int *count);
bspface_t		*BSP_Faces (bspfile_t *bsp, int *count);
bspleaf_t		*BSP_Leafs (bspfile_t *bsp, int *count);
unsigned		*BSP_Marksurfaces (bspfile_t *bsp, int *count);

// a BSPX lump by name; NULL if the map has none
const byte	*BSP_FindBSPXLump (const bspfile_t *bsp, const char *name, int *size);

// records why loading failed; returns false
bool	BSP_Fail (bspfile_t *bsp, const char *fmt, ...);
