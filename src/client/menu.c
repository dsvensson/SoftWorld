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
#include "cl_local.h"


static enum {m_none, m_main, m_singleplayer, m_load, m_save, m_multiplayer, m_setup, m_net, m_options, m_keys, m_help, m_quit, m_serialconfig, m_modemconfig, m_lanconfig, m_gameoptions, m_search, m_slist} m_state;

static void M_Menu_Main_f (void);
	static void M_Menu_SinglePlayer_f (void);
	static void M_Menu_MultiPlayer_f (void);
	static void M_Menu_Options_f (void);
		static void M_Menu_Keys_f (void);
	static void M_Menu_Help_f (void);
	void M_Menu_Quit_f (void);

static void M_Main_Draw (void);
	static void M_SinglePlayer_Draw (void);
	static void M_MultiPlayer_Draw (void);
	static void M_Options_Draw (void);
		static void M_Keys_Draw (void);
	static void M_Help_Draw (void);
	static void M_Quit_Draw (void);

static void M_Main_Key (int key);
	static void M_SinglePlayer_Key (int key);
	static void M_MultiPlayer_Key (int key);
	static void M_Options_Key (int key);
		static void M_Keys_Key (int key);
	static void M_Help_Key (int key);
	static void M_Quit_Key (int key);

static bool	m_entersound;		// play after drawing a frame, so caching
								// won't disrupt the sound
static bool	m_recursiveDraw;


#define StartingGame	(m_multiplayer_cursor == 1)
#define JoiningGame		(m_multiplayer_cursor == 0)
#define SerialConfig	(m_net_cursor == 0)
#define DirectConfig	(m_net_cursor == 1)
#define	IPXConfig		(m_net_cursor == 2)
#define	TCPIPConfig		(m_net_cursor == 3)


//=============================================================================
/* Support Routines */

/*
================
M_DrawCharacter

Draws one solid graphics character
================
*/
static void M_DrawCharacter (int cx, int line, int num)
{
	Draw_Character ( cx + ((vid.conwidth - 320)>>1), line, num);
}

static void M_Print (int cx, int cy, char *str)
{
	while (*str)
	{
		M_DrawCharacter (cx, cy, (*str)+128);
		str++;
		cx += 8;
	}
}

static void M_PrintWhite (int cx, int cy, char *str)
{
	while (*str)
	{
		M_DrawCharacter (cx, cy, *str);
		str++;
		cx += 8;
	}
}

static void M_DrawTransPic (int x, int y, qpic_t *pic)
{
	Draw_TransPic (x + ((vid.conwidth - 320)>>1), y, pic);
}

static void M_DrawPic (int x, int y, qpic_t *pic)
{
	Draw_Pic (x + ((vid.conwidth - 320)>>1), y, pic);
}


void M_DrawTextBox (int x, int y, int width, int lines)
{
	qpic_t	*p;
	int		cx, cy;
	int		n;

	// draw left side
	cx = x;
	cy = y;
	p = Draw_CachePic ("gfx/box_tl.lmp");
	M_DrawTransPic (cx, cy, p);
	p = Draw_CachePic ("gfx/box_ml.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		M_DrawTransPic (cx, cy, p);
	}
	p = Draw_CachePic ("gfx/box_bl.lmp");
	M_DrawTransPic (cx, cy+8, p);

	// draw middle
	cx += 8;
	while (width > 0)
	{
		cy = y;
		p = Draw_CachePic ("gfx/box_tm.lmp");
		M_DrawTransPic (cx, cy, p);
		p = Draw_CachePic ("gfx/box_mm.lmp");
		for (n = 0; n < lines; n++)
		{
			cy += 8;
			if (n == 1)
				p = Draw_CachePic ("gfx/box_mm2.lmp");
			M_DrawTransPic (cx, cy, p);
		}
		p = Draw_CachePic ("gfx/box_bm.lmp");
		M_DrawTransPic (cx, cy+8, p);
		width -= 2;
		cx += 16;
	}

	// draw right side
	cy = y;
	p = Draw_CachePic ("gfx/box_tr.lmp");
	M_DrawTransPic (cx, cy, p);
	p = Draw_CachePic ("gfx/box_mr.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		M_DrawTransPic (cx, cy, p);
	}
	p = Draw_CachePic ("gfx/box_br.lmp");
	M_DrawTransPic (cx, cy+8, p);
}

