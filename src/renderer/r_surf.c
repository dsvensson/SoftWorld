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
// r_surf.c: surface-related refresh code

#include "r_local.h"
#include "r_local.h"

drawsurf_t	r_drawsurf;

static int				lightleft, blocksize, sourcetstep;
static int				lightright, lightleftstep, lightrightstep, blockdivshift;
static unsigned		blockdivmask;
static pixel_t			*prowdestbase;
static unsigned char	*pbasesource;
static int				surfrowpixels;
static unsigned		*r_lightptr;			// the light at the column's block corners
static unsigned		*r_lightptr_rgb;
static int				r_stepback;
static int				r_lightwidth;
static int				r_numhblocks, r_numvblocks;
static unsigned char	*r_source, *r_sourcemax;

static void R_DrawSurfaceBlock (void);
static void R_DrawSurfaceBlockRGB (void);



static unsigned		*blocklights;			// r_lightmode 0: 8.8
static unsigned		*blocklights_rgb;		// r_lightmode 1: LIGHT_ONE is 1.0
static int			blocklights_size;		// samples they hold

// the light of the surface being built is on a grid of 1 << r_lightshift
// texels, r_lightgrid[0] x r_lightgrid[1] points; for a lightmap that isn't
// vanilla, where each point takes its luxels from
static int			r_lightshift;
static int			r_lightgrid[2];
typedef struct
{
	int		index;			// the top left luxel
	int		dx, dy;			// to the right and lower ones, 0 on the lightmap's edge
	int		fx, fy;			// the weights of those, 0 .. 256
} lightsample_t;
static lightsample_t	*r_lightsamples;

/*
===============
R_BlocklightsForSize

Room for the light samples of a surface; they grow to the largest surface drawn
===============
*/
static void R_BlocklightsForSize (int size)
{
	if (size <= blocklights_size)
		return;
	blocklights_size = size;
	blocklights = Mem_Realloc (blocklights, (size_t)size * sizeof(*blocklights));
	blocklights_rgb = Mem_Realloc (blocklights_rgb, (size_t)size * 3 * sizeof(*blocklights_rgb));
	r_lightsamples = Mem_Realloc (r_lightsamples, (size_t)size * sizeof(*r_lightsamples));
}

/*
===============
R_LightGrid

The grid the light of the surface is built on: a vanilla lightmap's own 16
texels, others as fine as their luxels but no finer than the mip level's
texels. Returns the number of points.
===============
*/
static int R_LightGrid (msurface_t *surf, int miplevel)
{
	lightsample_t	*ls;
	int				i, j, u0, v0;
	float			s, t, u, v;

	r_lightshift = surf->lmvanilla ? 4 : (surf->lmgridshift > miplevel ? surf->lmgridshift : miplevel);
	r_lightgrid[0] = (surf->extents[0] >> r_lightshift) + 1;
	r_lightgrid[1] = (surf->extents[1] >> r_lightshift) + 1;
	R_BlocklightsForSize (r_lightgrid[0] * r_lightgrid[1]);
	if (surf->lmvanilla || !surf->samples)
		return r_lightgrid[0] * r_lightgrid[1];

	ls = r_lightsamples;
	for (j = 0 ; j < r_lightgrid[1] ; j++)
	{
		for (i = 0 ; i < r_lightgrid[0] ; i++, ls++)
		{
			s = (float)(surf->texturemins[0] + (i << r_lightshift));
			t = (float)(surf->texturemins[1] + (j << r_lightshift));
			u = surf->lmvecs[0][0]*s + surf->lmvecs[0][1]*t + surf->lmvecs[0][2];
			v = surf->lmvecs[1][0]*s + surf->lmvecs[1][1]*t + surf->lmvecs[1][2];
			u = u < 0 ? 0 : (u > surf->lmwidth - 1 ? (float)(surf->lmwidth - 1) : u);
			v = v < 0 ? 0 : (v > surf->lmheight - 1 ? (float)(surf->lmheight - 1) : v);
			u0 = (int)u;
			v0 = (int)v;
			ls->index = v0 * surf->lmwidth + u0;
			ls->dx = u0 + 1 < surf->lmwidth ? 1 : 0;
			ls->dy = v0 + 1 < surf->lmheight ? surf->lmwidth : 0;
			ls->fx = (int)((u - u0) * 256);
			ls->fy = (int)((v - v0) * 256);
		}
	}
	return r_lightgrid[0] * r_lightgrid[1];
}

// a luxel of a style's plane at a grid point, bilinear, times 256
static inline unsigned R_SampleMono (const byte *plane, const lightsample_t *ls)
{
	const byte	*p = plane + ls->index;
	unsigned	top = p[0] * (256 - ls->fx) + p[ls->dx] * ls->fx;
	unsigned	bottom = p[ls->dy] * (256 - ls->fx) + p[ls->dy + ls->dx] * ls->fx;

	return (top * (256 - ls->fy) + bottom * ls->fy) >> 8;
}

