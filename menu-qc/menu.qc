// menu.qc -- id's QuakeWorld menus, as the C menu had them: main, options,
// keys, help, single and multiplayer, and the quit screen; and the entry
// points the engine calls

#define	M_NONE			0
#define	M_MAIN			1
#define	M_SINGLEPLAYER	2
#define	M_MULTIPLAYER	3
#define	M_OPTIONS		4
#define	M_KEYS			5
#define	M_HELP			6
#define	M_QUIT			7

float	m_state;
float	m_entersound;		// played after drawing a frame, so caching won't disrupt the sound

void() M_Menu_Main_f;
void() M_Menu_Options_f;
void() M_Menu_Keys_f;
void() M_Menu_Help_f;
void() M_Menu_Quit_f;
void(float state) M_DrawState;

/*
==============================================================================

MAIN MENU

==============================================================================
*/

#define	MAIN_ITEMS	5

float	m_main_cursor;
string	menudots[6] = {"gfx/menudot1.lmp", "gfx/menudot2.lmp", "gfx/menudot3.lmp", "gfx/menudot4.lmp",
	"gfx/menudot5.lmp", "gfx/menudot6.lmp"};

void() M_Menu_Main_f =
{
	setkeydest (KEY_MENU);
	m_state = M_MAIN;
	m_entersound = TRUE;
};

void() M_Main_Draw =
{
	M_DrawPic (16, 4, "gfx/qplaque.lmp");
	M_DrawTitle ("gfx/ttl_main.lmp");
	M_DrawPic (72, 32, "gfx/mainmenu.lmp");
	M_DrawPic (54, 32 + m_main_cursor * 20, menudots[floor (time * 10) % 6]);
};

