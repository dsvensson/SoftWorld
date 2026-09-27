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

#include "q_types.h"
#include "wad.h"

// draw.h -- the 2D: the only functions outside the refresh allowed to draw
// on the screen, into its 2D layer

extern	qpic_t		*draw_disc;	// also used on sbar

void Draw_Init (void);
void Draw_Character (int x, int y, int num);
void Draw_ColoredCharacter (int x, int y, int num, unsigned color);	// a text color (markup.h)
void Draw_MarkupString (int x, int y, const char *str);			// colored as the markup in it says
void Draw_Pixel (int x, int y, byte color);
void Draw_SubPic(int x, int y, qpic_t *pic, int srcx, int srcy, int width, int height);
void Draw_Pic (int x, int y, qpic_t *pic);
void Draw_TransPic (int x, int y, qpic_t *pic);
void Draw_ConsoleBackground (int lines, bool downloading);
void Draw_TileClear (int x, int y, int w, int h);
void Draw_Fill (int x, int y, int w, int h, int c);
void Draw_FadeScreen (void);
void Draw_String (int x, int y, char *str);
void Draw_Alt_String (int x, int y, char *str);
qpic_t *Draw_PicFromWad (char *name);
qpic_t *Draw_CachePic (char *path);

// The calls above are recorded; at the end of the frame Draw_Flush draws them
// into vid.hud if they aren't the last frame's (and sets vid.huddirty)
void Draw_Flush (void);
void Draw_Invalidate (void);		// the next Draw_Flush draws whatever the calls
