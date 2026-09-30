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
// r_skybox.c -- a sky of six images, the faces of a cube around the view,
// drawn in place of the sky's texture (D_DrawSkyboxScans). The images are
// named as Quake 2 named them: <name>rt, bk, lf, ft, up and dn looking along
// +x, +y, -x, -y, +z and -z, in env/ or gfx/env/.

#include "r_local.h"

#define	SKYBOX_MAXSIZE	2048		// larger faces are scaled down

static cvar_t	r_skybox = {.name = "r_skybox", .string = "", .archive = true,
	.description = "The skybox drawn in place of the sky's texture: six TGA images, <name>rt.tga, bk, lf, ft, "
		"up and dn (or <name>_rt.tga and so on), in env/ or gfx/env/. Empty for the map's own, the one its "
		"worldspawn's sky key names, if any."};

pixel_t	*r_skyfaces;
int		r_skyboxsize;

static char	r_skyboxname[MAX_QPATH];	// the one loaded, or tried
static bool	r_skyboxtried;

void R_SkyboxInit (void)
{
	Cvar_RegisterVariable (&r_skybox);
}

// the six faces, prefix and sep between the name and each face's suffix; false
// if the first isn't there, or (said on the console) another isn't
static bool R_LoadSkyboxFaces (const char *name, const char *prefix, const char *sep)
{
	static const char	*suffixes[SKYBOX_FACES] = {"rt", "bk", "lf", "ft", "up", "dn"};
	char		path[MAX_QPATH];
	byte		*rgba;
	pixel_t		*faces = NULL;
	int			i, w, h, size = 0;

	for (i = 0 ; i < SKYBOX_FACES ; i++)
	{
		snprintf (path, sizeof(path), "%s%s%s%s.tga", prefix, name, sep, suffixes[i]);
		rgba = R_LoadTGA (path, &w, &h);
		if (!rgba)
		{
			if (i)
				Con_Printf ("Skybox %s has no %s\n", name, path);
			Mem_Free (faces);
			return false;
		}
		if (!i)
		{
			// every face is made the size of the first
			size = w < h ? w : h;
			if (size > SKYBOX_MAXSIZE)
				size = SKYBOX_MAXSIZE;
			faces = Mem_Alloc ((size_t)SKYBOX_FACES * size * size * sizeof(*faces));
		}
		R_ImagePixels (rgba, w, h, faces + (size_t)i * size * size, size, size, false);
		Mem_Free (rgba);
	}

	Mem_Free (r_skyfaces);
	r_skyfaces = faces;
	r_skyboxsize = size;
	return true;
}

/*
===============
R_LoadSkybox

The skybox name names, from the first of env/<name>_rt.tga, env/<name>rt.tga,
gfx/env/<name>_rt.tga and gfx/env/<name>rt.tga there is, as FTE looks for
them; none for an empty name, or one that isn't there
===============
*/
static void R_LoadSkybox (const char *name)
{
	static const char	*prefixes[] = {"env/", "gfx/env/"};
	static const char	*seps[] = {"_", ""};
	int		i, j;

	Q_strncpyz (r_skyboxname, name, sizeof(r_skyboxname));
	r_skyboxtried = true;
	Mem_Free (r_skyfaces);
	r_skyfaces = NULL;
	r_skyboxsize = 0;

	if (!name[0])
		return;
	if (strstr (name, ".."))
	{
		Con_Printf ("Skybox %s is outside the game directory\n", name);
		return;
	}
	for (i = 0 ; i < (int)(sizeof(prefixes) / sizeof(prefixes[0])) ; i++)
		for (j = 0 ; j < (int)(sizeof(seps) / sizeof(seps[0])) ; j++)
			if (R_LoadSkyboxFaces (name, prefixes[i], seps[j]))
				return;
	Con_Printf ("Couldn't load skybox %s\n", name);
}

/*
===============
R_CheckSkybox

The skybox r_skybox names, or else the map's, loaded if it isn't; with
reload, loaded again even if it is (a new map)
===============
*/
void R_CheckSkybox (bool reload)
{
	const char	*name = r_skybox.string[0] ? r_skybox.string : r_worldspawn.sky;

	if (reload || !r_skyboxtried || strcmp (name, r_skyboxname))
		R_LoadSkybox (name);
}
