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
// in_xinput.c -- gamepads through XInput
//
// Buttons and triggers become key events (K_JOY1-4, K_AUX1-12) that can be bound;
// the sticks go to the client for movement and looking.

#include "win_local.h"
#include <xinput.h>

#include "cvar.h"
#include "in_events.h"
#include "keys.h"

#include <math.h>

static cvar_t	in_joystick = {.name = "joystick", .string = "1", .archive = true};

#define TRIGGER_THRESHOLD	30			// of 255
#define PROBE_INTERVAL		2000		// ms between looking for a pad when none is connected

static const struct { WORD mask; int key; } xi_buttons[] =
{
	{XINPUT_GAMEPAD_A,				K_JOY1},
	{XINPUT_GAMEPAD_B,				K_JOY2},
	{XINPUT_GAMEPAD_X,				K_JOY3},
	{XINPUT_GAMEPAD_Y,				K_JOY4},
	{XINPUT_GAMEPAD_LEFT_SHOULDER,	K_AUX1},
	{XINPUT_GAMEPAD_RIGHT_SHOULDER,	K_AUX2},
	{XINPUT_GAMEPAD_BACK,			K_AUX5},
	{XINPUT_GAMEPAD_START,			K_AUX6},
	{XINPUT_GAMEPAD_LEFT_THUMB,		K_AUX7},
	{XINPUT_GAMEPAD_RIGHT_THUMB,	K_AUX8},
	{XINPUT_GAMEPAD_DPAD_UP,		K_AUX9},
	{XINPUT_GAMEPAD_DPAD_DOWN,		K_AUX10},
	{XINPUT_GAMEPAD_DPAD_LEFT,		K_AUX11},
	{XINPUT_GAMEPAD_DPAD_RIGHT,		K_AUX12},
};

static int		xi_pad = -1;		// connected pad, -1 for none
static DWORD	xi_nextprobe;
static WORD		xi_buttonstate;
static bool		xi_ltdown, xi_rtdown;

/*
===============
IN_Stick

Scales a stick with a radial dead zone, so small offsets are ignored in every
direction and full deflection still reaches 1
===============
*/
static void IN_Stick (SHORT x, SHORT y, SHORT deadzone, float *outx, float *outy)
{
	float	fx, fy, len, dz, scale;

	fx = x / 32767.0f;
	fy = y / 32767.0f;
	len = sqrtf (fx*fx + fy*fy);
	dz = deadzone / 32767.0f;
	if (len <= dz)
	{
		*outx = *outy = 0;
		return;
	}
	scale = (len > 1 ? 1 : len);
	scale = (scale - dz) / (1 - dz) / len;
	*outx = fx * scale;
	*outy = fy * scale;
}

static void IN_ReleasePad (void)
{
	int		i;

	for (i=0 ; i<(int)(sizeof(xi_buttons)/sizeof(xi_buttons[0])) ; i++)
		if (xi_buttonstate & xi_buttons[i].mask)
			Key_Event (xi_buttons[i].key, false);
	if (xi_ltdown)
		Key_Event (K_AUX3, false);
	if (xi_rtdown)
		Key_Event (K_AUX4, false);
	xi_buttonstate = 0;
	xi_ltdown = xi_rtdown = false;
	IN_GamepadSticks (0, 0, 0, 0);
}

void IN_InitGamepad (void)
{
	Cvar_RegisterVariable (&in_joystick);
}

void IN_PollGamepad (void)
{
	XINPUT_STATE	state;
	WORD			changed;
	float			lx, ly, rx, ry;
	bool			down;
	int				i;
	DWORD			pad;

	if (!in_joystick.value)
	{
		if (xi_pad >= 0)
		{
			IN_ReleasePad ();
			xi_pad = -1;
		}
		return;
	}

	if (xi_pad < 0)
	{
		if (GetTickCount () < xi_nextprobe)
			return;
		xi_nextprobe = GetTickCount () + PROBE_INTERVAL;
		for (pad=0 ; pad<XUSER_MAX_COUNT ; pad++)
			if (XInputGetState (pad, &state) == ERROR_SUCCESS)
				break;
		if (pad == XUSER_MAX_COUNT)
			return;
		xi_pad = (int)pad;
	}

	if (XInputGetState ((DWORD)xi_pad, &state) != ERROR_SUCCESS)
	{	// unplugged
		IN_ReleasePad ();
		xi_pad = -1;
		return;
	}

	changed = state.Gamepad.wButtons ^ xi_buttonstate;
	for (i=0 ; i<(int)(sizeof(xi_buttons)/sizeof(xi_buttons[0])) ; i++)
		if (changed & xi_buttons[i].mask)
			Key_Event (xi_buttons[i].key, (state.Gamepad.wButtons & xi_buttons[i].mask) != 0);
	xi_buttonstate = state.Gamepad.wButtons;

	down = state.Gamepad.bLeftTrigger > TRIGGER_THRESHOLD;
	if (down != xi_ltdown)
		Key_Event (K_AUX3, down);
	xi_ltdown = down;
	down = state.Gamepad.bRightTrigger > TRIGGER_THRESHOLD;
	if (down != xi_rtdown)
		Key_Event (K_AUX4, down);
	xi_rtdown = down;

	IN_Stick (state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &lx, &ly);
	IN_Stick (state.Gamepad.sThumbRX, state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, &rx, &ry);
	IN_GamepadSticks (lx, ly, rx, ry);
}