//=============================================================================

static int m_save_demonum;
		
/*
================
M_ToggleMenu_f
================
*/
void M_ToggleMenu_f (void)
{
	m_entersound = true;

	if (cls.key_dest == key_menu)
	{
		if (m_state != m_main)
		{
			M_Menu_Main_f ();
			return;
		}
		cls.key_dest = key_game;
		m_state = m_none;
		return;
	}
	if (cls.key_dest == key_console)
	{
		Con_ToggleConsole_f ();
	}
	else
	{
		M_Menu_Main_f ();
	}
}

		
//=============================================================================
/* MAIN MENU */

static int	m_main_cursor;
#define	MAIN_ITEMS	5


static void M_Menu_Main_f (void)
{
	if (cls.key_dest != key_menu)
	{
		m_save_demonum = cls.demonum;
		cls.demonum = -1;
	}
	cls.key_dest = key_menu;
	m_state = m_main;
	m_entersound = true;
}
				

static void M_Main_Draw (void)
{
	int		f;
	qpic_t	*p;

	M_DrawTransPic (16, 4, Draw_CachePic ("gfx/qplaque.lmp") );
	p = Draw_CachePic ("gfx/ttl_main.lmp");
	M_DrawPic ( (320-p->width)/2, 4, p);
	M_DrawTransPic (72, 32, Draw_CachePic ("gfx/mainmenu.lmp") );

	f = (int)(host.realtime * 10)%6;
	
	M_DrawTransPic (54, 32 + m_main_cursor * 20,Draw_CachePic( va("gfx/menudot%i.lmp", f+1 ) ) );
}


static void M_Main_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		cls.key_dest = key_game;
		m_state = m_none;
		cls.demonum = m_save_demonum;
		if (cls.demonum != -1 && !cls.demoplayback && cls.state == ca_disconnected)
			CL_NextDemo ();
		break;
		
	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_main_cursor >= MAIN_ITEMS)
			m_main_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_main_cursor < 0)
			m_main_cursor = MAIN_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;

		switch (m_main_cursor)
		{
		case 0:
			M_Menu_SinglePlayer_f ();
			break;

		case 1:
			M_Menu_MultiPlayer_f ();
			break;

		case 2:
			M_Menu_Options_f ();
			break;

		case 3:
			M_Menu_Help_f ();
			break;

		case 4:
			M_Menu_Quit_f ();
			break;
		}
	}
}


//=============================================================================
/* OPTIONS MENU */

#define	OPTIONS_ITEMS	16

#define	SLIDER_RANGE	10

static int		options_cursor;

static void M_Menu_Options_f (void)
{
	cls.key_dest = key_menu;
	m_state = m_options;
	m_entersound = true;
}


