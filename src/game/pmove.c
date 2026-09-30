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

#include "cvar.h"
#include "pmove.h"
#include "print.h"
#include "sys.h"

#include <math.h>
#include <string.h>


// the move being run by PM_PlayerMove
static playermove_t		*pm;
static const movevars_t	*mv;

static float	frametime;
static vec3_t	forward, right, up;
static vec3_t	groundnormal;		// of the ground under the player, when on it

vec3_t	player_mins = {-16, -16, -24};
vec3_t	player_maxs = {16, 16, 32};

#define	STEPSIZE	18

#define	BUTTON_JUMP	2

#define	FLY_FRICTION	4			// PM_FLY

// what PM_SlideMove bumped into
#define	BLOCKED_FLOOR	1
#define	BLOCKED_STEP	2
#define	BLOCKED_OTHER	4

#define	MIN_STEP_NORMAL			0.7f	// roughly 45 degrees
#define	MAX_JUMPFIX_DOTPRODUCT	-0.1f	// moving into the ground

// rising faster than this is leaving the ground; pm_rampjump raises it on
// steep ramps
#define	MAXGROUNDSPEED_DEFAULT	180
#define	MAXGROUNDSPEED_MAXIMUM	240

static float	maxgroundspeed;

static cvar_t	cl_rollspeed = {.name = "cl_rollspeed", .string = "200",
	.description = "Sideways speed, in units a second, at which the view rolls all of cl_rollangle; "
		"slower rolls less."};
static cvar_t	cl_rollangle = {.name = "cl_rollangle", .string = "2.0",
	.description = "How far the view rolls moving sideways, in degrees; 0 is none. "
		"Players' models roll 4 times as far."};

/*
============
PM_Init
============
*/
void PM_Init (void)
{
	Cvar_RegisterVariable (&cl_rollspeed);
	Cvar_RegisterVariable (&cl_rollangle);
}

/*
============
PM_AddTouchedEnt

A physent touched in the move, once
============
*/
static void PM_AddTouchedEnt (int num)
{
	int		i;

	if (pm->numtouch == MAX_PHYSENTS)
		return;
	for (i=0 ; i<pm->numtouch ; i++)
		if (pm->touchindex[i] == num)
			return;
	pm->touchindex[pm->numtouch++] = num;
}

/*
==================
PM_ClipVelocity

Slide off of the impacting object
==================
*/
#define	STOP_EPSILON	0.1

static void PM_ClipVelocity (const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
	float	backoff, change;
	int		i;

	backoff = DotProduct (in, normal) * overbounce;
	for (i=0 ; i<3 ; i++)
	{
		change = normal[i]*backoff;
		out[i] = in[i] - change;
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
	}
}

/*
============
PM_SlideMove

The basic solid body movement clip that slides along multiple planes;
returns the BLOCKED_ flags
============
*/
#define	MAX_CLIP_PLANES	5

static int PM_SlideMove (void)
{
	int			bumpcount, numbumps;
	vec3_t		dir;
	float		d;
	int			numplanes;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity, original_velocity;
	int			i, j;
	trace_t		trace;
	vec3_t		end;
	float		time_left;
	int			blocked;

	numbumps = 4;
	blocked = 0;
	VectorCopy (pm->velocity, original_velocity);
	VectorCopy (pm->velocity, primal_velocity);
	numplanes = 0;

	time_left = frametime;

	for (bumpcount=0 ; bumpcount<numbumps ; bumpcount++)
	{
		VectorMA (pm->origin, time_left, pm->velocity, end);
		trace = PM_PlayerTrace (pm, pm->origin, end);

		if (trace.startsolid || trace.allsolid)
		{	// entity is trapped in another solid
			VectorCopy (vec3_origin, pm->velocity);
			return BLOCKED_FLOOR | BLOCKED_STEP;
		}

		if (trace.fraction > 0)
		{	// actually covered some distance
			VectorCopy (trace.endpos, pm->origin);
			numplanes = 0;
		}

		if (trace.fraction == 1)
			break;		// moved the entire distance

		// save entity for contact
		PM_AddTouchedEnt (trace.entnum);

		if (trace.plane.normal[2] >= MIN_STEP_NORMAL)
			blocked |= BLOCKED_FLOOR;
		else if (!trace.plane.normal[2])
			blocked |= BLOCKED_STEP;
		else
			blocked |= BLOCKED_OTHER;

		time_left -= time_left * trace.fraction;

		// cliped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{	// this shouldn't really happen
			VectorCopy (vec3_origin, pm->velocity);
			break;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

		// modify original_velocity so it parallels all of the clip planes
		for (i=0 ; i<numplanes ; i++)
		{
			PM_ClipVelocity (original_velocity, planes[i], pm->velocity, 1);
			for (j=0 ; j<numplanes ; j++)
				if (j != i && DotProduct (pm->velocity, planes[j]) < 0)
					break;	// not ok
			if (j == numplanes)
				break;
		}

		if (i == numplanes)
		{	// go along the crease
			if (numplanes != 2)
			{
				VectorCopy (vec3_origin, pm->velocity);
				break;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, pm->velocity);
			VectorScale (dir, d, pm->velocity);
		}

		// if velocity is against the original velocity, stop dead
		// to avoid tiny occilations in sloping corners
		if (DotProduct (pm->velocity, primal_velocity) <= 0)
		{
			VectorCopy (vec3_origin, pm->velocity);
			break;
		}
	}

	if (pm->waterjumptime)
		VectorCopy (primal_velocity, pm->velocity);

	return blocked;
}

