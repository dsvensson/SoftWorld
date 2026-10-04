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
// d_init.c: rasterization driver initialization

#include "r_local.h"
#include "d_local.h"

#define NUM_MIPS	4

static cvar_t	d_mipcap = {.name = "d_mipcap", .string = "0",
	.description = "The most detailed mip level the world's textures are drawn at, 0 (full detail) to 3 (coarsest)."};
static cvar_t	d_mipscale = {.name = "d_mipscale", .string = "1",
	.description = "Multiplies the scales at which surfaces go to coarser mip levels; higher goes sooner, "
		"blurrier and faster."};

surfcache_t		*d_initial_rover;
bool		d_roverwrapped;
int				d_minmip;
float			d_scalemip[NUM_MIPS-1];

static float	basemip[NUM_MIPS-1] = {1.0f, 0.5f*0.8f, 0.25f*0.8f};

extern int			d_aflatcolor;



/*
===============
D_Init
===============
*/
void D_Init (void)
{

	r_skydirect = 1;

	Cvar_RegisterVariable (&d_mipcap);
	Cvar_RegisterVariable (&d_mipscale);

	r_recursiveaffinetriangles = true;
	r_aliasuvscale = 1.0;
}

/*
===============
D_SetupFrame
===============
*/
void D_SetupFrame (void)
{
	int		i;

	if (r_dowarp)
	{
		d_viewbuffer = r_warpbuffer;
		screenwidth = vid.width;
	}
	else
	{
		d_viewbuffer = vid.buffer;
		screenwidth = vid.rowpixels;
	}

	d_roverwrapped = false;
	d_initial_rover = sc_rover;

	d_minmip = (int)d_mipcap.value;
	if (d_minmip > 3)
		d_minmip = 3;
	else if (d_minmip < 0)
		d_minmip = 0;

	for (i=0 ; i<(NUM_MIPS-1) ; i++)
		d_scalemip[i] = basemip[i] * d_mipscale.value;

	d_aflatcolor = 0;
}

/*
===============
R_SetRenderSize

Allocates the z-buffer, the surface cache (D_AllocCache, which may grow it
later) and every table of a width x height frame
===============
*/
void R_SetRenderSize (int width, int height, int scale)
{
	Mem_FreeAligned (d_pzbuffer);
	d_pzbuffer = Mem_AllocAligned ((size_t)width * height * sizeof (*d_pzbuffer), 64);
	D_AllocCache (D_SurfaceCacheForRes (width, height));

	Mem_Free (d_scantable);
	Mem_Free (zspantable);
	d_scantable = Mem_Calloc ((size_t)height, sizeof(*d_scantable));
	zspantable = Mem_Calloc ((size_t)height, sizeof(*zspantable));

	R_SetEdgeSize (width, height);
	R_SetWarpTable ((width > height ? width : height) / scale);
	D_SetWarpSize (width, height, scale);
	D_SetPolysetSize (height);
	D_SetSpriteSize (height);
	Draw_Invalidate ();		// vid.hud is new too

	vid.recalc_refdef = 1;
}
