// menu.qc -- id's QuakeWorld menus, as the C menu had them: main, single
// player, options, keys, help, and the quit screen; multiplayer is the server
// browser (sb_*.qc)

// what the menus show
enum { M_NONE, M_MAIN, M_SINGLEPLAYER, M_OPTIONS, M_KEYS, M_HELP, M_QUIT, M_SLIST };

float	m_state;
float	m_entersound;		// played after drawing a frame, so caching won't disrupt the sound

void() SB_Open;				// the server browser (sb_browser.qc)
void(float state) M_DrawState;

// a cursor moved through n items, round from either end
float(float v, float n) Wrap =
{
	return (v + n) % n;
};

// a cvar set as Cvar_SetValue sets one
void(string name, float value) M_SetValue =
{
	cvar_set (name, sprintf ("%f", value));
};

/*
==============================================================================

OPENING AND CLOSING

==============================================================================
*/

// a menu up, the keys the menus'
void(float state) M_Enter =
{
	setkeydest (KEY_MENU);
	m_state = state;
	m_entersound = TRUE;
};

// the menus gone, the keys the game's
void() M_Close =
{
	setkeydest (KEY_GAME);
	m_state = M_NONE;
};

float	help_page;
float	m_quit_prevstate;
float	wasinmenus;

void() M_Menu_Main_f = { M_Enter (M_MAIN); };
void() M_Menu_SinglePlayer_f = { M_Enter (M_SINGLEPLAYER); };
void() M_Menu_Options_f = { M_Enter (M_OPTIONS); };
void() M_Menu_Keys_f = { M_Enter (M_KEYS); };

void() M_Menu_Help_f =
{
	M_Enter (M_HELP);
	help_page = 0;
};

// over the menu it was asked from, which it goes back to
void() M_Menu_Quit_f =
{
	if (m_state == M_QUIT)
		return;
	wasinmenus = getkeydest () == KEY_MENU;
	m_quit_prevstate = m_state;
	M_Enter (M_QUIT);
};

/*
==============================================================================

MAIN MENU

==============================================================================
*/

#define	MAIN_ITEMS	5

float	m_main_cursor;
void()	m_main_open[MAIN_ITEMS] = {M_Menu_SinglePlayer_f, SB_Open, M_Menu_Options_f, M_Menu_Help_f, M_Menu_Quit_f};

void() M_Main_Draw =
{
	M_DrawPic (16, 4, "gfx/qplaque.lmp");
	M_DrawTitle ("gfx/ttl_main.lmp");
	M_DrawPic (72, 32, "gfx/mainmenu.lmp");
	M_DrawPic (54, 32 + m_main_cursor * 20, sprintf ("gfx/menudot%d.lmp", floor (time * 10) % 6 + 1));
};

void(float key) M_Main_Key =
{
	switch (key)
	{
	case K_ESCAPE:
		M_Close ();
		break;
	case K_DOWNARROW:
	case K_UPARROW:
		localsound ("misc/menu1.wav");
		m_main_cursor = Wrap (m_main_cursor + (key == K_DOWNARROW ? 1 : -1), MAIN_ITEMS);
		break;
	case K_ENTER:
		m_entersound = TRUE;
		m_main_open[m_main_cursor] ();
		break;
	}
};

/*
==============================================================================

OPTIONS MENU

==============================================================================
*/

// a row: an action (by its place), a slider over a cvar from its left end's
// value to its right end's, a step right, or a switch
enum { OPT_ACTION, OPT_SLIDER, OPT_SWITCH, OPT_ALWAYSRUN, OPT_INVERTMOUSE };

typedef struct
{
	string	label;
	float	kind;
	string	cvar;
	float	from, to, step;
} option_t;

#define	OPTIONS_ITEMS	16
#define	OPT_USEMOUSE	15			// not there in fullscreen

