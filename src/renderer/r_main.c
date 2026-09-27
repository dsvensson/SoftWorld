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
// r_main.c

#include "r_local.h"
#include "r_local.h"
static void	R_InitTurb (void);

//define	PASSAGES

static vec3_t		viewlightvec;
static alight_t	r_viewlighting = {.ambientlight = 128, .shadelight = 192, .plightvec = viewlightvec, .color = {1, 1, 1}};
float		r_time1;
int			r_numallocatededges;
bool	r_drawpolys;
bool	r_drawculledpolys;
bool	r_worldpolysbacktofront;
bool	r_recursiveaffinetriangles = true;
float		r_aliasuvscale = 1.0;
int			r_outofsurfaces;
int			r_outofedges;

bool	r_dowarp, r_dowarpold, r_viewchanged;
vrect_t	r_viewrect;		// where the client wants the view on screen
float	r_viewaspect;

int			numbtofpolys;
btofpoly_t	*pbtofpolys;
mvertex_t	*r_pcurrentvertbase;

int			c_surf;
int r_maxsurfsseen, r_maxedgesseen;
static int r_cnumsurfs;
static surf_t	*r_surfaces_mem;	// heap block behind surfaces (which points one element into it)
int			r_clipflags;

pixel_t		*r_warpbuffer;


entity_t	r_worldentity;

//
// view origin
//
vec3_t	vup, base_vup;
vec3_t	vpn, base_vpn;
vec3_t	vright, base_vright;
vec3_t	r_origin;

//
// screen size info
//
refdef_t	r_refdef;
float		xcenter, ycenter;
float		xscale, yscale;
float		xscaleinv, yscaleinv;
float		xscaleshrink, yscaleshrink;
float		aliasxscale, aliasyscale, aliasxcenter, aliasycenter;

int		screenwidth;

float	pixelAspect;
static float	screenAspect;
static float	verticalFieldOfView;
static float	xOrigin, yOrigin;

mplane_t	screenedge[4];

//
// refresh flags
//
int		r_framecount = 1;	// so frame counts initialized to 0 don't match
int		r_visframecount;
int		d_spanpixcount;
int		r_polycount;
int		r_drawnpolycount;
int		r_wholepolycount;

int			*pfrustum_indexes[4];
int			r_frustum_indexes[4*6];

								// must be reinitialized for current cache size

mleaf_t		*r_viewleaf, *r_oldviewleaf;

texture_t	*r_notexture_mip;

float		r_aliastransition, r_resfudge;

int		d_lightstylevalue[256];	// 8.8 fraction of base light value

float	dp_time1, dp_time2, db_time1, db_time2, rw_time1, rw_time2;
float	se_time1, se_time2, de_time1, de_time2, dv_time1, dv_time2;

static void R_MarkLeaves (void);

cvar_t	r_draworder = {.name = "r_draworder", .string = "0"};
static cvar_t	r_speeds = {.name = "r_speeds", .string = "0"};
static cvar_t	r_timegraph = {.name = "r_timegraph", .string = "0"};
static cvar_t	r_zgraph = {.name = "r_zgraph", .string = "0"};
cvar_t	r_graphheight = {.name = "r_graphheight", .string = "15"};
cvar_t	r_clearcolor = {.name = "r_clearcolor", .string = "2"};
cvar_t	r_waterwarp = {.name = "r_waterwarp", .string = "1"};
cvar_t	r_fullbright = {.name = "r_fullbright", .string = "0"};
// 0: light through the colormap as Quake did; 1: light in RGB with 4x headroom
static cvar_t	r_profile = {.name = "r_profile", .string = "0"};
static void R_Profile_f (void);
cvar_t	r_lightmode = {.name = "r_lightmode", .string = "1", .archive = true};
// dynamic lights have color (r_lightmode 1)
static cvar_t	r_dlight_color = {.name = "r_dlight_color", .string = "1", .archive = true};
// fullbright colors are this much brighter than white allows (r_lightmode 1)
static cvar_t	r_fullbright_scale = {.name = "r_fullbright_scale", .string = "1.3", .archive = true};
static cvar_t	r_drawentities = {.name = "r_drawentities", .string = "1"};

// how opaque liquids are drawn, 0 .. 1; seeing through them needs a map whose
// visibility was built for it
static cvar_t	r_wateralpha = {.name = "r_wateralpha", .string = "1", .archive = true};
static cvar_t	r_lavaalpha = {.name = "r_lavaalpha", .string = "1", .archive = true};
static cvar_t	r_slimealpha = {.name = "r_slimealpha", .string = "1", .archive = true};
static cvar_t	r_telealpha = {.name = "r_telealpha", .string = "1", .archive = true};
// every leaf is drawn, not just what the view's leaf sees; liquids can then be
// seen through on any map
static cvar_t	r_novis = {.name = "r_novis", .string = "0"};
static cvar_t	r_drawviewmodel = {.name = "r_drawviewmodel", .string = "1"};
static cvar_t	r_aliasstats = {.name = "r_polymodelstats", .string = "0"};
static cvar_t	r_dspeeds = {.name = "r_dspeeds", .string = "0"};
cvar_t	r_drawflat = {.name = "r_drawflat", .string = "0"};
cvar_t	r_ambient = {.name = "r_ambient", .string = "0"};
static cvar_t	r_reportsurfout = {.name = "r_reportsurfout", .string = "0"};
static cvar_t	r_maxsurfs = {.name = "r_maxsurfs", .string = "0"};
cvar_t	r_numsurfs = {.name = "r_numsurfs", .string = "0"};
static cvar_t	r_reportedgeout = {.name = "r_reportedgeout", .string = "0"};
static cvar_t	r_maxedges = {.name = "r_maxedges", .string = "0"};
cvar_t	r_numedges = {.name = "r_numedges", .string = "0"};
static cvar_t	r_aliastransbase = {.name = "r_aliastransbase", .string = "200"};
static cvar_t	r_aliastransadj = {.name = "r_aliastransadj", .string = "100"};



