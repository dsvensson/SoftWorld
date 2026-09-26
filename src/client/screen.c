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
// screen.c -- master for refresh, status bar, console, chat, notify, etc

#include "cl_local.h"
#include "png.h"

#include <time.h>

/*

background clear
rendering
turtle/net/ram icons
sbar
centerprint / slow centerprint
notify lines
intermission / finale overlay
loading plaque
console
menu

required background clears
required update regions


syncronous draw mode or async
One off screen buffer, with updates either copied or xblited
Need to double buffer?


async draw will require the refresh area to be cleared, because it will be
xblited, but sync draw can just ignore it.

sync
draw

CenterPrint ()
SlowPrint ()
Screen_Update ();
Con_Printf ();

net 
turn off messages option

the refresh is allways rendered, unless the console is full screen


console is:
	notify lines
	half
	full


*/


scr_state_t	scr;


static float		scr_conlines;		// lines of console to display

static float		oldscreensize, oldfov;
static float		oldsbar;
cvar_t		scr_viewsize = {.name = "viewsize", .string = "100", .archive = true};
static cvar_t		scr_fov = {.name = "fov", .string = "90"};	// 10 - 170
static cvar_t		scr_conspeed = {.name = "scr_conspeed", .string = "300"};
static cvar_t		scr_centertime = {.name = "scr_centertime", .string = "2"};
static cvar_t		scr_showram = {.name = "showram", .string = "1"};
static cvar_t		scr_showturtle = {.name = "showturtle", .string = "0"};
static cvar_t		scr_showpause = {.name = "showpause", .string = "1"};
static cvar_t		scr_printspeed = {.name = "scr_printspeed", .string = "8"};
static cvar_t		scr_allowsnap = {.name = "scr_allowsnap", .string = "1"};
static cvar_t		r_netgraph = {.name = "r_netgraph", .string = "0"};

static bool	scr_initialized;		// ready to draw

static qpic_t		*scr_ram;
static qpic_t		*scr_net;
static qpic_t		*scr_turtle;






void SCR_ScreenShot_f (void);
void SCR_RSShot_f (void);

/*
===============================================================================

CENTER PRINTING

===============================================================================
*/

static char		scr_centerstring[1024];
static float		scr_centertime_start;	// for slow victory printing
static float		scr_centertime_off;
static int			scr_center_lines;

/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void SCR_CenterPrint (char *str)
{
	strncpy (scr_centerstring, str, sizeof(scr_centerstring)-1);
	scr_centertime_off = scr_centertime.value;
	scr_centertime_start = (float)cl.time;

// count the number of lines for centering
	scr_center_lines = 1;
	while (*str)
	{
		if (*str == '\n')
			scr_center_lines++;
		str++;
	}
}

void SCR_DrawCenterString (void)
{
	char	*start;
	int		l;
	int		j;
	int		x, y;
	int		remaining;

// the finale prints the characters one at a time
	if (cl.intermission)
		remaining = (int)(scr_printspeed.value * (cl.time - scr_centertime_start));
	else
		remaining = 9999;

	start = scr_centerstring;

	if (scr_center_lines <= 4)
		y = (int)(vid.conheight*0.35);
	else
		y = 48;

	do	
	{
	// scan the width of the line
		for (l=0 ; l<40 ; l++)
			if (start[l] == '\n' || !start[l])
				break;
		x = (vid.conwidth - l*8)/2;
		for (j=0 ; j<l ; j++, x+=8)
		{
			Draw_Character (x, y, start[j]);	
			if (!remaining--)
				return;
		}
			
		y += 8;

		while (*start && *start != '\n')
			start++;

		if (!*start)
			break;
		start++;		// skip the \n
	} while (1);
}

void SCR_CheckDrawCenterString (void)
{

	scr_centertime_off = (float)(scr_centertime_off - cls.frametime);
	
	if (scr_centertime_off <= 0 && !cl.intermission)
		return;
	if (cls.key_dest != key_game)
		return;

	SCR_DrawCenterString ();
}

//=============================================================================

/*
====================
CalcFov
====================
*/
float CalcFov (float fov_x, float width, float height)
{
        float   a;
        float   x;

        if (fov_x < 1 || fov_x > 179)
                Sys_Error ("Bad fov: %f", fov_x);

        x = width/tanf((float)(fov_x/360*Q_PI));

        a = atanf(height/x);

        a = (float)(a*360/Q_PI);

        return a;
}

