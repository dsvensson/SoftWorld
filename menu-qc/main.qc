// main.qc -- the entry points the engine calls: the menus drawn, the keys
// given them, Escape's and togglemenu's, and the console's commands

void() m_init =
{
	registercommand ("menu_main");
	registercommand ("menu_multiplayer");
	registercommand ("menu_slist");
	registercommand ("addserver");
	registercommand ("menu_options");
	registercommand ("menu_keys");
	registercommand ("help");
	registercommand ("menu_quit");
	registercommand ("menu_load");
	registercommand ("menu_save");
};

void() m_shutdown =
{
};

void(vector size) m_draw =
{
	if (m_state == M_NONE || getkeydest () != KEY_MENU)
		return;

	if (m_state == M_SLIST)
		SB_Draw (size);
	else
	{
		menu_x = floor ((size_x - 320) / 2);
		Backdrop_Draw (size);
		M_DrawState (m_state);
	}

	if (m_entersound)
	{
		localsound ("misc/menu2.wav");
		m_entersound = FALSE;
	}
};

// a key, or alone a character it typed: id's menus have no text to take
void(float scan, float chr) m_keydown =
{
	if (m_state == M_SLIST)
		SB_KeyDown (scan, chr);
	else if (scan && m_state != M_NONE)
		menus[m_state].key (scan);
};

void(float scan, float chr) m_keyup =
{
	if (m_state == M_SLIST)
		SB_KeyUp (scan);
};

// Escape and togglemenu: the main menu opens, from another menu goes back to
// it, and from it closes
void(float wantmode) m_toggle =
{
	m_entersound = TRUE;
	if (getkeydest () == KEY_MENU && m_state == M_MAIN)
		M_Close ();
	else
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
	case "menu_multiplayer":
	case "menu_slist":
		SB_Open ();
		return TRUE;
	case "addserver":
		if (argv (1) == "")
			print ("Usage: addserver <address>\n");
		else if (!addhostcacheserver (argv (1)))
			print (sprintf ("%s isn't an address\n", argv (1)));
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
	case "menu_load":
		M_Menu_Load_f ();
		return TRUE;
	case "menu_save":
		M_Menu_Save_f ();
		return TRUE;
	}
	return FALSE;
};