static void M_AdjustSliders (int dir)
{
	S_LocalSound ("misc/menu3.wav");

	switch (options_cursor)
	{
	case 3:	// screen size
		scr_viewsize.value += dir * 10;
		if (scr_viewsize.value < 30)
			scr_viewsize.value = 30;
		if (scr_viewsize.value > 120)
			scr_viewsize.value = 120;
		Cvar_SetValue ("viewsize", scr_viewsize.value);
		break;
	case 4:	// gamma
		v_gamma.value -= dir * 0.05f;
		if (v_gamma.value < 0.5)
			v_gamma.value = 0.5;
		if (v_gamma.value > 1)
			v_gamma.value = 1;
		Cvar_SetValue ("gamma", v_gamma.value);
		break;
	case 5:	// mouse speed
		sensitivity.value += dir * 0.5f;
		if (sensitivity.value < 1)
			sensitivity.value = 1;
		if (sensitivity.value > 11)
			sensitivity.value = 11;
		Cvar_SetValue ("sensitivity", sensitivity.value);
		break;
	case 6:	// music volume
		bgmvolume.value += dir * 1.0f;
		if (bgmvolume.value < 0)
			bgmvolume.value = 0;
		if (bgmvolume.value > 1)
			bgmvolume.value = 1;
		Cvar_SetValue ("bgmvolume", bgmvolume.value);
		break;
	case 7:	// sfx volume
		volume.value += dir * 0.1f;
		if (volume.value < 0)
			volume.value = 0;
		if (volume.value > 1)
			volume.value = 1;
		Cvar_SetValue ("volume", volume.value);
		break;
		
	case 8:	// allways run
		if (cl_forwardspeed.value > 200)
		{
			Cvar_SetValue ("cl_forwardspeed", 200);
			Cvar_SetValue ("cl_backspeed", 200);
		}
		else
		{
			Cvar_SetValue ("cl_forwardspeed", 400);
			Cvar_SetValue ("cl_backspeed", 400);
		}
		break;
	
	case 9:	// invert mouse
		Cvar_SetValue ("m_pitch", -m_pitch.value);
		break;
	
	case 10:	// lookspring
		Cvar_SetValue ("lookspring", !lookspring.value);
		break;
	
	case 11:	// lookstrafe
		Cvar_SetValue ("lookstrafe", !lookstrafe.value);
		break;

	case 12:
		Cvar_SetValue ("cl_sbar", !cl_sbar.value);
		break;

	case 13:
		Cvar_SetValue ("cl_hudswap", !cl_hudswap.value);
		break;

	case 14:	// mouse look
		Cvar_SetValue ("freelook", !freelook.value);
		break;

	case 15:	// _windowed_mouse
		Cvar_SetValue ("_windowed_mouse", !_windowed_mouse.value);
		break;
	}
}


static void M_DrawSlider (int x, int y, float range)
{
	int	i;

	if (range < 0)
		range = 0;
	if (range > 1)
		range = 1;
	M_DrawCharacter (x-8, y, 128);
	for (i=0 ; i<SLIDER_RANGE ; i++)
		M_DrawCharacter (x + i*8, y, 129);
	M_DrawCharacter (x+i*8, y, 130);
	M_DrawCharacter ((int)(x + (SLIDER_RANGE-1)*8 * range), y, 131);
}

static void M_DrawCheckbox (int x, int y, int on)
{
	if (on)
		M_Print (x, y, "on");
	else
		M_Print (x, y, "off");
}