/*
===============
SCR_SetVrect
===============
*/
static void SCR_SetVrect (vrect_t *pvrectin, vrect_t *pvrect, int lineadj)
{
	int		h;
	float	size;
	bool full = false;

	if (scr_viewsize.value >= 100.0) {
		size = 100.0;
		full = true;
	} else
		size = scr_viewsize.value;

	if (cl.intermission)
	{
		full = true;
		size = 100.0;
		lineadj = 0;
	}
	size /= 100.0;

	if (!cl_sbar.value && full)
		h = pvrectin->height;
	else
		h = pvrectin->height - lineadj;

//	h = (!cl_sbar.value && size==1.0) ? pvrectin->height : (pvrectin->height - lineadj);
//	h = pvrectin->height - lineadj;
	if (full)
		pvrect->width = pvrectin->width;
	else
		pvrect->width = (int)(pvrectin->width * size);
	if (pvrect->width < 96)
	{
		size = 96.0f / pvrectin->width;
		pvrect->width = 96;	// min for icons
	}
	pvrect->width &= ~7;
	pvrect->height = (int)(pvrectin->height * size);
	if (cl_sbar.value || !full) {
		if (pvrect->height > pvrectin->height - lineadj)
			pvrect->height = pvrectin->height - lineadj;
	} else
		if (pvrect->height > pvrectin->height)
			pvrect->height = pvrectin->height;

	pvrect->height &= ~1;

	pvrect->x = (pvrectin->width - pvrect->width)/2;
	if (full)
		pvrect->y = 0;
	else
		pvrect->y = (h - pvrect->height)/2;
}

/*
=================
SCR_CalcRefdef

Must be called whenever vid changes
Internal use only
=================
*/
static void SCR_CalcRefdef (void)
{
	vrect_t		pixels;
	vrect_t		vrect;
	float		size;

	vid.recalc_refdef = 0;

// force the status bar to redraw

//========================================
	
// bound viewsize
	if (scr_viewsize.value < 30)
		Cvar_Set ("viewsize","30");
	if (scr_viewsize.value > 120)
		Cvar_Set ("viewsize","120");

// bound field of view
	if (scr_fov.value < 10)
		Cvar_Set ("fov","10");
	if (scr_fov.value > 170)
		Cvar_Set ("fov","170");

	r_refdef.fov_x = scr_fov.value;
	// a wider layout sees more to the sides, the same up and down
	if (vid.conwidth != 320)
		r_refdef.fov_x = atanf (tanf (scr_fov.value * (float)Q_PI / 360) * vid.conwidth / 320) * 360 / (float)Q_PI;
	r_refdef.fov_y = CalcFov (r_refdef.fov_x, (float)r_refdef.vrect.width, (float)r_refdef.vrect.height);

// intermission is always full screen	
	if (cl.intermission)
		size = 120;
	else
		size = scr_viewsize.value;

	if (size >= 120)
		scr.sb_lines = 0;		// no status bar at all
	else if (size >= 110)
		scr.sb_lines = 24;		// no inventory
	else
		scr.sb_lines = 24+16+8;

// these calculations mirror those in R_Init() for r_refdef, but take no
// account of water warping
	vrect.x = 0;
	vrect.y = 0;
	vrect.width = vid.conwidth;
	vrect.height = vid.conheight;

	SCR_SetVrect (&vrect, &scr.vrect, scr.sb_lines);

// guard against going from one mode to another that's less than half the
// vertical resolution
	if (scr.con_current > vid.conheight)
		scr.con_current = (float)vid.conheight;

// notify the refresh of the change
	pixels = scr.vrect;
	pixels.x *= vid.scale;
	pixels.y *= vid.scale;
	pixels.width *= vid.scale;
	pixels.height *= vid.scale;
	R_ViewChanged (&pixels, vid.aspect);
}


/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
void SCR_SizeUp_f (void)
{
	if (scr_viewsize.value < 120) {
	Cvar_SetValue ("viewsize",scr_viewsize.value+10);
	vid.recalc_refdef = 1;
	}
}


/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
void SCR_SizeDown_f (void)
{
	Cvar_SetValue ("viewsize",scr_viewsize.value-10);
	vid.recalc_refdef = 1;
}

//============================================================================

