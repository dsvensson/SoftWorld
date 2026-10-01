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
// cl.input.c  -- builds an intended movement command to send to the server

#include "cl_local.h"

static cvar_t	cl_nodelta = {.name = "cl_nodelta", .string = "0",
	.description = "Asks for whole entity updates, not changes against the last one received; costs bandwidth.",
	.values = (const cvar_value_t[]){{"0", "Changes against the last update received"},
		{"1", "Whole updates every packet"}, {0}}};

static cvar_t	cl_smartjump = {.name = "cl_smartjump", .string = "1", .archive = true,
	.description = "+jump moves up, as +moveup does, while swimming, flying, or flying a spectator's or "
		"an MVD's camera (ezQuake).",
	.values = (const cvar_value_t[]){{"0", "+jump always jumps"},
		{"1", "+jump moves up where there's nothing to jump from"}, {0}}};

/*
===============================================================================

KEY BUTTONS

Continuous button event tracking is complicated by the fact that two different
input sources (say, mouse button 1 and the control key) can both press the
same button, but the button should only be released when both of the
pressing key have been released.

When a key event issues a button command (+forward, +attack, etc), it appends
its key number as a parameter to the command so it can be matched up with
the release.

state bit 0 is the current state of the key
state bit 1 is edge triggered on the up to down transition
state bit 2 is edge triggered on the down to up transition

===============================================================================
*/


static kbutton_t	in_mlook, in_klook;

// the mouse looks around without holding +mlook
cvar_t	freelook = {.name = "freelook", .string = "1", .archive = true,
	.description = "The mouse looks up and down without +mlook held.",
	.values = (const cvar_value_t[]){{"0", "Moving the mouse up and down moves, unless +mlook is held"},
		{"1", "Moving the mouse up and down looks"}, {0}}};

static bool IN_MouseLook (void)
{
	return (in_mlook.state & 1) || freelook.value;
}
static kbutton_t	in_left, in_right, in_forward, in_back;
static kbutton_t	in_lookup, in_lookdown, in_moveleft, in_moveright;
static kbutton_t	in_strafe, in_speed, in_use, in_jump, in_attack;
static kbutton_t	in_up, in_down;

static int			in_impulse;


static void KeyDown (kbutton_t *b)
{
	int		k;
	char	*c;
	
	c = Cmd_Argv(1);
	if (c[0])
		k = atoi(c);
	else
		k = -1;		// typed manually at the console for continuous down

	if (k == b->down[0] || k == b->down[1])
		return;		// repeating key
	
	if (!b->down[0])
		b->down[0] = k;
	else if (!b->down[1])
		b->down[1] = k;
	else
	{
		Con_Printf ("Three keys down for a button!\n");
		return;
	}
	
	if (b->state & 1)
		return;		// still down
	b->state |= 1 + 2;	// down + impulse down
}

static void KeyUp (kbutton_t *b)
{
	int		k;
	char	*c;
	
	c = Cmd_Argv(1);
	if (c[0])
		k = atoi(c);
	else
	{ // typed manually at the console, assume for unsticking, so clear all
		b->down[0] = b->down[1] = 0;
		b->state = 4;	// impulse up
		return;
	}

	if (b->down[0] == k)
		b->down[0] = 0;
	else if (b->down[1] == k)
		b->down[1] = 0;
	else
		return;		// key up without coresponding down (menu pass through)
	if (b->down[0] || b->down[1])
		return;		// some other key is still holding it down

	if (!(b->state & 1))
		return;		// still up (this should not happen)
	b->state &= ~1;		// now up
	b->state |= 4; 		// impulse up
}

