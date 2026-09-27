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

vec3_t	player_mins = {-16, -16, -24};
vec3_t	player_maxs = {16, 16, 32};

// #define	PM_GRAVITY			800
// #define	PM_STOPSPEED		100
// #define	PM_MAXSPEED			320
// #define	PM_SPECTATORMAXSPEED	500
// #define	PM_ACCELERATE		10
// #define	PM_AIRACCELERATE	0.7
// #define	PM_WATERACCELERATE	10
// #define	PM_FRICTION			6
// #define	PM_WATERFRICTION	1

#define	STEPSIZE	18


#define	BUTTON_JUMP	2


static cvar_t	cl_rollspeed = {.name = "cl_rollspeed", .string = "200"};
static cvar_t	cl_rollangle = {.name = "cl_rollangle", .string = "2.0"};

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
==================
PM_ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define	STOP_EPSILON	0.1

int PM_ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
	float	backoff;
	float	change;
	int		i, blocked;
	
	blocked = 0;
	if (normal[2] > 0)
		blocked |= 1;		// floor
	if (!normal[2])
		blocked |= 2;		// step
	
	backoff = DotProduct (in, normal) * overbounce;

	for (i=0 ; i<3 ; i++)
	{
		change = normal[i]*backoff;
		out[i] = in[i] - change;
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
	}
	
	return blocked;
}


/*
============
PM_FlyMove

The basic solid body movement clip that slides along multiple planes
============
*/
#define	MAX_CLIP_PLANES	5

int PM_FlyMove (void)
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
		for (i=0 ; i<3 ; i++)
			end[i] = pm->origin[i] + time_left * pm->velocity[i];

		trace = PM_PlayerTrace (pm, pm->origin, end);

		if (trace.startsolid || trace.allsolid)
		{	// entity is trapped in another solid
			VectorCopy (vec3_origin, pm->velocity);
			return 3;
		}

		if (trace.fraction > 0)
		{	// actually covered some distance
			VectorCopy (trace.endpos, pm->origin);
			numplanes = 0;
		}

		if (trace.fraction == 1)
			 break;		// moved the entire distance

		// save entity for contact
		pm->touchindex[pm->numtouch] = trace.entnum;
		pm->numtouch++;

		if (trace.plane.normal[2] > 0.7)
		{
			blocked |= 1;		// floor
		}
		if (!trace.plane.normal[2])
		{
			blocked |= 2;		// step
		}

		time_left -= time_left * trace.fraction;
		
	// cliped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{	// this shouldn't really happen
			VectorCopy (vec3_origin, pm->velocity);
			break;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

//
// modify original_velocity so it parallels all of the clip planes
//
		for (i=0 ; i<numplanes ; i++)
		{
			PM_ClipVelocity (original_velocity, planes[i], pm->velocity, 1);
			for (j=0 ; j<numplanes ; j++)
				if (j != i)
				{
					if (DotProduct (pm->velocity, planes[j]) < 0)
						break;	// not ok
				}
			if (j == numplanes)
				break;
		}
		
		if (i != numplanes)
		{	// go along this plane
		}
		else
		{	// go along the crease
			if (numplanes != 2)
			{
//				Con_Printf ("clip velocity, numplanes == %i\n",numplanes);
				VectorCopy (vec3_origin, pm->velocity);
				break;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, pm->velocity);
			VectorScale (dir, d, pm->velocity);
		}

//
// if original velocity is against the original velocity, stop dead
// to avoid tiny occilations in sloping corners
//
		if (DotProduct (pm->velocity, primal_velocity) <= 0)
		{
			VectorCopy (vec3_origin, pm->velocity);
			break;
		}
	}

	if (pm->waterjumptime)
	{
		VectorCopy (primal_velocity, pm->velocity);
	}
	return blocked;
}

