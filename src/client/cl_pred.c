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
#include "cl_local.h"

static cvar_t	cl_nopred = {.name = "cl_nopred", .string = "0"};
static cvar_t	cl_pushlatency = {.name = "pushlatency", .string = "-999"};
// send commands at cl_physfps and draw frames at cl_maxfps
static cvar_t	cl_independentPhysics = {.name = "cl_independentPhysics", .string = "1", .archive = true};
// commands per second; 0 uses 77, never more than the server's maxfps
cvar_t	cl_physfps = {.name = "cl_physfps", .string = "0", .archive = true};
// don't interpolate the view between commands
static cvar_t	cl_nolerp = {.name = "cl_nolerp", .string = "0", .archive = true};

playermove_t	cl_pmove;


/*
==============
CL_PredictUsercmd
==============
*/
void CL_PredictUsercmd (player_state_t *from, player_state_t *to, usercmd_t *u, bool spectator)
{
	// split up very long moves
	if (u->msec > 50)
	{
		player_state_t	temp;
		usercmd_t	split;

		split = *u;
		split.msec /= 2;

		CL_PredictUsercmd (from, &temp, &split, spectator);
		CL_PredictUsercmd (&temp, to, &split, spectator);
		return;
	}

	VectorCopy (from->origin, cl_pmove.origin);
//	VectorCopy (from->viewangles, pmove.angles);
	VectorCopy (u->angles, cl_pmove.angles);
	VectorCopy (from->velocity, cl_pmove.velocity);

	cl_pmove.oldbuttons = from->oldbuttons;
	cl_pmove.waterjumptime = from->waterjumptime;
	cl_pmove.dead = cl.stats[STAT_HEALTH] <= 0;
	cl_pmove.spectator = spectator;

	cl_pmove.cmd = *u;

	PM_PlayerMove (&cl_pmove, &cl.movevars);
//for (i=0 ; i<3 ; i++)
//pmove.origin[i] = ((int)(pmove.origin[i]*8))*0.125;
	to->waterjumptime = cl_pmove.waterjumptime;
	to->oldbuttons = cl_pmove.cmd.buttons;
	VectorCopy (cl_pmove.origin, to->origin);
	VectorCopy (cl_pmove.angles, to->viewangles);
	VectorCopy (cl_pmove.velocity, to->velocity);
	to->onground = cl_pmove.onground;

	to->weaponframe = from->weaponframe;
}



bool CL_IndependentPhysics (void)
{
	return cl_independentPhysics.value && !cls.demoplayback;
}

/*
==============
CL_PredictOrigin

Runs the player move for every command the server hasn't acknowledged yet
==============
*/
static void CL_PredictOrigin (void)
{
	int			i;
	float		f;
	frame_t		*from, *to = NULL;
	int			oldphysent;

	// this is the last frame received from the server
	from = &cl.frames[cls.netchan.incoming_sequence & UPDATE_MASK];
	cl.onground = from->playerstate[cl.playernum].onground != -1;

	if (cl_nopred.value)
	{
		VectorCopy (from->playerstate[cl.playernum].velocity, cl.simvel);
		VectorCopy (from->playerstate[cl.playernum].origin, cl.simorg);
		return;
	}

	// predict forward until cl.time <= to->senttime
	oldphysent = cl_pmove.numphysent;
	CL_SetSolidPlayers (cl.playernum);

//	to = &cl.frames[cls.netchan.incoming_sequence & UPDATE_MASK];

	for (i=1 ; i<UPDATE_BACKUP-1 && cls.netchan.incoming_sequence+i <
			cls.netchan.outgoing_sequence; i++)
	{
		to = &cl.frames[(cls.netchan.incoming_sequence+i) & UPDATE_MASK];
		CL_PredictUsercmd (&from->playerstate[cl.playernum]
			, &to->playerstate[cl.playernum], &to->cmd, cl.spectator);
		cl.onground = to->playerstate[cl.playernum].onground != -1;
		if (to->senttime >= cl.time)
			break;
		from = to;
	}

	cl_pmove.numphysent = oldphysent;

	if (i == UPDATE_BACKUP-1 || !to)
		return;		// net hasn't deliver packets in a long time...

	// now interpolate some fraction of the final frame
	if (to->senttime == from->senttime)
		f = 0;
	else
	{
		f = (float)((cl.time - from->senttime) / (to->senttime - from->senttime));

		if (f < 0)
			f = 0;
		if (f > 1)
			f = 1;
	}

	for (i=0 ; i<3 ; i++)
		if ( fabs(from->playerstate[cl.playernum].origin[i] - to->playerstate[cl.playernum].origin[i]) > 128)
		{	// teleported, so don't lerp
			VectorCopy (to->playerstate[cl.playernum].velocity, cl.simvel);
			VectorCopy (to->playerstate[cl.playernum].origin, cl.simorg);
			return;
		}
		
	for (i=0 ; i<3 ; i++)
	{
		cl.simorg[i] = from->playerstate[cl.playernum].origin[i] 
			+ f*(to->playerstate[cl.playernum].origin[i] - from->playerstate[cl.playernum].origin[i]);
		cl.simvel[i] = from->playerstate[cl.playernum].velocity[i] 
			+ f*(to->playerstate[cl.playernum].velocity[i] - from->playerstate[cl.playernum].velocity[i]);
	}		
}

