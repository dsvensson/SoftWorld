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
// sv_phys.c

#include "sv_local.h"

/*


pushmove objects do not obey gravity, and do not interact with each other or trigger fields, but block normal movement and push normal objects when they move.

onground is set for toss objects when they come to a complete rest.  it is set for steping or walking objects 

doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
corpses are SOLID_NOT and MOVETYPE_TOSS
crates are SOLID_BBOX and MOVETYPE_TOSS
walking monsters are SOLID_SLIDEBOX and MOVETYPE_STEP
flying/floating monsters are SOLID_SLIDEBOX and MOVETYPE_FLY

solid_edge items only clip against bsp models.

*/

cvar_t	sv_maxvelocity = {.name = "sv_maxvelocity", .string = "2000",
	.description = "Caps each axis of a falling or thrown entity's velocity, in units per second; "
		"player movement doesn't use it."};

cvar_t	sv_gravity			 = {.name = "sv_gravity", .string = "800",
	.description = "Downward acceleration of players and falling entities, in units per second squared. "
		"Takes effect on the next map."};
cvar_t	sv_stopspeed		 = {.name = "sv_stopspeed", .string = "100",
	.description = "Players slower than this, in units per second, get ground friction as if this fast, so they stop. "
		"Takes effect on the next map."};
cvar_t	sv_maxspeed			 = {.name = "sv_maxspeed", .string = "320",
	.description = "Players' top running speed, in units per second, given to each as they enter the game; "
		"the game code may change a player's."};
cvar_t	sv_spectatormaxspeed = {.name = "sv_spectatormaxspeed", .string = "500",
	.description = "Spectators' top flying speed, in units per second. Takes effect on the next map."};
cvar_t	sv_accelerate		 = {.name = "sv_accelerate", .string = "10",
	.description = "Player acceleration on the ground and in the air, per second as a multiple of the speed "
		"wished for. Takes effect on the next map."};
cvar_t	sv_airaccelerate	 = {.name = "sv_airaccelerate", .string = "0.7",
	.description = "Sent to clients with the movement settings, but unused: acceleration in the air is "
		"sv_accelerate's."};
cvar_t	sv_wateraccelerate	 = {.name = "sv_wateraccelerate", .string = "10",
	.description = "Player acceleration in water, per second as a multiple of the speed wished for. "
		"Takes effect on the next map."};
cvar_t	sv_friction			 = {.name = "sv_friction", .string = "4",
	.description = "Ground friction on players: speed lost per second as a multiple of their speed, doubled "
		"at a ledge. Takes effect on the next map."};
cvar_t	sv_waterfriction	 = {.name = "sv_waterfriction", .string = "4",
	.description = "Water friction on players: speed lost per second as a multiple of their speed, times 2 waist-deep "
		"and 3 under. Takes effect on the next map."};


#define	MOVE_EPSILON	0.01

static void SV_Physics_Toss (edict_t *ent);

/*
================
SV_CheckVelocity
================
*/
static void SV_CheckVelocity (edict_t *ent)
{
	int		i;

//
// bound velocity
//
	for (i=0 ; i<3 ; i++)
	{
		if (IS_NAN(ent->v.velocity[i]))
		{
			Con_Printf ("Got a NaN velocity on %s\n", PR_GetString(ent->v.classname));
			ent->v.velocity[i] = 0;
		}
		if (IS_NAN(ent->v.origin[i]))
		{
			Con_Printf ("Got a NaN origin on %s\n", PR_GetString(ent->v.classname));
			ent->v.origin[i] = 0;
		}
		if (ent->v.velocity[i] > sv_maxvelocity.value)
			ent->v.velocity[i] = sv_maxvelocity.value;
		else if (ent->v.velocity[i] < -sv_maxvelocity.value)
			ent->v.velocity[i] = -sv_maxvelocity.value;
	}
}

/*
=============
SV_RunThink

Runs thinking code if time.  There is some play in the exact time the think
function will be called, because it is called before any movement is done
in a frame.  Not used for pushmove objects, because they must be exact.
Returns false if the entity removed itself.
=============
*/
bool SV_RunThink (edict_t *ent)
{
	float	thinktime;

	do
	{
		thinktime = ent->v.nextthink;
		if (thinktime <= 0)
			return true;
		if (thinktime > sv.time + sv.frametime)
			return true;
		
		if (thinktime < sv.time)
			thinktime = (float)sv.time;	// don't let things stay in the past.
									// it is possible to start that way
									// by a trigger with a local time.
		ent->v.nextthink = 0;
		PR_GLOBAL(time) = thinktime;
		PR_GLOBAL(self) = EDICT_TO_PROG(ent);
		PR_GLOBAL(other) = EDICT_TO_PROG(sv.edicts);
		PR_ExecuteProgram (ent->v.think);

		if (ent->free)
			return false;
		// a think that doesn't put the next past its own time would run here
		// forever: it runs once a frame, as NetQuake's do (FTE's; Arcane
		// Dimensions' lasers think on a local time that stands still)
		if (ent->v.nextthink <= thinktime)
			return true;
	} while (1);

	return true;
}