static void IN_KLookDown (void) {KeyDown(&in_klook);}
static void IN_KLookUp (void) {KeyUp(&in_klook);}
static void IN_MLookDown (void) {KeyDown(&in_mlook);}
static void IN_MLookUp (void) {
KeyUp(&in_mlook);
if ( !IN_MouseLook () && lookspring.value)
	V_StartPitchDrift();
}
static void IN_UpDown(void) {KeyDown(&in_up);}
static void IN_UpUp(void) {KeyUp(&in_up);}
static void IN_DownDown(void) {KeyDown(&in_down);}
static void IN_DownUp(void) {KeyUp(&in_down);}
static void IN_LeftDown(void) {KeyDown(&in_left);}
static void IN_LeftUp(void) {KeyUp(&in_left);}
static void IN_RightDown(void) {KeyDown(&in_right);}
static void IN_RightUp(void) {KeyUp(&in_right);}
static void IN_ForwardDown(void) {KeyDown(&in_forward);}
static void IN_ForwardUp(void) {KeyUp(&in_forward);}
static void IN_BackDown(void) {KeyDown(&in_back);}
static void IN_BackUp(void) {KeyUp(&in_back);}
static void IN_LookupDown(void) {KeyDown(&in_lookup);}
static void IN_LookupUp(void) {KeyUp(&in_lookup);}
static void IN_LookdownDown(void) {KeyDown(&in_lookdown);}
static void IN_LookdownUp(void) {KeyUp(&in_lookdown);}
static void IN_MoveleftDown(void) {KeyDown(&in_moveleft);}
static void IN_MoveleftUp(void) {KeyUp(&in_moveleft);}
static void IN_MoverightDown(void) {KeyDown(&in_moveright);}
static void IN_MoverightUp(void) {KeyUp(&in_moveright);}

static void IN_SpeedDown(void) {KeyDown(&in_speed);}
static void IN_SpeedUp(void) {KeyUp(&in_speed);}
static void IN_StrafeDown(void) {KeyDown(&in_strafe);}
static void IN_StrafeUp(void) {KeyUp(&in_strafe);}

static void IN_AttackDown(void) {KeyDown(&in_attack);}
static void IN_AttackUp(void) {KeyUp(&in_attack);}

static void IN_UseDown (void) {KeyDown(&in_use);}
static void IN_UseUp (void) {KeyUp(&in_use);}
/*
===============
IN_JumpDown

With cl_smartjump, as ezQuake: +jump goes up where there's nothing to jump
from, swimming, flying, or a spectator's or an MVD's free camera
===============
*/
static void IN_JumpDown (void)
{
	bool	up;
	int		pmt;

	if (cls.state != ca_active || !cl_smartjump.value)
		up = false;
	else if (cls.mvdplayback)
		up = CL_MVDFlying ();	// following, jump goes to the next player
	else if (cls.demoplayback)
		up = false;
	else if (cl.spectator)
		up = Cam_TrackNum () == -1;
	else if (cl.stats[STAT_HEALTH] <= 0)
		up = false;				// jump respawns
	else if (cl.validsequence && ((pmt = cl.frames[cl.validsequence & UPDATE_MASK].playerstate[cl.playernum].pm_type)
		== PM_FLY || pmt == PM_SPECTATOR || pmt == PM_OLD_SPECTATOR))
		up = true;
	else
		up = cl.waterlevel >= 2;

	KeyDown (up ? &in_up : &in_jump);
}

static void IN_JumpUp (void)
{
	// whichever the key went down as; the other ignores a key it doesn't hold
	KeyUp (&in_up);
	KeyUp (&in_jump);
}

static void IN_Impulse (void) {in_impulse=Q_atoi(Cmd_Argv(1));}

/*
===============
CL_KeyState

Returns 0.25 if a key was pressed and released during the frame,
0.5 if it was pressed and held
0 if held then released, and
1.0 if held for the entire time
===============
*/
static float CL_KeyState (kbutton_t *key)
{
	float		val;
	bool	impulsedown, impulseup, down;
	
	impulsedown = key->state & 2;
	impulseup = key->state & 4;
	down = key->state & 1;
	val = 0;
	
	if (impulsedown && !impulseup)
	{
		if (down)
			val = 0.5;	// pressed and held this frame
		else
			val = 0;	//	I_Error ();
	}
	if (impulseup && !impulsedown)
	{
		if (down)
			val = 0;	//	I_Error ();
		else
			val = 0;	// released this frame
	}
	if (!impulsedown && !impulseup)
	{
		if (down)
			val = 1.0;	// held the entire frame
		else
			val = 0;	// up the entire frame
	}
	if (impulsedown && impulseup)
	{
		if (down)
			val = 0.75;	// released and re-pressed this frame
		else
			val = 0.25;	// pressed and released this frame
	}

	key->state &= 1;		// clear impulses
	
	return val;
}




//==========================================================================