void R_ZGraph (void);

/*
==================
R_InitTextures
==================
*/
void	R_InitTextures (void)
{
	int		x,y, m;
	byte	*dest;
	
// create a simple checkerboard texture for the default
	r_notexture_mip = Mem_Calloc (1, sizeof(texture_t) + 16*16+8*8+4*4+2*2);
	
	r_notexture_mip->width = r_notexture_mip->height = 16;
	r_notexture_mip->offsets[0] = sizeof(texture_t);
	r_notexture_mip->offsets[1] = r_notexture_mip->offsets[0] + 16*16;
	r_notexture_mip->offsets[2] = r_notexture_mip->offsets[1] + 8*8;
	r_notexture_mip->offsets[3] = r_notexture_mip->offsets[2] + 4*4;
	
	for (m=0 ; m<4 ; m++)
	{
		dest = (byte *)r_notexture_mip + r_notexture_mip->offsets[m];
		for (y=0 ; y< (16>>m) ; y++)
			for (x=0 ; x< (16>>m) ; x++)
			{
				if (  (y< (8>>m) ) ^ (x< (8>>m) ) )
					*dest++ = 0;
				else
					*dest++ = 0xff;
			}
	}	
}

/*
===============
R_Init
===============
*/
void R_Init (void)
{
	R_InitTurb ();
	
	Cmd_AddCommand ("timerefresh", R_TimeRefresh_f);	
	Cmd_AddCommand ("pointfile", R_ReadPointFile_f);	

	Cvar_RegisterVariable (&r_draworder);
	Cvar_RegisterVariable (&r_speeds);
	Cvar_RegisterVariable (&r_timegraph);
	Cvar_RegisterVariable (&r_zgraph);
	Cvar_RegisterVariable (&r_graphheight);
	Cvar_RegisterVariable (&r_drawflat);
	Cvar_RegisterVariable (&r_ambient);
	Cvar_RegisterVariable (&r_clearcolor);
	Cvar_RegisterVariable (&r_waterwarp);
	R_LightDataInit ();
	Cvar_RegisterVariable (&r_lightmode);
	Cvar_RegisterVariable (&r_profile);
	Cmd_AddCommand ("r_profile_show", R_Profile_f);
	Cvar_RegisterVariable (&r_dlight_color);
	Cvar_RegisterVariable (&r_fullbright_scale);
	Cvar_RegisterVariable (&r_fullbright);
	Cvar_RegisterVariable (&r_drawentities);
	Cvar_RegisterVariable (&r_wateralpha);
	Cvar_RegisterVariable (&r_lavaalpha);
	Cvar_RegisterVariable (&r_slimealpha);
	Cvar_RegisterVariable (&r_telealpha);
	Cvar_RegisterVariable (&r_novis);
	Cvar_RegisterVariable (&r_drawviewmodel);
	Cvar_RegisterVariable (&r_aliasstats);
	Cvar_RegisterVariable (&r_dspeeds);
	Cvar_RegisterVariable (&r_reportsurfout);
	Cvar_RegisterVariable (&r_maxsurfs);
	Cvar_RegisterVariable (&r_numsurfs);
	Cvar_RegisterVariable (&r_reportedgeout);
	Cvar_RegisterVariable (&r_maxedges);
	Cvar_RegisterVariable (&r_numedges);
	Cvar_RegisterVariable (&r_aliastransbase);
	Cvar_RegisterVariable (&r_aliastransadj);

	Cvar_SetValue ("r_maxedges", (float)MINEDGES);
	Cvar_SetValue ("r_maxsurfs", (float)MINSURFACES);

	view_clipplanes[0].leftedge = true;
	view_clipplanes[1].rightedge = true;
	view_clipplanes[1].leftedge = view_clipplanes[2].leftedge =
			view_clipplanes[3].leftedge = false;
	view_clipplanes[0].rightedge = view_clipplanes[2].rightedge =
			view_clipplanes[3].rightedge = false;

	r_refdef.xOrigin = XCENTERING;
	r_refdef.yOrigin = YCENTERING;

	R_InitParticles ();

// TODO: collect 386-specific code in one place

	D_Init ();
}

r_scene_t	r_scene;

/*
===============
R_AllocEdges

The edges and surfaces of a frame; R_EdgeDrawing grows them when a frame
needs more
===============
*/
static void R_AllocEdges (int numedges, int numsurfs)
{
	if (numedges < MINEDGES)
		numedges = MINEDGES;
	if (numsurfs < MINSURFACES)
		numsurfs = MINSURFACES;

	Mem_Free (r_edges);
	r_numallocatededges = numedges;
	r_edges = Mem_Calloc ((size_t)r_numallocatededges, sizeof(edge_t));

	Mem_Free (r_surfaces_mem);
	r_cnumsurfs = numsurfs;
	r_surfaces_mem = Mem_Calloc ((size_t)r_cnumsurfs, sizeof(surf_t));
	surf_max = &r_surfaces_mem[r_cnumsurfs];
// surface 0 doesn't really exist; it's just a dummy because index 0
// is used to indicate no edge attached to surface
	surfaces = r_surfaces_mem - 1;
	surface_p = surfaces;
}