/*
===============================================================================

VIEW SMOOTHING

===============================================================================
*/

static bool		lerp_disabled[2];		// don't interpolate to lerp_origin[0] / [1]
static bool		lerp_disable_next;

void CL_DisableLerpMove (void)
{
	lerp_disabled[0] = lerp_disabled[1] = lerp_disable_next = true;
}

/*
==============
CL_LerpMove

Commands are sent at the physics rate. In between, the view moves smoothly
from the position predicted for one command to the next, running a little
behind: the delay adapts so the view stays between the last three commands.
==============
*/
static void CL_LerpMove (void)
{
	static int		lastsequence;
	static vec3_t	lerp_origin[3];
	static double	lerp_times[3];		// command time of each origin
	static double	lerp_delay = 0.01;	// realtime minus the command time shown
	double			simtime, now;
	float			frac;
	int				i, from, to;
	bool			newcmd;

	if (cl_nolerp.value)
	{
		lerp_disabled[0] = lerp_disabled[1] = lerp_disable_next = false;
		lastsequence = INT_MAX;		// start over when turned back on
		return;
	}

	if (cls.netchan.outgoing_sequence < lastsequence)
	{	// new connection
		lastsequence = -1;
		lerp_times[0] = -1;
		lerp_delay = 0.01;
	}

	now = realtime;
	newcmd = cls.netchan.outgoing_sequence != lastsequence;
	if (newcmd)
	{
		lastsequence = cls.netchan.outgoing_sequence;

		lerp_times[2] = lerp_times[1];
		lerp_times[1] = lerp_times[0];
		lerp_times[0] = cl.cmdtime_msec * 0.001;
		VectorCopy (lerp_origin[1], lerp_origin[2]);
		VectorCopy (lerp_origin[0], lerp_origin[1]);
		VectorCopy (cl.simorg, lerp_origin[0]);

		lerp_disabled[1] = lerp_disabled[0];
		lerp_disabled[0] = lerp_disable_next;
		lerp_disable_next = false;

		// a teleport or the like
		for (i=0 ; i<3 ; i++)
			if (fabsf (lerp_origin[0][i] - lerp_origin[1][i]) > 100)
				lerp_disabled[0] = true;
	}

	simtime = now - lerp_delay;
	if (simtime > lerp_times[0])
		lerp_delay = now - lerp_times[0];		// running ahead: catch up
	else if (simtime < lerp_times[2])
		lerp_delay = now - lerp_times[2];		// running too far behind
	else if (newcmd)
		lerp_delay -= fmin (cls.physframetime * 0.005, 0.001);	// creep closer
	simtime = now - lerp_delay;

	if (simtime > lerp_times[1])
	{
		from = 1;
		to = 0;
	}
	else
	{
		from = 2;
		to = 1;
	}
	if (lerp_disabled[to] || lerp_times[to] <= lerp_times[from])
		return;

	frac = (float)((simtime - lerp_times[from]) / (lerp_times[to] - lerp_times[from]));
	frac = fmaxf (0, fminf (frac, 1));
	for (i=0 ; i<3 ; i++)
		cl.simorg[i] = lerp_origin[from][i] + (lerp_origin[to][i] - lerp_origin[from][i]) * frac;
}