// moving runs by default, as ezQuake's; the menu's Always Run sets 200 to walk
cvar_t	cl_upspeed = {.name = "cl_upspeed", .string = "400",
	.description = "Speed of +moveup and +movedown, swimming or flying, in units a second."};
cvar_t	cl_forwardspeed = {.name = "cl_forwardspeed", .string = "400", .archive = true,
	.description = "Speed of +forward and the left stick, in units a second; the server caps it at sv_maxspeed. "
		"200 walks unless +speed is held."};
cvar_t	cl_backspeed = {.name = "cl_backspeed", .string = "400", .archive = true,
	.description = "Speed of +back, in units a second; the server caps it at sv_maxspeed. "
		"200 walks unless +speed is held."};
cvar_t	cl_sidespeed = {.name = "cl_sidespeed", .string = "350",
	.description = "Speed of strafing, by key or the left stick, in units a second; the server caps it at sv_maxspeed."};

cvar_t	cl_movespeedkey = {.name = "cl_movespeedkey", .string = "2.0",
	.description = "How many times faster moving is while +speed is held."};

cvar_t	cl_yawspeed = {.name = "cl_yawspeed", .string = "140",
	.description = "How fast +left and +right turn, in degrees a second."};
cvar_t	cl_pitchspeed = {.name = "cl_pitchspeed", .string = "150",
	.description = "How fast +lookup, +lookdown and +klook look up and down, in degrees a second."};

cvar_t	cl_anglespeedkey = {.name = "cl_anglespeedkey", .string = "1.5",
	.description = "How many times faster turning and looking by keys is while +speed is held."};


/*
================
CL_AdjustAngles

Moves the local angle positions
================
*/
static void CL_AdjustAngles (void)
{
	float	speed;
	float	up, down;
	
	if (in_speed.state & 1)
		speed = (float)(cls.physframetime * cl_anglespeedkey.value);
	else
		speed = (float)cls.physframetime;

	if (!(in_strafe.state & 1))
	{
		cl.viewangles[YAW] -= speed*cl_yawspeed.value*CL_KeyState (&in_right);
		cl.viewangles[YAW] += speed*cl_yawspeed.value*CL_KeyState (&in_left);
		cl.viewangles[YAW] = anglemod(cl.viewangles[YAW]);
	}
	if (in_klook.state & 1)
	{
		V_StopPitchDrift ();
		cl.viewangles[PITCH] -= speed*cl_pitchspeed.value * CL_KeyState (&in_forward);
		cl.viewangles[PITCH] += speed*cl_pitchspeed.value * CL_KeyState (&in_back);
	}
	
	up = CL_KeyState (&in_lookup);
	down = CL_KeyState(&in_lookdown);
	
	cl.viewangles[PITCH] -= speed*cl_pitchspeed.value * up;
	cl.viewangles[PITCH] += speed*cl_pitchspeed.value * down;

	if (up || down)
		V_StopPitchDrift ();
		
	if (cl.viewangles[PITCH] > cl.maxpitch)
		cl.viewangles[PITCH] = cl.maxpitch;
	if (cl.viewangles[PITCH] < cl.minpitch)
		cl.viewangles[PITCH] = cl.minpitch;

	if (cl.viewangles[ROLL] > 50)
		cl.viewangles[ROLL] = 50;
	if (cl.viewangles[ROLL] < -50)
		cl.viewangles[ROLL] = -50;
		
}

/*
================
CL_FlyMove

The movement keys as speeds forward, right and up, for a camera flown
through a demo: a player's, faster with the speed key
================
*/
void CL_FlyMove (float move[3])
{
	float	speed = (in_speed.state & 1) ? cl_movespeedkey.value : 1;

	move[0] = (cl_forwardspeed.value * CL_KeyState (&in_forward) - cl_backspeed.value * CL_KeyState (&in_back)) * speed;
	move[1] = cl_sidespeed.value * (CL_KeyState (&in_moveright) - CL_KeyState (&in_moveleft)) * speed;
	move[2] = cl_upspeed.value * (CL_KeyState (&in_up) - CL_KeyState (&in_down)) * speed;
}