option_t	options[OPTIONS_ITEMS] = {
	{"Customize controls", OPT_ACTION},
	{"Go to console", OPT_ACTION},
	{"Reset to defaults", OPT_ACTION},
	{"Screen size", OPT_SLIDER, "viewsize", 30, 120, 10},
	{"Brightness", OPT_SLIDER, "gamma", 1, 0.5, -0.05},
	{"Mouse Speed", OPT_SLIDER, "sensitivity", 1, 11, 0.5},
	{"CD Music Volume", OPT_SLIDER, "bgmvolume", 0, 1, 1},
	{"Sound Volume", OPT_SLIDER, "volume", 0, 1, 0.1},
	{"Always Run", OPT_ALWAYSRUN},
	{"Invert Mouse", OPT_INVERTMOUSE},
	{"Lookspring", OPT_SWITCH, "lookspring"},
	{"Lookstrafe", OPT_SWITCH, "lookstrafe"},
	{"Use old status bar", OPT_SWITCH, "cl_sbar"},
	{"HUD on left side", OPT_SWITCH, "cl_hudswap"},
	{"Mouse Look", OPT_SWITCH, "freelook"},
	{"Use Mouse", OPT_SWITCH, "_windowed_mouse"}
};

float	options_cursor;

void(float dir) M_AdjustSliders =
{
	option_t	o = options[options_cursor];
	float		speed = cvar ("cl_forwardspeed") > 200 ? 200 : 400;

	localsound ("misc/menu3.wav");
	switch (o.kind)
	{
	case OPT_SLIDER:
		M_SetValue (o.cvar, bound (min (o.from, o.to), cvar (o.cvar) + dir * o.step, max (o.from, o.to)));
		break;
	case OPT_SWITCH:
		M_SetValue (o.cvar, !cvar (o.cvar));
		break;
	case OPT_ALWAYSRUN:
		M_SetValue ("cl_forwardspeed", speed);
		M_SetValue ("cl_backspeed", speed);
		break;
	case OPT_INVERTMOUSE:
		M_SetValue ("m_pitch", -cvar ("m_pitch"));
		break;
	}
};

void() M_Options_Draw =
{
	float		i, y;
	option_t	o;

	M_DrawPic (16, 4, "gfx/qplaque.lmp");
	M_DrawTitle ("gfx/p_option.lmp");
	for (i = 0 ; i < OPTIONS_ITEMS ; i++)
	{
		if (i == OPT_USEMOUSE && isfullscreen ())
			continue;
		o = options[i];
		y = 32 + i * 8;
		M_Print (16, y, strpad (-22, o.label));
		switch (o.kind)
		{
		case OPT_SLIDER:
			M_DrawSlider (220, y, (cvar (o.cvar) - o.from) / (o.to - o.from));
			break;
		case OPT_SWITCH:
			M_DrawCheckbox (220, y, (int)cvar (o.cvar) != 0);
			break;
		case OPT_ALWAYSRUN:
			M_DrawCheckbox (220, y, cvar ("cl_forwardspeed") > 200);
			break;
		case OPT_INVERTMOUSE:
			M_DrawCheckbox (220, y, cvar ("m_pitch") < 0);
			break;
		}
	}
	M_DrawCharacter (200, 32 + options_cursor * 8, UI_Cursor ());
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
	case K_DOWNARROW:
		localsound ("misc/menu1.wav");
		options_cursor = Wrap (options_cursor + (k == K_DOWNARROW ? 1 : -1), OPTIONS_ITEMS);
		break;
	case K_LEFTARROW:
		M_AdjustSliders (-1);
		break;
	case K_RIGHTARROW:
		M_AdjustSliders (1);
		break;
	}
	if (options_cursor == OPT_USEMOUSE && isfullscreen ())
		options_cursor = k == K_UPARROW ? OPT_USEMOUSE - 1 : 0;
};

/*
==============================================================================

KEYS MENU

==============================================================================
*/

typedef struct
{
	string	command;
	string	name;
} binding_t;

#define	NUMCOMMANDS	18

binding_t	bindings[NUMCOMMANDS] = {
	{"+attack", "attack"},
	{"impulse 10", "change weapon"},
	{"+jump", "jump / swim up"},
	{"+forward", "walk forward"},
	{"+back", "backpedal"},
	{"+left", "turn left"},
	{"+right", "turn right"},
	{"+speed", "run"},
	{"+moveleft", "step left"},
	{"+moveright", "step right"},
	{"+strafe", "sidestep"},
	{"+lookup", "look up"},
	{"+lookdown", "look down"},
	{"centerview", "center view"},
	{"+mlook", "mouse look"},
	{"+klook", "keyboard look"},
	{"+moveup", "swim up"},
	{"+movedown", "swim down"}
};