/*
==================
SCR_Init
==================
*/
void SCR_Init (void)
{
	Cvar_RegisterVariable (&scr_fov);
	Cvar_RegisterVariable (&scr_viewsize);
	Cvar_RegisterVariable (&scr_conspeed);
	Cvar_RegisterVariable (&scr_showram);
	Cvar_RegisterVariable (&scr_showturtle);
	Cvar_RegisterVariable (&scr_showpause);
	Cvar_RegisterVariable (&scr_centertime);
	Cvar_RegisterVariable (&scr_printspeed);
	Cvar_RegisterVariable (&scr_allowsnap);
	Cvar_RegisterVariable (&r_netgraph);

//
// register our commands
//
	Cmd_AddCommand ("screenshot",SCR_ScreenShot_f);
	Cmd_AddCommand ("snap",SCR_RSShot_f);
	Cmd_AddCommand ("sizeup",SCR_SizeUp_f);
	Cmd_AddCommand ("sizedown",SCR_SizeDown_f);

	scr_ram = W_GetLumpName ("ram");
	scr_net = W_GetLumpName ("net");
	scr_turtle = W_GetLumpName ("turtle");

	scr_initialized = true;
}


/*
==============
SCR_DrawRam
==============
*/
void SCR_DrawRam (void)
{
	if (!scr_showram.value)
		return;

	if (!r_cache_thrash)
		return;

	Draw_Pic (scr.vrect.x+32, scr.vrect.y, scr_ram);
}

/*
==============
SCR_DrawTurtle
==============
*/
void SCR_DrawTurtle (void)
{
	static int	count;
	
	if (!scr_showturtle.value)
		return;

	if (cls.frametime < 0.1)
	{
		count = 0;
		return;
	}

	count++;
	if (count < 3)
		return;

	Draw_Pic (scr.vrect.x, scr.vrect.y, scr_turtle);
}

/*
==============
SCR_DrawNet
==============
*/
void SCR_DrawNet (void)
{
	if (cls.netchan.outgoing_sequence - cls.netchan.incoming_acknowledged < UPDATE_BACKUP-1)
		return;
	if (cls.demoplayback)
		return;

	Draw_Pic (scr.vrect.x+64, scr.vrect.y, scr_net);
}

void SCR_DrawFPS (void)
{
	extern cvar_t show_fps;
	static double lastframetime;
	double t;
	static int lastfps;
	int x, y;
	char st[80];

	if (!show_fps.value)
		return;

	t = Sys_DoubleTime();
	if ((t - lastframetime) >= 1.0) {
		lastfps = cls.fps_count;
		cls.fps_count = 0;
		lastframetime = t;
	}

	snprintf(st, sizeof(st), "%3d FPS", lastfps);
	x = (int)(vid.conwidth - strlen(st) * 8 - 8);
	y = vid.conheight - scr.sb_lines - 8;
//	Draw_TileClear(x, y, strlen(st) * 8, 8);
	Draw_String(x, y, st);
}

/*
==============
DrawPause
==============
*/
void SCR_DrawPause (void)
{
	qpic_t	*pic;

	if (!scr_showpause.value)		// turn off for screenshots
		return;

	if (!cl.paused)
		return;

	pic = Draw_CachePic ("gfx/pause.lmp");
	Draw_Pic ( (vid.conwidth - pic->width)/2, 
		(vid.conheight - 48 - pic->height)/2, pic);
}


//=============================================================================


/*
==================
SCR_SetUpToDrawConsole
==================
*/
void SCR_SetUpToDrawConsole (void)
{
	Con_CheckResize ();
	
// decide on the height of the console
	if (cls.state != ca_active)
	{
		scr_conlines = (float)vid.conheight;		// full screen
		scr.con_current = scr_conlines;
	}
	else if (cls.key_dest == key_console)
		scr_conlines = (float)(vid.conheight/2);	// half screen
	else
		scr_conlines = 0;				// none visible
	
	if (scr_conlines < scr.con_current)
	{
		scr.con_current = (float)(scr.con_current - scr_conspeed.value*cls.frametime);
		if (scr_conlines > scr.con_current)
			scr.con_current = scr_conlines;

	}
	else if (scr_conlines > scr.con_current)
	{
		scr.con_current = (float)(scr.con_current + scr_conspeed.value*cls.frametime);
		if (scr_conlines < scr.con_current)
			scr.con_current = scr_conlines;
	}

}
	
