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
// view.c -- player eye positioning

#include "cl_local.h"

/*

The view is allowed to move slightly from it's true position for bobbing,
but if it exceeds 8 pixels linear distance (spherical, not box), the list of
entities sent from the server may not include everything in the pvs, especially
when crossing a water boudnary.

*/



static cvar_t	cl_bob = {.name = "cl_bob", .string = "0.02"};
static cvar_t	cl_bobcycle = {.name = "cl_bobcycle", .string = "0.6"};
static cvar_t	cl_bobup = {.name = "cl_bobup", .string = "0.5"};

static cvar_t	v_kicktime = {.name = "v_kicktime", .string = "0.5"};
static cvar_t	v_kickroll = {.name = "v_kickroll", .string = "0.6"};
static cvar_t	v_kickpitch = {.name = "v_kickpitch", .string = "0.6"};

static cvar_t	v_iyaw_cycle = {.name = "v_iyaw_cycle", .string = "2"};
static cvar_t	v_iroll_cycle = {.name = "v_iroll_cycle", .string = "0.5"};
static cvar_t	v_ipitch_cycle = {.name = "v_ipitch_cycle", .string = "1"};
static cvar_t	v_iyaw_level = {.name = "v_iyaw_level", .string = "0.3"};
static cvar_t	v_iroll_level = {.name = "v_iroll_level", .string = "0.1"};
static cvar_t	v_ipitch_level = {.name = "v_ipitch_level", .string = "0.3"};

static cvar_t	v_idlescale = {.name = "v_idlescale", .string = "0"};

static cvar_t	crosshair = {.name = "crosshair", .string = "0", .archive = true};
static cvar_t	crosshaircolor = {.name = "crosshaircolor", .string = "79", .archive = true};

static cvar_t  cl_crossx = {.name = "cl_crossx", .string = "0", .archive = true};
static cvar_t  cl_crossy = {.name = "cl_crossy", .string = "0", .archive = true};


static cvar_t  v_contentblend = {.name = "v_contentblend", .string = "1"};

static float	v_dmg_time, v_dmg_roll, v_dmg_pitch;

extern	int in_forward2;

static frame_t		*view_frame;
static player_state_t		*view_message;

/*
===============
V_CalcRoll

===============
*/
static float V_CalcRoll (vec3_t angles, vec3_t velocity)
{
	return PM_CalcRoll (angles, velocity);
}


/*
===============
V_CalcBob

===============
*/
static float V_CalcBob (void)
{
	static	double	bobtime;
	static float	bob;
	float	cycle;
	
	if (cl.spectator)
		return 0;

	// the local player's ground: cl.pmove holds whichever player moved last,
	// often another one run forward to be drawn
	if (!cl.onground)
		return bob;		// just use old value

	bobtime += cls.frametime;
	cycle = (float)(bobtime - (int)(bobtime/cl_bobcycle.value)*cl_bobcycle.value);
	cycle /= cl_bobcycle.value;
	if (cycle < cl_bobup.value)
		cycle = (float)(Q_PI * cycle / cl_bobup.value);
	else
		cycle = (float)(Q_PI + Q_PI*(cycle-cl_bobup.value)/(1.0f - cl_bobup.value));

// bob is proportional to simulated velocity in the xy plane
// (don't count Z, or jumping messes it up)

	bob = sqrtf(cl.simvel[0]*cl.simvel[0] + cl.simvel[1]*cl.simvel[1]) * cl_bob.value;
	bob = bob*0.3f + bob*0.7f*sinf(cycle);
	if (bob > 4)
		bob = 4;
	else if (bob < -7)
		bob = -7;
	return bob;
	
}


//=============================================================================


static cvar_t	v_centermove = {.name = "v_centermove", .string = "0.15"};
static cvar_t	v_centerspeed = {.name = "v_centerspeed", .string = "500"};


void V_StartPitchDrift (void)
{
	if (cl.laststop == cl.time)
	{
		return;		// something else is keeping it from drifting
	}
	if (cl.nodrift || !cl.pitchvel)
	{
		cl.pitchvel = v_centerspeed.value;
		cl.nodrift = false;
		cl.driftmove = 0;
	}
}