/*
=============
PM_GroundMove

Player is on ground, with no upwards velocity
=============
*/
void PM_GroundMove (void)
{
	vec3_t	start, dest;
	trace_t	trace;
	vec3_t	original, originalvel, down, uporg, downvel;
	float	downdist, updist;

	pm->velocity[2] = 0;
	if (!pm->velocity[0] && !pm->velocity[1] && !pm->velocity[2])
		return;

	// first try just moving to the destination	
	dest[0] = pm->origin[0] + pm->velocity[0]*frametime;
	dest[1] = pm->origin[1] + pm->velocity[1]*frametime;	
	dest[2] = pm->origin[2];

	// first try moving directly to the next spot
	VectorCopy (dest, start);
	trace = PM_PlayerTrace (pm, pm->origin, dest);
	if (trace.fraction == 1)
	{
		VectorCopy (trace.endpos, pm->origin);
		return;
	}

	// try sliding forward both on ground and up 16 pixels
	// take the move that goes farthest
	VectorCopy (pm->origin, original);
	VectorCopy (pm->velocity, originalvel);

	// slide move
	PM_FlyMove ();

	VectorCopy (pm->origin, down);
	VectorCopy (pm->velocity, downvel);

	VectorCopy (original, pm->origin);
	VectorCopy (originalvel, pm->velocity);

// move up a stair height
	VectorCopy (pm->origin, dest);
	dest[2] += STEPSIZE;
	trace = PM_PlayerTrace (pm, pm->origin, dest);
	if (!trace.startsolid && !trace.allsolid)
	{
		VectorCopy (trace.endpos, pm->origin);
	}

// slide move
	PM_FlyMove ();

// press down the stepheight
	VectorCopy (pm->origin, dest);
	dest[2] -= STEPSIZE;
	trace = PM_PlayerTrace (pm, pm->origin, dest);
	if ( trace.plane.normal[2] < 0.7)
		goto usedown;
	if (!trace.startsolid && !trace.allsolid)
	{
		VectorCopy (trace.endpos, pm->origin);
	}
	VectorCopy (pm->origin, uporg);

	// decide which one went farther
	downdist = (down[0] - original[0])*(down[0] - original[0])
		+ (down[1] - original[1])*(down[1] - original[1]);
	updist = (uporg[0] - original[0])*(uporg[0] - original[0])
		+ (uporg[1] - original[1])*(uporg[1] - original[1]);

	if (downdist > updist)
	{
usedown:
		VectorCopy (down, pm->origin);
		VectorCopy (downvel, pm->velocity);
	} else // copy z value from slide move
		pm->velocity[2] = downvel[2];

// if at a dead stop, retry the move with nudges to get around lips

}



/*
==================
PM_Friction

Handles both ground friction and water friction
==================
*/
void PM_Friction (void)
{
	float	*vel;
	float	speed, newspeed, control;
	float	friction;
	float	drop;
	vec3_t	start, stop;
	trace_t		trace;
	
	if (pm->waterjumptime)
		return;

	vel = pm->velocity;
	
	speed = (float)sqrt(vel[0]*vel[0] +vel[1]*vel[1] + vel[2]*vel[2]);
	if (speed < 1)
	{
		vel[0] = 0;
		vel[1] = 0;
		return;
	}

	friction = mv->friction;

// if the leading edge is over a dropoff, increase friction
	if (pm->onground != -1) {
		start[0] = stop[0] = pm->origin[0] + vel[0]/speed*16;
		start[1] = stop[1] = pm->origin[1] + vel[1]/speed*16;
		start[2] = pm->origin[2] + player_mins[2];
		stop[2] = start[2] - 34;

		trace = PM_PlayerTrace (pm, start, stop);

		if (trace.fraction == 1) {
			friction *= 2;
		}
	}

	drop = 0;

	if (pm->waterlevel >= 2) // apply water friction
		drop += speed*mv->waterfriction*pm->waterlevel*frametime;
	else if (pm->onground != -1) // apply ground friction
	{
		control = speed < mv->stopspeed ? mv->stopspeed : speed;
		drop += control*friction*frametime;
	}


// scale the velocity
	newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	vel[0] = vel[0] * newspeed;
	vel[1] = vel[1] * newspeed;
	vel[2] = vel[2] * newspeed;
}


/*
==============
PM_Accelerate
==============
*/
void PM_Accelerate (vec3_t wishdir, float wishspeed, float accel)
{
	int			i;
	float		addspeed, accelspeed, currentspeed;

	if (pm->dead)
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
	
	for (i=0 ; i<3 ; i++)
		pm->velocity[i] += accelspeed*wishdir[i];	
}

void PM_AirAccelerate (vec3_t wishdir, float wishspeed, float accel)
{
	int			i;
	float		addspeed, accelspeed, currentspeed, wishspd = wishspeed;
		
	if (pm->dead)
		return;
	if (pm->waterjumptime)
		return;

	if (wishspd > 30)
		wishspd = 30;
	currentspeed = DotProduct (pm->velocity, wishdir);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel * wishspeed * frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;
	
	for (i=0 ; i<3 ; i++)
		pm->velocity[i] += accelspeed*wishdir[i];	
}



