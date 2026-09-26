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



static unsigned		blocklights[18*18];
static unsigned		blocklights_rgb[18*18*3];	// r_lightmode 1: LIGHT_ONE is 1.0

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
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
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
			td = (int)(local[1] - t*16);
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = (int)(local[0] - s*16);
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
	int			smax, tmax;
	int			t;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;

	surf = r_drawsurf.surf;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
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
			for (i=0 ; i<size ; i++)
				blocklights[i] += lightmap[i] * scale;
			lightmap += size;	// skip to next lightmap
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
	int				smax, tmax, size, i, c, maps;
	unsigned		scale, m, v;
	byte			*lightmap;
	unsigned short	*rgb;
	msurface_t		*surf;
	unsigned		*bl = blocklights_rgb;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;
	rgb = surf->samples_rgb;

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
		if (rgb)
		{
			for (i=0 ; i<size*3 ; i++)
				bl[i] += (rgb[i] * scale) >> 4;	// 2048 is 1.0
			rgb += size*3;
		}
		else if (lightmap)
		{
			for (i=0 ; i<size ; i++)
			{
				v = lightmap[i] * scale;
				bl[i*3] += v;
				bl[i*3+1] += v;
				bl[i*3+2] += v;
			}
			lightmap += size;
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

	blocksize = 16 >> r_drawsurf.surfmip;
	blockdivshift = 4 - r_drawsurf.surfmip;
	blockdivmask = (1 << blockdivshift) - 1;
	
	r_lightwidth = (r_drawsurf.surf->extents[0]>>4)+1;

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
	int				v, i, b, lightstep, lighttemp, light;
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

			light = lightright;

			for (b=blocksize-1; b>=0; b--)
			{
				prowdest[b] = d_cm30[(light & 0xFF00) + psource[b]];
				light += lightstep;
			}

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
	int				v, i, b, c;
	int				shift = blockdivshift;
	int				left[3], right[3], leftstep[3], rightstep[3], light[3], step[3];
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
			{
				step[c] = (left[c] - right[c]) >> shift;
				light[c] = right[c];
			}

			for (b=blocksize-1; b>=0; b--)
			{
				prowdest[b] = R_LitPixel (psource[b], light[0] > 0 ? (unsigned)light[0] : 0,
					light[1] > 0 ? (unsigned)light[1] : 0, light[2] > 0 ? (unsigned)light[2] : 0);
				for (c=0 ; c<3 ; c++)
					light[c] += step[c];
			}

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