/*
==================
SV_Impact

Two entities have touched, so run their touch functions
==================
*/
static void SV_Impact (edict_t *e1, edict_t *e2)
{
	int		old_self, old_other;
	
	old_self = PR_GLOBAL(self);
	old_other = PR_GLOBAL(other);
	
	PR_GLOBAL(time) = (float)sv.time;
	if (e1->v.touch && e1->v.solid != SOLID_NOT)
	{
		PR_GLOBAL(self) = EDICT_TO_PROG(e1);
		PR_GLOBAL(other) = EDICT_TO_PROG(e2);
		PR_ExecuteProgram (e1->v.touch);
	}
	
	if (e2->v.touch && e2->v.solid != SOLID_NOT)
	{
		PR_GLOBAL(self) = EDICT_TO_PROG(e2);
		PR_GLOBAL(other) = EDICT_TO_PROG(e1);
		PR_ExecuteProgram (e2->v.touch);
	}

	PR_GLOBAL(self) = old_self;
	PR_GLOBAL(other) = old_other;
}


/*
==================
ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define	STOP_EPSILON	0.1

static int ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
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
SV_FlyMove

The basic solid body movement clip that slides along multiple planes
Returns the clipflags if the velocity was modified (hit something solid)
1 = floor
2 = wall / step
4 = dead stop
If steptrace is not NULL, the trace of any vertical wall hit will be stored
============
*/
#define	MAX_CLIP_PLANES	5
static int SV_FlyMove (edict_t *ent, float time, trace_t *steptrace)
{
	int			bumpcount, numbumps;
	vec3_t		dir;
	float		d;
	int			numplanes;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity, original_velocity, new_velocity;
	int			i, j;
	trace_t		trace;
	vec3_t		end;
	float		time_left;
	int			blocked;
	
	numbumps = 4;
	
	blocked = 0;
	VectorCopy (ent->v.velocity, original_velocity);
	VectorCopy (ent->v.velocity, primal_velocity);
	numplanes = 0;
	
	time_left = time;

	for (bumpcount=0 ; bumpcount<numbumps ; bumpcount++)
	{
		for (i=0 ; i<3 ; i++)
			end[i] = ent->v.origin[i] + time_left * ent->v.velocity[i];

		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, false, ent);

		if (trace.allsolid)
		{	// entity is trapped in another solid
			VectorCopy (vec3_origin, ent->v.velocity);
			return 3;
		}

		if (trace.fraction > 0)
		{	// actually covered some distance
			VectorCopy (trace.endpos, ent->v.origin);
			VectorCopy (ent->v.velocity, original_velocity);
			numplanes = 0;
		}

		if (trace.fraction == 1)
			 break;		// moved the entire distance

		if (!trace.ent)
			SV_Error ("SV_FlyMove: !trace.ent");

		if (trace.plane.normal[2] > 0.7)
		{
			blocked |= 1;		// floor
			if (trace.ent->v.solid == SOLID_BSP)
			{
				ent->v.flags = (float)((int)ent->v.flags | FL_ONGROUND);
				ent->v.groundentity = EDICT_TO_PROG(trace.ent);
			}
		}
		if (!trace.plane.normal[2])
		{
			blocked |= 2;		// step
			if (steptrace)
				*steptrace = trace;	// save for player extrafriction
		}

//
// run the impact function
//
		SV_Impact (ent, trace.ent);
		if (ent->free)
			break;		// removed by the impact function

		
		time_left -= time_left * trace.fraction;
		
	// cliped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{	// this shouldn't really happen
			VectorCopy (vec3_origin, ent->v.velocity);
			return 3;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

//
// modify original_velocity so it parallels all of the clip planes
//
		for (i=0 ; i<numplanes ; i++)
		{
			ClipVelocity (original_velocity, planes[i], new_velocity, 1);
			for (j=0 ; j<numplanes ; j++)
				if (j != i)
				{
					if (DotProduct (new_velocity, planes[j]) < 0)
						break;	// not ok
				}
			if (j == numplanes)
				break;
		}
		
		if (i != numplanes)
		{	// go along this plane
			VectorCopy (new_velocity, ent->v.velocity);
		}
		else
		{	// go along the crease
			if (numplanes != 2)
			{
//				Con_Printf ("clip velocity, numplanes == %i\n",numplanes);
				VectorCopy (vec3_origin, ent->v.velocity);
				return 7;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, ent->v.velocity);
			VectorScale (dir, d, ent->v.velocity);
		}

//
// if original velocity is against the original velocity, stop dead
// to avoid tiny occilations in sloping corners
//
		if (DotProduct (ent->v.velocity, primal_velocity) <= 0)
		{
			VectorCopy (vec3_origin, ent->v.velocity);
			return blocked;
		}
	}

	return blocked;
}


/*
============
SV_AddGravity

============
*/
static void SV_AddGravity (edict_t *ent, float scale)
{
	ent->v.velocity[2] = (float)(ent->v.velocity[2] - scale * sv.movevars.gravity * sv.frametime);
}

/*
===============================================================================

PUSHMOVE

===============================================================================
*/

/*
============
SV_PushEntity

Does not change the entities velocity at all
============
*/
static trace_t SV_PushEntity (edict_t *ent, vec3_t push)
{
	trace_t	trace;
	vec3_t	end;
		
	VectorAdd (ent->v.origin, push, end);

	if (ent->v.movetype == MOVETYPE_FLYMISSILE)
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_MISSILE, ent);
	else if (ent->v.solid == SOLID_TRIGGER || ent->v.solid == SOLID_NOT)
	// only clip against bmodels
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_NOMONSTERS, ent);
	else
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_NORMAL, ent);
	
	VectorCopy (trace.endpos, ent->v.origin);
	SV_LinkEdict (ent, true);

	if (trace.ent)
		SV_Impact (ent, trace.ent);		

	return trace;
}					