// the same for channel c of 16 bit RGB luxels, not scaled
static inline unsigned R_SampleRGB (const unsigned short *plane, const lightsample_t *ls, int c)
{
	const unsigned short	*p = plane + ls->index * 3 + c;
	uint64_t	top = (uint64_t)p[0] * (256 - ls->fx) + (uint64_t)p[ls->dx * 3] * ls->fx;
	uint64_t	bottom = (uint64_t)p[ls->dy * 3] * (256 - ls->fx) + (uint64_t)p[(ls->dy + ls->dx) * 3] * ls->fx;

	return (unsigned)((top * (256 - ls->fy) + bottom * ls->fy) >> 16);
}

/*
===============
R_AddDynamicLights
===============
*/
static void R_AddDynamicLights (unsigned *bl, bool rgb)
{
	msurface_t *surf;
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight, color[3];
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	surf = r_drawsurf.surf;
	smax = r_lightgrid[0];
	tmax = r_lightgrid[1];
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !(surf->dlightbits & (1<<lnum) ) )
			continue;		// not lit by this light

		rad = r_scene.dlights[lnum].radius;
		dist = DotProduct (r_scene.dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabsf(dist);
		minlight = r_scene.dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;
		R_DlightColor (&r_scene.dlights[lnum], color);

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = r_scene.dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];
		
		for (t = 0 ; t<tmax ; t++)
		{
			td = (int)(local[1] - (t << r_lightshift));
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = (int)(local[0] - (s << r_lightshift));
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = (float)(sd + (td>>1));
				else
					dist = (float)(td + (sd>>1));
				if (dist >= minlight)
					continue;
				if (!rgb)
				{
					bl[t*smax + s] = (unsigned)(bl[t*smax + s] + (rad - dist)*256);
					continue;
				}
				for (i=0 ; i<3 ; i++)
					bl[(t*smax + s)*3 + i] = (unsigned)(bl[(t*smax + s)*3 + i] + (rad - dist)*256*color[i]);
			}
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (void)
{
	int			t;
	int			i, size, planesize;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;

	surf = r_drawsurf.surf;
	size = R_LightGrid (surf, r_drawsurf.surfmip);
	planesize = surf->lmwidth * surf->lmheight;
	lightmap = surf->samples;

	if (/* r_fullbright.value || */ !r_scene.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 0;
		return;
	}

// clear to ambient
	for (i=0 ; i<size ; i++)
		blocklights[i] = r_refdef.ambientlight<<8;


// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fraction
			if (surf->lmvanilla)
			{
				for (i=0 ; i<size ; i++)
					blocklights[i] += lightmap[i] * scale;
			}
			else
			{
				for (i=0 ; i<size ; i++)
					blocklights[i] += (R_SampleMono (lightmap, &r_lightsamples[i]) * scale) >> 8;
			}
			lightmap += planesize;	// skip to next lightmap
		}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights (blocklights, false);

// bound, invert, and shift
	for (i=0 ; i<size ; i++)
	{
		t = (255*256 - (int)blocklights[i]) >> (8 - VID_CBITS);

		if (t < (1 << 6))
			t = (1 << 6);

		blocklights[i] = t;
	}
}