void V_StopPitchDrift (void)
{
	cl.laststop = cl.time;
	cl.nodrift = true;
	cl.pitchvel = 0;
}

/*
===============
V_DriftPitch

Moves the client pitch angle towards cl.idealpitch sent by the server.

If the user is adjusting pitch manually, either with lookup/lookdown,
mlook and mouse, or klook and keyboard, pitch drifting is constantly stopped.

Drifting is enabled when the center view key is hit, mlook is released and
lookspring is non 0, or when 
===============
*/
static void V_DriftPitch (void)
{
	float		delta, move;

	if (!view_message->onground || cls.demoplayback )
	{
		cl.driftmove = 0;
		cl.pitchvel = 0;
		return;
	}

// don't count small mouse motion
	if (cl.nodrift)
	{
		if ( abs(cl.frames[(cls.netchan.outgoing_sequence-1)&UPDATE_MASK].cmd.forwardmove) < 200)
			cl.driftmove = 0;
		else
			cl.driftmove = (float)(cl.driftmove + cls.frametime);
	
		if ( cl.driftmove > v_centermove.value)
		{
			V_StartPitchDrift ();
		}
		return;
	}
	
	delta = 0 - cl.viewangles[PITCH];

	if (!delta)
	{
		cl.pitchvel = 0;
		return;
	}

	move = (float)(cls.frametime * cl.pitchvel);
	cl.pitchvel = (float)(cl.pitchvel + cls.frametime * v_centerspeed.value);
	
//Con_Printf ("move: %f (%f)\n", move, host_frametime);

	if (delta > 0)
	{
		if (move > delta)
		{
			cl.pitchvel = 0;
			move = delta;
		}
		cl.viewangles[PITCH] += move;
	}
	else if (delta < 0)
	{
		if (move > -delta)
		{
			cl.pitchvel = 0;
			move = -delta;
		}
		cl.viewangles[PITCH] -= move;
	}
}


/*
============================================================================== 
 
						PALETTE FLASHES 
 
============================================================================== 
*/ 


static cshift_t	cshift_empty = { {130,80,50}, 0 };
static cshift_t	cshift_water = { {130,80,50}, 128 };
static cshift_t	cshift_slime = { {0,25,5}, 150 };
static cshift_t	cshift_lava = { {255,80,0}, 150 };

cvar_t		v_gamma = {.name = "gamma", .string = "1", .archive = true};



/*
===============
V_ParseDamage
===============
*/
void V_ParseDamage (void)
{
	int		armor, blood;
	vec3_t	from;
	int		i;
	vec3_t	forward, right, up;
	float	side;
	float	count;
	
	armor = MSG_ReadByte ();
	blood = MSG_ReadByte ();
	for (i=0 ; i<3 ; i++)
		from[i] = MSG_ReadCoord ();

	count = blood*0.5f + armor*0.5f;
	if (count < 10)
		count = 10;

	cl.faceanimtime = (float)(cl.time + 0.2f);		// but sbar face into pain frame

	cl.cshifts[CSHIFT_DAMAGE].percent = (int)(cl.cshifts[CSHIFT_DAMAGE].percent + 3*count);
	if (cl.cshifts[CSHIFT_DAMAGE].percent < 0)
		cl.cshifts[CSHIFT_DAMAGE].percent = 0;
	if (cl.cshifts[CSHIFT_DAMAGE].percent > 150)
		cl.cshifts[CSHIFT_DAMAGE].percent = 150;

	if (armor > blood)		
	{
		cl.cshifts[CSHIFT_DAMAGE].destcolor[0] = 200;
		cl.cshifts[CSHIFT_DAMAGE].destcolor[1] = 100;
		cl.cshifts[CSHIFT_DAMAGE].destcolor[2] = 100;
	}
	else if (armor)
	{
		cl.cshifts[CSHIFT_DAMAGE].destcolor[0] = 220;
		cl.cshifts[CSHIFT_DAMAGE].destcolor[1] = 50;
		cl.cshifts[CSHIFT_DAMAGE].destcolor[2] = 50;
	}
	else
	{
		cl.cshifts[CSHIFT_DAMAGE].destcolor[0] = 255;
		cl.cshifts[CSHIFT_DAMAGE].destcolor[1] = 0;
		cl.cshifts[CSHIFT_DAMAGE].destcolor[2] = 0;
	}

//
// calculate view angle kicks
//
	VectorSubtract (from, cl.simorg, from);
	VectorNormalize (from);
	
	AngleVectors (cl.simangles, forward, right, up);

	side = DotProduct (from, right);
	v_dmg_roll = count*side*v_kickroll.value;
	
	side = DotProduct (from, forward);
	v_dmg_pitch = count*side*v_kickpitch.value;

	v_dmg_time = v_kicktime.value;
}