/*
================
CL_BaseMove

Send the intended movement message to the server
================
*/
static void CL_BaseMove (usercmd_t *cmd)
{	
	CL_AdjustAngles ();
	
	memset (cmd, 0, sizeof(*cmd));
	
	VectorCopy (cl.viewangles, cmd->angles);
	if (in_strafe.state & 1)
	{
		cmd->sidemove = (short)(cmd->sidemove + cl_sidespeed.value * CL_KeyState (&in_right));
		cmd->sidemove = (short)(cmd->sidemove - cl_sidespeed.value * CL_KeyState (&in_left));
	}

	cmd->sidemove = (short)(cmd->sidemove + cl_sidespeed.value * CL_KeyState (&in_moveright));
	cmd->sidemove = (short)(cmd->sidemove - cl_sidespeed.value * CL_KeyState (&in_moveleft));

	cmd->upmove = (short)(cmd->upmove + cl_upspeed.value * CL_KeyState (&in_up));
	cmd->upmove = (short)(cmd->upmove - cl_upspeed.value * CL_KeyState (&in_down));

	if (! (in_klook.state & 1) )
	{
		cmd->forwardmove = (short)(cmd->forwardmove + cl_forwardspeed.value * CL_KeyState (&in_forward));
		cmd->forwardmove = (short)(cmd->forwardmove - cl_backspeed.value * CL_KeyState (&in_back));
	}

//
// adjust for speed key
//
	if (in_speed.state & 1)
	{
		cmd->forwardmove = (short)(cmd->forwardmove * cl_movespeedkey.value);
		cmd->sidemove = (short)(cmd->sidemove * cl_movespeedkey.value);
		cmd->upmove = (short)(cmd->upmove * cl_movespeedkey.value);
	}
}

static int MakeChar (int i)
{
	i &= ~3;
	if (i < -127*4)
		i = -127*4;
	if (i > 127*4)
		i = 127*4;
	return i;
}

/*
==============
CL_FinishMove
==============
*/
static void CL_FinishMove (usercmd_t *cmd)
{
	static double	extramsec;
	int		i;
	int		ms;

//
// allways dump the first two message, because it may contain leftover inputs
// from the last level
//
	if (++cl.movemessages <= 2)
		return;
//
// figure button bits
//	
	if ( in_attack.state & 3 )
		cmd->buttons |= 1;
	in_attack.state &= ~2;
	
	if (in_jump.state & 3)
		cmd->buttons |= 2;
	in_jump.state &= ~2;

	// send milliseconds of time to apply the move, keeping the fractions
	extramsec += cls.physframetime * 1000;
	ms = (int)extramsec;
	extramsec -= ms;
	if (ms > 250)
		ms = 100;		// time was unreasonable
	cmd->msec = (byte)ms;
	cl.cmdtime_msec += ms;

	VectorCopy (cl.viewangles, cmd->angles);

	cmd->impulse = (byte)in_impulse;
	in_impulse = 0;


//
// chop down so no extra bits are kept that the server wouldn't get
//
	cmd->forwardmove = (short)MakeChar (cmd->forwardmove);
	cmd->sidemove = (short)MakeChar (cmd->sidemove);
	cmd->upmove = (short)MakeChar (cmd->upmove);

	for (i=0 ; i<3 ; i++)
		cmd->angles[i] = (float)(((int)(cmd->angles[i]*65536.0/360)&65535) * (360.0/65536.0));
}