/*
============
SV_Push

============
*/
static bool SV_Push (edict_t *pusher, vec3_t move)
{
	int			i, e;
	edict_t		*check, *block;
	vec3_t		mins, maxs;
	vec3_t		pushorig;
	int			num_moved;
	static edict_t	*moved_edict[MAX_EDICTS];	// static: too big for the stack
	static vec3_t	moved_from[MAX_EDICTS];

	for (i=0 ; i<3 ; i++)
	{
		mins[i] = pusher->v.absmin[i] + move[i];
		maxs[i] = pusher->v.absmax[i] + move[i];
	}

	VectorCopy (pusher->v.origin, pushorig);
	
// move the pusher to it's final position

	VectorAdd (pusher->v.origin, move, pusher->v.origin);
	SV_LinkEdict (pusher, false);

// see if any solid entities are inside the final position
	num_moved = 0;
	check = NEXT_EDICT(sv.edicts);
	for (e=1 ; e<sv.num_edicts ; e++, check = NEXT_EDICT(check))
	{
		if (check->free)
			continue;
		if (check->v.movetype == MOVETYPE_PUSH
		|| check->v.movetype == MOVETYPE_NONE
		|| check->v.movetype == MOVETYPE_NOCLIP)
			continue;

		pusher->v.solid = SOLID_NOT;
		block = SV_TestEntityPosition (check);
		pusher->v.solid = SOLID_BSP;
		if (block)
			continue;

	// if the entity is standing on the pusher, it will definately be moved
		if ( ! ( ((int)check->v.flags & FL_ONGROUND)
		&& PROG_TO_EDICT(check->v.groundentity) == pusher) )
		{
			if ( check->v.absmin[0] >= maxs[0]
			|| check->v.absmin[1] >= maxs[1]
			|| check->v.absmin[2] >= maxs[2]
			|| check->v.absmax[0] <= mins[0]
			|| check->v.absmax[1] <= mins[1]
			|| check->v.absmax[2] <= mins[2] )
				continue;

		// see if the ent's bbox is inside the pusher's final position
			if (!SV_TestEntityPosition (check))
				continue;
		}

		VectorCopy (check->v.origin, moved_from[num_moved]);
		moved_edict[num_moved] = check;
		num_moved++;

		// try moving the contacted entity 
		VectorAdd (check->v.origin, move, check->v.origin);
		block = SV_TestEntityPosition (check);
		if (!block)
		{	// pushed ok
			SV_LinkEdict (check, false);
			continue;
		}

		// if it is ok to leave in the old position, do it
		VectorSubtract (check->v.origin, move, check->v.origin);
		block = SV_TestEntityPosition (check);
		if (!block)
		{
			num_moved--;
			continue;
		}

	// if it is still inside the pusher, block
		if (check->v.mins[0] == check->v.maxs[0])
		{
			SV_LinkEdict (check, false);
			continue;
		}
		if (check->v.solid == SOLID_NOT || check->v.solid == SOLID_TRIGGER)
		{	// corpse
			check->v.mins[0] = check->v.mins[1] = 0;
			VectorCopy (check->v.mins, check->v.maxs);
			SV_LinkEdict (check, false);
			continue;
		}
		
		VectorCopy (pushorig, pusher->v.origin);
		SV_LinkEdict (pusher, false);

		// if the pusher has a "blocked" function, call it
		// otherwise, just stay in place until the obstacle is gone
		if (pusher->v.blocked)
		{
			PR_GLOBAL(self) = EDICT_TO_PROG(pusher);
			PR_GLOBAL(other) = EDICT_TO_PROG(check);
			PR_ExecuteProgram (pusher->v.blocked);
		}
		
	// move back any entities we already moved
		for (i=0 ; i<num_moved ; i++)
		{
			VectorCopy (moved_from[i], moved_edict[i]->v.origin);
			SV_LinkEdict (moved_edict[i], false);
		}
		return false;
	}

	return true;
}

/*
============
SV_PushMove

============
*/
static void SV_PushMove (edict_t *pusher, float movetime)
{
	int			i;
	vec3_t		move;

	if (!pusher->v.velocity[0] && !pusher->v.velocity[1] && !pusher->v.velocity[2])
	{
		pusher->v.ltime += movetime;
		return;
	}

	for (i=0 ; i<3 ; i++)
		move[i] = pusher->v.velocity[i] * movetime;

	if (SV_Push (pusher, move))
		pusher->v.ltime += movetime;
}


/*
================
SV_Physics_Pusher

================
*/
static void SV_Physics_Pusher (edict_t *ent)
{
	float	thinktime;
	float	oldltime;
	float	movetime;
vec3_t oldorg, move;
float	l;

	oldltime = ent->v.ltime;
	
	thinktime = ent->v.nextthink;
	if (thinktime < ent->v.ltime + sv.frametime)
	{
		movetime = thinktime - ent->v.ltime;
		if (movetime < 0)
			movetime = 0;
	}
	else
		movetime = (float)sv.frametime;

	if (movetime)
	{
		SV_PushMove (ent, movetime);	// advances ent->v.ltime if not blocked
	}
		
	if (thinktime > oldltime && thinktime <= ent->v.ltime)
	{
VectorCopy (ent->v.origin, oldorg);
		ent->v.nextthink = 0;
		PR_GLOBAL(time) = (float)sv.time;
		PR_GLOBAL(self) = EDICT_TO_PROG(ent);
		PR_GLOBAL(other) = EDICT_TO_PROG(sv.edicts);
		PR_ExecuteProgram (ent->v.think);
		if (ent->free)
			return;
VectorSubtract (ent->v.origin, oldorg, move);

l = Length(move);
if (l > 1.0/64)
{
//	Con_Printf ("**** snap: %f\n", Length (l));
	VectorCopy (oldorg, ent->v.origin);
	SV_Push (ent, move);
}

	}

}