/*
==================
V_cshift_f
==================
*/
static void V_cshift_f (void)
{
	cshift_empty.destcolor[0] = atoi(Cmd_Argv(1));
	cshift_empty.destcolor[1] = atoi(Cmd_Argv(2));
	cshift_empty.destcolor[2] = atoi(Cmd_Argv(3));
	cshift_empty.percent = atoi(Cmd_Argv(4));
}


/*
==================
V_BonusFlash_f

When you run over an item, the server sends this command
==================
*/
static void V_BonusFlash_f (void)
{
	cl.cshifts[CSHIFT_BONUS].destcolor[0] = 215;
	cl.cshifts[CSHIFT_BONUS].destcolor[1] = 186;
	cl.cshifts[CSHIFT_BONUS].destcolor[2] = 69;
	cl.cshifts[CSHIFT_BONUS].percent = 50;
}

/*
=============
V_SetContentsColor

Underwater, lava, etc each has a color shift
=============
*/
static void V_SetContentsColor (int contents)
{
	if (!v_contentblend.value) {
		cl.cshifts[CSHIFT_CONTENTS] = cshift_empty;
		return;
	}

	switch (contents)
	{
	case CONTENTS_EMPTY:
		cl.cshifts[CSHIFT_CONTENTS] = cshift_empty;
		break;
	case CONTENTS_LAVA:
		cl.cshifts[CSHIFT_CONTENTS] = cshift_lava;
		break;
	case CONTENTS_SOLID:
	case CONTENTS_SLIME:
		cl.cshifts[CSHIFT_CONTENTS] = cshift_slime;
		break;
	default:
		cl.cshifts[CSHIFT_CONTENTS] = cshift_water;
	}
}

/*
=============
V_CalcPowerupCshift
=============
*/
static void V_CalcPowerupCshift (void)
{
	if (cl.stats[STAT_ITEMS] & IT_QUAD)
	{
		cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 0;
		cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 0;
		cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 255;
		cl.cshifts[CSHIFT_POWERUP].percent = 30;
	}
	else if (cl.stats[STAT_ITEMS] & IT_SUIT)
	{
		cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 0;
		cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 255;
		cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 0;
		cl.cshifts[CSHIFT_POWERUP].percent = 20;
	}
	else if (cl.stats[STAT_ITEMS] & IT_INVISIBILITY)
	{
		cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 100;
		cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 100;
		cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 100;
		cl.cshifts[CSHIFT_POWERUP].percent = 100;
	}
	else if (cl.stats[STAT_ITEMS] & IT_INVULNERABILITY)
	{
		cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 255;
		cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 255;
		cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 0;
		cl.cshifts[CSHIFT_POWERUP].percent = 30;
	}
	else
		cl.cshifts[CSHIFT_POWERUP].percent = 0;
}


/*
=============
V_CalcBlend
=============
*/