/*
=============
PM_StepSlideMove

Each intersection will try to step over the obstruction instead of sliding
along it. In the air (pm_airstep) only a step with ground under it is
stepped onto.
=============
*/
static int PM_StepSlideMove (bool in_air)
{
	vec3_t	original, originalvel, down, uporg, downvel, dest;
	float	downdist, updist, stepsize, scale;
	float	*org;
	trace_t	trace;
	int		blocked;

	// try sliding forward both on ground and up 16 pixels
	// take the move that goes farthest
	VectorCopy (pm->origin, original);
	VectorCopy (pm->velocity, originalvel);

	blocked = PM_SlideMove ();
	if (!blocked)
		return blocked;		// moved the entire distance

	if (in_air)
	{	// only up a step that was bumped into, with ground under it
		if (!(blocked & BLOCKED_STEP))
			return blocked;

		org = originalvel[2] < 0 ? pm->origin : original;
		VectorCopy (org, dest);
		dest[2] -= STEPSIZE;
		trace = PM_PlayerTrace (pm, org, dest);
		if (trace.fraction == 1 || trace.plane.normal[2] < MIN_STEP_NORMAL)
			return blocked;

		// no higher than a step from the ground
		stepsize = STEPSIZE - (org[2] - trace.endpos[2]);
	}
	else
		stepsize = STEPSIZE;

	VectorCopy (pm->origin, down);
	VectorCopy (pm->velocity, downvel);

	VectorCopy (original, pm->origin);
	VectorCopy (originalvel, pm->velocity);

	// move up a stair height
	VectorCopy (pm->origin, dest);
	dest[2] += stepsize;
	trace = PM_PlayerTrace (pm, pm->origin, dest);
	if (!trace.startsolid && !trace.allsolid)
		VectorCopy (trace.endpos, pm->origin);

	if (in_air && originalvel[2] < 0)
		pm->velocity[2] = 0;

	PM_SlideMove ();

	// press down the stepheight
	VectorCopy (pm->origin, dest);
	dest[2] -= stepsize;
	trace = PM_PlayerTrace (pm, pm->origin, dest);
	if (trace.fraction != 1 && trace.plane.normal[2] < MIN_STEP_NORMAL)
		goto usedown;
	if (!trace.startsolid && !trace.allsolid)
		VectorCopy (trace.endpos, pm->origin);

	if (pm->origin[2] < original[2])
		goto usedown;

	VectorCopy (pm->origin, uporg);

	// decide which one went farther
	downdist = (down[0] - original[0])*(down[0] - original[0])
		+ (down[1] - original[1])*(down[1] - original[1]);
	updist = (uporg[0] - original[0])*(uporg[0] - original[0])
		+ (uporg[1] - original[1])*(uporg[1] - original[1]);

	if (downdist >= updist)
	{
usedown:
		VectorCopy (down, pm->origin);
		VectorCopy (downvel, pm->velocity);
		return blocked;
	}

	// copy z value from slide move
	pm->velocity[2] = downvel[2];

	if (!pm->onground && pm->waterlevel < 2 && (blocked & BLOCKED_STEP))
	{	// in the air (pm_airstep), a 16 unit step takes 16% of the speed
		scale = 1 - 0.01f*(pm->origin[2] - original[2]);
		pm->velocity[0] *= scale;
		pm->velocity[1] *= scale;
	}

	return blocked;
}

