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

#include "cmodel.h"
#include "protocol.h"

#define	MAX_PHYSENTS	32
typedef struct
{
	vec3_t	origin;
	const cmodel_t	*model;	// only for bsp models
	vec3_t	mins, maxs;	// only for non-bsp models
	int		info;		// for client or server to identify
} physent_t;


typedef struct
{
	int		sequence;	// just for debugging prints

	// player state
	vec3_t	origin;
	vec3_t	angles;
	vec3_t	velocity;
	int		oldbuttons;
	float		waterjumptime;
	bool	dead;
	int		spectator;

	// world state
	int		numphysent;
	physent_t	physents[MAX_PHYSENTS];	// 0 should be the world

	// input
	usercmd_t	cmd;

	// results
	int		numtouch;
	int		touchindex[MAX_PHYSENTS];
	int		onground;		// physent the player stands on, -1 if in the air
	int		waterlevel;		// 0 dry .. 3 head under
	int		watertype;		// CONTENTS_ of the water
} playermove_t;

typedef struct {
	float	gravity;
	float	stopspeed;
	float	maxspeed;
	float	spectatormaxspeed;
	float	accelerate;
	float	airaccelerate;
	float	wateraccelerate;
	float	friction;
	float	waterfriction;
	float	entgravity;
} movevars_t;


// the player's bounding box
extern	vec3_t	player_mins;
extern	vec3_t	player_maxs;

// runs pm->cmd: origin, angles and velocity are modified in place and the
// results filled in
void	PM_PlayerMove (playermove_t *pm, const movevars_t *mv);

// how far the view rolls when strafing at velocity
// the view roll while strafing, from cl_rollangle and cl_rollspeed
float	PM_CalcRoll (const vec3_t angles, const vec3_t velocity);
void	PM_Init (void);		// registers the variables pmove reads

int		PM_PointContents (const playermove_t *pm, const vec3_t point);
bool	PM_TestPlayerPosition (const playermove_t *pm, const vec3_t point);

// traces the player's box against pm's physents
trace_t	PM_PlayerTrace (const playermove_t *pm, const vec3_t start, const vec3_t stop);