/*
=============
V_UpdateBlend

The color shifts and gamma go to the presenter, which applies them to the 3D
view
=============
*/
void V_UpdateBlend (void)
{
	vid_present_t	present = {0};
	float			p, keep, rgb[3] = {0, 0, 0};
	int				i, j;

	V_CalcPowerupCshift ();

// drop the damage value
	cl.cshifts[CSHIFT_DAMAGE].percent = (int)(cl.cshifts[CSHIFT_DAMAGE].percent - cls.frametime*150);
	if (cl.cshifts[CSHIFT_DAMAGE].percent <= 0)
		cl.cshifts[CSHIFT_DAMAGE].percent = 0;

// drop the bonus value
	cl.cshifts[CSHIFT_BONUS].percent = (int)(cl.cshifts[CSHIFT_BONUS].percent - cls.frametime*100);
	if (cl.cshifts[CSHIFT_BONUS].percent <= 0)
		cl.cshifts[CSHIFT_BONUS].percent = 0;

// the shifts apply one after another, each moving the color percent/256 of
// the way to its own; together they are one move of the color toward rgb/(1-keep)
	keep = 1;
	for (i=0 ; i<NUM_CSHIFTS ; i++)
	{
		p = fminf (fmaxf (cl.cshifts[i].percent / 256.0f, 0), 1);
		for (j=0 ; j<3 ; j++)
			rgb[j] = rgb[j] * (1 - p) + cl.cshifts[i].destcolor[j] / 255.0f * p;
		keep *= 1 - p;
	}
	present.blend[3] = 1 - keep;
	for (j=0 ; j<3 ; j++)
		present.blend[j] = present.blend[3] > 0 ? rgb[j] / present.blend[3] : 0;

	present.gamma = v_gamma.value > 0 ? v_gamma.value : 1;
	VID_SetPresent (&present);
}


/* 
============================================================================== 
 
						VIEW RENDERING 
 
============================================================================== 
*/ 

static float angledelta (float a)
{
	a = anglemod(a);
	if (a > 180)
		a -= 360;
	return a;
}

/*
==================
CalcGunAngle
==================
*/
static void CalcGunAngle (void)
{	
	float	yaw, pitch, move;
	static float oldyaw = 0;
	static float oldpitch = 0;
	
	yaw = r_refdef.viewangles[YAW];
	pitch = -r_refdef.viewangles[PITCH];

	yaw = angledelta(yaw - r_refdef.viewangles[YAW]) * 0.4f;
	if (yaw > 10)
		yaw = 10;
	if (yaw < -10)
		yaw = -10;
	pitch = angledelta(-pitch - r_refdef.viewangles[PITCH]) * 0.4f;
	if (pitch > 10)
		pitch = 10;
	if (pitch < -10)
		pitch = -10;
	move = (float)(cls.frametime*20);
	if (yaw > oldyaw)
	{
		if (oldyaw + move < yaw)
			yaw = oldyaw + move;
	}
	else
	{
		if (oldyaw - move > yaw)
			yaw = oldyaw - move;
	}
	
	if (pitch > oldpitch)
	{
		if (oldpitch + move < pitch)
			pitch = oldpitch + move;
	}
	else
	{
		if (oldpitch - move > pitch)
			pitch = oldpitch - move;
	}
	
	oldyaw = yaw;
	oldpitch = pitch;

	cl.viewent.angles[YAW] = r_refdef.viewangles[YAW] + yaw;
	cl.viewent.angles[PITCH] = - (r_refdef.viewangles[PITCH] + pitch);
}

/*
==============
V_AddIdle

Idle swaying
==============
*/
static void V_AddIdle (void)
{
	r_refdef.viewangles[ROLL] += v_idlescale.value * (float)sin(cl.time*v_iroll_cycle.value) * v_iroll_level.value;
	r_refdef.viewangles[PITCH] += v_idlescale.value * (float)sin(cl.time*v_ipitch_cycle.value) * v_ipitch_level.value;
	r_refdef.viewangles[YAW] += v_idlescale.value * (float)sin(cl.time*v_iyaw_cycle.value) * v_iyaw_level.value;

	cl.viewent.angles[ROLL] -= v_idlescale.value * (float)sin(cl.time*v_iroll_cycle.value) * v_iroll_level.value;
	cl.viewent.angles[PITCH] -= v_idlescale.value * (float)sin(cl.time*v_ipitch_cycle.value) * v_ipitch_level.value;
	cl.viewent.angles[YAW] -= v_idlescale.value * (float)sin(cl.time*v_iyaw_cycle.value) * v_iyaw_level.value;
}