/*
=================
CL_SendCmd
=================
*/
void CL_SendCmd (void)
{
	sizebuf_t	buf;
	byte		data[MAX_MSGLEN];
	int			i;
	usercmd_t	*cmd, *oldcmd;
	int			checksumIndex;
	int			lost;
	int			seq_hash;
	int			want, sent;
	frame_t		*f;

	if (cls.demoplayback)
	{	// sendcmds come from the demo; an MVD's buttons pick who to watch
		if (cls.mvdplayback)
			CL_MVDButtons ((in_attack.state & 2) != 0, (in_jump.state & 2) != 0);
		in_attack.state &= ~2;
		in_jump.state &= ~2;
		return;
	}

	// save this command off for prediction
	i = cls.netchan.outgoing_sequence & UPDATE_MASK;
	cmd = &cl.frames[i].cmd;
	cl.frames[i].senttime = host.realtime;
	cl.frames[i].receivedtime = -1;		// we haven't gotten a reply yet

//	seq_hash = (cls.netchan.outgoing_sequence & 0xffff) ; // ^ QW_CHECK_HASH;
	seq_hash = cls.netchan.outgoing_sequence;

	// get basic movement from keyboard
	CL_BaseMove (cmd);

	// allow mice or other external controllers to add to the move
	IN_Move (cmd);

	// if we are spectator, try autocam
	if (cl.spectator)
		Cam_Track(cmd);

	CL_FinishMove(cmd);

	Cam_FinishMove(cmd);

// send this and the previous cmds in the message, so
// if the last packet was dropped, it can be recovered
	buf.maxsize = sizeof(data);
	buf.cursize = 0;
	buf.data = data;

	MSG_WriteByte (&buf, clc_move);

	// save the position for a checksum byte
	checksumIndex = buf.cursize;
	MSG_WriteByte (&buf, 0);

	// write our lossage percentage
	lost = CL_CalcNet();
	MSG_WriteByte (&buf, (byte)lost);

	i = (cls.netchan.outgoing_sequence-2) & UPDATE_MASK;
	cmd = &cl.frames[i].cmd;
	MSG_WriteDeltaUsercmd (&buf, &nullcmd, cmd);
	oldcmd = cmd;

	i = (cls.netchan.outgoing_sequence-1) & UPDATE_MASK;
	cmd = &cl.frames[i].cmd;
	MSG_WriteDeltaUsercmd (&buf, oldcmd, cmd);
	oldcmd = cmd;

	i = (cls.netchan.outgoing_sequence) & UPDATE_MASK;
	cmd = &cl.frames[i].cmd;
	MSG_WriteDeltaUsercmd (&buf, oldcmd, cmd);

	// calculate a checksum over the move commands
	buf.data[checksumIndex] = COM_BlockSequenceCRCByte(
		buf.data + checksumIndex + 1, buf.cursize - checksumIndex - 1,
		seq_hash);

	// request delta compression of entities
	if (cls.netchan.outgoing_sequence - cl.validsequence >= UPDATE_BACKUP-1)
		cl.validsequence = 0;

	if (cl.validsequence && !cl_nodelta.value && cls.state == ca_active &&
		!cls.demorecording)
	{
		cl.frames[cls.netchan.outgoing_sequence&UPDATE_MASK].delta_sequence = cl.validsequence;
		MSG_WriteByte (&buf, clc_delta);
		MSG_WriteByte (&buf, cl.validsequence&255);
	}
	else
		cl.frames[cls.netchan.outgoing_sequence&UPDATE_MASK].delta_sequence = -1;

	if (cls.demorecording)
		CL_WriteDemoCmd(cmd);

	// a chunked download's requests ride along
	want = CL_DownloadRequests ();
	want -= CL_WriteDownloadRequests (&buf, want);

//
// deliver the message
//
	Netchan_Transmit (&cls.netchan, buf.cursize, buf.data);

	// before the client is in the game nothing needs the packet rate: the
	// requests one packet can't hold go in packets of their own, which repeat
	// the command for the frames that follow
	while (want > 0 && cls.state < ca_active)
	{
		buf.cursize = 0;
		sent = CL_WriteDownloadRequests (&buf, want);
		if (!sent)
			break;
		want -= sent;

		f = &cl.frames[cls.netchan.outgoing_sequence & UPDATE_MASK];
		f->cmd = *cmd;
		f->senttime = host.realtime;
		f->receivedtime = -1;
		f->delta_sequence = -1;
		Netchan_Transmit (&cls.netchan, buf.cursize, buf.data);
	}
}



/*
============
CL_InitInput
============
*/
/*
===============================================================================

MOUSE AND GAMEPAD

===============================================================================
*/

static cvar_t	m_filter = {.name = "m_filter", .string = "0",
	.description = "Averages the mouse's motion with the previous command's: smoother, a little behind.",
	.values = (const cvar_value_t[]){{"0", "The motion as it is"}, {"1", "Averaged over two commands"}, {0}}};
static cvar_t	joy_yawspeed = {.name = "joy_yawspeed", .string = "220", .archive = true,	// degrees per second
	.description = "How fast the gamepad's right stick turns, pushed fully, in degrees a second."};
static cvar_t	joy_pitchspeed = {.name = "joy_pitchspeed", .string = "160", .archive = true,
	.description = "How fast the gamepad's right stick looks up and down, pushed fully, in degrees a second."};