/*
===============
R_LightTint

The color of summed light, its brightest channel 1
===============
*/
static void R_LightTint (const vec3_t rgb, float color[3])
{
	float	m = fmaxf (rgb[0], fmaxf (rgb[1], rgb[2]));
	int		i;

	for (i=0 ; i<3 ; i++)
		color[i] = m > 0 ? rgb[i] / m : 1;
}

static double	r_prof[PROF_COUNT];
static int		r_profframes;
static double	r_profsince;		// when the first frame counted began
static int64_t	r_profn[PROFN_COUNT];
static int		r_profthrash;		// frames the surface cache ran out in

double R_ProfStart (void)
{
	return r_profile.value ? Sys_DoubleTime () : 0;
}

void R_ProfEnd (prof_t stage, double start)
{
	if (start)
		r_prof[stage] += Sys_DoubleTime () - start;
}

void R_ProfCount (profn_t what, int n)
{
	if (r_profile.value)
		r_profn[what] += n;
}

/*
===============
R_Profile_f

Average microseconds per frame of each stage since the last call, of the
whole frame, and of what no stage covers (the game, sound, the waits)
===============
*/
static void R_Profile_f (void)
{
	static const char	*names[PROF_COUNT] = {"edges", "spans", "draw", "surfcache", "models", "viewmodel",
		"particles", "warp", "2d", "present"};
	double	frame, staged;
	int		i;

	if (!r_profframes)
	{
		Con_Printf ("no frames profiled; set r_profile 1\n");
		return;
	}
	Con_Printf ("%d frames, microseconds per frame:\n", r_profframes);
	staged = 0;
	for (i=0 ; i<PROF_COUNT ; i++)
	{
		Con_Printf ("  %-10s %8.1f\n", names[i], r_prof[i] * 1e6 / r_profframes);
		if (i != PROF_DRAW && i != PROF_SURFCACHE)		// parts of PROF_SPANS
			staged += r_prof[i];
	}
	frame = Sys_DoubleTime () - r_profsince;
	Con_Printf ("  %-10s %8.1f\n", "other", (frame - staged) * 1e6 / r_profframes);
	Con_Printf ("  %-10s %8.1f\n", "frame", frame * 1e6 / r_profframes);
	Con_Printf ("  %-10s %8.1f a frame (%.1f for dynamic lights), %.0f texels; cache ran out in %.1f%% of frames\n",
		"surfaces", (double)r_profn[PROFN_SURFACES] / r_profframes, (double)r_profn[PROFN_DLIT] / r_profframes,
		(double)r_profn[PROFN_TEXELS] / r_profframes, 100.0 * r_profthrash / r_profframes);
	Con_Printf ("  %-10s %8.1f%% of frames drew the 2D layer again\n", "hud",
		100.0 * (double)r_profn[PROFN_HUD] / r_profframes);
	memset (r_prof, 0, sizeof(r_prof));
	memset (r_profn, 0, sizeof(r_profn));
	r_profframes = 0;
	r_profthrash = 0;
}

/*
===============
R_DlightColor

A dynamic light's color with its brightest channel 1; white without color
===============
*/
void R_DlightColor (const dlight_t *dl, float color[3])
{
	float	m = fmaxf (dl->color[0], fmaxf (dl->color[1], dl->color[2]));
	int		i;

	for (i=0 ; i<3 ; i++)
		color[i] = r_dlight_color.value && m > 0 ? dl->color[i] / m : 1;
}

/*
===============
R_CheckLightSettings

Surfaces cached with other light settings are drawn again
===============
*/
static void R_CheckLightSettings (void)
{
	static float	lightmode = -1, dlightcolor = -1, fbscale = -1;

	if (r_fullbright_scale.value != fbscale)
	{
		fbscale = r_fullbright_scale.value;
		R_SetFullbrightScale (fbscale > 0 ? fbscale : 1);
		lightmode = -1;
	}
	if (r_lightmode.value != lightmode || r_dlight_color.value != dlightcolor)
	{
		lightmode = r_lightmode.value;
		dlightcolor = r_dlight_color.value;
		D_FlushCaches ();
	}
}

/*
===============
R_CheckLiquidVis

The liquids the map's visibility sees through: those that fill a leaf which
sees open air. Most maps are built with liquids opaque to visibility, and
what is behind their surface isn't drawn, so R_SurfaceAlpha keeps them
opaque whatever r_*alpha says, as Quakespasm does.
===============
*/
static int	r_liquidvis;	// the kinds seen through: SURF_LAVA, SURF_SLIME, SURF_TELE, SURF_DRAWTURB for water

static int R_LiquidKind (int flags)
{
	return flags & (SURF_LAVA | SURF_SLIME | SURF_TELE) ? flags & (SURF_LAVA | SURF_SLIME | SURF_TELE) : SURF_DRAWTURB;
}