/*
=============
SV_Physics_None

Non moving objects can only think
=============
*/
static void SV_Physics_None (edict_t *ent)
{
// regular thinking
	SV_RunThink (ent);
}

/*
=============
SV_Physics_Noclip

A moving object that doesn't obey physics
=============
*/
static void SV_Physics_Noclip (edict_t *ent)
{
// regular thinking
	if (!SV_RunThink (ent))
		return;
	
	VectorMA (ent->v.angles, (float)sv.frametime, ent->v.avelocity, ent->v.angles);
	VectorMA (ent->v.origin, (float)sv.frametime, ent->v.velocity, ent->v.origin);

	SV_LinkEdict (ent, false);
}

/*
==============================================================================

TOSS / BOUNCE

==============================================================================
*/

/*
=============
SV_CheckWaterTransition

=============
*/
static void SV_CheckWaterTransition (edict_t *ent)
{
	int		cont;

	cont = SV_PointContents (ent->v.origin);
	if (!ent->v.watertype)
	{	// just spawned here
		ent->v.watertype = (float)cont;
		ent->v.waterlevel = 1;
		return;
	}
	
	if (cont <= CONTENTS_WATER)
	{
		if (ent->v.watertype == CONTENTS_EMPTY)
		{	// just crossed into water
			SV_StartSound (ent, 0, "misc/h2ohit1.wav", 255, 1);
		}		
		ent->v.watertype = (float)cont;
		ent->v.waterlevel = 1;
	}
	else
	{
		if (ent->v.watertype != CONTENTS_EMPTY)
		{	// just crossed into water
			SV_StartSound (ent, 0, "misc/h2ohit1.wav", 255, 1);
		}		
		ent->v.watertype = CONTENTS_EMPTY;
		ent->v.waterlevel = (float)cont;
	}
}

/*
=============
SV_Physics_Toss

Toss, bounce, and fly movement.  When onground, do nothing.
=============
*/
static void SV_Physics_Toss (edict_t *ent)
{
	trace_t	trace;
	vec3_t	move;
	float	backoff;

// regular thinking
	if (!SV_RunThink (ent))
		return;

	if (ent->v.velocity[2] > 0)
		ent->v.flags = (float)((int)ent->v.flags & ~FL_ONGROUND);

// if onground, return without moving
	if ( ((int)ent->v.flags & FL_ONGROUND) )
		return;

	SV_CheckVelocity (ent);

// add gravity
	if (ent->v.movetype != MOVETYPE_FLY
	&& ent->v.movetype != MOVETYPE_FLYMISSILE)
		SV_AddGravity (ent, 1.0);

// move angles
	VectorMA (ent->v.angles, (float)sv.frametime, ent->v.avelocity, ent->v.angles);

// move origin
	VectorScale (ent->v.velocity, (float)sv.frametime, move);
	trace = SV_PushEntity (ent, move);
	if (trace.fraction == 1)
		return;
	if (ent->free)
		return;
	
	if (ent->v.movetype == MOVETYPE_BOUNCE)
		backoff = 1.5;
	else
		backoff = 1;

	ClipVelocity (ent->v.velocity, trace.plane.normal, ent->v.velocity, backoff);

// stop if on ground
	if (trace.plane.normal[2] > 0.7)
	{		
		if (ent->v.velocity[2] < 60 || ent->v.movetype != MOVETYPE_BOUNCE )
		{
			ent->v.flags = (float)((int)ent->v.flags | FL_ONGROUND);
			ent->v.groundentity = EDICT_TO_PROG(trace.ent);
			VectorCopy (vec3_origin, ent->v.velocity);
			VectorCopy (vec3_origin, ent->v.avelocity);
		}
	}
	
// check for in water
	SV_CheckWaterTransition (ent);
}

/*
===============================================================================

STEPPING MOVEMENT

===============================================================================
*/

/*
=============
SV_Physics_Step

Monsters freefall when they don't have a ground entity, otherwise
all movement is done with discrete steps.

This is also used for objects that have become still on the ground, but
will fall if the floor is pulled out from under them.
FIXME: is this true?
=============
*/
static void SV_Physics_Step (edict_t *ent)
{
	bool	hitsound;

// frefall if not onground
	if ( ! ((int)ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM) ) )
	{
		if (ent->v.velocity[2] < sv.movevars.gravity*-0.1)
			hitsound = true;
		else
			hitsound = false;

		SV_AddGravity (ent, 1.0);
		SV_CheckVelocity (ent);
		SV_FlyMove (ent, (float)sv.frametime, NULL);
		SV_LinkEdict (ent, true);

		if ( (int)ent->v.flags & FL_ONGROUND )	// just hit ground
		{
			if (hitsound)
				SV_StartSound (ent, 0, "demon/dland2.wav", 255, 1);
		}
	}

// regular thinking
	SV_RunThink (ent);
	
	SV_CheckWaterTransition (ent);
}

/*
===============================================================================

NETQUAKE'S PLAYERS

NetQuake's progs move their players as NetQuake does (id's SV_ClientThink and
SV_Physics_Client), in the world's frame, by the newest move each sent; the
client doesn't predict them (PM_NONE). FTE's sv_nqplayerphysics does the same
for NetQuake's single player.

===============================================================================
*/

static vec3_t	nq_forward, nq_right, nq_up;
static vec3_t	nq_wishdir;
static float	nq_wishspeed;
static bool		nq_onground;