static void M_Options_Draw (void)
{
	float		r;
	qpic_t	*p;
	
	M_DrawTransPic (16, 4, Draw_CachePic ("gfx/qplaque.lmp") );
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic ( (320-p->width)/2, 4, p);
	
	M_Print (16, 32, "    Customize controls");
	M_Print (16, 40, "         Go to console");
	M_Print (16, 48, "     Reset to defaults");

	M_Print (16, 56, "           Screen size");
	r = (scr_viewsize.value - 30) / (120 - 30);
	M_DrawSlider (220, 56, r);

	M_Print (16, 64, "            Brightness");
	r = (1.0f - v_gamma.value) / 0.5f;
	M_DrawSlider (220, 64, r);

	M_Print (16, 72, "           Mouse Speed");
	r = (sensitivity.value - 1)/10;
	M_DrawSlider (220, 72, r);

	M_Print (16, 80, "       CD Music Volume");
	r = bgmvolume.value;
	M_DrawSlider (220, 80, r);

	M_Print (16, 88, "          Sound Volume");
	r = volume.value;
	M_DrawSlider (220, 88, r);

	M_Print (16, 96,  "            Always Run");
	M_DrawCheckbox (220, 96, cl_forwardspeed.value > 200);

	M_Print (16, 104, "          Invert Mouse");
	M_DrawCheckbox (220, 104, m_pitch.value < 0);

	M_Print (16, 112, "            Lookspring");
	M_DrawCheckbox (220, 112, (int)lookspring.value);

	M_Print (16, 120, "            Lookstrafe");
	M_DrawCheckbox (220, 120, (int)lookstrafe.value);

	M_Print (16, 128, "    Use old status bar");
	M_DrawCheckbox (220, 128, (int)cl_sbar.value);

	M_Print (16, 136, "      HUD on left side");
	M_DrawCheckbox (220, 136, (int)cl_hudswap.value);

	M_Print (16, 144, "            Mouse Look");
	M_DrawCheckbox (220, 144, (int)freelook.value);

	if (!VID_IsFullscreen ())
	{
		M_Print (16, 152, "             Use Mouse");
		M_DrawCheckbox (220, 152, (int)_windowed_mouse.value);
	}

// cursor
	M_DrawCharacter (200, 32 + options_cursor*8, 12+((int)(host.realtime*4)&1));
}


static void M_Options_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;
		
	case K_ENTER:
		m_entersound = true;
		switch (options_cursor)
		{
		case 0:
			M_Menu_Keys_f ();
			break;
		case 1:
			m_state = m_none;
			Con_ToggleConsole_f ();
			break;
		case 2:
			Cbuf_AddText ("exec default.cfg\n");
			break;
		default:
			M_AdjustSliders (1);
			break;
		}
		return;
	
	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		options_cursor--;
		if (options_cursor < 0)
			options_cursor = OPTIONS_ITEMS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		options_cursor++;
		if (options_cursor >= OPTIONS_ITEMS)
			options_cursor = 0;
		break;	

	case K_LEFTARROW:
		M_AdjustSliders (-1);
		break;

	case K_RIGHTARROW:
		M_AdjustSliders (1);
		break;
	}

	if ((options_cursor == 15) 
	&& VID_IsFullscreen ()
	)
	{
		if (k == K_UPARROW)
			options_cursor = 14;
		else
			options_cursor = 0;
	}
}


//=============================================================================
/* KEYS MENU */

static char *bindnames[][2] =
{
{"+attack", 		"attack"},
{"impulse 10", 		"change weapon"},
{"+jump", 			"jump / swim up"},
{"+forward", 		"walk forward"},
{"+back", 			"backpedal"},
{"+left", 			"turn left"},
{"+right", 			"turn right"},
{"+speed", 			"run"},
{"+moveleft", 		"step left"},
{"+moveright", 		"step right"},
{"+strafe", 		"sidestep"},
{"+lookup", 		"look up"},
{"+lookdown", 		"look down"},
{"centerview", 		"center view"},
{"+mlook", 			"mouse look"},
{"+klook", 			"keyboard look"},
{"+moveup",			"swim up"},
{"+movedown",		"swim down"}
};

#define	NUMCOMMANDS	(sizeof(bindnames)/sizeof(bindnames[0]))

static int		keys_cursor;
static int		bind_grab;

static void M_Menu_Keys_f (void)
{
	cls.key_dest = key_menu;
	m_state = m_keys;
	m_entersound = true;
}


static void M_FindKeysForCommand (char *command, int *twokeys)
{
	int		count;
	int		j;
	int		l;
	const char	*b;

	twokeys[0] = twokeys[1] = -1;
	l = (int)strlen(command);
	count = 0;

	for (j=0 ; j<256 ; j++)
	{
		b = Key_BindingForKey (j);
		if (!b)
			continue;
		if (!strncmp (b, command, l) )
		{
			twokeys[count] = j;
			count++;
			if (count == 2)
				break;
		}
	}
}