static void R_CheckLiquidVis (void)
{
	model_t		*world = r_scene.worldmodel;
	mleaf_t		*leaf;
	byte		*air, *vis;
	int			i, j, k, kinds;

	r_liquidvis = 0;
	air = Mem_Calloc ((size_t)world->visbytes, 1);
	for (i=0 ; i<world->numleafs ; i++)
		if (world->leafs[i+1].contents == CONTENTS_EMPTY)
			air[i>>3] |= 1<<(i&7);

	for (i=0 ; i<world->numleafs ; i++)
	{
		leaf = &world->leafs[i+1];
		if (leaf->contents != CONTENTS_WATER && leaf->contents != CONTENTS_SLIME && leaf->contents != CONTENTS_LAVA)
			continue;
		kinds = 0;
		for (j=0 ; j<leaf->nummarksurfaces ; j++)
			if (leaf->firstmarksurface[j]->flags & SURF_DRAWTURB)
				kinds |= R_LiquidKind (leaf->firstmarksurface[j]->flags);
		if (!(kinds & ~r_liquidvis))
			continue;		// nothing new to learn here
		vis = Mod_LeafPVS (leaf, world);
		for (k=0 ; k<world->visbytes ; k++)
			if (vis[k] & air[k])
			{
				r_liquidvis |= kinds;
				break;
			}
	}
	Mem_Free (air);
	Con_DPrintf ("Liquids the map sees through:%s%s%s%s\n", r_liquidvis & SURF_DRAWTURB ? " water" : "",
		r_liquidvis & SURF_LAVA ? " lava" : "", r_liquidvis & SURF_SLIME ? " slime" : "",
		r_liquidvis & SURF_TELE ? " tele" : "");
}

/*
===============
R_NewMap
===============
*/
void R_NewMap (void)
{
	int		i;

	memset (&r_worldentity, 0, sizeof(r_worldentity));
	r_worldentity.model = r_scene.worldmodel;

// clear out efrags in case the level hasn't been reloaded
// FIXME: is this one short?
	for (i=0 ; i<r_scene.worldmodel->numleafs ; i++)
		r_scene.worldmodel->leafs[i].efrags = NULL;
		 	
	r_viewleaf = NULL;
	R_ClearParticles ();
	R_CheckLiquidVis ();

	r_maxedgesseen = 0;
	r_maxsurfsseen = 0;

	R_AllocEdges ((int)r_maxedges.value, (int)r_maxsurfs.value);

	r_dowarpold = false;
	r_viewchanged = false;
}

/*
===============
R_ViewChanged

Called every time the vid structure or r_refdef changes.
Guaranteed to be called before the first refresh
===============
*/
void R_ViewChanged (vrect_t *vrect, float aspect)
{
	r_viewrect = *vrect;
	r_viewaspect = aspect;
	R_SetViewRect (vrect, aspect);
	r_viewchanged = true;
}

