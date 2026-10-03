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
// in_gamepad_web.c -- gamepads through the browser's Gamepad API
//
// As in_xinput.c on Windows: buttons and triggers become key events (K_JOY1-4,
// K_AUX1-12) that can be bound, and the sticks go to the client for movement
// and looking. The first pad the browser lays out as an Xbox pad ("standard")
// is read each frame.

#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "web_local.h"

#include <emscripten/html5.h>

#include <math.h>
#include <string.h>

static cvar_t	in_joystick = {.name = "joystick", .string = "1", .archive = true,
	.description = "Reads the first gamepad: buttons and triggers as keys to bind, sticks for moving and looking.",
	.values = (const cvar_value_t[]){{"0", "Gamepads ignored"}, {"1", "The first gamepad read"}, {0}}};

#define TRIGGER_THRESHOLD	(30.0 / 255.0)	// XInput's 30 of 255
#define LEFT_DEADZONE		(7849.0f / 32767.0f)	// XInput's dead zones
#define RIGHT_DEADZONE		(8689.0f / 32767.0f)

#define PAD_BUTTONS			14

// the buttons in the order of pad_keys, as XInput's map to them, by their
// index in the standard layout
static const int	pad_keys[PAD_BUTTONS] =
{
	K_JOY1, K_JOY2, K_JOY3, K_JOY4,			// A, B, X, Y
	K_AUX1, K_AUX2,							// shoulders
	K_AUX5, K_AUX6,							// back, start
	K_AUX7, K_AUX8,							// the sticks' buttons
	K_AUX9, K_AUX10, K_AUX11, K_AUX12,		// d-pad up, down, left, right
};
static const int	pad_index[PAD_BUTTONS] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};

static int			pad = -1;			// the pad read, -1 for none
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
	pad = -1;
}

// a pad of the standard layout, connected
static bool IN_PadState (int index, EmscriptenGamepadEvent *state)
{
	return emscripten_get_gamepad_status (index, state) == EMSCRIPTEN_RESULT_SUCCESS && state->connected
		&& !strcmp (state->mapping, "standard") && state->numButtons > 15 && state->numAxes >= 4;
}

void IN_InitGamepad (void)
{
	Cvar_RegisterVariable (&in_joystick);
}

void IN_PollGamepad (void)
{
	EmscriptenGamepadEvent	g;
	uint32_t	state = 0, changed;
	float		lx, ly, rx, ry;
	bool		down;
	int			i, count;

	if (!in_joystick.value)
	{
		if (pad >= 0)
			IN_ReleasePad ();
		return;
	}
	if (emscripten_sample_gamepad_data () != EMSCRIPTEN_RESULT_SUCCESS)
		return;

	// unplugged
	if (pad >= 0 && !IN_PadState (pad, &g))
		IN_ReleasePad ();

	// the first connected pad, as Windows takes the first XInput one
	if (pad < 0)
	{
		count = emscripten_get_num_gamepads ();
		for (i = 0 ; i < count ; i++)
			if (IN_PadState (i, &g))
			{
				pad = i;
				break;
			}
		if (pad < 0)
			return;
	}

	for (i = 0 ; i < PAD_BUTTONS ; i++)
		if (g.digitalButton[pad_index[i]])
			state |= 1u << i;
	changed = state ^ pad_buttonstate;
	for (i = 0 ; i < PAD_BUTTONS ; i++)
		if (changed & (1u << i))
			Key_Event (pad_keys[i], (state & (1u << i)) != 0);
	pad_buttonstate = state;

	down = g.analogButton[6] > TRIGGER_THRESHOLD;
	if (down != pad_ltdown)
		Key_Event (K_AUX3, down);
	pad_ltdown = down;
	down = g.analogButton[7] > TRIGGER_THRESHOLD;
	if (down != pad_rtdown)
		Key_Event (K_AUX4, down);
	pad_rtdown = down;

	// the browser's +y is down, XInput's up
	IN_Stick ((float)g.axis[0], (float)-g.axis[1], LEFT_DEADZONE, &lx, &ly);
	IN_Stick ((float)g.axis[2], (float)-g.axis[3], RIGHT_DEADZONE, &rx, &ry);
	IN_GamepadSticks (lx, ly, rx, ry);
}