// whether a client moves as NetQuake's do
bool SV_NQPhysics (const client_t *cl)
{
	return pr.nq && !cl->spectator;
}

static void SV_UserFriction (void)
{
	float	*vel = sv_player->v.velocity, *origin = sv_player->v.origin;
	float	speed, newspeed, control, friction;
	vec3_t	start, stop;
	trace_t	trace;

	speed = sqrtf (vel[0]*vel[0] + vel[1]*vel[1]);
	if (!speed)
		return;

// if the leading edge is over a dropoff, increase friction
	start[0] = stop[0] = origin[0] + vel[0]/speed*16;
	start[1] = stop[1] = origin[1] + vel[1]/speed*16;
	start[2] = origin[2] + sv_player->v.mins[2];
	stop[2] = start[2] - 34;

	trace = SV_Move (start, vec3_origin, vec3_origin, stop, true, sv_player);

	friction = sv.movevars.friction;
	if (trace.fraction == 1.0)
		friction *= 2;		// NetQuake's edgefriction

// apply friction
	control = speed < sv.movevars.stopspeed ? sv.movevars.stopspeed : speed;
	newspeed = speed - (float)sv.frametime*control*friction;

	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	vel[0] = vel[0] * newspeed;
	vel[1] = vel[1] * newspeed;
	vel[2] = vel[2] * newspeed;
}

static void SV_Accelerate (void)
{
	int		i;
	float	addspeed, accelspeed, currentspeed;

	currentspeed = DotProduct (sv_player->v.velocity, nq_wishdir);
	addspeed = nq_wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = sv.movevars.accelerate*(float)sv.frametime*nq_wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		sv_player->v.velocity[i] += accelspeed*nq_wishdir[i];
}

// as id had it, the whole wish speed scaling the acceleration
static void SV_AirAccelerate (vec3_t wishveloc)
{
	int		i;
	float	addspeed, wishspd, accelspeed, currentspeed;

	wishspd = VectorNormalize (wishveloc);
	if (wishspd > 30)
		wishspd = 30;
	currentspeed = DotProduct (sv_player->v.velocity, wishveloc);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = sv.movevars.accelerate*nq_wishspeed*(float)sv.frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		sv_player->v.velocity[i] += accelspeed*wishveloc[i];
}

static void DropPunchAngle (void)
{
	float	len;

	len = VectorNormalize (sv_player->v.punchangle);
	len -= 10*(float)sv.frametime;
	if (len < 0)
		len = 0;
	VectorScale (sv_player->v.punchangle, len, sv_player->v.punchangle);
}

static void SV_WaterMove (const usercmd_t *cmd)
{
	int		i;
	vec3_t	wishvel;
	float	speed, newspeed, wishspeed, addspeed, accelspeed;
	float	*velocity = sv_player->v.velocity;

//
// user intentions
//
	AngleVectors (sv_player->v.v_angle, nq_forward, nq_right, nq_up);

	for (i=0 ; i<3 ; i++)
		wishvel[i] = nq_forward[i]*cmd->forwardmove + nq_right[i]*cmd->sidemove;

	if (!cmd->forwardmove && !cmd->sidemove && !cmd->upmove)
		wishvel[2] -= 60;		// drift towards bottom
	else
		wishvel[2] += cmd->upmove;

	wishspeed = Length (wishvel);
	if (wishspeed > sv.movevars.maxspeed)
	{
		VectorScale (wishvel, sv.movevars.maxspeed/wishspeed, wishvel);
		wishspeed = sv.movevars.maxspeed;
	}
	wishspeed *= 0.7f;

//
// water friction
//
	speed = Length (velocity);
	if (speed)
	{
		newspeed = speed - (float)sv.frametime * speed * sv.movevars.friction;
		if (newspeed < 0)
			newspeed = 0;
		VectorScale (velocity, newspeed/speed, velocity);
	}
	else
		newspeed = 0;

//
// water acceleration
//
	if (!wishspeed)
		return;

	addspeed = wishspeed - newspeed;
	if (addspeed <= 0)
		return;

	VectorNormalize (wishvel);
	accelspeed = sv.movevars.accelerate * wishspeed * (float)sv.frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed * wishvel[i];
}

static void SV_WaterJump (void)
{
	if (sv.time > sv_player->v.teleport_time || !sv_player->v.waterlevel)
	{
		sv_player->v.flags = (float)((int)sv_player->v.flags & ~FL_WATERJUMP);
		sv_player->v.teleport_time = 0;
	}
	sv_player->v.velocity[0] = sv_player->v.movedir[0];
	sv_player->v.velocity[1] = sv_player->v.movedir[1];
}

static void SV_AirMove (const usercmd_t *cmd)
{
	int		i;
	vec3_t	wishvel;
	float	fmove, smove;

	AngleVectors (sv_player->v.angles, nq_forward, nq_right, nq_up);

	fmove = cmd->forwardmove;
	smove = cmd->sidemove;

// hack to not let you back into teleporter
	if (sv.time < sv_player->v.teleport_time && fmove < 0)
		fmove = 0;

	for (i=0 ; i<3 ; i++)
		wishvel[i] = nq_forward[i]*fmove + nq_right[i]*smove;

	if ((int)sv_player->v.movetype != MOVETYPE_WALK)
		wishvel[2] = cmd->upmove;
	else
		wishvel[2] = 0;

	VectorCopy (wishvel, nq_wishdir);
	nq_wishspeed = VectorNormalize (nq_wishdir);
	if (nq_wishspeed > sv.movevars.maxspeed)
	{
		VectorScale (wishvel, sv.movevars.maxspeed/nq_wishspeed, wishvel);
		nq_wishspeed = sv.movevars.maxspeed;
	}

	if (sv_player->v.movetype == MOVETYPE_NOCLIP)
	{
		VectorCopy (wishvel, sv_player->v.velocity);
	}
	else if (nq_onground)
	{
		SV_UserFriction ();
		SV_Accelerate ();
	}
	else
	{	// not on ground, so little effect on velocity
		SV_AirAccelerate (wishvel);
	}
}