/*
==================
PM_Friction

Handles both ground friction and water friction
==================
*/
static void PM_Friction (void)
{
	float	speed, newspeed, control;
	float	friction;
	float	drop;
	vec3_t	start, stop;
	trace_t	trace;

	if (pm->waterjumptime)
		return;

	speed = Length (pm->velocity);
	if (speed < 1)
	{
		pm->velocity[0] = 0;
		pm->velocity[1] = 0;
		if (pm->pm_type == PM_FLY)
			pm->velocity[2] = 0;
		return;
	}

	if (pm->waterlevel >= 2)	// water friction, flying or not
		drop = speed*mv->waterfriction*pm->waterlevel*frametime;
	else if (pm->pm_type == PM_FLY)
		drop = speed*FLY_FRICTION*frametime;
	else if (pm->onground)
	{
		friction = mv->friction;

		// if the leading edge is over a dropoff, increase friction
		start[0] = stop[0] = pm->origin[0] + pm->velocity[0]/speed*16;
		start[1] = stop[1] = pm->origin[1] + pm->velocity[1]/speed*16;
		start[2] = pm->origin[2] + player_mins[2];
		stop[2] = start[2] - 34;
		trace = PM_PlayerTrace (pm, start, stop);
		if (trace.fraction == 1)
			friction *= 2;

		control = speed < mv->stopspeed ? mv->stopspeed : speed;
		drop = control*friction*frametime;
	}
	else
		return;		// in the air, no friction

	// scale the velocity
	newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;
	VectorScale (pm->velocity, newspeed, pm->velocity);
}