/*
==============
CL_CalcCrouch

Smooths stair step ups: the view lags behind when the player steps up, and
catches up faster on steep stairs
==============
*/
static void CL_CalcCrouch (void)
{
	static vec3_t	oldorigin;
	static float	oldz, extracrouch, crouchspeed = 100;
	vec3_t			delta;
	float			frametime = (float)host_frametime;
	bool			teleported;

	VectorSubtract (cl.simorg, oldorigin, delta);
	teleported = lerp_disabled[0] || DotProduct (delta, delta) > 48*48;
	VectorCopy (cl.simorg, oldorigin);

	if (teleported)
	{	// teleported or respawned
		oldz = cl.simorg[2];
		extracrouch = 0;
		crouchspeed = 100;
		cl.crouch = 0;
		return;
	}

	if (cl.onground && cl.simorg[2] - oldz > 0)
	{
		if (cl.simorg[2] - oldz > 20)
		{	// steep stairs: catch up faster
			if (crouchspeed < 160)
			{
				extracrouch = cl.simorg[2] - oldz - frametime * 200 - 15;
				extracrouch = fminf (extracrouch, 5);
			}
			crouchspeed = 160;
		}

		oldz += frametime * crouchspeed;
		if (oldz > cl.simorg[2])
			oldz = cl.simorg[2];
		if (cl.simorg[2] - oldz > 15 + extracrouch)
			oldz = cl.simorg[2] - 15 - extracrouch;
		extracrouch -= frametime * 200;
		extracrouch = fmaxf (extracrouch, 0);

		cl.crouch = oldz - cl.simorg[2];
	}
	else
	{	// in the air or moving down
		oldz = cl.simorg[2];
		cl.crouch += frametime * 150;
		if (cl.crouch > 0)
			cl.crouch = 0;
		crouchspeed = 100;
		extracrouch = 0;
	}
}

/*
==============
CL_PredictMove
==============
*/
void CL_PredictMove (bool repredict)
{
	if (cl_pushlatency.value > 0)
		Cvar_Set ("pushlatency", "0");

	if (cl.paused)
		return;

	cl.time = realtime - cls.latency - cl_pushlatency.value*0.001;
	if (cl.time > realtime)
		cl.time = realtime;
	r_scene.time = cl.time;

	if (cl.intermission)
	{
		cl.crouch = 0;
		return;
	}

	if (!cl.validsequence)
		return;

	if (cls.netchan.outgoing_sequence - cls.netchan.incoming_sequence >= UPDATE_BACKUP-1)
		return;

	VectorCopy (cl.viewangles, cl.simangles);

	// we can now render a frame
	if (cls.state == ca_onserver)
	{	// first update is the final signon stage
		char		text[1024];

		cls.state = ca_active;
		snprintf (text, sizeof(text), "QuakeWorld: %s", cls.servername);
		VID_SetCaption (text);
	}

	if (repredict || !CL_IndependentPhysics ())
		CL_PredictOrigin ();
	if (CL_IndependentPhysics ())
		CL_LerpMove ();
	CL_CalcCrouch ();
}


/*
==============
CL_InitPrediction
==============
*/
void CL_InitPrediction (void)
{
	Cvar_RegisterVariable (&cl_pushlatency);
	Cvar_RegisterVariable (&cl_nopred);
	Cvar_RegisterVariable (&cl_independentPhysics);
	Cvar_RegisterVariable (&cl_physfps);
	Cvar_RegisterVariable (&cl_nolerp);
}

