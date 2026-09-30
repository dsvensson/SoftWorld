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
// r_worldspawn.c -- what the 3D view takes from a map's worldspawn entity, the
// first in its entity text: the skybox and the fog. As FTE reads them, a key
// may start with an underscore, "_sky" being "sky"; of "sky" and "skyname" the
// later one counts.

#include "r_local.h"

worldspawn_t	r_worldspawn;

/*
===============
R_ParseWorldspawn
===============
*/
void R_ParseWorldspawn (char *entities)
{
	char	key[64];
	char	*data = entities;

	memset (&r_worldspawn, 0, sizeof(r_worldspawn));
	if (!data)
		return;
	data = COM_Parse (data);
	if (!data || com_token[0] != '{')
		return;

	while (1)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] == '}')
			break;
		Q_strncpyz (key, com_token[0] == '_' ? com_token + 1 : com_token, sizeof(key));
		data = COM_Parse (data);
		if (!data)
			break;

		if (!strcmp (key, "sky") || !strcmp (key, "skyname"))
			Q_strncpyz (r_worldspawn.sky, com_token, sizeof(r_worldspawn.sky));
		else if (!strcmp (key, "fog"))
			Q_strncpyz (r_worldspawn.fog, com_token, sizeof(r_worldspawn.fog));
		else if (!strcmp (key, "skyfog"))
		{
			r_worldspawn.skyfog = Q_atof (com_token);
			r_worldspawn.hasskyfog = true;
		}
	}
}