void(float key) M_Main_Key =
{
	switch (key)
	{
	case K_ESCAPE:
		setkeydest (KEY_GAME);
		m_state = M_NONE;
		break;

	case K_DOWNARROW:
		localsound ("misc/menu1.wav");
		if (++m_main_cursor >= MAIN_ITEMS)
			m_main_cursor = 0;
		break;

	case K_UPARROW:
		localsound ("misc/menu1.wav");
		if (--m_main_cursor < 0)
			m_main_cursor = MAIN_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = TRUE;
		switch (m_main_cursor)
		{
		case 0:
			m_state = M_SINGLEPLAYER;
			break;
		case 1:
			m_state = M_MULTIPLAYER;
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
		break;
	}
};

/*
==============================================================================

OPTIONS MENU

==============================================================================
*/

#define	OPTIONS_ITEMS	16

float	options_cursor;

void() M_Menu_Options_f =
{
	setkeydest (KEY_MENU);
	m_state = M_OPTIONS;
	m_entersound = TRUE;
};

// a cvar set as Cvar_SetValue sets one
void(string name, float value) M_SetValue =
{
	cvar_set (name, sprintf ("%f", value));
};

void(float dir) M_AdjustSliders =
{
	float	v;

	localsound ("misc/menu3.wav");

	switch (options_cursor)
	{
	case 3:	// screen size
		v = cvar ("viewsize") + dir * 10;
		if (v < 30)
			v = 30;
		if (v > 120)
			v = 120;
		M_SetValue ("viewsize", v);
		break;
	case 4:	// gamma
		v = cvar ("gamma") - dir * 0.05;
		if (v < 0.5)
			v = 0.5;
		if (v > 1)
			v = 1;
		M_SetValue ("gamma", v);
		break;
	case 5:	// mouse speed
		v = cvar ("sensitivity") + dir * 0.5;
		if (v < 1)
			v = 1;
		if (v > 11)
			v = 11;
		M_SetValue ("sensitivity", v);
		break;
	case 6:	// music volume
		v = cvar ("bgmvolume") + dir * 1.0;
		if (v < 0)
			v = 0;
		if (v > 1)
			v = 1;
		M_SetValue ("bgmvolume", v);
		break;
	case 7:	// sfx volume
		v = cvar ("volume") + dir * 0.1;
		if (v < 0)
			v = 0;
		if (v > 1)
			v = 1;
		M_SetValue ("volume", v);
		break;

	case 8:	// always run
		if (cvar ("cl_forwardspeed") > 200)
		{
			M_SetValue ("cl_forwardspeed", 200);
			M_SetValue ("cl_backspeed", 200);
		}
		else
		{
			M_SetValue ("cl_forwardspeed", 400);
			M_SetValue ("cl_backspeed", 400);
		}
		break;

	case 9:	// invert mouse
		M_SetValue ("m_pitch", -cvar ("m_pitch"));
		break;

	case 10:	// lookspring
		M_SetValue ("lookspring", !cvar ("lookspring"));
		break;

	case 11:	// lookstrafe
		M_SetValue ("lookstrafe", !cvar ("lookstrafe"));
		break;

	case 12:
		M_SetValue ("cl_sbar", !cvar ("cl_sbar"));
		break;

	case 13:
		M_SetValue ("cl_hudswap", !cvar ("cl_hudswap"));
		break;

	case 14:	// mouse look
		M_SetValue ("freelook", !cvar ("freelook"));
		break;

	case 15:	// _windowed_mouse
		M_SetValue ("_windowed_mouse", !cvar ("_windowed_mouse"));
		break;
	}
};

// a checkbox's cvar as an int, as the C menu read it
float(string name) M_Checked =
{
	return (int)cvar (name) != 0;
};

void() M_Options_Draw =
{
	M_DrawPic (16, 4, "gfx/qplaque.lmp");
	M_DrawTitle ("gfx/p_option.lmp");

	M_Print (16, 32, "    Customize controls");
	M_Print (16, 40, "         Go to console");
	M_Print (16, 48, "     Reset to defaults");

	M_Print (16, 56, "           Screen size");
	M_DrawSlider (220, 56, (cvar ("viewsize") - 30) / (120 - 30));

	M_Print (16, 64, "            Brightness");
	M_DrawSlider (220, 64, (1.0 - cvar ("gamma")) / 0.5);

	M_Print (16, 72, "           Mouse Speed");
	M_DrawSlider (220, 72, (cvar ("sensitivity") - 1) / 10);

	M_Print (16, 80, "       CD Music Volume");
	M_DrawSlider (220, 80, cvar ("bgmvolume"));

	M_Print (16, 88, "          Sound Volume");
	M_DrawSlider (220, 88, cvar ("volume"));

	M_Print (16, 96,  "            Always Run");
	M_DrawCheckbox (220, 96, cvar ("cl_forwardspeed") > 200);

	M_Print (16, 104, "          Invert Mouse");
	M_DrawCheckbox (220, 104, cvar ("m_pitch") < 0);

	M_Print (16, 112, "            Lookspring");
	M_DrawCheckbox (220, 112, M_Checked ("lookspring"));

	M_Print (16, 120, "            Lookstrafe");
	M_DrawCheckbox (220, 120, M_Checked ("lookstrafe"));

	M_Print (16, 128, "    Use old status bar");
	M_DrawCheckbox (220, 128, M_Checked ("cl_sbar"));

	M_Print (16, 136, "      HUD on left side");
	M_DrawCheckbox (220, 136, M_Checked ("cl_hudswap"));

	M_Print (16, 144, "            Mouse Look");
	M_DrawCheckbox (220, 144, M_Checked ("freelook"));

	if (!isfullscreen ())
	{
		M_Print (16, 152, "             Use Mouse");
		M_DrawCheckbox (220, 152, M_Checked ("_windowed_mouse"));
	}

// cursor
	M_DrawCharacter (200, 32 + options_cursor * 8, M_Cursor ());
};

void(float k) M_Options_Key =
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_ENTER:
		m_entersound = TRUE;
		switch (options_cursor)
		{
		case 0:
			M_Menu_Keys_f ();
			break;
		case 1:
			m_state = M_NONE;
			localcmd ("toggleconsole\n");
			break;
		case 2:
			localcmd ("exec default.cfg\n");
			break;
		default:
			M_AdjustSliders (1);
			break;
		}
		return;

	case K_UPARROW:
		localsound ("misc/menu1.wav");
		options_cursor--;
		if (options_cursor < 0)
			options_cursor = OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		localsound ("misc/menu1.wav");
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

	if (options_cursor == 15 && isfullscreen ())
	{
		if (k == K_UPARROW)
			options_cursor = 14;
		else
			options_cursor = 0;
	}
};

