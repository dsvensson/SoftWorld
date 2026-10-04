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
// r_misc.c

#include "r_local.h"
#include "r_local.h"

/*
====================
R_TimeRefresh_f

For program optimization
====================
*/
void R_TimeRefresh_f (void)
{
	int			i;
	float		start, stop, time;
	int			startangle;

	startangle = (int)r_refdef.viewangles[1];
	
	start = (float)Sys_DoubleTime ();
	for (i=0 ; i<128 ; i++)
	{
		r_refdef.viewangles[1] = i/128.0f*360.0f;

		R_RenderView ();

		VID_Update ();
	}
	stop = (float)Sys_DoubleTime ();
	time = stop-start;
	Con_Printf ("%f seconds (%f fps)\n", time, 128/time);
	
	r_refdef.viewangles[1] = (vec_t)startangle;
}

/*
================
R_LineGraph

Only called by R_DisplayTime
================
*/
void R_LineGraph (int x, int y, int h)
{
	int		i;
	int		s;
	int		color;

// FIXME: should be disabled on no-buffer adapters, or should be in the driver
	
//	x += r_refdef.vrect.x;
//	y += r_refdef.vrect.y;
	
	
	s = (int)r_graphheight.value;

	if (h == 10000)
		color = 0x6f;	// yellow
	else if (h == 9999)
		color = 0x4f;	// red
	else if (h == 9998)
		color = 0xd0;	// blue
	else
		color = 0xff;	// pink

	if (h>s)
		h = s;
	
	for (i=0 ; i<h ; i++)
		Draw_Pixel (x, y - i*2, (byte)color);
}

/*
==============
R_TimeGraph

Performance monitoring tool
==============
*/
#define	MAX_TIMINGS		100
extern float mouse_x, mouse_y;
static int		graphval;
void R_TimeGraph (void)
{
	int		conwidth = r_refdef.vrect.width / (int)vid.scale;
	int		conheight = r_refdef.vrect.height / (int)vid.scale;
	static	int		timex;
	int		a;
	float	r_time2;
	static byte	r_timings[MAX_TIMINGS];
	int		x;
	
	r_time2 = (float)Sys_DoubleTime ();

	a = (int)((r_time2-r_time1)/0.01);
//a = fabs(mouse_y * 0.05);
//a = (int)((r_refdef.vieworg[2] + 1024)/1)%(int)r_graphheight.value;
//a = (int)((pmove.velocity[2] + 500)/10);
//a = fabs(velocity[0])/20;
//a = ((int)fabs(origin[0])/8)%20;
//a = (cl.idealpitch + 30)/5;
//a = (int)(cl.simangles[YAW] * 64/360) & 63;
a = graphval;

	r_timings[timex] = (byte)a;
	a = timex;

	if (conwidth <= MAX_TIMINGS)
		x = conwidth-1;
	else
		x = conwidth -
				(conwidth - MAX_TIMINGS)/2;
	do
	{
		R_LineGraph (x, conheight-2, r_timings[a]);
		if (x==0)
			break;		// screen too small to hold entire thing
		x--;
		a--;
		if (a == -1)
			a = MAX_TIMINGS-1;
	} while (a != timex);

	timex = (timex+1)%MAX_TIMINGS;
}

/*
==============
R_ZGraph
==============
*/
void R_ZGraph (void)
{
	int		conwidth = r_refdef.vrect.width / (int)vid.scale;
	int		conheight = r_refdef.vrect.height / (int)vid.scale;
	int		a, x, w, i;
	static	int	height[256];

	if (conwidth <= 256)
		w = conwidth;
	else
		w = 256;

	height[r_framecount&255] = ((int)r_origin[2]) & 31;

	x = 0;
	for (a=0 ; a<w ; a++)
	{
		i = (r_framecount-a) & 255;
		R_LineGraph (x+w-1-a, conheight-2, height[i]);
	}
}

/*
=============
R_PrintTimes
=============
*/
void R_PrintTimes (void)
{
	float	r_time2;
	float		ms;

	r_time2 = (float)Sys_DoubleTime ();

	ms = 1000* (r_time2 - r_time1);
	
	Con_Printf ("%5.1f ms %3i/%3i/%3i poly %3i surf\n",
				ms, c_faceclip, r_polycount, r_drawnpolycount, c_surf);
	c_surf = 0;
}


/*
=============
R_PrintDSpeeds
=============
*/
void R_PrintDSpeeds (void)
{
	float	ms, dp_time, r_time2, rw_time, db_time, se_time, de_time, dv_time;

	r_time2 = (float)Sys_DoubleTime ();

	dp_time = (dp_time2 - dp_time1) * 1000;
	rw_time = (rw_time2 - rw_time1) * 1000;
	db_time = (db_time2 - db_time1) * 1000;
	se_time = (se_time2 - se_time1) * 1000;
	de_time = (de_time2 - de_time1) * 1000;
	dv_time = (dv_time2 - dv_time1) * 1000;
	ms = (r_time2 - r_time1) * 1000;

	Con_Printf ("%3i %4.1fp %3iw %4.1fb %3is %4.1fe %4.1fv\n",
				(int)ms, dp_time, (int)rw_time, db_time, (int)se_time, de_time,
				dv_time);
}