/*
===================
SV_ClientThink

host_client's newest move, as NetQuake applies it before the world's frame:
the move fields specify an intended velocity in pix/sec, the angle fields an
exact angular motion in degrees
===================
*/
static void SV_ClientThink (void)
{
	const usercmd_t	*cmd = &host_client->nqcmd;
	vec3_t			v_angle;
	float			*angles = sv_player->v.angles;

	if (sv_player->v.movetype == MOVETYPE_NONE)
		return;

	nq_onground = ((int)sv_player->v.flags & FL_ONGROUND) != 0;

	DropPunchAngle ();

//
// if dead, behave differently
//
	if (sv_player->v.health <= 0)
		return;

//
// angles
// show 1/3 the pitch angle and all the roll angle
	VectorAdd (sv_player->v.v_angle, sv_player->v.punchangle, v_angle);
	angles[ROLL] = PM_CalcRoll (sv_player->v.angles, sv_player->v.velocity)*4;
	if (!sv_player->v.fixangle)
	{
		angles[PITCH] = -v_angle[PITCH]/3;
		angles[YAW] = v_angle[YAW];
	}

	if ((int)sv_player->v.flags & FL_WATERJUMP)
	{
		SV_WaterJump ();
		return;
	}
//
// walk
//
	if (sv_player->v.waterlevel >= 2 && sv_player->v.movetype != MOVETYPE_NOCLIP)
	{
		SV_WaterMove (cmd);
		return;
	}

	SV_AirMove (cmd);
}

/*
=============
SV_CheckStuck

This is a big hack to try and fix the rare case of getting stuck in the world
clipping hull.
=============
*/
static void SV_CheckStuck (edict_t *ent)
{
	int		i, j;
	int		z;
	vec3_t	org;

	if (!SV_TestEntityPosition(ent))
	{
		VectorCopy (ent->v.origin, ent->v.oldorigin);
		return;
	}

	VectorCopy (ent->v.origin, org);
	VectorCopy (ent->v.oldorigin, ent->v.origin);
	if (!SV_TestEntityPosition(ent))
	{
		Con_DPrintf ("Unstuck.\n");
		SV_LinkEdict (ent, true);
		return;
	}

	for (z=0 ; z< 18 ; z++)
		for (i=-1 ; i <= 1 ; i++)
			for (j=-1 ; j <= 1 ; j++)
			{
				ent->v.origin[0] = org[0] + i;
				ent->v.origin[1] = org[1] + j;
				ent->v.origin[2] = org[2] + z;
				if (!SV_TestEntityPosition(ent))
				{
					Con_DPrintf ("Unstuck.\n");
					SV_LinkEdict (ent, true);
					return;
				}
			}

	VectorCopy (org, ent->v.origin);
	Con_DPrintf ("player is stuck.\n");
}

// a player's water level and type, as NetQuake has them; true when swimming
static bool SV_CheckWater (edict_t *ent)
{
	vec3_t	point;
	int		cont;

	point[0] = ent->v.origin[0];
	point[1] = ent->v.origin[1];
	point[2] = ent->v.origin[2] + ent->v.mins[2] + 1;

	ent->v.waterlevel = 0;
	ent->v.watertype = CONTENTS_EMPTY;
	cont = SV_PointContents (point);
	if (cont <= CONTENTS_WATER)
	{
		ent->v.watertype = (float)cont;
		ent->v.waterlevel = 1;
		point[2] = ent->v.origin[2] + (ent->v.mins[2] + ent->v.maxs[2])*0.5f;
		cont = SV_PointContents (point);
		if (cont <= CONTENTS_WATER)
		{
			ent->v.waterlevel = 2;
			point[2] = ent->v.origin[2] + ent->v.view_ofs[2];
			cont = SV_PointContents (point);
			if (cont <= CONTENTS_WATER)
				ent->v.waterlevel = 3;
		}
	}

	return ent->v.waterlevel > 1;
}

static void SV_WallFriction (edict_t *ent, trace_t *trace)
{
	vec3_t	forward, right, up;
	float	d, i;
	vec3_t	into, side;

	AngleVectors (ent->v.v_angle, forward, right, up);
	d = DotProduct (trace->plane.normal, forward);

	d += 0.5;
	if (d >= 0)
		return;

// cut the tangential velocity
	i = DotProduct (trace->plane.normal, ent->v.velocity);
	VectorScale (trace->plane.normal, i, into);
	VectorSubtract (ent->v.velocity, into, side);

	ent->v.velocity[0] = side[0] * (1 + d);
	ent->v.velocity[1] = side[1] * (1 + d);
}