/*
==================
SCR_DrawConsole
==================
*/
void SCR_DrawConsole (void)
{
	if (scr.con_current)
	{
		Con_DrawConsole ((int)scr.con_current);
	}
	else
	{
		if (cls.key_dest == key_game || cls.key_dest == key_message)
			Con_DrawNotify ();	// only draw notify in game
	}
}


/* 
============================================================================== 
 
						SCREEN SHOTS 
 
============================================================================== 
*/ 


/* 
============== 
WritePCXfile 
============== 
*/ 
void WritePCXfile (char *filename, byte *data, int width, int height,
	int rowbytes, byte *palette, bool upload) 
{
	int		i, j, length;
	pcx_t	*pcx;
	byte		*pack;
	  
	pcx = Mem_Alloc ((size_t)width*height*2+1000);
	if (pcx == NULL)
	{
		Con_Printf("SCR_ScreenShot_f: not enough memory\n");
		return;
	} 
 
	pcx->manufacturer = 0x0a;	// PCX id
	pcx->version = 5;			// 256 color
 	pcx->encoding = 1;		// uncompressed
	pcx->bits_per_pixel = 8;		// 256 color
	pcx->xmin = 0;
	pcx->ymin = 0;
	pcx->xmax = LittleShort((short)(width-1));
	pcx->ymax = LittleShort((short)(height-1));
	pcx->hres = LittleShort((short)width);
	pcx->vres = LittleShort((short)height);
	Q_memset (pcx->palette,0,sizeof(pcx->palette));
	pcx->color_planes = 1;		// chunky image
	pcx->bytes_per_line = LittleShort((short)width);
	pcx->palette_type = LittleShort(2);		// not a grey scale
	Q_memset (pcx->filler,0,sizeof(pcx->filler));

// pack the image
	pack = &pcx->data;
	
	for (i=0 ; i<height ; i++)
	{
		for (j=0 ; j<width ; j++)
		{
			if ( (*data & 0xc0) != 0xc0)
				*pack++ = *data++;
			else
			{
				*pack++ = 0xc1;
				*pack++ = *data++;
			}
		}

		data += rowbytes - width;
	}
			
// write the palette
	*pack++ = 0x0c;	// palette ID byte
	for (i=0 ; i<768 ; i++)
		*pack++ = *palette++;
		
// write output file 
	length = (int)(pack - (byte *)pcx);
	if (upload)
		CL_StartUpload((void *)pcx, length);
	else
		COM_WriteFile (filename, pcx, length);

	Mem_Free (pcx);
} 


/* 
================== 
SCR_ScreenShot_f
================== 
*/  
void SCR_ScreenShot_f (void)
{
	int		i;
	char	filename[80];
	char	path[MAX_OSPATH];
	byte	*rgb;

//
// find a file name to save it to
//
	for (i=0 ; i<=99 ; i++)
	{
		snprintf (filename, sizeof(filename), "quake%02d.png", i);
		snprintf (path, sizeof(path), "%s/%s", com_gamedir, filename);
		if (Sys_FileTime (path) == -1)
			break;	// file doesn't exist
	}
	if (i==100)
	{
		Con_Printf ("SCR_ScreenShot_f: Couldn't create a PNG\n");
		return;
	}

//
// save what the screen shows
//
	rgb = Mem_Alloc ((size_t)vid.width * vid.height * 3);
	VID_FrameToRGB (rgb);
	if (PNG_WriteRGB (path, (int)vid.width, (int)vid.height, rgb, (int)vid.width * 3))
		Con_Printf ("Wrote %s\n", filename);
	else
		Con_Printf ("Couldn't write %s\n", filename);
	Mem_Free (rgb);
}

/*
Find closest color in the palette for named color
*/
int MipColor(int r, int g, int b)
{
	int i;
	float dist;
	int best = 0;
	float bestdist;
	int r1, g1, b1;
	static int lr = -1, lg = -1, lb = -1;
	static int lastbest;

	if (r == lr && g == lg && b == lb)
		return lastbest;

	bestdist = 256*256*3;

	for (i = 0; i < 256; i++) {
		r1 = cls.basepal[i*3] - r;
		g1 = cls.basepal[i*3+1] - g;
		b1 = cls.basepal[i*3+2] - b;
		dist = (float)(r1*r1 + g1*g1 + b1*b1);
		if (dist < bestdist) {
			bestdist = dist;
			best = i;
		}
	}
	lr = r; lg = g; lb = b;
	lastbest = best;
	return best;
}