/*
===============
R_SetViewRect

Sets up the projection for rendering into vrect of the view buffer
===============
*/
void R_SetViewRect (const vrect_t *vrect, float aspect)
{
	int		i;
	float	res_scale;

	r_refdef.vrect = *vrect;

	r_refdef.horizontalFieldOfView = 2.0f * tanf((float)(r_refdef.fov_x/360*Q_PI));
	r_refdef.fvrectx = (float)r_refdef.vrect.x;
	r_refdef.fvrectx_adj = (float)r_refdef.vrect.x - 0.5f;
	r_refdef.vrect_x_adj_shift20 = ((int64_t)r_refdef.vrect.x<<20) + (1<<19) - 1;
	r_refdef.fvrecty = (float)r_refdef.vrect.y;
	r_refdef.fvrecty_adj = (float)r_refdef.vrect.y - 0.5f;
	r_refdef.vrectright = r_refdef.vrect.x + r_refdef.vrect.width;
	r_refdef.vrectright_adj_shift20 = ((int64_t)r_refdef.vrectright<<20) + (1<<19) - 1;
	r_refdef.fvrectright = (float)r_refdef.vrectright;
	r_refdef.fvrectright_adj = (float)r_refdef.vrectright - 0.5f;
	r_refdef.vrectrightedge = (float)r_refdef.vrectright - 0.99f;
	r_refdef.vrectbottom = r_refdef.vrect.y + r_refdef.vrect.height;
	r_refdef.fvrectbottom = (float)r_refdef.vrectbottom;
	r_refdef.fvrectbottom_adj = (float)r_refdef.vrectbottom - 0.5f;

	r_refdef.aliasvrect.x = (int)(r_refdef.vrect.x * r_aliasuvscale);
	r_refdef.aliasvrect.y = (int)(r_refdef.vrect.y * r_aliasuvscale);
	r_refdef.aliasvrect.width = (int)(r_refdef.vrect.width * r_aliasuvscale);
	r_refdef.aliasvrect.height = (int)(r_refdef.vrect.height * r_aliasuvscale);
	r_refdef.aliasvrectright = r_refdef.aliasvrect.x +
			r_refdef.aliasvrect.width;
	r_refdef.aliasvrectbottom = r_refdef.aliasvrect.y +
			r_refdef.aliasvrect.height;

	pixelAspect = aspect;
	xOrigin = r_refdef.xOrigin;
	yOrigin = r_refdef.yOrigin;
	
	screenAspect = r_refdef.vrect.width*pixelAspect /
			r_refdef.vrect.height;
// 320*200 1.0 pixelAspect = 1.6 screenAspect
// 320*240 1.0 pixelAspect = 1.3333 screenAspect
// proper 320*200 pixelAspect = 0.8333333

	verticalFieldOfView = r_refdef.horizontalFieldOfView / screenAspect;

// values for perspective projection
// if math were exact, the values would range from 0.5 to to range+0.5
// hopefully they wll be in the 0.000001 to range+.999999 and truncate
// the polygon rasterization will never render in the first row or column
// but will definately render in the [range] row and column, so adjust the
// buffer origin to get an exact edge to edge fill
	xcenter = (float)(((float)r_refdef.vrect.width * XCENTERING) +
			r_refdef.vrect.x - 0.5f);
	aliasxcenter = xcenter * r_aliasuvscale;
	ycenter = (float)(((float)r_refdef.vrect.height * YCENTERING) +
			r_refdef.vrect.y - 0.5f);
	aliasycenter = ycenter * r_aliasuvscale;

	xscale = r_refdef.vrect.width / r_refdef.horizontalFieldOfView;
	aliasxscale = xscale * r_aliasuvscale;
	xscaleinv = 1.0f / xscale;
	yscale = xscale * pixelAspect;
	aliasyscale = yscale * r_aliasuvscale;
	yscaleinv = 1.0f / yscale;
	xscaleshrink = (r_refdef.vrect.width-6)/r_refdef.horizontalFieldOfView;
	yscaleshrink = xscaleshrink*pixelAspect;

// left side clip
	screenedge[0].normal[0] = -1.0f / (xOrigin*r_refdef.horizontalFieldOfView);
	screenedge[0].normal[1] = 0;
	screenedge[0].normal[2] = 1;
	screenedge[0].type = PLANE_ANYZ;
	
// right side clip
	screenedge[1].normal[0] =
			1.0f / ((1.0f-xOrigin)*r_refdef.horizontalFieldOfView);
	screenedge[1].normal[1] = 0;
	screenedge[1].normal[2] = 1;
	screenedge[1].type = PLANE_ANYZ;
	
// top side clip
	screenedge[2].normal[0] = 0;
	screenedge[2].normal[1] = -1.0f / (yOrigin*verticalFieldOfView);
	screenedge[2].normal[2] = 1;
	screenedge[2].type = PLANE_ANYZ;
	
// bottom side clip
	screenedge[3].normal[0] = 0;
	screenedge[3].normal[1] = 1.0f / ((1.0f-yOrigin)*verticalFieldOfView);
	screenedge[3].normal[2] = 1;	
	screenedge[3].type = PLANE_ANYZ;
	
	for (i=0 ; i<4 ; i++)
		VectorNormalize (screenedge[i].normal);

	res_scale = (float)(sqrt ((double)(r_refdef.vrect.width * r_refdef.vrect.height) /
			          (320.0f * 152.0f)) *
			(2.0 / r_refdef.horizontalFieldOfView));
	r_aliastransition = r_aliastransbase.value * res_scale;
	r_resfudge = r_aliastransadj.value * res_scale;

// TODO: collect 386-specific code in one place

	D_ViewChanged ();
}


/*
===============
R_MarkLeaves
===============
*/
static void R_MarkLeaves (void)
{
	static bool	oldnovis;
	byte	*vis;
	mnode_t	*node;
	int		i;

	if (r_oldviewleaf == r_viewleaf && oldnovis == (r_novis.value != 0))
		return;
	
	r_visframecount++;
	r_oldviewleaf = r_viewleaf;
	oldnovis = r_novis.value != 0;

	vis = oldnovis ? r_scene.worldmodel->novis : Mod_LeafPVS (r_viewleaf, r_scene.worldmodel);
		
	for (i=0 ; i<r_scene.worldmodel->numleafs ; i++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			node = (mnode_t *)&r_scene.worldmodel->leafs[i+1];
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
}


/*
=============
R_EntityAlpha

How opaque an entity is drawn, of 256
=============
*/
int R_EntityAlpha (const entity_t *ent)
{
	if (!ent->alpha || ent->alpha >= 255)
		return 256;
	return (ent->alpha * 256 + 127) / 254;
}

/*
=============
R_SurfaceAlpha

How opaque a surface of currententity is drawn, of 256: the entity's alpha,
and a liquid's r_*alpha where the map sees through it (liquids of brush
models don't fill leafs of their own and always may)
=============
*/
int R_SurfaceAlpha (const msurface_t *surf)
{
	float	alpha;
	int		a;

	if (surf->flags & (SURF_DRAWSKY | SURF_DRAWBACKGROUND))
		return 256;
	alpha = R_EntityAlpha (currententity) / 256.0f;
	if ((surf->flags & SURF_DRAWTURB) &&
		(currententity != &r_worldentity || r_novis.value || (r_liquidvis & R_LiquidKind (surf->flags))))
	{
		if (surf->flags & SURF_LAVA)
			alpha *= r_lavaalpha.value;
		else if (surf->flags & SURF_SLIME)
			alpha *= r_slimealpha.value;
		else if (surf->flags & SURF_TELE)
			alpha *= r_telealpha.value;
		else
			alpha *= r_wateralpha.value;
	}
	a = (int)(alpha * 256 + 0.5f);
	return a < 0 ? 0 : a > 256 ? 256 : a;
}

/*
=============
R_DrawAliasEntity

currententity, an alias model, lit from the world and the dynamic lights
=============
*/
void R_DrawAliasEntity (void)
{
	int			j;
	int			lnum;
	alight_t	lighting;
	vec3_t		rgb;
	float		color[3];
// FIXME: remove and do real lighting
	float		lightvec[3] = {-1, 0, 0};
	vec3_t		dist;
	float		add;

	VectorCopy (currententity->origin, r_entorigin);
	VectorSubtract (r_origin, r_entorigin, modelorg);

// see if the bounding box lets us trivially reject, also sets
// trivial accept status
	if (!R_AliasCheckBBox ())
		return;

	j = R_LightPoint (currententity->origin, rgb);

	lighting.ambientlight = j;
	lighting.shadelight = j;

	lighting.plightvec = lightvec;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if (r_scene.dlights[lnum].die >= r_scene.time)
		{
			VectorSubtract (currententity->origin,
							r_scene.dlights[lnum].origin,
							dist);
			add = r_scene.dlights[lnum].radius - Length(dist);

			if (add > 0)
			{
				lighting.ambientlight = (int)(lighting.ambientlight + add);
				R_DlightColor (&r_scene.dlights[lnum], color);
				VectorMA (rgb, add, color, rgb);
			}
		}
	}
	R_LightTint (rgb, lighting.color);

// clamp lighting so it doesn't overbright as much
	if (lighting.ambientlight > 128)
		lighting.ambientlight = 128;
	if (lighting.ambientlight + lighting.shadelight > 192)
		lighting.shadelight = 192 - lighting.ambientlight;

	R_AliasDrawModel (&lighting);
}

