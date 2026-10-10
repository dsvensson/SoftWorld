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
// d_part.c: software driver module for drawing particles

#include "r_local.h"
#include "d_local.h"


/*
==============
D_EndParticles
==============
*/
void D_EndParticles (void)
{
// not used by software driver
}


/*
==============
D_StartParticles
==============
*/
void D_StartParticles (void)
{
// not used by software driver
}



/*
==============
D_DrawParticle
==============
*/
void D_DrawParticle (particle_t *pparticle)
{
	vec3_t	local, transformed;
	float	zi;
	pixel_t	*pdest, color;
	float	*pz;
	int		izi, pix, count, u, v, n;

// transform point
	VectorSubtract (pparticle->org, r_origin, local);

	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);		

	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

// project the point
// FIXME: preadjust xcenter and ycenter
	zi = 1.0f / transformed[2];
	u = (int)(xcenter + zi * transformed[0] + 0.5);
	v = (int)(ycenter - zi * transformed[1] + 0.5);

	if ((v > d_vrectbottom_particle) || 
		(u > d_vrectright_particle) ||
		(v < d_vrecty) ||
		(u < d_vrectx))
	{
		return;
	}

	pz = d_pzbuffer + (d_zwidth * v) + u;
	pdest = d_viewbuffer + d_scantable[v] + u;
	izi = (int)(zi * 0x8000);

	pix = (izi * (int)vid.scale) >> 7;	// the size in the 320x200 layout, in pixels

	if (pix < d_pix_min)
		pix = d_pix_min;
	else if (pix > d_pix_max)
		pix = d_pix_max;

	color = d_pal30_particle[(byte)pparticle->color];
	if (r_fogactive)
		color = R_FogPixel (color, zi);
	count = pix << d_y_aspect_shift;

	if (!D_Keeping ())
	{
		D_ParticleLines (pdest, pz, color, zi, pix, count);
		return;
	}
	// a piece in each strip of lines it's in (d_batch.c)
	for ( ; count ; count -= n, v += n, pz += n * d_zwidth, pdest += n * screenwidth)
	{
		n = D_STRIP_LINES - v % D_STRIP_LINES;
		if (n > count)
			n = count;
		D_KeepParticle (v, pdest, pz, color, zi, pix, n);
	}
}

/*
==============
D_ParticleLines

lines lines of a particle's square, width wide, depth tested and written
==============
*/
void D_ParticleLines (pixel_t *pdest, float *pz, pixel_t color, float zi, int width, int lines)
{
	int		i;

	for ( ; lines ; lines--, pz += d_zwidth, pdest += screenwidth)
	{
		for (i=0 ; i<width ; i++)
		{
			if (pz[i] <= zi)
			{
				pz[i] = zi;
				pdest[i] = color;
			}
		}
	}
}