/*
=====================
SV_TryUnstick

Player has come to a dead stop, possibly due to the problem with limited
float precision at some angle joins in the BSP hull.

Try fixing by pushing one pixel in each direction.

This is a hack, but in the interest of good gameplay...
======================
*/
static int SV_TryUnstick (edict_t *ent, const vec3_t oldvel)
{
	static const vec3_t	dirs[8] = {{2, 0, 0}, {0, 2, 0}, {-2, 0, 0}, {0, -2, 0},
		{2, 2, 0}, {-2, 2, 0}, {2, -2, 0}, {-2, -2, 0}};
	int		i;
	vec3_t	oldorg, dir;
	int		clip;
	trace_t	steptrace;

	VectorCopy (ent->v.origin, oldorg);

	for (i=0 ; i<8 ; i++)
	{
	// try pushing a little in an axial direction
		VectorCopy (dirs[i], dir);
		SV_PushEntity (ent, dir);

	// retry the original move
		ent->v.velocity[0] = oldvel[0];
		ent->v.velocity[1] = oldvel[1];
		ent->v.velocity[2] = 0;
		clip = SV_FlyMove (ent, 0.1f, &steptrace);

		if (fabsf (oldorg[1] - ent->v.origin[1]) > 4 || fabsf (oldorg[0] - ent->v.origin[0]) > 4)
			return clip;

	// go back to the original pos and try again
		VectorCopy (oldorg, ent->v.origin);
	}

	VectorCopy (vec3_origin, ent->v.velocity);
	return 7;		// still not moving
}

/*
=====================
SV_WalkMove

A player's move, up steps
======================
*/
#define	STEPSIZE	18
static void SV_WalkMove (edict_t *ent)
{
	vec3_t		upmove, downmove;
	vec3_t		oldorg, oldvel;
	vec3_t		nosteporg, nostepvel;
	int			clip;
	int			oldonground;
	trace_t		steptrace, downtrace;

//
// do a regular slide move unless it looks like you ran into a step
//
	oldonground = (int)ent->v.flags & FL_ONGROUND;
	ent->v.flags = (float)((int)ent->v.flags & ~FL_ONGROUND);

	VectorCopy (ent->v.origin, oldorg);
	VectorCopy (ent->v.velocity, oldvel);

	clip = SV_FlyMove (ent, (float)sv.frametime, &steptrace);

	if (!(clip & 2))
		return;		// move didn't block on a step

	if (!oldonground && ent->v.waterlevel == 0)
		return;		// don't stair up while jumping

	if (ent->v.movetype != MOVETYPE_WALK)
		return;		// gibbed by a trigger

	if ((int)ent->v.flags & FL_WATERJUMP)
		return;

	VectorCopy (ent->v.origin, nosteporg);
	VectorCopy (ent->v.velocity, nostepvel);

//
// try moving up and forward to go up a step
//
	VectorCopy (oldorg, ent->v.origin);	// back to start pos

	VectorCopy (vec3_origin, upmove);
	VectorCopy (vec3_origin, downmove);
	upmove[2] = STEPSIZE;
	downmove[2] = -STEPSIZE + oldvel[2]*(float)sv.frametime;

// move up
	SV_PushEntity (ent, upmove);	// FIXME: don't link?

// move forward
	ent->v.velocity[0] = oldvel[0];
	ent->v.velocity[1] = oldvel[1];
	ent->v.velocity[2] = 0;
	clip = SV_FlyMove (ent, (float)sv.frametime, &steptrace);

// check for stuckness, possibly due to the limited precision of floats
// in the clipping hulls
	if (clip)
	{
		if (fabsf (oldorg[1] - ent->v.origin[1]) < 0.03125f && fabsf (oldorg[0] - ent->v.origin[0]) < 0.03125f)
		{	// stepping up didn't make any progress
			clip = SV_TryUnstick (ent, oldvel);
		}
	}

// extra friction based on view angle
	if (clip & 2)
		SV_WallFriction (ent, &steptrace);

// move down
	downtrace = SV_PushEntity (ent, downmove);	// FIXME: don't link?

	if (downtrace.plane.normal[2] > 0.7)
	{
		if (ent->v.solid == SOLID_BSP)
		{
			ent->v.flags = (float)((int)ent->v.flags | FL_ONGROUND);
			ent->v.groundentity = EDICT_TO_PROG(downtrace.ent);
		}
	}
	else
	{
// if the push down didn't end up on good ground, use the move without
// the step up.  This happens near wall / slope combinations, and can
// cause the player to hop up higher on a slope too steep to climb
		VectorCopy (nosteporg, ent->v.origin);
		VectorCopy (nostepvel, ent->v.velocity);
	}
}

/*
================
SV_Physics_Client

A NetQuake player's turn in the world's frame: PlayerPreThink, the move its
movetype makes, PlayerPostThink
================
*/
static void SV_Physics_Client (edict_t *ent)
{
	float	gravity;

//
// call standard client pre-think
//
	PR_GLOBAL(time) = (float)sv.time;
	PR_GLOBAL(self) = EDICT_TO_PROG(ent);
	PR_ExecuteProgram (PR_GLOBAL(PlayerPreThink));

//
// do a move
//
	SV_CheckVelocity (ent);

//
// decide which move function to call
//
	switch ((int)ent->v.movetype)
	{
	case MOVETYPE_NONE:
		if (!SV_RunThink (ent))
			return;
		break;

	case MOVETYPE_WALK:
		if (!SV_RunThink (ent))
			return;
		if (!SV_CheckWater (ent) && !((int)ent->v.flags & FL_WATERJUMP))
		{
			gravity = pr.fofs_gravity ? E_FLOAT (ent, pr.fofs_gravity) : 0;
			SV_AddGravity (ent, gravity ? gravity : 1);
		}
		SV_CheckStuck (ent);
		SV_WalkMove (ent);
		break;

	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
		SV_Physics_Toss (ent);
		break;

	case MOVETYPE_FLY:
		if (!SV_RunThink (ent))
			return;
		SV_FlyMove (ent, (float)sv.frametime, NULL);
		break;

	case MOVETYPE_NOCLIP:
		if (!SV_RunThink (ent))
			return;
		VectorMA (ent->v.origin, (float)sv.frametime, ent->v.velocity, ent->v.origin);
		break;

	default:
		SV_Error ("SV_Physics_Client: bad movetype %i", (int)ent->v.movetype);
	}

//
// call standard player post-think
//
	SV_LinkEdict (ent, true);

	PR_GLOBAL(time) = (float)sv.time;
	PR_GLOBAL(self) = EDICT_TO_PROG(ent);
	PR_ExecuteProgram (PR_GLOBAL(PlayerPostThink));
}