/*
===================
PM_WaterMove

===================
*/
void PM_WaterMove (void)
{
	int		i;
	vec3_t	wishvel;
	float	wishspeed;
	vec3_t	wishdir;
	vec3_t	start, dest;
	trace_t	trace;

//
// user intentions
//
	for (i=0 ; i<3 ; i++)
		wishvel[i] = forward[i]*pm->cmd.forwardmove + right[i]*pm->cmd.sidemove;

	if (!pm->cmd.forwardmove && !pm->cmd.sidemove && !pm->cmd.upmove)
		wishvel[2] -= 60;		// drift towards bottom
	else
		wishvel[2] += pm->cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	if (wishspeed > mv->maxspeed)
	{
		VectorScale (wishvel, mv->maxspeed/wishspeed, wishvel);
		wishspeed = mv->maxspeed;
	}
	wishspeed = (float)(wishspeed * 0.7);

//
// water acceleration
//
	PM_Accelerate (wishdir, wishspeed, mv->wateraccelerate);

// assume it is a stair or a slope, so press down from stepheight above
	VectorMA (pm->origin, frametime, pm->velocity, dest);
	VectorCopy (dest, start);
	start[2] += STEPSIZE + 1;
	trace = PM_PlayerTrace (pm, start, dest);
	if (!trace.startsolid && !trace.allsolid)	// FIXME: check steep slope?
	{	// walked up the step
		VectorCopy (trace.endpos, pm->origin);
		return;
	}
	
	PM_FlyMove ();
}


/*
===================
PM_AirMove

===================
*/
void PM_AirMove (void)
{
	int			i;
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
	wishspeed = VectorNormalize(wishdir);

//
// clamp to server defined max speed
//
	if (wishspeed > mv->maxspeed)
	{
		VectorScale (wishvel, mv->maxspeed/wishspeed, wishvel);
		wishspeed = mv->maxspeed;
	}
	

	if ( pm->onground != -1)
	{
		pm->velocity[2] = 0;
		PM_Accelerate (wishdir, wishspeed, mv->accelerate);
		pm->velocity[2] -= mv->entgravity * mv->gravity * frametime;
		PM_GroundMove ();
	}
	else
	{	// not on ground, so little effect on velocity
		PM_AirAccelerate (wishdir, wishspeed, mv->accelerate);

		// add gravity
		pm->velocity[2] -= mv->entgravity * mv->gravity * frametime;

		PM_FlyMove ();

	}


}



/*
=============
PM_CatagorizePosition
=============
*/
void PM_CatagorizePosition (void)
{
	vec3_t		point;
	int			cont;
	trace_t		tr;

// if the player hull point one unit down is solid, the player
// is on ground

// see if standing on something solid	
	point[0] = pm->origin[0];
	point[1] = pm->origin[1];
	point[2] = pm->origin[2] - 1;
	if (pm->velocity[2] > 180)
	{
		pm->onground = -1;
	}
	else
	{
		tr = PM_PlayerTrace (pm, pm->origin, point);
		if ( tr.plane.normal[2] < 0.7)
			pm->onground = -1;	// too steep
		else
			pm->onground = tr.entnum;
		if (pm->onground != -1)
		{
			pm->waterjumptime = 0;
			if (!tr.startsolid && !tr.allsolid)
				VectorCopy (tr.endpos, pm->origin);
		}

		// standing on an entity other than the world
		if (tr.entnum > 0)
		{
			pm->touchindex[pm->numtouch] = tr.entnum;
			pm->numtouch++;
		}
	}

//
// get waterlevel
//
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
}


/*
=============
JumpButton
=============
*/
void JumpButton (void)
{
	if (pm->dead)
	{
		pm->oldbuttons |= BUTTON_JUMP;	// don't jump again until released
		return;
	}

	if (pm->waterjumptime)
	{
		pm->waterjumptime -= frametime;
		if (pm->waterjumptime < 0)
			pm->waterjumptime = 0;
		return;
	}

	if (pm->waterlevel >= 2)
	{	// swimming, not jumping
		pm->onground = -1;

		if (pm->watertype == CONTENTS_WATER)
			pm->velocity[2] = 100;
		else if (pm->watertype == CONTENTS_SLIME)
			pm->velocity[2] = 80;
		else
			pm->velocity[2] = 50;
		return;
	}

	if (pm->onground == -1)
		return;		// in air, so no effect

	if ( pm->oldbuttons & BUTTON_JUMP )
		return;		// don't pogo stick

	pm->onground = -1;
	pm->velocity[2] += 270;

	pm->oldbuttons |= BUTTON_JUMP;	// don't jump again until released
}