static cvar_t	joy_invert = {.name = "joy_invert", .string = "0", .archive = true,
	.description = "Inverts the gamepad's right stick for looking up and down.",
	.values = (const cvar_value_t[]){{"0", "Pushing up looks up"}, {"1", "Pushing up looks down"}, {0}}};

static int		in_mouse_dx, in_mouse_dy;		// motion since the last move
static int		in_old_mouse_x, in_old_mouse_y;	// for m_filter
static float	in_stick[4];					// left x, left y, right x, right y

void IN_MouseMotion (int dx, int dy)
{
	in_mouse_dx += dx;
	in_mouse_dy += dy;
}

void IN_GamepadSticks (float lx, float ly, float rx, float ry)
{
	in_stick[0] = lx;
	in_stick[1] = ly;
	in_stick[2] = rx;
	in_stick[3] = ry;
}

bool IN_WantsMouse (void)
{
	return cls.key_dest == key_game;
}

bool IN_WantsMouseButtons (void)
{
	return cls.key_dest == key_menu;
}

void IN_ClearStates (void)
{
	in_mouse_dx = in_mouse_dy = 0;
	in_old_mouse_x = in_old_mouse_y = 0;
	in_stick[0] = in_stick[1] = in_stick[2] = in_stick[3] = 0;
}

static void Force_CenterView_f (void)
{
	cl.viewangles[PITCH] = 0;
}

static void IN_ClampPitch (void)
{
	if (cl.viewangles[PITCH] > cl.maxpitch)
		cl.viewangles[PITCH] = cl.maxpitch;
	if (cl.viewangles[PITCH] < cl.minpitch)
		cl.viewangles[PITCH] = cl.minpitch;
}

static void IN_MouseMove (usercmd_t *cmd)
{
	int		mx, my;
	float	mouse_x, mouse_y;

	mx = in_mouse_dx;
	my = in_mouse_dy;
	in_mouse_dx = in_mouse_dy = 0;

	if (m_filter.value)
	{
		mouse_x = (mx + in_old_mouse_x) * 0.5f;
		mouse_y = (my + in_old_mouse_y) * 0.5f;
	}
	else
	{
		mouse_x = (float)mx;
		mouse_y = (float)my;
	}
	in_old_mouse_x = mx;
	in_old_mouse_y = my;

	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;

// add mouse X/Y movement to cmd
	if ( (in_strafe.state & 1) || (lookstrafe.value && IN_MouseLook ()))
		cmd->sidemove = (short)(cmd->sidemove + m_side.value * mouse_x);
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;

	if (IN_MouseLook ())
		V_StopPitchDrift ();

	if (IN_MouseLook () && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		IN_ClampPitch ();
	}
	else
		cmd->forwardmove = (short)(cmd->forwardmove - m_forward.value * mouse_y);
}

static void IN_GamepadMove (usercmd_t *cmd)
{
	float	speed, frametime;

	if (!in_stick[0] && !in_stick[1] && !in_stick[2] && !in_stick[3])
		return;

	speed = (in_speed.state & 1) ? cl_movespeedkey.value : 1;
	cmd->forwardmove = (short)(cmd->forwardmove + in_stick[1] * cl_forwardspeed.value * speed);
	cmd->sidemove = (short)(cmd->sidemove + in_stick[0] * cl_sidespeed.value * speed);

	frametime = (float)cls.frametime;
	cl.viewangles[YAW] -= in_stick[2] * joy_yawspeed.value * frametime;
	if (in_stick[3])
	{
		cl.viewangles[PITCH] += (joy_invert.value ? 1 : -1) * in_stick[3] * joy_pitchspeed.value * frametime;
		V_StopPitchDrift ();
		IN_ClampPitch ();
	}
}

void IN_Move (usercmd_t *cmd)
{
	IN_MouseMove (cmd);
	IN_GamepadMove (cmd);
}