/*
==============================================================================

KEYS MENU

==============================================================================
*/

#define	NUMCOMMANDS	18

string	bindcommands[NUMCOMMANDS] = {"+attack", "impulse 10", "+jump", "+forward", "+back", "+left", "+right",
	"+speed", "+moveleft", "+moveright", "+strafe", "+lookup", "+lookdown", "centerview", "+mlook", "+klook",
	"+moveup", "+movedown"};
string	bindnames[NUMCOMMANDS] = {"attack", "change weapon", "jump / swim up", "walk forward", "backpedal",
	"turn left", "turn right", "run", "step left", "step right", "sidestep", "look up", "look down",
	"center view", "mouse look", "keyboard look", "swim up", "swim down"};

float	keys_cursor;
float	bind_grab;
float	keys0, keys1;		// M_FindKeysForCommand's

void() M_Menu_Keys_f =
{
	setkeydest (KEY_MENU);
	m_state = M_KEYS;
	m_entersound = TRUE;
};

// the first two keys bound to command (-1 for none)
void(string command) M_FindKeysForCommand =
{
	tokenize (findkeysforcommand (command));
	keys0 = stof (argv (0));
	keys1 = stof (argv (1));
};

// every key bound to command unbound
void(string command) M_UnbindCommand =
{
	float	key, l = strlen (command);

	for (key = 0 ; key < 1024 ; key++)
		if (substring (getkeybind (key), 0, l) == command)
			setkeybind (key, "");
};

void() M_Keys_Draw =
{
	float	i, x, y;
	string	keyname;

	M_DrawTitle ("gfx/ttl_cstm.lmp");

	if (bind_grab)
		M_Print (12, 32, "Press a key or button for this action");
	else
		M_Print (18, 32, "Enter to change, backspace to clear");

// search for known bindings
	for (i = 0 ; i < NUMCOMMANDS ; i++)
	{
		y = 48 + 8 * i;

		M_Print (16, y, bindnames[i]);

		M_FindKeysForCommand (bindcommands[i]);

		if (keys0 == -1)
		{
			M_Print (140, y, "???");
		}
		else
		{
			keyname = keynumtostring (keys0);
			M_Print (140, y, keyname);
			x = strlen (keyname) * 8;
			if (keys1 != -1)
			{
				M_Print (140 + x + 8, y, "or");
				M_Print (140 + x + 32, y, keynumtostring (keys1));
			}
		}
	}

	if (bind_grab)
		M_DrawCharacter (130, 48 + keys_cursor * 8, '=');
	else
		M_DrawCharacter (130, 48 + keys_cursor * 8, M_Cursor ());
};

