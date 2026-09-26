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

#pragma once
// snd_local.h -- everything the sound module's own files use

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "args.h"
#include "arena.h"
#include "bspfile.h"
#include "cmd.h"
#include "crc.h"
#include "cvar.h"
#include "fs.h"
#include "info.h"
#include "link.h"
#include "mathlib.h"
#include "md4.h"
#include "mem.h"
#include "msg.h"
#include "print.h"
#include "protocol.h"
#include "q_endian.h"
#include "q_string.h"
#include "q_types.h"
#include "sys.h"
#include "version.h"
#include "vmarray.h"
#include "sound.h"

// 0 to MAX_DYNAMIC_CHANNELS-1	= normal entity sounds
// MAX_DYNAMIC_CHANNELS to MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS -1 = water, etc
// MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS to total_channels = static sounds
typedef struct
{
	bool		initialized;	// S_Init ran
	bool		started;		// an output device is open
	int			blocked;		// > 0 while the window is inactive
	dma_t		dma;			// the output ring

	channel_t	channels[MAX_CHANNELS];
	int			total_channels;
	int			paintedtime;	// sample pairs mixed
	int			soundtime;		// sample pairs the device has taken

	int			viewentity;		// the listener's own entity
	vec3_t		listener_origin;
	vec3_t		listener_forward;
	vec3_t		listener_right;
	vec3_t		listener_up;

	sfx_t		*known_sfx;		// [MAX_SFX]
	int			num_sfx;
	sfx_t		*ambient_sfx[NUM_AMBIENTS];
	bool		ambient;		// ambient sounds play
} snd_state_t;

extern	snd_state_t	snd;
extern	cvar_t		loadas8bit;

// closes and reopens the output after it failed
void S_RestartOutput (void);
