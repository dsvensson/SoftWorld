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
// in_gamepad_mac.m -- gamepads through GameController
//
// As in_xinput.c on Windows: buttons and triggers become key events (K_JOY1-4,
// K_AUX1-12) that can be bound, and the sticks go to the client for movement
// and looking. The pad's state is read on the main thread each frame.

#import <GameController/GameController.h>

#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "mac_local.h"

#include <math.h>

static cvar_t	in_joystick = {.name = "joystick", .string = "1", .archive = true};

#define TRIGGER_THRESHOLD	(30.0f / 255.0f)	// XInput's 30 of 255
#define LEFT_DEADZONE		(7849.0f / 32767.0f)	// XInput's dead zones
#define RIGHT_DEADZONE		(8689.0f / 32767.0f)

#define PAD_BUTTONS			14

// the buttons in the order of pad_keys, as XInput's map to them
static const int	pad_keys[PAD_BUTTONS] =
{
	K_JOY1, K_JOY2, K_JOY3, K_JOY4,			// A, B, X, Y
	K_AUX1, K_AUX2,							// shoulders
	K_AUX5, K_AUX6,							// options (back), menu (start)
	K_AUX7, K_AUX8,							// the sticks' buttons
	K_AUX9, K_AUX10, K_AUX11, K_AUX12,		// d-pad up, down, left, right
};

static GCController	*pad;				// connected pad, nil for none
static uint32_t		pad_buttonstate;
static bool			pad_ltdown, pad_rtdown;

/*
===============
IN_Stick

Scales a stick with a radial dead zone, so small offsets are ignored in every
direction and full deflection still reaches 1
===============
*/
static void IN_Stick (float fx, float fy, float dz, float *outx, float *outy)
{
	float	len, scale;

	len = sqrtf (fx*fx + fy*fy);
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

static uint32_t IN_PadButtons (GCExtendedGamepad *g)
{
	GCControllerButtonInput	*buttons[PAD_BUTTONS] =
	{
		g.buttonA, g.buttonB, g.buttonX, g.buttonY,
		g.leftShoulder, g.rightShoulder,
		g.buttonOptions, g.buttonMenu,
		g.leftThumbstickButton, g.rightThumbstickButton,
		g.dpad.up, g.dpad.down, g.dpad.left, g.dpad.right,
	};
	uint32_t	state = 0;
	int			i;

	for (i = 0 ; i < PAD_BUTTONS ; i++)
		if (buttons[i] && buttons[i].pressed)
			state |= 1u << i;
	return state;
}

static void IN_ReleasePad (void)
{
	int		i;

	for (i = 0 ; i < PAD_BUTTONS ; i++)
		if (pad_buttonstate & (1u << i))
			Key_Event (pad_keys[i], false);
	if (pad_ltdown)
		Key_Event (K_AUX3, false);
	if (pad_rtdown)
		Key_Event (K_AUX4, false);
	pad_buttonstate = 0;
	pad_ltdown = pad_rtdown = false;
	IN_GamepadSticks (0, 0, 0, 0);
	pad = nil;
}

void IN_InitGamepad (void)
{
	Cvar_RegisterVariable (&in_joystick);
}

void IN_PollGamepad (void)
{
	GCExtendedGamepad	*g;
	uint32_t			state, changed;
	float				lx, ly, rx, ry;
	bool				down;
	int					i;

	if (!in_joystick.value)
	{
		if (pad)
			IN_ReleasePad ();
		return;
	}

	@autoreleasepool
	{
		// unplugged
		if (pad && (![GCController.controllers containsObject:pad] || !pad.extendedGamepad))
			IN_ReleasePad ();

		// the first connected pad, as Windows takes the first XInput one
		if (!pad)
			for (GCController *controller in GCController.controllers)
				if (controller.extendedGamepad)
				{
					pad = controller;
					break;
				}
		if (!pad)
			return;
		g = pad.extendedGamepad;

		state = IN_PadButtons (g);
		changed = state ^ pad_buttonstate;
		for (i = 0 ; i < PAD_BUTTONS ; i++)
			if (changed & (1u << i))
				Key_Event (pad_keys[i], (state & (1u << i)) != 0);
		pad_buttonstate = state;

		down = g.leftTrigger.value > TRIGGER_THRESHOLD;
		if (down != pad_ltdown)
			Key_Event (K_AUX3, down);
		pad_ltdown = down;
		down = g.rightTrigger.value > TRIGGER_THRESHOLD;
		if (down != pad_rtdown)
			Key_Event (K_AUX4, down);
		pad_rtdown = down;

		// GameController's +y is up, as XInput's
		IN_Stick (g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value, LEFT_DEADZONE, &lx, &ly);
		IN_Stick (g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value, RIGHT_DEADZONE, &rx, &ry);
		IN_GamepadSticks (lx, ly, rx, ry);
	}
}