/*
=============
R_DrawEntitiesOnList

Translucent alias models go to R_DrawTranslucent, drawn back to front with
the translucent surfaces
=============
*/
static void R_DrawEntitiesOnList (void)
{
	int			i;

	if (!r_drawentities.value)
		return;

	for (i=0 ; i<(*r_scene.numvisedicts) ; i++)
	{
		currententity = &r_scene.visedicts[i];

		switch (currententity->model->type)
		{
		case mod_sprite:
			VectorCopy (currententity->origin, r_entorigin);
			VectorSubtract (r_origin, r_entorigin, modelorg);
			R_DrawSprite ();
			break;

		case mod_alias:
			if (R_EntityAlpha (currententity) < 256)
				R_AddTranslucentEntity (currententity);
			else
				R_DrawAliasEntity ();
			break;

		default:
			break;
		}
	}
}

/*
=============
R_DrawViewModel
=============
*/
static void R_DrawViewModel (void)
{
	vec3_t		rgb;
	float		color[3];
// FIXME: remove and do real lighting
	float		lightvec[3] = {-1, 0, 0};
	int			j;
	int			lnum;
	vec3_t		dist;
	float		add;
	dlight_t	*dl;
	
	float		saved[6], width;

	if (!r_drawviewmodel.value || !r_scene.drawviewmodel)
		return;


	currententity = r_scene.viewent;
	if (!currententity->model)
		return;

	VectorCopy (currententity->origin, r_entorigin);
	VectorSubtract (r_origin, r_entorigin, modelorg);

	VectorCopy (vup, viewlightvec);
	VectorInverse (viewlightvec);

	j = R_LightPoint (currententity->origin, rgb);

	if (j < 24)
		j = 24;		// allways give some light on gun
	r_viewlighting.ambientlight = j;
	r_viewlighting.shadelight = j;

// add dynamic lights		
	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		dl = &r_scene.dlights[lnum];
		if (!dl->radius)
			continue;
		if (!dl->radius)
			continue;
		if (dl->die < r_scene.time)
			continue;

		VectorSubtract (currententity->origin, dl->origin, dist);
		add = dl->radius - Length(dist);
		if (add > 0)
		{
			r_viewlighting.ambientlight = (int)(r_viewlighting.ambientlight + add);
			R_DlightColor (dl, color);
			VectorMA (rgb, add, color, rgb);
		}
	}
	R_LightTint (rgb, r_viewlighting.color);

// clamp lighting so it doesn't overbright as much
	if (r_viewlighting.ambientlight > 128)
		r_viewlighting.ambientlight = 128;
	if (r_viewlighting.ambientlight + r_viewlighting.shadelight > 192)
		r_viewlighting.shadelight = 192 - r_viewlighting.ambientlight;

	r_viewlighting.plightvec = lightvec;

	// the gun's own field of view, the rest of the projection as the scene's
	saved[0] = xscale;
	saved[1] = yscale;
	saved[2] = xscaleinv;
	saved[3] = yscaleinv;
	saved[4] = aliasxscale;
	saved[5] = aliasyscale;
	if (r_refdef.viewmodel_fov_x > 0)
	{
		width = 2.0f * tanf (r_refdef.viewmodel_fov_x / 360 * (float)Q_PI);
		xscale = r_refdef.vrect.width / width;
		yscale = xscale * pixelAspect;
		xscaleinv = 1.0f / xscale;
		yscaleinv = 1.0f / yscale;
		aliasxscale = xscale * r_aliasuvscale;
		aliasyscale = yscale * r_aliasuvscale;
	}

	R_AliasDrawModel (&r_viewlighting);

	xscale = saved[0];
	yscale = saved[1];
	xscaleinv = saved[2];
	yscaleinv = saved[3];
	aliasxscale = saved[4];
	aliasyscale = saved[5];
}