static void M_UnbindCommand (char *command)
{
	int		j;
	int		l;
	const char	*b;

	l = (int)strlen(command);

	for (j=0 ; j<256 ; j++)
	{
		b = Key_BindingForKey (j);
		if (!b)
			continue;
		if (!strncmp (b, command, l) )
			Key_SetBinding (j, "");
	}
}


static void M_Keys_Draw (void)
{
	int		i;
	int		keys[2];
	char	*keyname;
	int		x, y;
	qpic_t	*p;

	p = Draw_CachePic ("gfx/ttl_cstm.lmp");
	M_DrawPic ( (320-p->width)/2, 4, p);

	if (bind_grab)
		M_Print (12, 32, "Press a key or button for this action");
	else
		M_Print (18, 32, "Enter to change, backspace to clear");

// search for known bindings
	for (i=0 ; i<(int)NUMCOMMANDS ; i++)
	{
		y = 48 + 8*i;

		M_Print (16, y, bindnames[i][1]);

		M_FindKeysForCommand (bindnames[i][0], keys);

		if (keys[0] == -1)
		{
			M_Print (140, y, "???");
		}
		else
		{
			keyname = Key_KeynumToString (keys[0]);
			M_Print (140, y, keyname);
			x = (int)(strlen(keyname) * 8);
			if (keys[1] != -1)
			{
				M_Print (140 + x + 8, y, "or");
				M_Print (140 + x + 32, y, Key_KeynumToString (keys[1]));
			}
		}
	}
	
	if (bind_grab)
		M_DrawCharacter (130, 48 + keys_cursor*8, '=');
	else
		M_DrawCharacter (130, 48 + keys_cursor*8, 12+((int)(host.realtime*4)&1));
}


static void M_Keys_Key (int k)
{
	char	cmd[80];
	int		keys[2];
	
	if (bind_grab)
	{	// defining a key
		S_LocalSound ("misc/menu1.wav");
		if (k == K_ESCAPE)
		{
			bind_grab = false;
		}
		else if (k != '`')
		{
			snprintf (cmd, sizeof(cmd), "bind %s \"%s\"\n", Key_KeynumToString (k), bindnames[keys_cursor][0]);
			Cbuf_InsertText (cmd);
		}
		
		bind_grab = false;
		return;
	}
	
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;

	case K_LEFTARROW:
	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		keys_cursor--;
		if (keys_cursor < 0)
			keys_cursor = NUMCOMMANDS-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		keys_cursor++;
		if (keys_cursor >= (int)NUMCOMMANDS)
			keys_cursor = 0;
		break;

	case K_ENTER:		// go into bind mode
		M_FindKeysForCommand (bindnames[keys_cursor][0], keys);
		S_LocalSound ("misc/menu2.wav");
		if (keys[1] != -1)
			M_UnbindCommand (bindnames[keys_cursor][0]);
		bind_grab = true;
		break;

	case K_BACKSPACE:		// delete bindings
	case K_DEL:				// delete bindings
		S_LocalSound ("misc/menu2.wav");
		M_UnbindCommand (bindnames[keys_cursor][0]);
		break;
	}
}

//=============================================================================
/* HELP MENU */

static int		help_page;
#define	NUM_HELP_PAGES	6


static void M_Menu_Help_f (void)
{
	cls.key_dest = key_menu;
	m_state = m_help;
	m_entersound = true;
	help_page = 0;
}



static void M_Help_Draw (void)
{
	M_DrawPic (0, 0, Draw_CachePic ( va("gfx/help%i.lmp", help_page)) );
}


static void M_Help_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;
		
	case K_UPARROW:
	case K_RIGHTARROW:
		m_entersound = true;
		if (++help_page >= NUM_HELP_PAGES)
			help_page = 0;
		break;

	case K_DOWNARROW:
	case K_LEFTARROW:
		m_entersound = true;
		if (--help_page < 0)
			help_page = NUM_HELP_PAGES-1;
		break;
	}

}