void CL_InitInput (void)
{
	Cmd_AddCommand ("+moveup",IN_UpDown, "Swims or flies up while held, at cl_upspeed.");
	Cmd_AddCommand ("-moveup",IN_UpUp, "Releases +moveup.");
	Cmd_AddCommand ("+movedown",IN_DownDown, "Swims or flies down while held, at cl_upspeed.");
	Cmd_AddCommand ("-movedown",IN_DownUp, "Releases +movedown.");
	Cmd_AddCommand ("+left",IN_LeftDown, "Turns left while held, at cl_yawspeed; with +strafe held, strafes left.");
	Cmd_AddCommand ("-left",IN_LeftUp, "Releases +left.");
	Cmd_AddCommand ("+right",IN_RightDown, "Turns right while held, at cl_yawspeed; with +strafe held, strafes right.");
	Cmd_AddCommand ("-right",IN_RightUp, "Releases +right.");
	Cmd_AddCommand ("+forward",IN_ForwardDown, "Moves forward while held; with +klook held, looks up.");
	Cmd_AddCommand ("-forward",IN_ForwardUp, "Releases +forward.");
	Cmd_AddCommand ("+back",IN_BackDown, "Moves back while held; with +klook held, looks down.");
	Cmd_AddCommand ("-back",IN_BackUp, "Releases +back.");
	Cmd_AddCommand ("+lookup", IN_LookupDown, "Looks up while held, at cl_pitchspeed.");
	Cmd_AddCommand ("-lookup", IN_LookupUp, "Releases +lookup.");
	Cmd_AddCommand ("+lookdown", IN_LookdownDown, "Looks down while held, at cl_pitchspeed.");
	Cmd_AddCommand ("-lookdown", IN_LookdownUp, "Releases +lookdown.");
	Cmd_AddCommand ("+strafe", IN_StrafeDown, "Makes the turn keys and the mouse move instead of turn while held.");
	Cmd_AddCommand ("-strafe", IN_StrafeUp, "Releases +strafe.");
	Cmd_AddCommand ("+moveleft", IN_MoveleftDown, "Strafes left while held.");
	Cmd_AddCommand ("-moveleft", IN_MoveleftUp, "Releases +moveleft.");
	Cmd_AddCommand ("+moveright", IN_MoverightDown, "Strafes right while held.");
	Cmd_AddCommand ("-moveright", IN_MoverightUp, "Releases +moveright.");
	Cmd_AddCommand ("+speed", IN_SpeedDown,
		"Runs while held: moves cl_movespeedkey and turns cl_anglespeedkey times as fast.");
	Cmd_AddCommand ("-speed", IN_SpeedUp, "Releases +speed.");
	Cmd_AddCommand ("+attack", IN_AttackDown,
		"Fires while held; as a spectator it toggles following a player, and in an MVD flying the camera.");
	Cmd_AddCommand ("-attack", IN_AttackUp, "Releases +attack.");
	Cmd_AddCommand ("+use", IN_UseDown, "Does nothing: QuakeWorld sends no use button; kept for configs that bind it.");
	Cmd_AddCommand ("-use", IN_UseUp, "Releases +use.");
	Cmd_AddCommand ("+jump", IN_JumpDown,
		"Jumps or swims up while held; as a spectator following a player, and in an MVD, goes to the next player. "
		"With cl_smartjump it moves up instead where there's nothing to jump from.");
	Cmd_AddCommand ("-jump", IN_JumpUp, "Releases +jump.");
	Cmd_AddCommand ("impulse", IN_Impulse,
		"Sends an impulse to the game with the next command, such as a weapon to switch to. Usage: impulse <number>");
	Cmd_AddCommand ("+klook", IN_KLookDown, "Makes +forward and +back look up and down while held.");
	Cmd_AddCommand ("-klook", IN_KLookUp, "Releases +klook.");
	Cmd_AddCommand ("+mlook", IN_MLookDown,
		"Makes the mouse look up and down while held, as freelook always does; on release lookspring centers the view.");
	Cmd_AddCommand ("-mlook", IN_MLookUp, "Releases +mlook.");

	Cvar_RegisterVariable (&cl_nodelta);
	Cvar_RegisterVariable (&cl_smartjump);
	Cvar_RegisterVariable (&m_filter);
	Cvar_RegisterVariable (&freelook);
	Cvar_RegisterVariable (&joy_yawspeed);
	Cvar_RegisterVariable (&joy_pitchspeed);
	Cvar_RegisterVariable (&joy_invert);
	Cmd_AddCommand ("force_centerview", Force_CenterView_f, "Levels the view, looking straight ahead.");
}