/*
=============
R_BmodelCheckBBox
=============
*/
static int R_BmodelCheckBBox (model_t *clmodel, float *minmaxs)
{
	int			i, *pindex, clipflags;
	vec3_t		acceptpt, rejectpt;
	double		d;

	clipflags = 0;

	if (currententity->angles[0] || currententity->angles[1]
		|| currententity->angles[2])
	{
		for (i=0 ; i<4 ; i++)
		{
			d = DotProduct (currententity->origin, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d <= -clmodel->radius)
				return BMODEL_FULLY_CLIPPED;

			if (d <= clmodel->radius)
				clipflags |= (1<<i);
		}
	}
	else
	{
		for (i=0 ; i<4 ; i++)
		{
		// generate accept and reject points
		// FIXME: do with fast look-ups or integer tests based on the sign bit
		// of the floating point values

			pindex = pfrustum_indexes[i];

			rejectpt[0] = minmaxs[pindex[0]];
			rejectpt[1] = minmaxs[pindex[1]];
			rejectpt[2] = minmaxs[pindex[2]];
			
			d = DotProduct (rejectpt, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d <= 0)
				return BMODEL_FULLY_CLIPPED;

			acceptpt[0] = minmaxs[pindex[3+0]];
			acceptpt[1] = minmaxs[pindex[3+1]];
			acceptpt[2] = minmaxs[pindex[3+2]];

			d = DotProduct (acceptpt, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d <= 0)
				clipflags |= (1<<i);
		}
	}

	return clipflags;
}


/*
=============
R_DrawBEntitiesOnList
=============
*/
static void R_DrawBEntitiesOnList (void)
{
	int			i, j, k, clipflags;
	vec3_t		oldorigin;
	model_t		*clmodel;
	float		minmaxs[6];

	if (!r_drawentities.value)
		return;

	VectorCopy (modelorg, oldorigin);
	insubmodel = true;
	r_dlightframecount = r_framecount;

	for (i=0 ; i<(*r_scene.numvisedicts) ; i++)
	{
		currententity = &r_scene.visedicts[i];

		switch (currententity->model->type)
		{
		case mod_brush:

			clmodel = currententity->model;

		// see if the bounding box lets us trivially reject, also sets
		// trivial accept status
			for (j=0 ; j<3 ; j++)
			{
				minmaxs[j] = currententity->origin[j] +
						clmodel->mins[j];
				minmaxs[3+j] = currententity->origin[j] +
						clmodel->maxs[j];
			}

			clipflags = R_BmodelCheckBBox (clmodel, minmaxs);

			if (clipflags != BMODEL_FULLY_CLIPPED)
			{
				VectorCopy (currententity->origin, r_entorigin);
				VectorSubtract (r_origin, r_entorigin, modelorg);
			// FIXME: is this needed?
				VectorCopy (modelorg, r_worldmodelorg);
		
				r_pcurrentvertbase = clmodel->vertexes;
		
			// FIXME: stop transforming twice
				R_RotateBmodel ();

			// calculate dynamic lighting for bmodel if it's not an
			// instanced model
				if (clmodel->firstmodelsurface != 0)
				{
					for (k=0 ; k<MAX_DLIGHTS ; k++)
					{
						if ((r_scene.dlights[k].die < r_scene.time) ||
							(!r_scene.dlights[k].radius))
						{
							continue;
						}

						R_MarkLights (&r_scene.dlights[k], 1<<k,
							clmodel->nodes + clmodel->firstnode);
					}
				}

			// a translucent one is drawn after the models, blended, its faces
			// sorted with the other translucent things
				if (R_EntityAlpha (currententity) < 256)
				{
					R_AddTranslucentModel (clmodel);
				}
			// if the driver wants polygons, deliver those. Z-buffering is on
			// at this point, so no clipping to the world tree is needed, just
			// frustum clipping
				else if (r_drawpolys | r_drawculledpolys)
				{
					R_ZDrawSubmodelPolys (clmodel);
				}
				else
				{
					r_pefragtopnode = NULL;

					for (j=0 ; j<3 ; j++)
					{
						r_emins[j] = minmaxs[j];
						r_emaxs[j] = minmaxs[3+j];
					}

					R_SplitEntityOnNode2 (r_scene.worldmodel->nodes);

					if (r_pefragtopnode)
					{
						currententity->topnode = r_pefragtopnode;
	
						if (r_pefragtopnode->contents >= 0)
						{
						// not a leaf; has to be clipped to the world BSP
							r_clipflags = clipflags;
							R_DrawSolidClippedSubmodelPolygons (clmodel);
						}
						else
						{
						// falls entirely in one leaf, so we just put all the
						// edges in the edge list and let 1/z sorting handle
						// drawing order
							R_DrawSubmodelPolygons (clmodel, clipflags);
						}
	
						currententity->topnode = NULL;
					}
				}

			// put back world rotation and frustum clipping		
			// FIXME: R_RotateBmodel should just work off base_vxx
				VectorCopy (base_vpn, vpn);
				VectorCopy (base_vup, vup);
				VectorCopy (base_vright, vright);
				VectorCopy (base_modelorg, modelorg);
				VectorCopy (oldorigin, modelorg);
				R_TransformFrustum ();
			}

			break;

		default:
			break;
		}
	}

	insubmodel = false;
}