void(float k) M_Keys_Key =
{
	if (bind_grab)
	{	// defining a key
		localsound ("misc/menu1.wav");
		if (k != K_ESCAPE && k != '`')
			setkeybind (k, bindcommands[keys_cursor]);
		bind_grab = FALSE;
		return;
	}

	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;

	case K_LEFTARROW:
	case K_UPARROW:
		localsound ("misc/menu1.wav");
		keys_cursor--;
		if (keys_cursor < 0)
			keys_cursor = NUMCOMMANDS - 1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		localsound ("misc/menu1.wav");
		keys_cursor++;
		if (keys_cursor >= NUMCOMMANDS)
			keys_cursor = 0;
		break;

	case K_ENTER:		// go into bind mode
		M_FindKeysForCommand (bindcommands[keys_cursor]);
		localsound ("misc/menu2.wav");
		if (keys1 != -1)
			M_UnbindCommand (bindcommands[keys_cursor]);
		bind_grab = TRUE;
		break;

	case K_BACKSPACE:	// delete bindings
	case K_DEL:
		localsound ("misc/menu2.wav");
		M_UnbindCommand (bindcommands[keys_cursor]);
		break;
	}
};

/*
==============================================================================

HELP MENU

==============================================================================
*/

#define	NUM_HELP_PAGES	6

float	help_page;
string	helppages[NUM_HELP_PAGES] = {"gfx/help0.lmp", "gfx/help1.lmp", "gfx/help2.lmp", "gfx/help3.lmp",
	"gfx/help4.lmp", "gfx/help5.lmp"};

void() M_Menu_Help_f =
{
	setkeydest (KEY_MENU);
	m_state = M_HELP;
	m_entersound = TRUE;
	help_page = 0;
};

void() M_Help_Draw =
{
	M_DrawPic (0, 0, helppages[help_page]);
};

void(float key) M_Help_Key =
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_UPARROW:
	case K_RIGHTARROW:
		m_entersound = TRUE;
		if (++help_page >= NUM_HELP_PAGES)
			help_page = 0;
		break;

	case K_DOWNARROW:
	case K_LEFTARROW:
		m_entersound = TRUE;
		if (--help_page < 0)
			help_page = NUM_HELP_PAGES - 1;
		break;
	}
};

/*
==============================================================================

SINGLE AND MULTIPLAYER

What QuakeWorld has instead

==============================================================================
*/

void() M_SinglePlayer_Draw =
{
	M_DrawPic (16, 4, "gfx/qplaque.lmp");
	M_DrawTitle ("gfx/ttl_sgl.lmp");

	M_DrawTextBox (60, 10 * 8, 23, 4);
	M_PrintWhite (92, 12 * 8, "QuakeWorld is for");
	M_PrintWhite (88, 13 * 8, "Internet play only");
};

void() M_MultiPlayer_Draw =
{
	M_DrawPic (16, 4, "gfx/qplaque.lmp");
	M_DrawTitle ("gfx/p_multi.lmp");

	M_DrawTextBox (46, 8 * 8, 27, 9);
	M_PrintWhite (72, 10 * 8, "If you want to find QW  ");
	M_PrintWhite (72, 11 * 8, "games, head on over to: ");
	     M_Print (72, 12 * 8, "   www.quakeworld.net   ");
	M_PrintWhite (72, 13 * 8, "          or            ");
	     M_Print (72, 14 * 8, "   www.quakespy.com     ");
	M_PrintWhite (72, 15 * 8, "For pointers on getting ");
	M_PrintWhite (72, 16 * 8, "        started!        ");
};

// Escape or Enter: back to the main menu
void(float key) M_TextScreen_Key =
{
	if (key == K_ESCAPE || key == K_ENTER)
		m_state = M_MAIN;
};

/*
==============================================================================

QUIT MENU

==============================================================================
*/

float	m_quit_prevstate;
float	wasinmenus;

void() M_Menu_Quit_f =
{
	if (m_state == M_QUIT)
		return;
	wasinmenus = getkeydest () == KEY_MENU;
	setkeydest (KEY_MENU);
	m_quit_prevstate = m_state;
	m_state = M_QUIT;
	m_entersound = TRUE;
};

void(float key) M_Quit_Key =
{
	switch (key)
	{
	case K_ESCAPE:
	case 'n':
		if (wasinmenus)
		{
			m_state = m_quit_prevstate;
			m_entersound = TRUE;
		}
		else
		{
			setkeydest (KEY_GAME);
			m_state = M_NONE;
		}
		break;

	case 'y':
		localcmd ("quit force\n");
		break;
	}
};