float	keys_cursor;
float	bind_grab;
float	keys0, keys1;		// M_FindKeysForCommand's

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
	float	i, y;
	string	keyname;

	M_DrawTitle ("gfx/ttl_cstm.lmp");
	if (bind_grab)
		M_Print (12, 32, "Press a key or button for this action");
	else
		M_Print (18, 32, "Enter to change, backspace to clear");

	for (i = 0 ; i < NUMCOMMANDS ; i++)
	{
		y = 48 + 8 * i;
		M_Print (16, y, bindings[i].name);
		M_FindKeysForCommand (bindings[i].command);
		if (keys0 == -1)
		{
			M_Print (140, y, "???");
			continue;
		}
		keyname = keynumtostring (keys0);
		M_Print (140, y, keyname);
		if (keys1 != -1)
		{
			M_Print (140 + strlen (keyname) * 8 + 8, y, "or");
			M_Print (140 + strlen (keyname) * 8 + 32, y, keynumtostring (keys1));
		}
	}
	M_DrawCharacter (130, 48 + keys_cursor * 8, bind_grab ? '=' : UI_Cursor ());
};

void(float k) M_Keys_Key =
{
	string	command = bindings[keys_cursor].command;

	if (bind_grab)
	{	// defining a key
		localsound ("misc/menu1.wav");
		if (k != K_ESCAPE && k != '`')
			setkeybind (k, command);
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
	case K_DOWNARROW:
	case K_RIGHTARROW:
		localsound ("misc/menu1.wav");
		keys_cursor = Wrap (keys_cursor + ((k == K_LEFTARROW || k == K_UPARROW) ? -1 : 1), NUMCOMMANDS);
		break;
	case K_ENTER:		// go into bind mode
		M_FindKeysForCommand (command);
		localsound ("misc/menu2.wav");
		if (keys1 != -1)
			M_UnbindCommand (command);
		bind_grab = TRUE;
		break;
	case K_BACKSPACE:	// delete bindings
	case K_DEL:
		localsound ("misc/menu2.wav");
		M_UnbindCommand (command);
		break;
	}
};

/*
==============================================================================

HELP MENU

==============================================================================
*/

#define	NUM_HELP_PAGES	6

void() M_Help_Draw =
{
	M_DrawPic (0, 0, sprintf ("gfx/help%d.lmp", help_page));
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
	case K_DOWNARROW:
	case K_LEFTARROW:
		m_entersound = TRUE;
		help_page = Wrap (help_page + ((key == K_UPARROW || key == K_RIGHTARROW) ? 1 : -1), NUM_HELP_PAGES);
		break;
	}
};

/*
==============================================================================

SINGLE PLAYER

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

// Escape or Enter: back to the main menu
void(float key) M_SinglePlayer_Key =
{
	if (key == K_ESCAPE || key == K_ENTER)
		m_state = M_MAIN;
};

/*
==============================================================================

QUIT MENU

==============================================================================
*/

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
			M_Close ();
		break;
	case 'y':
		localcmd ("quit force\n");
		break;
	}
};

#define	NUM_CREDITS	22

// each line's first character: 0 white, 1 red
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
		line = substring (credits[i], 1, strlen (credits[i]) - 1);
		if (str2chr (credits[i], 0) == '0')
			M_PrintWhite (16, 12 + i * 8, line);
		else
			M_Print (16, 12 + i * 8, line);
	}
};

/*
==============================================================================

THE MENUS BY STATE

==============================================================================
*/

typedef struct
{
	void()			draw;
	void(float key)	key;
} menu_t;

// the server browser (M_SLIST) draws and takes its keys itself
menu_t	menus[M_SLIST] = {
	{__NULL__, __NULL__},
	{M_Main_Draw, M_Main_Key},
	{M_SinglePlayer_Draw, M_SinglePlayer_Key},
	{M_Options_Draw, M_Options_Key},
	{M_Keys_Draw, M_Keys_Key},
	{M_Help_Draw, M_Help_Key},
	{M_Quit_Draw, M_Quit_Key}
};

void(float state) M_DrawState =
{
	if (state > M_NONE && state < M_SLIST)
		menus[state].draw ();
};