/*
================
R_EdgeDrawing
================
*/
static void R_EdgeDrawing (void)
{
	double	prof;

	if (!r_edges)
		R_AllocEdges (MINEDGES, MINSURFACES);

// nothing is drawn until the spans are scanned, so a frame that runs out
// of edges or surfaces is built again with more
	while (1)
	{
		r_outofsurfaces = 0;
		r_outofedges = 0;

		R_BeginEdgeFrame ();
		R_ClearFences ();
		prof = R_ProfStart ();

		if (r_dspeeds.value)
		{
			rw_time1 = (float)Sys_DoubleTime ();
		}

		R_RenderWorld ();

		if (r_dspeeds.value)
		{
			rw_time2 = (float)Sys_DoubleTime ();
			db_time1 = rw_time2;
		}

		R_DrawBEntitiesOnList ();
		R_ProfEnd (PROF_EDGES, prof);

		if (!r_outofsurfaces && !r_outofedges)
			break;
		R_AllocEdges (r_outofedges ? r_numallocatededges * 2 : r_numallocatededges,
			r_outofsurfaces ? r_cnumsurfs * 2 : r_cnumsurfs);
	}

	if (r_dspeeds.value)
	{
		db_time2 = (float)Sys_DoubleTime ();
		se_time1 = db_time2;
	}

	prof = R_ProfStart ();
	R_ScanEdges ();
	R_DrawFences ();
	R_ProfEnd (PROF_SPANS, prof);
}


/*
================
R_RenderView

r_refdef must be set before the first call
================
*/
void R_RenderView (void)
{
	double	prof;

	if (r_profile.value)
	{
		if (!r_profframes)
			r_profsince = Sys_DoubleTime ();
		r_profframes++;
	}
	if (r_timegraph.value || r_speeds.value || r_dspeeds.value)
		r_time1 = (float)Sys_DoubleTime ();

	R_CheckLightSettings ();
	R_SetupFrame ();

	R_MarkLeaves ();	// done here so we know if we're in water

// make FDIV fast. This reduces timing precision after we've been running for a
// while, so we don't do it globally.  This also sets chop mode, and we do it
// here so that setup stuff like the refresh area calculations match what's
// done in screen.c

	if (!r_worldentity.model || !r_scene.worldmodel)
		Sys_Error ("R_RenderView: NULL worldmodel");
		
	R_EdgeDrawing ();

	if (r_dspeeds.value)
	{
		se_time2 = (float)Sys_DoubleTime ();
		de_time1 = se_time2;
	}

	prof = R_ProfStart ();
	R_DrawEntitiesOnList ();
	R_DrawTranslucent ();
	R_DrawRings ();
	R_ProfEnd (PROF_MODELS, prof);

	if (r_dspeeds.value)
	{
		de_time2 = (float)Sys_DoubleTime ();
		dv_time1 = de_time2;
	}

	prof = R_ProfStart ();
	R_DrawViewModel ();
	R_ProfEnd (PROF_VIEWMODEL, prof);

	if (r_dspeeds.value)
	{
		dv_time2 = (float)Sys_DoubleTime ();
		dp_time1 = (float)Sys_DoubleTime ();
	}

	prof = R_ProfStart ();
	R_DrawParticles ();
	R_ProfEnd (PROF_PARTICLES, prof);

	if (r_dspeeds.value)
		dp_time2 = (float)Sys_DoubleTime ();

	prof = R_ProfStart ();
	if (r_dowarp)
		D_WarpScreen ();
	R_ProfEnd (PROF_WARP, prof);
	if (r_profile.value && r_cache_thrash)
		r_profthrash++;

	r_scene.viewcontents = r_viewleaf->contents;

	if (r_timegraph.value)
		R_TimeGraph ();

	if (r_zgraph.value)
		R_ZGraph ();

	if (r_aliasstats.value)
		R_PrintAliasStats ();
		
	if (r_speeds.value)
		R_PrintTimes ();

	if (r_dspeeds.value)
		R_PrintDSpeeds ();

	if (r_reportsurfout.value && r_outofsurfaces)
		Con_Printf ("Short %d surfaces\n", r_outofsurfaces);

	if (r_reportedgeout.value && r_outofedges)
		Con_Printf ("Short roughly %d edges\n", r_outofedges * 2 / 3);

// back to high floating-point precision
}

/*
================
R_InitTurb
================
*/
static void R_InitTurb (void)
{
	int		i;

	for (i=0 ; i<CYCLE*2 ; i++)
		sintable[i] = (int)(AMP + sin(i*3.14159*2/CYCLE)*AMP);
}

/*
================
R_SetWarpTable

3.14159 is not quite pi, so the table isn't periodic; it is read with an
offset within a cycle, one screen of the 320x200 layout wide or high
================
*/
void R_SetWarpTable (int size)
{
	int		i;

	Mem_Free (intsintable);
	intsintable = Mem_Alloc ((size_t)(size + CYCLE) * sizeof(*intsintable));
	for (i=0 ; i<size+CYCLE ; i++)
		intsintable[i] = (int)(AMP2 + sin(i*3.14159*2/CYCLE)*AMP2);	// AMP2, not 20
}