/*
==============
PM_Accelerate
==============
*/
static void PM_Accelerate (vec3_t wishdir, float wishspeed, float accel)
{
	float	addspeed, accelspeed, currentspeed;

	if (pm->pm_type == PM_DEAD)
		return;
	if (pm->waterjumptime)
		return;

	currentspeed = DotProduct (pm->velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel*frametime*wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	VectorMA (pm->velocity, accelspeed, wishdir, pm->velocity);
}

/*
==============
PM_AirAccelerate

With pm_bunnyspeedcap, speed gained in the air stops at that many times
maxspeed
==============
*/
static void PM_AirAccelerate (vec3_t wishdir, float wishspeed, float accel)
{
	float	addspeed, accelspeed, currentspeed, wishspd = wishspeed;
	float	originalspeed = 0, newspeed, speedcap;

	if (pm->pm_type == PM_DEAD)
		return;
	if (pm->waterjumptime)
		return;

	if (mv->bunnyspeedcap > 0)
		originalspeed = sqrtf (pm->velocity[0]*pm->velocity[0] + pm->velocity[1]*pm->velocity[1]);

	if (wishspd > 30)
		wishspd = 30;
	currentspeed = DotProduct (pm->velocity, wishdir);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel * wishspeed * frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	VectorMA (pm->velocity, accelspeed, wishdir, pm->velocity);

	if (mv->bunnyspeedcap > 0)
	{
		newspeed = sqrtf (pm->velocity[0]*pm->velocity[0] + pm->velocity[1]*pm->velocity[1]);
		speedcap = mv->maxspeed * mv->bunnyspeedcap;
		if (newspeed > originalspeed && newspeed > speedcap)
		{
			if (originalspeed < speedcap)
				originalspeed = speedcap;
			pm->velocity[0] *= originalspeed / newspeed;
			pm->velocity[1] *= originalspeed / newspeed;
		}
	}
}

/*
===================
PM_WaterMove
===================
*/
static int PM_WaterMove (void)
{
	int		i;
	vec3_t	wishvel;
	float	wishspeed;
	vec3_t	wishdir;

	// user intentions
	for (i=0 ; i<3 ; i++)
		wishvel[i] = forward[i]*pm->cmd.forwardmove + right[i]*pm->cmd.sidemove;

	if (pm->pm_type != PM_FLY && !pm->cmd.forwardmove && !pm->cmd.sidemove && !pm->cmd.upmove)
		wishvel[2] -= 60;		// drift towards bottom
	else
		wishvel[2] += pm->cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	if (wishspeed > mv->maxspeed)
	{
		VectorScale (wishvel, mv->maxspeed/wishspeed, wishvel);
		wishspeed = mv->maxspeed;
	}
	wishspeed *= 0.7f;

	// water acceleration
	PM_Accelerate (wishdir, wishspeed, mv->wateraccelerate);

	return PM_StepSlideMove (false);
}

/*
===================
PM_FlyMove

MOVETYPE_FLY: steering in three dimensions, bumping into walls
===================
*/
static int PM_FlyMove (void)
{
	int		i;
	vec3_t	wishvel;
	float	wishspeed;
	vec3_t	wishdir;

	for (i=0 ; i<3 ; i++)
		wishvel[i] = forward[i]*pm->cmd.forwardmove + right[i]*pm->cmd.sidemove;
	wishvel[2] += pm->cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	if (wishspeed > mv->maxspeed)
	{
		VectorScale (wishvel, mv->maxspeed/wishspeed, wishvel);
		wishspeed = mv->maxspeed;
	}

	PM_Accelerate (wishdir, wishspeed, mv->accelerate);
	return PM_StepSlideMove (false);
}

/*
===================
PM_AirMove

On the ground or in the air
===================
*/
static int PM_AirMove (void)
{
	int			i, blocked;
	vec3_t		wishvel;
	float		fmove, smove;
	vec3_t		wishdir;
	float		wishspeed;

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.sidemove;

	forward[2] = 0;
	right[2] = 0;
	VectorNormalize (forward);
	VectorNormalize (right);

	for (i=0 ; i<2 ; i++)
		wishvel[i] = forward[i]*fmove + right[i]*smove;
	wishvel[2] = 0;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	// clamp to server defined max speed
	if (wishspeed > mv->maxspeed)
	{
		VectorScale (wishvel, mv->maxspeed/wishspeed, wishvel);
		wishspeed = mv->maxspeed;
	}

	if (pm->onground)
	{
		if (mv->slidefix)
		{	// down ramps as NetQuake does: gravity on the ground too
			if (pm->velocity[2] > 0)
				pm->velocity[2] = 0;
			PM_Accelerate (wishdir, wishspeed, mv->accelerate);
			pm->velocity[2] -= mv->entgravity * mv->gravity * frametime;
		}
		else
		{
			pm->velocity[2] = 0;
			PM_Accelerate (wishdir, wishspeed, mv->accelerate);
		}

		if (!pm->velocity[0] && !pm->velocity[1])
		{
			pm->velocity[2] = 0;
			return 0;
		}
		return PM_StepSlideMove (false);
	}

	// not on ground, so little effect on velocity
	PM_AirAccelerate (wishdir, wishspeed, mv->accelerate);

	// add gravity
	pm->velocity[2] -= mv->entgravity * mv->gravity * frametime;

	blocked = mv->airstep ? PM_StepSlideMove (true) : PM_SlideMove ();

	// with pm_pground the ground is only found by landing on it
	if (mv->pground && (blocked & BLOCKED_FLOOR))
		pm->onground = true;

	return blocked;
}

/*
=============
PM_GroundTrace

The trace one unit down from the player; its plane becomes the ground
normal when it is ground
=============
*/
static bool PM_FarFromGround (const trace_t *trace)
{
	return trace->fraction == 1 || trace->plane.normal[2] < MIN_STEP_NORMAL;
}

static trace_t PM_GroundTrace (const vec3_t point)
{
	trace_t	trace;

	trace = PM_PlayerTrace (pm, pm->origin, point);
	if (!PM_FarFromGround (&trace))
		VectorCopy (trace.plane.normal, groundnormal);
	return trace;
}

/*
=============
PM_CategorizePosition

Sets onground, watertype and waterlevel
=============
*/
static void PM_CategorizePosition (void)
{
	vec3_t		point;
	int			cont;
	trace_t		trace = {.fraction = 1, .entnum = -1};
	float		range;

	maxgroundspeed = MAXGROUNDSPEED_DEFAULT;

	// if the player hull point one unit down is solid, the player
	// is on ground
	point[0] = pm->origin[0];
	point[1] = pm->origin[1];
	point[2] = pm->origin[2] - 1;

	if (mv->rampjump)
	{	// moving up a ramp of the world, the speed that leaves the ground
		// rises with its steepness, up to 45 degrees
		trace = PM_GroundTrace (point);
		if (!PM_FarFromGround (&trace) && trace.entnum == 0 && groundnormal[2] < 1
		 && DotProduct (groundnormal, pm->velocity) < MAX_JUMPFIX_DOTPRODUCT)
		{
			range = 1 - asinf (groundnormal[2]) * 2 / (float)Q_PI;
			if (range > 0.5f)
				range = 0.5f;
			maxgroundspeed += (int)((MAXGROUNDSPEED_MAXIMUM - MAXGROUNDSPEED_DEFAULT) * range * 2);
		}
	}

	if (pm->velocity[2] > maxgroundspeed)
		pm->onground = false;
	else if (!mv->pground || pm->onground)
	{
		if (!mv->rampjump)
			trace = PM_GroundTrace (point);
		if (PM_FarFromGround (&trace))
			pm->onground = false;
		else
		{
			pm->onground = true;
			pm->groundent = trace.entnum;
			pm->waterjumptime = 0;
		}

		// standing on an entity other than the world
		if (trace.entnum > 0)
			PM_AddTouchedEnt (trace.entnum);
	}

	// get waterlevel
	pm->waterlevel = 0;
	pm->watertype = CONTENTS_EMPTY;

	point[2] = pm->origin[2] + player_mins[2] + 1;
	cont = PM_PointContents (pm, point);
	if (cont <= CONTENTS_WATER)
	{
		pm->watertype = cont;
		pm->waterlevel = 1;
		point[2] = pm->origin[2] + (player_mins[2] + player_maxs[2])*0.5f;
		cont = PM_PointContents (pm, point);
		if (cont <= CONTENTS_WATER)
		{
			pm->waterlevel = 2;
			point[2] = pm->origin[2] + 22;
			cont = PM_PointContents (pm, point);
			if (cont <= CONTENTS_WATER)
				pm->waterlevel = 3;
		}
	}

	// snap to the ground, so that a jump goes no higher than it should
	if (!mv->pground && pm->onground && pm->pm_type != PM_FLY && pm->waterlevel < 2
	 && !trace.startsolid && !trace.allsolid)
		VectorCopy (trace.endpos, pm->origin);
}

/*
=============
PM_CheckJump
=============
*/
static void PM_CheckJump (void)
{
	float	ktjump;

	if (pm->pm_type == PM_FLY)
		return;

	if (pm->pm_type == PM_DEAD)
	{
		pm->jump_held = true;	// don't jump on respawn
		return;
	}

	if (!(pm->cmd.buttons & BUTTON_JUMP))
	{
		pm->jump_held = false;
		return;
	}

	if (pm->waterjumptime)
		return;

	if (pm->waterlevel >= 2)
	{	// swimming, not jumping
		pm->onground = false;

		if (pm->watertype == CONTENTS_WATER)
			pm->velocity[2] = 100;
		else if (pm->watertype == CONTENTS_SLIME)
			pm->velocity[2] = 80;
		else
			pm->velocity[2] = 50;
		return;
	}

	if (!pm->onground)
		return;		// in air, so no effect

	if (pm->jump_held && !pm->jump_msec)
		return;		// don't pogo stick

	// the jump fix: velocity into the ground, as when landing on a ramp, is
	// clipped by it first (with pm_rampjump even when not falling)
	if (!mv->pground && (mv->rampjump || pm->velocity[2] < 0)
	 && DotProduct (pm->velocity, groundnormal) < MAX_JUMPFIX_DOTPRODUCT)
		PM_ClipVelocity (pm->velocity, groundnormal, pm->velocity, 1);

	pm->onground = false;
	// kept on a ramp by pm_rampjump: no higher than from flat ground
	if (maxgroundspeed > MAXGROUNDSPEED_DEFAULT && pm->velocity[2] > MAXGROUNDSPEED_DEFAULT)
		pm->velocity[2] = MAXGROUNDSPEED_DEFAULT;
	pm->velocity[2] += 270;

	// pm_ktjump: a jump from a descent gets that much of a full jump's speed
	if (mv->ktjump > 0)
	{
		ktjump = mv->ktjump > 1 ? 1 : mv->ktjump;
		if (pm->velocity[2] < 270)
			pm->velocity[2] = pm->velocity[2] * (1 - ktjump) + 270 * ktjump;
	}

	pm->jump_held = true;	// don't jump again until released
	pm->jump_msec = pm->cmd.msec;
}

/*
=============
PM_CheckWaterJump
=============
*/
static void PM_CheckWaterJump (void)
{
	vec3_t	spot;
	int		cont;
	vec3_t	flatforward;

	if (pm->waterjumptime)
		return;

	// ZOID, don't hop out if we just jumped in
	if (pm->velocity[2] < -180)
		return; // only hop out if we are moving up

	// see if near an edge
	flatforward[0] = forward[0];
	flatforward[1] = forward[1];
	flatforward[2] = 0;
	VectorNormalize (flatforward);

	VectorMA (pm->origin, 24, flatforward, spot);
	spot[2] += 8;
	cont = PM_PointContentsAllBSPs (pm, spot);
	if (cont != CONTENTS_SOLID)
		return;
	spot[2] += 24;
	cont = PM_PointContentsAllBSPs (pm, spot);
	if (cont != CONTENTS_EMPTY)
		return;
	// jump out of water
	VectorScale (flatforward, 50, pm->velocity);
	pm->velocity[2] = 310;
	pm->waterjumptime = 2;	// safety net
	pm->jump_held = true;	// don't jump again until released
}

/*
=================
PM_NudgePosition

If pm->origin is in a solid position,
try nudging slightly on all axis to
allow for the cut precision of the net coordinates
=================
*/
static void PM_NudgePosition (void)
{
	vec3_t	base;
	int		x, y, z;
	int		i;
	static const int	sign[3] = {0, -1, 1};

	VectorCopy (pm->origin, base);

	for (i=0 ; i<3 ; i++)
		pm->origin[i] = ((int)(pm->origin[i]*8)) * 0.125f;

	for (z=0 ; z<=2 ; z++)
	{
		for (y=0 ; y<=2 ; y++)
		{
			for (x=0 ; x<=2 ; x++)
			{
				pm->origin[0] = base[0] + (sign[x] * 1.0f/8);
				pm->origin[1] = base[1] + (sign[y] * 1.0f/8);
				pm->origin[2] = base[2] + (sign[z] * 1.0f/8);
				if (PM_TestPlayerPosition (pm, pm->origin))
					return;
			}
		}
	}

	// some maps spawn the player several units into the ground
	for (z=1 ; z<=18 ; z++)
	{
		pm->origin[0] = base[0];
		pm->origin[1] = base[1];
		pm->origin[2] = base[2] + z;
		if (PM_TestPlayerPosition (pm, pm->origin))
			return;
	}

	VectorCopy (base, pm->origin);
}

/*
===============
PM_SpectatorMove
===============
*/
static void PM_SpectatorMove (void)
{
	float	speed, drop, friction, control, newspeed;
	float	currentspeed, addspeed, accelspeed;
	int			i;
	vec3_t		wishvel;
	float		fmove, smove;
	vec3_t		wishdir;
	float		wishspeed;

	// friction
	speed = Length (pm->velocity);
	if (speed < 1)
	{
		VectorCopy (vec3_origin, pm->velocity);
	}
	else
	{
		friction = mv->friction*1.5f;	// extra friction
		control = speed < mv->stopspeed ? mv->stopspeed : speed;
		drop = control*friction*frametime;

		// scale the velocity
		newspeed = speed - drop;
		if (newspeed < 0)
			newspeed = 0;
		newspeed /= speed;

		VectorScale (pm->velocity, newspeed, pm->velocity);
	}

	// accelerate
	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.sidemove;

	VectorNormalize (forward);
	VectorNormalize (right);

	for (i=0 ; i<3 ; i++)
		wishvel[i] = forward[i]*fmove + right[i]*smove;
	wishvel[2] += pm->cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	// clamp to server defined max speed
	if (wishspeed > mv->spectatormaxspeed)
	{
		VectorScale (wishvel, mv->spectatormaxspeed/wishspeed, wishvel);
		wishspeed = mv->spectatormaxspeed;
	}

	currentspeed = DotProduct (pm->velocity, wishdir);
	addspeed = wishspeed - currentspeed;

	// QuakeWorld's spectator doesn't move when it doesn't accelerate: kept
	// for PM_OLD_SPECTATOR
	if (addspeed <= 0 && pm->pm_type == PM_OLD_SPECTATOR)
		return;

	if (addspeed > 0)
	{
		accelspeed = mv->accelerate*frametime*wishspeed;
		if (accelspeed > addspeed)
			accelspeed = addspeed;
		VectorMA (pm->velocity, accelspeed, wishdir, pm->velocity);
	}

	// move
	VectorMA (pm->origin, frametime, pm->velocity, pm->origin);
}

/*
=============
PM_PlayerMove

Returns with origin, angles, and velocity modified in place.

Numtouch and touchindex[] will be set if any of the physents
were contacted during the move.
=============
*/
int PM_PlayerMove (playermove_t *pmove, const movevars_t *movevars)
{
	int		blocked;

	pm = pmove;
	mv = movevars;

	frametime = (float)(pm->cmd.msec * 0.001);
	pm->numtouch = 0;

	if (pm->pm_type == PM_NONE || pm->pm_type == PM_LOCK)
	{
		PM_CategorizePosition ();
		return 0;
	}

	// take angles directly from command
	VectorCopy (pm->cmd.angles, pm->angles);
	AngleVectors (pm->angles, forward, right, up);

	if (pm->pm_type == PM_SPECTATOR || pm->pm_type == PM_OLD_SPECTATOR)
	{
		PM_SpectatorMove ();
		pm->onground = false;
		return 0;
	}

	PM_NudgePosition ();

	// set onground, watertype, and waterlevel
	PM_CategorizePosition ();

	if (pm->waterlevel == 2 && pm->pm_type != PM_FLY)
		PM_CheckWaterJump ();

	if (pm->velocity[2] < 0 || pm->pm_type == PM_DEAD)
		pm->waterjumptime = 0;

	if (pm->waterjumptime)
	{
		pm->waterjumptime -= frametime;
		if (pm->waterjumptime < 0)
			pm->waterjumptime = 0;
	}

	if (pm->jump_msec)
	{
		pm->jump_msec += pm->cmd.msec;
		if (pm->jump_msec > 50)
			pm->jump_msec = 0;
	}

	PM_CheckJump ();

	PM_Friction ();

	if (pm->waterlevel >= 2)
		blocked = PM_WaterMove ();
	else if (pm->pm_type == PM_FLY)
		blocked = PM_FlyMove ();
	else
		blocked = PM_AirMove ();

	// set onground, watertype, and waterlevel for final spot
	PM_CategorizePosition ();

	// a hard landing is clipped by the ground, so that the landing sound and
	// the falling damage come once
	if (!mv->pground && pm->onground && pm->velocity[2] < -300
	 && DotProduct (pm->velocity, groundnormal) < MAX_JUMPFIX_DOTPRODUCT)
		PM_ClipVelocity (pm->velocity, groundnormal, pm->velocity, 1);

	return blocked;
}


/*
================
PM_RotateMove

As ezQuake and mvdsv turn it: the move as (side, forward), rotated about the
up axis
================
*/
void PM_RotateMove (usercmd_t *cmd, float degrees)
{
	double	angle = degrees * Q_PI / 180;
	float	c = (float)cos (angle), s = (float)sin (angle);
	float	side = cmd->sidemove, fwd = cmd->forwardmove;

	cmd->sidemove = (short)(c * side - s * fwd);
	cmd->forwardmove = (short)(s * side + c * fwd);
}

/*
===============
PM_CalcRoll

===============
*/
float PM_CalcRoll (const vec3_t angles, const vec3_t velocity)
{
	vec3_t	fwd, rt, u;
	float	sign;
	float	side;

	AngleVectors (angles, fwd, rt, u);
	side = DotProduct (velocity, rt);
	sign = (float)(side < 0 ? -1 : 1);
	side = fabsf(side);

	if (side < cl_rollspeed.value)
		side = side * cl_rollangle.value / cl_rollspeed.value;
	else
		side = cl_rollangle.value;

	return side*sign;
}