#define	NUM_CREDITS	22

string	credits[NUM_CREDITS] = {
//   0123456789012345678901234567890123456789
	"0          SoftWorld " SW_VERSION,
	"1    from QuakeWorld by id Software",
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
	"0All rights reserved. Press y to exit"};

void() M_Quit_Draw =
{
	float	i;
	string	line;

	// the menu it was asked from under it
	if (wasinmenus)
		M_DrawState (m_quit_prevstate);
	M_DrawTextBox (0, 0, 38, 23);
	for (i = 0 ; i < NUM_CREDITS ; i++)
	{
		line = credits[i];
		if (str2chr (line, 0) == '0')
			M_PrintWhite (16, 12 + i * 8, substring (line, 1, strlen (line) - 1));
		else
			M_Print (16, 12 + i * 8, substring (line, 1, strlen (line) - 1));
	}
};

/*
==============================================================================

THE ENTRY POINTS

==============================================================================
*/

void(float state) M_DrawState =
{
	switch (state)
	{
	case M_MAIN:
		M_Main_Draw ();
		break;
	case M_SINGLEPLAYER:
		M_SinglePlayer_Draw ();
		break;
	case M_MULTIPLAYER:
		M_MultiPlayer_Draw ();
		break;
	case M_OPTIONS:
		M_Options_Draw ();
		break;
	case M_KEYS:
		M_Keys_Draw ();
		break;
	case M_HELP:
		M_Help_Draw ();
		break;
	case M_QUIT:
		M_Quit_Draw ();
		break;
	}
};

void() m_init =
{
	registercommand ("menu_main");
	registercommand ("menu_options");
	registercommand ("menu_keys");
	registercommand ("help");
	registercommand ("menu_quit");
};

void() m_shutdown =
{
};

void(vector size) m_draw =
{
	if (m_state == M_NONE || getkeydest () != KEY_MENU)
		return;

	menu_x = floor ((size_x - 320) / 2);
	Backdrop_Draw (size);
	M_DrawState (m_state);

	if (m_entersound)
	{
		localsound ("misc/menu2.wav");
		m_entersound = FALSE;
	}
};

void(float scan, float chr) m_keydown =
{
	switch (m_state)
	{
	case M_MAIN:
		M_Main_Key (scan);
		break;
	case M_SINGLEPLAYER:
	case M_MULTIPLAYER:
		M_TextScreen_Key (scan);
		break;
	case M_OPTIONS:
		M_Options_Key (scan);
		break;
	case M_KEYS:
		M_Keys_Key (scan);
		break;
	case M_HELP:
		M_Help_Key (scan);
		break;
	case M_QUIT:
		M_Quit_Key (scan);
		break;
	}
};

void(float scan, float chr) m_keyup =
{
};

// Escape and togglemenu: the main menu opens, from another menu goes back to
// it, and from it closes
void(float wantmode) m_toggle =
{
	m_entersound = TRUE;
	if (getkeydest () == KEY_MENU)
	{
		if (m_state != M_MAIN)
		{
			M_Menu_Main_f ();
			return;
		}
		setkeydest (KEY_GAME);
		m_state = M_NONE;
		return;
	}
	M_Menu_Main_f ();
};

float(string cmd) m_consolecommand =
{
	tokenize (cmd);
	switch (argv (0))
	{
	case "menu_main":
		M_Menu_Main_f ();
		return TRUE;
	case "menu_options":
		M_Menu_Options_f ();
		return TRUE;
	case "menu_keys":
		M_Menu_Keys_f ();
		return TRUE;
	case "help":
		M_Menu_Help_f ();
		return TRUE;
	case "menu_quit":
		M_Menu_Quit_f ();
		return TRUE;
	}
	return FALSE;
};