/*
==============
V_CalcViewRoll

Roll is induced by movement and damage
==============
*/
static void V_CalcViewRoll (void)
{
	float		side;
		
	side = V_CalcRoll (cl.simangles, cl.simvel);
	r_refdef.viewangles[ROLL] += side;

	if (v_dmg_time > 0)
	{
		r_refdef.viewangles[ROLL] += v_dmg_time/v_kicktime.value*v_dmg_roll;
		r_refdef.viewangles[PITCH] += v_dmg_time/v_kicktime.value*v_dmg_pitch;
		v_dmg_time = (float)(v_dmg_time - cls.frametime);
	}

}


/*
==================
V_CalcIntermissionRefdef

==================
*/
static void V_CalcIntermissionRefdef (void)
{
	entity_t	*view;
	float		old;

// view is the weapon model
	view = &cl.viewent;

	VectorCopy (cl.simorg, r_refdef.vieworg);
	VectorCopy (cl.simangles, r_refdef.viewangles);
	view->model = NULL;

// allways idle in intermission
	old = v_idlescale.value;
	v_idlescale.value = 1;
	V_AddIdle ();
	v_idlescale.value = old;
}

/*
==================
V_CalcRefdef

==================
*/
static void V_CalcRefdef (void)
{
	entity_t	*view;
	int			i;
	vec3_t		forward, right, up;
	float		bob;

	V_DriftPitch ();

// view is the weapon model (only visible from inside body)
	view = &cl.viewent;

	bob = V_CalcBob ();
	
// refresh position from simulated origin
	VectorCopy (cl.simorg, r_refdef.vieworg);

	r_refdef.vieworg[2] += bob;

// never let it sit exactly on a node line, because a water plane can
// dissapear when viewed with the eye exactly on it.
// the server protocol only specifies to 1/8 pixel, so add 1/16 in each axis
	r_refdef.vieworg[0] += 1.0/16;
	r_refdef.vieworg[1] += 1.0/16;
	r_refdef.vieworg[2] += 1.0/16;

	VectorCopy (cl.simangles, r_refdef.viewangles);
	V_CalcViewRoll ();
	V_AddIdle ();

	if (view_message->flags & PF_GIB)
		r_refdef.vieworg[2] += 8;	// gib view height
	else if (view_message->flags & PF_DEAD)
		r_refdef.vieworg[2] -= 16;	// corpse view height
	else
		r_refdef.vieworg[2] += CL_ViewHeight ();

	if (view_message->flags & PF_DEAD)		// PF_GIB will also set PF_DEAD
		r_refdef.viewangles[ROLL] = 80;	// dead view angle


// offsets
	AngleVectors (cl.simangles, forward, right, up);
	
// set up gun position
	VectorCopy (cl.simangles, view->angles);
	
	CalcGunAngle ();

	VectorCopy (cl.simorg, view->origin);
	view->origin[2] += CL_ViewHeight ();

	for (i=0 ; i<3 ; i++)
	{
		view->origin[i] += forward[i]*bob*0.4f;
//		view->origin[i] += right[i]*bob*0.4;
//		view->origin[i] += up[i]*bob*0.8;
	}
	view->origin[2] += bob;

// fudge position around to keep amount of weapon visible
// roughly equal with different FOV
	if (scr_viewsize.value == 110)
		view->origin[2] += 1;
	else if (scr_viewsize.value == 100)
		view->origin[2] += 2;
	else if (scr_viewsize.value == 90)
		view->origin[2] += 1;
	else if (scr_viewsize.value == 80)
		view->origin[2] += 0.5;

	if (view_message->flags & (PF_GIB|PF_DEAD) )
 		view->model = NULL;
 	else
		view->model = CL_Model (cl.stats[STAT_WEAPON]);
	view->frame = view_message->weaponframe;
	view->translate = NULL;
	view->palette = NULL;

// set up the refresh position
	r_refdef.viewangles[PITCH] += cl.punchangle;

// smooth out stair step ups (CL_CalcCrouch)
	r_refdef.vieworg[2] += cl.crouch;
	view->origin[2] += cl.crouch;
}

/*
=============
DropPunchAngle
=============
*/
static void DropPunchAngle (void)
{
	cl.punchangle = (float)(cl.punchangle - 10*cls.frametime);
	if (cl.punchangle < 0)
		cl.punchangle = 0;
}