/*
=============
CheckWaterJump
=============
*/
void CheckWaterJump (void)
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
	cont = PM_PointContents (pm, spot);
	if (cont != CONTENTS_SOLID)
		return;
	spot[2] += 24;
	cont = PM_PointContents (pm, spot);
	if (cont != CONTENTS_EMPTY)
		return;
	// jump out of water
	VectorScale (flatforward, 50, pm->velocity);
	pm->velocity[2] = 310;
	pm->waterjumptime = 2;	// safety net
	pm->oldbuttons |= BUTTON_JUMP;	// don't jump again until released
}

/*
=================
NudgePosition

If pm->origin is in a solid position,
try nudging slightly on all axis to
allow for the cut precision of the net coordinates
=================
*/
void NudgePosition (void)
{
	vec3_t	base;
	int		x, y, z;
	int		i;
	static int		sign[3] = {0, -1, 1};

	VectorCopy (pm->origin, base);

	for (i=0 ; i<3 ; i++)
		pm->origin[i] = ((int)(pm->origin[i]*8)) * 0.125f;
//	pm->origin[2] += 0.124;

//	if (pm->dead)
//		return;		// might be a squished point, so don'y bother
//	if (PM_TestPlayerPosition (pm->origin) )
//		return;

	for (z=0 ; z<=2 ; z++)
	{
		for (x=0 ; x<=2 ; x++)
		{
			for (y=0 ; y<=2 ; y++)
			{
				pm->origin[0] = base[0] + (sign[x] * 1.0f/8);
				pm->origin[1] = base[1] + (sign[y] * 1.0f/8);
				pm->origin[2] = base[2] + (sign[z] * 1.0f/8);
				if (PM_TestPlayerPosition (pm, pm->origin))
					return;
			}
		}
	}
	VectorCopy (base, pm->origin);
//	Con_DPrintf ("NudgePosition: stuck\n");
}

/*
===============
SpectatorMove
===============
*/
void SpectatorMove (void)
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
		VectorCopy (vec3_origin, pm->velocity)
	}
	else
	{
		drop = 0;

		friction = mv->friction*1.5f;	// extra friction
		control = speed < mv->stopspeed ? mv->stopspeed : speed;
		drop += control*friction*frametime;

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
	wishspeed = VectorNormalize(wishdir);

	//
	// clamp to server defined max speed
	//
	if (wishspeed > mv->spectatormaxspeed)
	{
		VectorScale (wishvel, mv->spectatormaxspeed/wishspeed, wishvel);
		wishspeed = mv->spectatormaxspeed;
	}

	currentspeed = DotProduct(pm->velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = mv->accelerate*frametime*wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;
	
	for (i=0 ; i<3 ; i++)
		pm->velocity[i] += accelspeed*wishdir[i];	


	// move
	VectorMA (pm->origin, frametime, pm->velocity, pm->origin);
}

/*
=============
PlayerMove

Returns with origin, angles, and velocity modified in place.

Numtouch and touchindex[] will be set if any of the physents
were contacted during the move.
=============
*/
void PM_PlayerMove (playermove_t *pmove, const movevars_t *movevars)
{
	pm = pmove;
	mv = movevars;

	frametime = (float)(pm->cmd.msec * 0.001);
	pm->numtouch = 0;

	AngleVectors (pm->angles, forward, right, up);

	if (pm->spectator)
	{
		SpectatorMove ();
		return;
	}

	NudgePosition ();

	// take angles directly from command
	VectorCopy (pm->cmd.angles, pm->angles);

	// set onground, watertype, and waterlevel
	PM_CatagorizePosition ();

	if (pm->waterlevel == 2)
		CheckWaterJump ();

	if (pm->velocity[2] < 0)
		pm->waterjumptime = 0;

	if (pm->cmd.buttons & BUTTON_JUMP)
		JumpButton ();
	else
		pm->oldbuttons &= ~BUTTON_JUMP;

	PM_Friction ();

	if (pm->waterlevel >= 2)
		PM_WaterMove ();
	else
		PM_AirMove ();

	// set onground, watertype, and waterlevel for final spot
	PM_CatagorizePosition ();
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
