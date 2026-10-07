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
// slist_routes.c -- the shortest ways to the servers through relays: the
// relaxation of every relay's edges until a pass improves none (a chain
// through two relays falls out of it unasked)

#include "slist_local.h"

void SL_Routes (const int *direct, int numhosts, const slrelay_t *relays, int numrelays, int *cost, int *via)
{
	const slrelay_t	*r;
	bool		improved;
	int			pass, i, j, peer, total;

	for (i = 0 ; i < numhosts ; i++)
	{
		cost[i] = direct[i];
		via[i] = -1;
	}

	// a pass can lengthen a route by a hop only, so the passes bound a chain;
	// most stop at the second, which improves nothing
	for (pass = 0 ; pass <= numrelays ; pass++)
	{
		improved = false;
		for (i = 0, r = relays ; i < numrelays ; i++, r++)
		{
			if (cost[r->host] < 0)
				continue;		// a relay we don't reach leads nowhere
			for (j = 0 ; j < r->numpeers ; j++)
			{
				peer = r->peers[j];
				if (peer == r->host)
					continue;
				total = cost[r->host] + r->ms[j];
				if (cost[peer] < 0 || total < cost[peer])
				{
					cost[peer] = total;
					via[peer] = r->host;
					improved = true;
				}
			}
		}
		if (!improved)
			break;
	}
}