// the players NetQuake's way: each one's newest move, before the world's frame
static void SV_ClientThinks (void)
{
	int		i;

	for (i=0, host_client = svs.clients ; i<MAX_CLIENTS ; i++, host_client++)
	{
		if (host_client->state != cs_spawned || !SV_NQPhysics (host_client))
			continue;
		sv_player = host_client->edict;
		SV_ClientThink ();
	}
}

//============================================================================

void SV_ProgStartFrame (void)
{
// let the progs know that a new frame has started
	PR_GLOBAL(self) = EDICT_TO_PROG(sv.edicts);
	PR_GLOBAL(other) = EDICT_TO_PROG(sv.edicts);
	PR_GLOBAL(time) = (float)sv.time;
	PR_ExecuteProgram (PR_GLOBAL(StartFrame));
}

/*
================
SV_RunEntity

================
*/
static void SV_RunEntity (edict_t *ent)
{
	if (ent->v.lastruntime == (float)host.realtime)
		return;
	ent->v.lastruntime = (float)host.realtime;

	switch ( (int)ent->v.movetype)
	{
	case MOVETYPE_PUSH:
		SV_Physics_Pusher (ent);
		break;
	case MOVETYPE_NONE:
		SV_Physics_None (ent);
		break;
	case MOVETYPE_NOCLIP:
		SV_Physics_Noclip (ent);
		break;
	case MOVETYPE_STEP:
		SV_Physics_Step (ent);
		break;
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
		SV_Physics_Toss (ent);
		break;
	default:
		SV_Error ("SV_Physics: bad movetype %i", (int)ent->v.movetype);			
	}
}

/*
================
SV_RunNewmis

================
*/
void SV_RunNewmis (void)
{
	edict_t	*ent;

	if (!PR_GLOBAL(newmis))
		return;
	ent = PROG_TO_EDICT(PR_GLOBAL(newmis));
	sv.frametime = 0.05;
	PR_GLOBAL(newmis) = 0;
	
	SV_RunEntity (ent);		
}

/*
================
SV_Physics

================
*/
/*
================
SV_NextFrameWait

Seconds until SV_Physics runs again, for the host to sleep in between
================
*/
// the shortest physics step: FTE's 0.013 while NetQuake's players move in it
static double SV_MinTic (void)
{
	return pr.nq && sv_mintic.value > 0.013f ? 0.013 : sv_mintic.value;
}

double SV_NextFrameWait (void)
{
	double	wait;
	int		i;

	if (sv.paused)
		return 0.1;

	// with nobody connected, physics can run at the slowest rate
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (svs.clients[i].state != cs_free)
			break;
	wait = sv.physicstime + (i == MAX_CLIENTS ? sv_maxtic.value : SV_MinTic ()) - sv.time;
	if (wait < 0)
		return 0;
	return wait > 0.1 ? 0.1 : wait;
}

void SV_Physics (void)
{
	int		i;
	edict_t	*ent;

// don't bother running a frame if sys_ticrate seconds haven't passed
	sv.frametime = sv.time - sv.physicstime;
	if (sv.frametime < SV_MinTic ())
		return;
	if (sv.frametime > sv_maxtic.value)
		sv.frametime = sv_maxtic.value;
	sv.physicstime = sv.time;

	PR_GLOBAL(frametime) = (float)sv.frametime;

	if (pr.nq)
		SV_ClientThinks ();

	SV_ProgStartFrame ();
	PR_RunThreads ();

//
// treat each object in turn
// even the world gets a chance to think
//
	ent = sv.edicts;
	for (i=0 ; i<sv.num_edicts ; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;

		if (PR_GLOBAL(force_retouch))
			SV_LinkEdict (ent, true);	// force retouch even for stationary

		if (i > 0 && i <= MAX_CLIENTS)
		{	// QuakeWorld's are run directly from packets
			host_client = &svs.clients[i-1];
			if (host_client->state == cs_spawned && SV_NQPhysics (host_client))
			{
				sv_player = ent;
				SV_Physics_Client (ent);
			}
			continue;
		}

		SV_RunEntity (ent);
		SV_RunNewmis ();
	}
	
	if (PR_GLOBAL(force_retouch))
		PR_GLOBAL(force_retouch)--;	
}

void SV_SetMoveVars(void)
{
	sv.movevars.gravity			= sv_gravity.value; 
	sv.movevars.stopspeed		    = sv_stopspeed.value;		 
	sv.movevars.maxspeed			= sv_maxspeed.value;			 
	sv.movevars.spectatormaxspeed  = sv_spectatormaxspeed.value; 
	sv.movevars.accelerate		    = sv_accelerate.value;		 
	sv.movevars.airaccelerate	    = sv_airaccelerate.value;	 
	sv.movevars.wateraccelerate	= sv_wateraccelerate.value;	   
	sv.movevars.friction			= sv_friction.value;			 
	sv.movevars.waterfriction	    = sv_waterfriction.value;	 
	sv.movevars.entgravity			= 1.0;
}