// in draw.c
extern byte		*draw_chars;				// 8*8 graphic characters

void SCR_DrawCharToSnap (int num, byte *dest, int width)
{
	int		row, col;
	byte	*source;
	int		drawline;
	int		x;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x])
				dest[x] = source[x];
			else
				dest[x] = 98;
		source += 128;
		dest += width;
	}

}

void SCR_DrawStringToSnap (const char *s, byte *buf, int x, int y, int width)
{
	byte *dest;
	const unsigned char *p;

	dest = buf + ((y * width) + x);

	p = (const unsigned char *)s;
	while (*p) {
		SCR_DrawCharToSnap(*p++, dest, width);
		dest += 8;
	}
}


/* 
================== 
SCR_RSShot_f
================== 
*/  
void SCR_RSShot_f (void) 
{ 
	int     x, y;
	pixel_t		*src;
	unsigned char		*dest;
	char		pcxname[80] = "snap.pcx";
	unsigned char		*newbuf;
	int w, h;
	int dx, dy, dex, dey, nx;
	int r, b, g;
	int count;
	float fracw, frach;
	char st[80];
	time_t now;

	if (CL_IsUploading())
		return; // already one pending

	if (cls.state < ca_onserver)
		return; // gotta be connected

	if (!scr_allowsnap.value) {
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, "snap\n");
		Con_Printf("Refusing remote screen shot request.\n");
		return;
	}

	Con_Printf("Remote screen shot requested.\n");


// 
// save the pcx file 
// 

	w = (vid.width < RSSHOT_WIDTH) ? vid.width : RSSHOT_WIDTH;
	h = (vid.height < RSSHOT_HEIGHT) ? vid.height : RSSHOT_HEIGHT;

	fracw = (float)vid.width / (float)w;
	frach = (float)vid.height / (float)h;

	newbuf = malloc(w*h);

	for (y = 0; y < h; y++) {
		dest = newbuf + (w * y);

		for (x = 0; x < w; x++) {
			r = g = b = 0;

			dx = (int)(x * fracw);
			dex = (int)((x + 1) * fracw);
			if (dex == dx) dex++; // at least one
			dy = (int)(y * frach);
			dey = (int)((y + 1) * frach);
			if (dey == dy) dey++; // at least one

			count = 0;
			for (/* */; dy < dey; dy++) {
				src = vid.buffer + (vid.rowpixels * dy) + dx;
				for (nx = dx; nx < dex; nx++) {
					r += RGB30_R (*src) > 255 ? 255 : RGB30_R (*src);
					g += RGB30_G (*src) > 255 ? 255 : RGB30_G (*src);
					b += RGB30_B (*src) > 255 ? 255 : RGB30_B (*src);
					src++;
					count++;
				}
			}
			r /= count;
			g /= count;
			b /= count;
			*dest++  = (unsigned char)MipColor(r, g, b);
		}
	}

	time(&now);
	Q_strncpyz(st, ctime(&now), sizeof(st));
	st[strlen(st) - 1] = 0;
	SCR_DrawStringToSnap (st, newbuf, w - (int)strlen(st)*8, 0, w);

	strncpy(st, cls.servername, sizeof(st));
	st[sizeof(st) - 1] = 0;
	SCR_DrawStringToSnap (st, newbuf, w - (int)strlen(st)*8, 10, w);

	strncpy(st, name.string, sizeof(st));
	st[sizeof(st) - 1] = 0;
	SCR_DrawStringToSnap (st, newbuf, w - (int)strlen(st)*8, 20, w);

	WritePCXfile (pcxname, newbuf, w, h, w, cls.basepal, true);

	free(newbuf);


//	Con_Printf ("Wrote %s\n", pcxname);
	Con_Printf ("Sending shot to server...\n");
} 


//=============================================================================

static char	*scr_notifystring;
static bool	scr_drawdialog;

void SCR_DrawNotifyString (void)
{
	char	*start;
	int		l;
	int		j;
	int		x, y;

	start = scr_notifystring;

	y = (int)(vid.height*0.35);

	do	
	{
	// scan the width of the line
		for (l=0 ; l<40 ; l++)
			if (start[l] == '\n' || !start[l])
				break;
		x = (vid.width - l*8)/2;
		for (j=0 ; j<l ; j++, x+=8)
			Draw_Character (x, y, start[j]);	
			
		y += 8;

		while (*start && *start != '\n')
			start++;

		if (!*start)
			break;
		start++;		// skip the \n
	} while (1);
}