/*
===============
R_BuildLightMapRGB

The light of each block corner in red, green and blue, LIGHT_ONE for
1.0, from RGB samples if the map has them. The brightest channel is
held to LIGHT_MAX, the others scaled with it to keep the hue.
===============
*/
static void R_BuildLightMapRGB (void)
{
	int				size, planesize, i, c, maps;
	unsigned		scale, m, v;
	byte			*lightmap;
	unsigned short	*rgb;
	msurface_t		*surf;
	unsigned		*bl;

	surf = r_drawsurf.surf;
	size = R_LightGrid (surf, r_drawsurf.surfmip);
	planesize = surf->lmwidth * surf->lmheight;
	lightmap = surf->samples;
	rgb = surf->samples_rgb;
	bl = blocklights_rgb;

	if (!r_scene.worldmodel->lightdata)
	{
		for (i=0 ; i<size*3 ; i++)
			bl[i] = 255 << 8;
		return;
	}

	for (i=0 ; i<size*3 ; i++)
		bl[i] = (unsigned)r_refdef.ambientlight << 8;

	for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ; maps++)
	{
		scale = r_drawsurf.lightadj[maps];	// 8.8 fraction
		if (rgb && surf->lmvanilla)
		{
			for (i=0 ; i<size*3 ; i++)
				bl[i] += (rgb[i] * scale) >> 4;	// 2048 is 1.0
			rgb += planesize*3;
		}
		else if (rgb)
		{
			for (i=0 ; i<size ; i++)
				for (c=0 ; c<3 ; c++)
					bl[i*3 + c] += (R_SampleRGB (rgb, &r_lightsamples[i], c) * scale) >> 4;
			rgb += planesize*3;
		}
		else if (lightmap)
		{
			for (i=0 ; i<size ; i++)
			{
				v = surf->lmvanilla ? lightmap[i] * scale : (R_SampleMono (lightmap, &r_lightsamples[i]) * scale) >> 8;
				bl[i*3] += v;
				bl[i*3+1] += v;
				bl[i*3+2] += v;
			}
			lightmap += planesize;
		}
	}

	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights (bl, true);

	for (i=0 ; i<size ; i++, bl += 3)
	{
		m = bl[0] > bl[1] ? bl[0] : bl[1];
		m = bl[2] > m ? bl[2] : m;
		if (m <= LIGHT_MAX)
			continue;
		for (c=0 ; c<3 ; c++)
			bl[c] = (unsigned)((uint64_t)bl[c] * LIGHT_MAX / m);
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	int		reletive;
	int		count;

	if (currententity->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(r_scene.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
===============
R_DrawSurface
===============
*/
void R_DrawSurface (void)
{
	unsigned char	*basetptr;
	int				smax, tmax, twidth;
	int				u;
	int				soffset, basetoffset, texwidth;
	int				horzblockstep;
	pixel_t			*pcolumndest;
	texture_t		*mt;

// calculate the lightings
	if (r_lightmode.value)
		R_BuildLightMapRGB ();
	else
		R_BuildLightMap ();
	
	surfrowpixels = r_drawsurf.rowpixels;

	mt = r_drawsurf.texture;
	
	r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];
	
// the fractional light values should range from 0 to (VID_GRADES - 1) << 16
// from a source range of 0 - 255
	
	texwidth = mt->width >> r_drawsurf.surfmip;

	blockdivshift = r_lightshift - r_drawsurf.surfmip;
	blocksize = 1 << blockdivshift;
	blockdivmask = (1 << blockdivshift) - 1;

	r_lightwidth = r_lightgrid[0];

	r_numhblocks = r_drawsurf.surfwidth >> blockdivshift;
	r_numvblocks = r_drawsurf.surfheight >> blockdivshift;

//==============================

	horzblockstep = blocksize;

	smax = mt->width >> r_drawsurf.surfmip;
	twidth = texwidth;
	tmax = mt->height >> r_drawsurf.surfmip;
	sourcetstep = texwidth;
	r_stepback = tmax * twidth;

	r_sourcemax = r_source + (tmax * smax);

	soffset = r_drawsurf.surf->texturemins[0];
	basetoffset = r_drawsurf.surf->texturemins[1];

// << 16 components are to guarantee positive values for %
	soffset = ((soffset >> r_drawsurf.surfmip) + (smax << 16)) % smax;
	basetptr = &r_source[((((basetoffset >> r_drawsurf.surfmip) 
		+ (tmax << 16)) % tmax) * twidth)];

	pcolumndest = r_drawsurf.surfdat;

	for (u=0 ; u<r_numhblocks; u++)
	{
		r_lightptr = blocklights + u;
		r_lightptr_rgb = blocklights_rgb + u*3;

		prowdestbase = pcolumndest;

		pbasesource = basetptr + soffset;

		if (r_lightmode.value)
			R_DrawSurfaceBlockRGB ();
		else
			R_DrawSurfaceBlock ();

		soffset = soffset + blocksize;
		if (soffset >= smax)
			soffset = 0;

		pcolumndest += horzblockstep;
	}
}


//=============================================================================


/*
================
R_DrawSurfaceBlock

A column of blocks, 16 >> miplevel texels square, lit by interpolating
the light at the block corners
================
*/
static void R_DrawSurfaceBlock (void)
{
	int				v, i, lightstep, lighttemp;
	int				shift = blockdivshift;
	byte			*psource;
	pixel_t			*prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> shift;
		lightrightstep = (r_lightptr[1] - lightright) >> shift;

		for (i=0 ; i<blocksize ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> shift;

			// the right texel gets lightright, each one left of it lightstep more
			simd_litrow_colormap (prowdest, psource, d_cm30, lightright, lightstep, blocksize);

			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowpixels;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}

/*
================
R_DrawSurfaceBlockRGB

R_DrawSurfaceBlock for r_lightmode 1: each channel's light interpolated
between the block corners multiplies the texel's color
================
*/
static void R_DrawSurfaceBlockRGB (void)
{
	int				v, i, c;
	int				shift = blockdivshift;
	int				left[3], right[3], leftstep[3], rightstep[3], step[3];
	const unsigned	*lp = r_lightptr_rgb;
	byte			*psource;
	pixel_t			*prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
		for (c=0 ; c<3 ; c++)
		{
			left[c] = (int)lp[c];
			right[c] = (int)lp[3 + c];
		}
		lp += r_lightwidth * 3;
		for (c=0 ; c<3 ; c++)
		{
			leftstep[c] = ((int)lp[c] - left[c]) >> shift;
			rightstep[c] = ((int)lp[3 + c] - right[c]) >> shift;
		}

		for (i=0 ; i<blocksize ; i++)
		{
			for (c=0 ; c<3 ; c++)
				step[c] = (left[c] - right[c]) >> shift;

			simd_litrow_rgb (prowdest, psource, d_pal30, d_pal30_floor, right, step, blocksize);

			psource += sourcetstep;
			for (c=0 ; c<3 ; c++)
			{
				right[c] += rightstep[c];
				left[c] += leftstep[c];
			}
			prowdest += surfrowpixels;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}

//============================================================================

