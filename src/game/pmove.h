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


// how the player moves (ZQuake's, as mvdsv and ezQuake have it)
typedef enum
{
	PM_NORMAL,			// walking, swimming, jumping
	PM_OLD_SPECTATOR,	// flying through walls, QuakeWorld's way
	PM_SPECTATOR,		// flying through walls
	PM_DEAD,			// no acceleration, no jumping
	PM_FLY,				// flying, bumping into walls
	PM_NONE,			// not moving
	PM_LOCK				// the server moves the player and turns the view
} pmtype_t;

typedef struct
{
	int		sequence;	// just for debugging prints

	// player state
	vec3_t	origin;
	vec3_t	angles;
	vec3_t	velocity;
	bool	jump_held;	// don't jump again until the button is released
	int		jump_msec;	// msec since a jump, for 50 of them
	float		waterjumptime;
	int		pm_type;	// pmtype_t

	// world state
	int		numphysent;
	physent_t	physents[MAX_PHYSENTS];	// 0 should be the world

	// input
	usercmd_t	cmd;

	// results
	int		numtouch;
	int		touchindex[MAX_PHYSENTS];
	bool	onground;		// also an input with pm_pground
	int		groundent;		// the physent stood on, while onground
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

	// the serverinfo's pm_ keys (mvdsv's)
	float	bunnyspeedcap;	// speed gained in the air stops at this many times maxspeed; 0 off
	float	ktjump;			// how much of a full jump a jump while descending gets
	bool	slidefix;		// gravity on the ground too: down ramps as NetQuake goes
	bool	airstep;		// up steps in the air
	bool	pground;		// the ground is found by landing on it, and kept (needs Z_EXT_PF_ONGROUND)
	bool	rampjump;		// the ground holds a player moving up a steep ramp longer; the jump fix always
} movevars_t;


// the player's bounding box
extern	vec3_t	player_mins;
extern	vec3_t	player_maxs;

// runs pm->cmd: origin, angles and velocity are modified in place and the
// results filled in; returns what the move bumped into
int		PM_PlayerMove (playermove_t *pm, const movevars_t *mv);

// turns a command's movement, its forward and side moves, by degrees of yaw;
// its angles stay (MVD1 high-lag teleport, both ends)
void	PM_RotateMove (usercmd_t *cmd, float degrees);

// how far the view rolls when strafing at velocity
// the view roll while strafing, from cl_rollangle and cl_rollspeed
float	PM_CalcRoll (const vec3_t angles, const vec3_t velocity);
void	PM_Init (void);		// registers the variables pmove reads

int		PM_PointContents (const playermove_t *pm, const vec3_t point);
// of the world and the brush models, solid first (for the water jump)
int		PM_PointContentsAllBSPs (const playermove_t *pm, const vec3_t point);
bool	PM_TestPlayerPosition (const playermove_t *pm, const vec3_t point);

// traces the player's box against pm's physents
trace_t	PM_PlayerTrace (const playermove_t *pm, const vec3_t start, const vec3_t stop);