//=============================================================================

/*
==============
SCR_DrawNetGraph
==============
*/
static void SCR_DrawNetGraph (void)
{
	int		a, x, y, y2, w, i;
	int lost;
	char st[80];

	if (vid.conwidth - 16 <= NET_TIMINGS)
		w = vid.conwidth - 16;
	else
		w = NET_TIMINGS;

	x =	-(int)((vid.conwidth - 320)>>1);
	y = vid.conheight - scr.sb_lines - 24 - (int)r_graphheight.value*2 - 2;

	M_DrawTextBox (x, y, (w+7)/8, ((int)r_graphheight.value*2+7)/8 + 1);
	y2 = y + 8;
	y = vid.conheight - scr.sb_lines - 8 - 2;

	x = 8;
	lost = CL_CalcNet();
	for (a=NET_TIMINGS-w ; a<w ; a++)
	{
		i = (cls.netchan.outgoing_sequence-a) & NET_TIMINGSMASK;
		R_LineGraph (x+w-1-a, y, cl.packet_latency[i]);
	}
	snprintf(st, sizeof(st), "%3i%% packet loss", lost);
	Draw_String(8, y2, st);
}

/*
==================
SCR_TileClear

The backdrop around the 3D view
==================
*/
static void SCR_TileClear (void)
{
	int		top = scr.vrect.y, bottom = scr.vrect.y + scr.vrect.height;
	int		right = scr.vrect.x + scr.vrect.width;

	if (top > 0)
		Draw_TileClear (0, 0, vid.conwidth, top);
	if (scr.vrect.x > 0)
		Draw_TileClear (0, top, scr.vrect.x, scr.vrect.height);
	if (right < (int)vid.conwidth)
		Draw_TileClear (right, top, vid.conwidth - right, scr.vrect.height);
	if (bottom < (int)vid.conheight)
		Draw_TileClear (0, bottom, vid.conwidth, vid.conheight - bottom);
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.

WARNING: be very careful calling this from elsewhere, because the refresh
needs almost the entire 256k of stack space!
==================
*/
void SCR_UpdateScreen (void)
{
	double			prof;
	static float	oldscr_viewsize;

	if (scr.disabled_for_loading || VID_IsMinimized ())
		return;

	if (!scr_initialized || !con.initialized)
		return;				// not initialized yet

	if (scr_viewsize.value != oldscr_viewsize)
	{
		oldscr_viewsize = scr_viewsize.value;
		vid.recalc_refdef = 1;
	}
	
//
// check for vid changes
//
	if (oldfov != scr_fov.value)
	{
		oldfov = scr_fov.value;
		vid.recalc_refdef = true;
	}
	
	if (oldscreensize != scr_viewsize.value)
	{
		oldscreensize = scr_viewsize.value;
		vid.recalc_refdef = true;
	}

	if (oldsbar != cl_sbar.value)
	{
		oldsbar = cl_sbar.value;
		vid.recalc_refdef = true;
	}
	
	if (vid.recalc_refdef)
	{
		// something changed, so reorder the screen
		SCR_CalcRefdef ();
	}

//
// do 3D refresh drawing, and then update the screen
//

	SCR_TileClear ();



	SCR_SetUpToDrawConsole ();


	V_RenderView ();
	prof = R_ProfStart ();
	if (r_netgraph.value)
		SCR_DrawNetGraph ();


	if (scr_drawdialog)
	{
		Sbar_Draw ();
		Draw_FadeScreen ();
		SCR_DrawNotifyString ();
	}
	else if (cl.intermission == 1 && cls.key_dest == key_game)
	{
		Sbar_IntermissionOverlay ();
	}
	else if (cl.intermission == 2 && cls.key_dest == key_game)
	{
		Sbar_FinaleOverlay ();
		SCR_CheckDrawCenterString ();
	}
	else
	{
		SCR_DrawRam ();
		SCR_DrawNet ();
		SCR_DrawTurtle ();
		SCR_DrawPause ();
		SCR_DrawFPS ();
		SCR_CheckDrawCenterString ();
		Sbar_Draw ();
		SCR_DrawConsole ();	
		M_Draw ();
	}


	R_ProfEnd (PROF_2D, prof);
	V_UpdateBlend ();
	prof = R_ProfStart ();
	VID_Update ();
	R_ProfEnd (PROF_PRESENT, prof);
}