/*
=============
R_PrintAliasStats
=============
*/
void R_PrintAliasStats (void)
{
	Con_Printf ("%3i polygon model drawn\n", r_amodels_drawn);
}

/*
===================
R_TransformFrustum
===================
*/
void R_TransformFrustum (void)
{
	int		i;
	vec3_t	v, v2;
	
	for (i=0 ; i<4 ; i++)
	{
		v[0] = screenedge[i].normal[2];
		v[1] = -screenedge[i].normal[0];
		v[2] = screenedge[i].normal[1];

		v2[0] = v[1]*vright[0] + v[2]*vup[0] + v[0]*vpn[0];
		v2[1] = v[1]*vright[1] + v[2]*vup[1] + v[0]*vpn[1];
		v2[2] = v[1]*vright[2] + v[2]*vup[2] + v[0]*vpn[2];

		VectorCopy (v2, view_clipplanes[i].normal);

		view_clipplanes[i].dist = DotProduct (modelorg, v2);
	}
}



/*
===================
R_TransformBandFrustum

R_TransformFrustum for a band's view
===================
*/
void R_TransformBandFrustum (rband_t *b)
{
	int		i;
	vec3_t	v, v2;
	
	for (i=0 ; i<4 ; i++)
	{
		v[0] = screenedge[i].normal[2];
		v[1] = -screenedge[i].normal[0];
		v[2] = screenedge[i].normal[1];

		v2[0] = v[1]*b->vright[0] + v[2]*b->vup[0] + v[0]*b->vpn[0];
		v2[1] = v[1]*b->vright[1] + v[2]*b->vup[1] + v[0]*b->vpn[1];
		v2[2] = v[1]*b->vright[2] + v[2]*b->vup[2] + v[0]*b->vpn[2];

		VectorCopy (v2, b->clipplanes[i].normal);

		b->clipplanes[i].dist = DotProduct (b->modelorg, v2);
	}
}

/*
================
TransformVector
================
*/
void TransformVector (vec3_t in, vec3_t out)
{
	out[0] = DotProduct(in,vright);
	out[1] = DotProduct(in,vup);
	out[2] = DotProduct(in,vpn);		
}

/*
===============
R_SetUpFrustumIndexes
===============
*/
static void R_SetUpFrustumIndexes (void)
{
	int		i, j, *pindex;

	pindex = r_frustum_indexes;

	for (i=0 ; i<4 ; i++)
	{
		for (j=0 ; j<3 ; j++)
		{
			if (view_clipplanes[i].normal[j] < 0)
			{
				pindex[j] = j;
				pindex[j+3] = j+3;
			}
			else
			{
				pindex[j] = j+3;
				pindex[j+3] = j;
			}
		}

	// FIXME: do just once at start
		pfrustum_indexes[i] = pindex;
		pindex += 6;
	}
}


/*
===============
R_SetupFrame
===============
*/
void R_SetupFrame (void)
{
	int				i, count, room;

// don't allow cheats in multiplayer
r_fullbright.value = 0;
r_ambient.value = 0;
r_drawflat.value = 0;

// the last frame's, of all its bands
	if (r_numsurfs.value)
	{
		for (i = count = room = 0 ; i<r_numbands ; i++)
		{
			count += (int)(r_bands[i].surface_p - r_bands[i].surfaces);
			room += r_bands[i].maxsurfs;
		}
		if (count > r_maxsurfsseen)
			r_maxsurfsseen = count;

		Con_Printf ("Used %d of %d surfs; %d max\n", count, room, r_maxsurfsseen);
	}

	if (r_numedges.value)
	{
		for (i = count = room = 0 ; i<r_numbands ; i++)
		{
			count += (int)(r_bands[i].edge_p - r_bands[i].edges);
			room += r_bands[i].maxedges;
		}
		if (count > r_maxedgesseen)
			r_maxedgesseen = count;

		Con_Printf ("Used %d of %d edges; %d max\n", count, room, r_maxedgesseen);
	}

	r_refdef.ambientlight = (int)r_ambient.value;

	if (r_refdef.ambientlight < 0)
		r_refdef.ambientlight = 0;

	R_AnimateLight ();

	r_framecount++;

// debugging

// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, modelorg);
	VectorCopy (r_refdef.vieworg, r_origin);

	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, r_scene.worldmodel);

	r_dowarpold = r_dowarp;
	r_dowarp = r_waterwarp.value && (r_viewleaf->contents <= CONTENTS_WATER);

	if ((r_dowarp != r_dowarpold) || r_viewchanged)
	{
		R_SetViewRect (&r_viewrect, r_viewaspect);

		r_viewchanged = false;
	}

// start off with just the four screen edge clip planes
	R_TransformFrustum ();

// save base values
	VectorCopy (vpn, base_vpn);
	VectorCopy (vright, base_vright);
	VectorCopy (vup, base_vup);
	VectorCopy (modelorg, base_modelorg);

	R_SetSkyFrame ();

	R_SetUpFrustumIndexes ();

	// the last frame's surfaces didn't fit in the cache at once
	if (r_cache_thrash)
		D_GrowCache ();
	r_cache_thrash = false;

// clear frame counts
	c_faceclip = 0;
	d_spanpixcount = 0;
	r_polycount = 0;
	r_drawnpolycount = 0;
	r_wholepolycount = 0;
	r_amodels_drawn = 0;

	D_SetupFrame ();
}

