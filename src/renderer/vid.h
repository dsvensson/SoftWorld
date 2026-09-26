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
// vid.h -- video driver defs

#include "q_types.h"

#define VID_CBITS	6
#define VID_GRADES	(1 << VID_CBITS)

// A pixel is RGB30: 10 bits per channel, red lowest. SDR white is 255, which
// leaves headroom for light brighter than white.
typedef uint32_t pixel_t;

#define RGB30_WHITE		255

static inline pixel_t RGB30 (unsigned r, unsigned g, unsigned b)
{
	return r | (g << 10) | (b << 20);
}

static inline unsigned RGB30_R (pixel_t p) { return p & 1023; }
static inline unsigned RGB30_G (pixel_t p) { return (p >> 10) & 1023; }
static inline unsigned RGB30_B (pixel_t p) { return (p >> 20) & 1023; }

#define MAX_CONWIDTH	1024		// widest 2D layout

typedef struct vrect_s
{
	int				x,y,width,height;
	struct vrect_s	*pnext;
} vrect_t;

typedef struct
{
	pixel_t			*buffer;		// the frame being drawn
	unsigned		rowpixels;		// pixels from one row to the next
	unsigned		width;
	unsigned		height;
	float			aspect;			// width / height -- < 0 is taller than wide
	int				recalc_refdef;	// if true, recalc vid-based stuff
	unsigned		conwidth;		// the 2D layout: the frame in scale x scale blocks
	unsigned		conheight;
	unsigned		scale;			// render pixels per pixel of the 320x200 layout
} viddef_t;

extern	viddef_t	vid;				// global video state

void	VID_Init (void);
// opens the window and allocates the frame

void	VID_Shutdown (void);
// Called at shutdown

void	VID_Update (void);
// shows the frame

void	VID_SetCaption (const char *text);
// sets the window title

void	VID_BringToFront (void);
// restores and activates the window

bool	VID_IsFullscreen (void);

bool	VID_IsActive (void);		// the window has the focus
bool	VID_IsMinimized (void);

// how the presenter turns the frame into screen colors
typedef struct
{
	float	blend[4];		// rgb and how much of it covers the whole view (0: none)
	float	gamma;			// exponent applied to the frame; 1 keeps it
	float	contrast;		// multiplier; 1 keeps it
} vid_present_t;

void	VID_SetPresent (const vid_present_t *present);

// the frame as the screen shows it in SDR, 8 bit RGB rows vid.width wide
void	VID_FrameToRGB (byte *rgb);
