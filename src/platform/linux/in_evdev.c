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
// in_evdev.c -- gamepads through the kernel's evdev devices
//
// As in_xinput.c on Windows: buttons and triggers become key events (K_JOY1-4,
// K_AUX1-12) that can be bound, and the sticks go to the client for movement
// and looking. The first device with a gamepad's buttons and a stick is the pad,
// read each frame; the buttons are the kernel's gamepad codes, by where they are
// on the pad (BTN_SOUTH is Xbox's A). Pads come and go: /dev/input is watched,
// for new devices and for the permissions that let the player open them, which
// are set just after.

#include "cvar.h"
#include "in_events.h"
#include "keys.h"
#include "print.h"
#include "q_string.h"
#include "linux_local.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

static cvar_t	in_joystick = {.name = "joystick", .string = "1", .archive = true,
	.description = "Reads the first gamepad: buttons and triggers as keys to bind, sticks for moving and looking.",
	.values = (const cvar_value_t[]){{"0", "Gamepads ignored"}, {"1", "The first gamepad read"}, {0}}};

#define TRIGGER_THRESHOLD	(30.0f / 255.0f)	// XInput's 30 of 255
#define LEFT_DEADZONE		(7849.0f / 32767.0f)	// XInput's dead zones
#define RIGHT_DEADZONE		(8689.0f / 32767.0f)

#define PAD_BUTTONS			14

// the buttons in the order of pad_keys, as XInput's map to them
static const int	pad_codes[PAD_BUTTONS] =
{
	BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
	BTN_TL, BTN_TR,
	BTN_SELECT, BTN_START,
	BTN_THUMBL, BTN_THUMBR,
	BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};

static const int	pad_keys[PAD_BUTTONS] =
{
	K_JOY1, K_JOY2, K_JOY3, K_JOY4,			// A, B, X, Y
	K_AUX1, K_AUX2,							// shoulders
	K_AUX5, K_AUX6,							// back, start
	K_AUX7, K_AUX8,							// the sticks' buttons
	K_AUX9, K_AUX10, K_AUX11, K_AUX12,		// d-pad up, down, left, right
};

// the axes read, and what they are
enum { AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY, AXIS_LT, AXIS_RT, AXIS_HATX, AXIS_HATY, NUM_AXES };
static const int	pad_axiscodes[NUM_AXES] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};

static int			pad_fd = -1;			// the pad's device, -1 for none
static char			pad_name[128];
static struct input_absinfo	pad_info[NUM_AXES];	// each axis's range; max == min for none
static int			pad_axis[NUM_AXES];
static bool			pad_down[PAD_BUTTONS];	// as the device has them
static bool			pad_tl2, pad_tr2;		// digital triggers
static uint32_t		pad_buttonstate;		// sent
static bool			pad_ltdown, pad_rtdown;
static int			pad_watch = -1;			// inotify on /dev/input
static bool			pad_rescan = true;

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

// an axis from -1 to 1 (0 to 1 for a trigger that starts at its minimum)
static float IN_Axis (int axis)
{
	const struct input_absinfo	*info = &pad_info[axis];
	float	range = (float)(info->maximum - info->minimum);

	if (range <= 0)
		return 0;
	if (axis == AXIS_LT || axis == AXIS_RT)
		return (pad_axis[axis] - info->minimum) / range;
	return (pad_axis[axis] - info->minimum) / range * 2 - 1;
}

static bool IN_TestBit (const unsigned long *bits, int bit)
{
	return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
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
	if (pad_fd >= 0)
		close (pad_fd);
	pad_fd = -1;
}