//=============================================================================
/* QUIT MENU */

static int		msgNumber;
static int		m_quit_prevstate;
static bool	wasInMenus;

void M_Menu_Quit_f (void)
{
	if (m_state == m_quit)
		return;
	wasInMenus = (cls.key_dest == key_menu);
	cls.key_dest = key_menu;
	m_quit_prevstate = m_state;
	m_state = m_quit;
	m_entersound = true;
	msgNumber = rand()&7;
}


static void M_Quit_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
	case 'n':
	case 'N':
		if (wasInMenus)
		{
			m_state = m_quit_prevstate;
			m_entersound = true;
		}
		else
		{
			cls.key_dest = key_game;
			m_state = m_none;
		}
		break;

	case 'Y':
	case 'y':
		cls.key_dest = key_console;
		CL_Disconnect ();
		Sys_Quit ();
		break;

	default:
		break;
	}

}

static void M_Menu_SinglePlayer_f (void) {
	m_state = m_singleplayer;
}

static void M_SinglePlayer_Draw (void) {
	qpic_t	*p;

	M_DrawTransPic (16, 4, Draw_CachePic ("gfx/qplaque.lmp") );
//	M_DrawTransPic (16, 4, Draw_CachePic ("gfx/qplaque.lmp") );
	p = Draw_CachePic ("gfx/ttl_sgl.lmp");
	M_DrawPic ( (320-p->width)/2, 4, p);
//	M_DrawTransPic (72, 32, Draw_CachePic ("gfx/sp_menu.lmp") );

	M_DrawTextBox (60, 10*8, 23, 4);	
	M_PrintWhite (92, 12*8, "QuakeWorld is for");
	M_PrintWhite (88, 13*8, "Internet play only");

}

static void M_SinglePlayer_Key (int key) {
	if (key == K_ESCAPE || key==K_ENTER)
		m_state = m_main;
}

static void M_Menu_MultiPlayer_f (void) {
	m_state = m_multiplayer;
}

static void M_MultiPlayer_Draw (void) {
	qpic_t	*p;

	M_DrawTransPic (16, 4, Draw_CachePic ("gfx/qplaque.lmp") );
//	M_DrawTransPic (16, 4, Draw_CachePic ("gfx/qplaque.lmp") );
	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic ( (320-p->width)/2, 4, p);
//	M_DrawTransPic (72, 32, Draw_CachePic ("gfx/sp_menu.lmp") );

	M_DrawTextBox (46, 8*8, 27, 9);	
	M_PrintWhite (72, 10*8, "If you want to find QW  ");
	M_PrintWhite (72, 11*8, "games, head on over to: ");
	     M_Print (72, 12*8, "   www.quakeworld.net   ");
	M_PrintWhite (72, 13*8, "          or            ");
	     M_Print (72, 14*8, "   www.quakespy.com     ");
	M_PrintWhite (72, 15*8, "For pointers on getting ");
	M_PrintWhite (72, 16*8, "        started!        ");
}

static void M_MultiPlayer_Key (int key) {
	if (key == K_ESCAPE || key==K_ENTER)
		m_state = m_main;
}