static void V_DrawCrosshair (void)
{
	int x, y;
	byte c = (byte)crosshaircolor.value;

	if (crosshair.value == 2) {
		x = (int)(scr.vrect.x + scr.vrect.width/2 + cl_crossx.value); 
		y = (int)(scr.vrect.y + scr.vrect.height/2 + cl_crossy.value);
		Draw_Pixel(x - 1, y, c);
		Draw_Pixel(x - 3, y, c);
		Draw_Pixel(x + 1, y, c);
		Draw_Pixel(x + 3, y, c);
		Draw_Pixel(x, y - 1, c);
		Draw_Pixel(x, y - 3, c);
		Draw_Pixel(x, y + 1, c);
		Draw_Pixel(x, y + 3, c);
	} else if (crosshair.value)
		Draw_Character (
			(int)(scr.vrect.x + scr.vrect.width/2-4 + cl_crossx.value),
			(int)(scr.vrect.y + scr.vrect.height/2-4 + cl_crossy.value),
			'+');
}

/*
==================
V_RenderView

The player's clipping box goes from (-16 -16 -24) to (16 16 32) from
the entity origin, so any view position inside that will be valid
==================
*/

void V_RenderView (void)
{
//	if (cl.simangles[ROLL])
//		Sys_Error ("cl.simangles[ROLL]");	// DEBUG
cl.simangles[ROLL] = 0;	// FIXME @@@ 

	if (cls.state != ca_active)
		return;

	view_frame = &cl.frames[cls.netchan.incoming_sequence & UPDATE_MASK];
	// an MVD's player followed, as last sent
	view_message = cls.mvdplayback ? &cl.mvd_prev[cl.viewplayer] : &view_frame->playerstate[cl.playernum];

	DropPunchAngle ();
	if (cl.intermission)
	{	// intermission / finale rendering
		V_CalcIntermissionRefdef ();	
	}
	else
	{
		V_CalcRefdef ();
	}

	r_scene.visedicts = cl.visedicts;
	r_scene.frametime = (float)cls.frametime;
	r_scene.drawviewmodel = Cam_DrawViewModel ()
		&& !(cl.stats[STAT_ITEMS] & IT_INVISIBILITY) && cl.stats[STAT_HEALTH] > 0;

	R_PushDlights ();
	R_RenderView ();
	V_SetContentsColor (r_scene.viewcontents);

	if (crosshair.value)
		V_DrawCrosshair ();
		
}

//============================================================================

/*
=============
V_Init
=============
*/
void V_Init (void)
{
	Cmd_AddCommand ("v_cshift", V_cshift_f);	
	Cmd_AddCommand ("bf", V_BonusFlash_f);
	Cmd_AddCommand ("centerview", V_StartPitchDrift);

	Cvar_RegisterVariable (&v_centermove);
	Cvar_RegisterVariable (&v_centerspeed);

	Cvar_RegisterVariable (&v_iyaw_cycle);
	Cvar_RegisterVariable (&v_iroll_cycle);
	Cvar_RegisterVariable (&v_ipitch_cycle);
	Cvar_RegisterVariable (&v_iyaw_level);
	Cvar_RegisterVariable (&v_iroll_level);
	Cvar_RegisterVariable (&v_ipitch_level);

	Cvar_RegisterVariable (&v_contentblend);

	Cvar_RegisterVariable (&v_idlescale);
	Cvar_RegisterVariable (&crosshaircolor);
	Cvar_RegisterVariable (&crosshair);
	Cvar_RegisterVariable (&cl_crossx);
	Cvar_RegisterVariable (&cl_crossy);

	PM_Init ();
	Cvar_RegisterVariable (&cl_bob);
	Cvar_RegisterVariable (&cl_bobcycle);
	Cvar_RegisterVariable (&cl_bobup);

	Cvar_RegisterVariable (&v_kicktime);
	Cvar_RegisterVariable (&v_kickroll);
	Cvar_RegisterVariable (&v_kickpitch);	

	Cvar_RegisterVariable (&v_gamma);
}