// a device with a gamepad's buttons and a stick
static bool IN_OpenPad (const char *path)
{
	unsigned long	keys[KEY_CNT / (8 * sizeof(long)) + 1] = {0};
	unsigned long	axes[ABS_CNT / (8 * sizeof(long)) + 1] = {0};
	int				fd, i;

	fd = open (path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (ioctl (fd, EVIOCGBIT (EV_KEY, sizeof(keys)), keys) < 0 || ioctl (fd, EVIOCGBIT (EV_ABS, sizeof(axes)), axes) < 0
		|| !IN_TestBit (keys, BTN_SOUTH) || !IN_TestBit (axes, ABS_X))
	{
		close (fd);
		return false;
	}

	pad_fd = fd;
	if (ioctl (fd, EVIOCGNAME (sizeof(pad_name)), pad_name) < 0)
		Q_snprintfz (pad_name, sizeof(pad_name), "%s", path);
	for (i = 0 ; i < NUM_AXES ; i++)
	{
		memset (&pad_info[i], 0, sizeof(pad_info[i]));
		if (IN_TestBit (axes, pad_axiscodes[i]))
			ioctl (fd, EVIOCGABS (pad_axiscodes[i]), &pad_info[i]);
		pad_axis[i] = pad_info[i].value;
	}
	// the sticks start centered, where the device doesn't say
	memset (pad_down, 0, sizeof(pad_down));
	pad_tl2 = pad_tr2 = false;
	Con_Printf ("Gamepad: %s\n", pad_name);
	return true;
}

// the first pad among the event devices
static void IN_FindPad (void)
{
	DIR				*dir;
	struct dirent	*entry;
	char			path[300];

	pad_rescan = false;
	dir = opendir ("/dev/input");
	if (!dir)
		return;
	while (pad_fd < 0 && (entry = readdir (dir)))
	{
		if (strncmp (entry->d_name, "event", 5))
			continue;
		snprintf (path, sizeof(path), "/dev/input/%s", entry->d_name);
		IN_OpenPad (path);
	}
	closedir (dir);
}

// devices came, or were let to be opened: look again, if there is no pad
static void IN_CheckWatch (void)
{
	char	events[4096];

	if (pad_watch < 0)
		return;
	while (read (pad_watch, events, sizeof(events)) > 0)
		pad_rescan = true;
}

// the device's events since the last frame; false when it is gone
static bool IN_ReadPad (void)
{
	struct input_event	ev[64];
	ssize_t				got;
	int					i, n, b;

	while ((got = read (pad_fd, ev, sizeof(ev))) > 0)
	{
		n = (int)(got / (ssize_t)sizeof(ev[0]));
		for (i = 0 ; i < n ; i++)
		{
			if (ev[i].type == EV_KEY)
			{
				for (b = 0 ; b < PAD_BUTTONS ; b++)
					if (ev[i].code == pad_codes[b])
						pad_down[b] = ev[i].value != 0;
				if (ev[i].code == BTN_TL2)
					pad_tl2 = ev[i].value != 0;
				else if (ev[i].code == BTN_TR2)
					pad_tr2 = ev[i].value != 0;
			}
			else if (ev[i].type == EV_ABS)
			{
				for (b = 0 ; b < NUM_AXES ; b++)
					if (ev[i].code == pad_axiscodes[b])
						pad_axis[b] = ev[i].value;
			}
		}
	}
	return got >= 0 || errno == EAGAIN || errno == EINTR;
}

void IN_InitGamepad (void)
{
	Cvar_RegisterVariable (&in_joystick);
	pad_watch = inotify_init1 (IN_NONBLOCK | IN_CLOEXEC);
	if (pad_watch >= 0 && inotify_add_watch (pad_watch, "/dev/input", IN_CREATE | IN_ATTRIB) < 0)
	{
		close (pad_watch);
		pad_watch = -1;
	}
}

void IN_PollGamepad (void)
{
	uint32_t	state, changed;
	float		lx, ly, rx, ry, hatx, haty;
	bool		down;
	int			i;

	if (!in_joystick.value)
	{
		if (pad_fd >= 0)
			IN_ReleasePad ();
		pad_rescan = true;
		return;
	}

	IN_CheckWatch ();
	if (pad_fd < 0 && pad_rescan)
		IN_FindPad ();
	if (pad_fd < 0)
		return;
	if (!IN_ReadPad ())
	{
		Con_Printf ("Gamepad gone: %s\n", pad_name);
		IN_ReleasePad ();
		pad_rescan = true;
		return;
	}

	// the d-pad as buttons, from its hat where it has one
	hatx = pad_info[AXIS_HATX].maximum > pad_info[AXIS_HATX].minimum ? (float)pad_axis[AXIS_HATX] : 0;
	haty = pad_info[AXIS_HATY].maximum > pad_info[AXIS_HATY].minimum ? (float)pad_axis[AXIS_HATY] : 0;
	state = 0;
	for (i = 0 ; i < PAD_BUTTONS ; i++)
		if (pad_down[i])
			state |= 1u << i;
	if (haty < 0)
		state |= 1u << 10;
	if (haty > 0)
		state |= 1u << 11;
	if (hatx < 0)
		state |= 1u << 12;
	if (hatx > 0)
		state |= 1u << 13;

	changed = state ^ pad_buttonstate;
	for (i = 0 ; i < PAD_BUTTONS ; i++)
		if (changed & (1u << i))
			Key_Event (pad_keys[i], (state & (1u << i)) != 0);
	pad_buttonstate = state;

	down = pad_tl2 || IN_Axis (AXIS_LT) > TRIGGER_THRESHOLD;
	if (down != pad_ltdown)
		Key_Event (K_AUX3, down);
	pad_ltdown = down;
	down = pad_tr2 || IN_Axis (AXIS_RT) > TRIGGER_THRESHOLD;
	if (down != pad_rtdown)
		Key_Event (K_AUX4, down);
	pad_rtdown = down;

	// evdev's +y is down, the client's up
	IN_Stick (IN_Axis (AXIS_LX), -IN_Axis (AXIS_LY), LEFT_DEADZONE, &lx, &ly);
	IN_Stick (IN_Axis (AXIS_RX), -IN_Axis (AXIS_RY), RIGHT_DEADZONE, &rx, &ry);
	IN_GamepadSticks (lx, ly, rx, ry);
}