static void M_Quit_Draw (void)
{
#define VSTR(x) #x
#define VSTR2(x) VSTR(x)
	char *cmsg[] = {
//    0123456789012345678901234567890123456789
	"0            QuakeWorld",
	"1    version " VSTR2(VERSION) " by id Software",
	"0Programming",
	"1 John Carmack    Michael Abrash",
	"1 John Cash       Christian Antkow",
	"0Additional Programming",
	"1 Dave 'Zoid' Kirsch",
	"1 Jack 'morbid' Mathews",
	"0Id Software is not responsible for",
    "0providing technical support for",
	"0QUAKEWORLD(tm). (c)1996 Id Software,",
	"0Inc.  All Rights Reserved.",
	"0QUAKEWORLD(tm) is a trademark of Id",
	"0Software, Inc.",
	"1NOTICE: THE COPYRIGHT AND TRADEMARK",
	"1NOTICES APPEARING  IN YOUR COPY OF",
	"1QUAKE(r) ARE NOT MODIFIED BY THE USE",
	"1OF QUAKEWORLD(tm) AND REMAIN IN FULL",
	"1FORCE.",
	"0NIN(r) is a registered trademark",
	"0licensed to Nothing Interactive, Inc.",
	"0All rights reserved. Press y to exit",
	NULL };
	char **p;
	int y;

	if (wasInMenus)
	{
		m_state = m_quit_prevstate;
		m_recursiveDraw = true;
		M_Draw ();
		m_state = m_quit;
	}
	M_DrawTextBox (0, 0, 38, 23);
	y = 12;
	for (p = cmsg; *p; p++, y += 8) {
		if (**p == '0')
			M_PrintWhite (16, y, *p + 1);
		else
			M_Print (16, y,	*p + 1);
	}
}



//=============================================================================
/* Menu Subsystem */


void M_Init (void)
{
	Cmd_AddCommand ("togglemenu", M_ToggleMenu_f);

	Cmd_AddCommand ("menu_main", M_Menu_Main_f);
	Cmd_AddCommand ("menu_options", M_Menu_Options_f);
	Cmd_AddCommand ("menu_keys", M_Menu_Keys_f);
	Cmd_AddCommand ("help", M_Menu_Help_f);
	Cmd_AddCommand ("menu_quit", M_Menu_Quit_f);
}


void M_Draw (void)
{
	if (m_state == m_none || cls.key_dest != key_menu)
		return;

	if (!m_recursiveDraw)
	{
		if (scr.con_current)
		{
			Draw_ConsoleBackground (vid.conheight, cls.download != NULL);
			S_ExtraUpdate ();
		}
		else
			Draw_FadeScreen ();
	}
	else
	{
		m_recursiveDraw = false;
	}

	switch (m_state)
	{
	case m_none:
		break;

	case m_main:
		M_Main_Draw ();
		break;

	case m_singleplayer:
		M_SinglePlayer_Draw ();
		break;

	case m_load:
		break;

	case m_save:
		break;

	case m_multiplayer:
		M_MultiPlayer_Draw ();
		break;

	case m_setup:
		break;

	case m_net:
		break;

	case m_options:
		M_Options_Draw ();
		break;

	case m_keys:
		M_Keys_Draw ();
		break;

	case m_help:
		M_Help_Draw ();
		break;

	case m_quit:
		M_Quit_Draw ();
		break;

	case m_serialconfig:
		break;

	case m_modemconfig:
		break;

	case m_lanconfig:
		break;

	case m_gameoptions:
		break;

	case m_search:
		break;

	case m_slist:
		break;
	}

	if (m_entersound)
	{
		S_LocalSound ("misc/menu2.wav");
		m_entersound = false;
	}

	S_ExtraUpdate ();
}


void M_Keydown (int key)
{
	switch (m_state)
	{
	case m_none:
		return;

	case m_main:
		M_Main_Key (key);
		return;

	case m_singleplayer:
		M_SinglePlayer_Key (key);
		return;

	case m_load:
		return;

	case m_save:
		return;

	case m_multiplayer:
		M_MultiPlayer_Key (key);
		return;

	case m_setup:
		return;

	case m_net:
		return;

	case m_options:
		M_Options_Key (key);
		return;

	case m_keys:
		M_Keys_Key (key);
		return;

	case m_help:
		M_Help_Key (key);
		return;

	case m_quit:
		M_Quit_Key (key);
		return;

	case m_serialconfig:
		return;

	case m_modemconfig:
		return;

	case m_lanconfig:
		return;

	case m_gameoptions:
		return;

	case m_search:
		break;

	case m_slist:
		return;
	}
}


