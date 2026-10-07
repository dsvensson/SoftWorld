// sb_servers.qc -- the server browser's servers: their columns, the status
// bar under them (the players' page's too), and their keys

/*
==============================================================================

THE COLUMNS

==============================================================================
*/

typedef struct
{
	string	name;				// its header
	float	width;				// in characters
	string	cvar;				// that shows it
	string(float i)	text;
	string(float i)	colour;		// ezQuake's &cRGB, where it isn't red when selected
} sbcolumn_t;

#define	SB_COLUMNS	8

// right to left, as they make room, the name's what is left; Ctrl and their
// digit toggles them, Alt and it sorts by them (sb_sortservers' digits)
sbcolumn_t	sb_columns[SB_COLUMNS] = {
	{"name", 60},
	{"address", 21, "sb_showaddress", SB_AddressText},
	{"png", 3, "sb_showping", SB_PingText, SB_PingColour},
	{"gamedir", 8, "sb_showgamedir", SB_GamedirText},
	{"map", 8, "sb_showmap", SB_MapText},
	{"plyrs", 5, "sb_showplayers", SB_PlayersText, SB_PlayersColour},
	{"fl", 2, "sb_showfraglimit", SB_FraglimitText},
	{"tl", 2, "sb_showtimelimit", SB_TimelimitText}
};
float	sb_colpos[SB_COLUMNS];		// a column's first character, -1 for one not shown
float	sb_namewidth;				// the name's, what is left

// each column from the right, the one before it a character apart, those
// with no room dropped; the name has the rest
void(float w) SB_Columns =
{
	float	pos = floor (w / 8), c;

	for (c = SB_COLUMNS - 1 ; c >= 1 ; c--)
	{
		sb_colpos[c] = -1;
		if (!cvar (sb_columns[c].cvar) || pos - sb_columns[c].width - 1 <= 5)
			continue;
		pos -= sb_columns[c].width;
		sb_colpos[c] = pos;
		pos--;
	}
	sb_namewidth = pos;
};

void(float x, float y, float w) SB_Header =
{
	float	c;

	UI_Fill (x, y, w, 8, '10 10 10', 200);
	for (c = SB_COLUMNS - 1 ; c >= 1 ; c--)
		if (sb_colpos[c] >= 0)
			UI_PrintCenter (x + sb_colpos[c] * 8, y, sb_columns[c].width * 8, sb_columns[c].name, TRUE);
	UI_Print (x, y, "name", TRUE);
};

// a server's row: the selected one marked, proxies coloured, the rest striped
void(float x, float y, float w, float i, float selected) SB_Row =
{
	float	c, cx, cw;

	if (selected)
	{
		UI_GrayBox (x, y, w, 8);
		UI_Char (x + 8 * sb_namewidth, y, UI_Cursor ());
	}
	else if (SB_IsQizmo (i))
		UI_Fill (x, y, w, 8, '25 25 75', 255);
	else if (SB_IsQwfwd (i))
		UI_Fill (x, y, w, 8, '25 50 25', 255);
	else if (i & 1)
		UI_Fill (x, y, w, 8, '25 25 25', 125);
	else
		UI_Fill (x, y, w, 8, '50 50 50', 125);

	for (c = SB_COLUMNS - 1 ; c >= 1 ; c--)
	{
		if (sb_colpos[c] < 0)
			continue;
		cx = x + sb_colpos[c] * 8;
		cw = sb_columns[c].width * 8;
		if (sb_columns[c].colour)
			UI_PrintColoured (cx, y, cw, sb_columns[c].colour (i), sb_columns[c].text (i));
		else
			UI_PrintCenter (cx, y, cw, sb_columns[c].text (i), selected);
	}
	UI_Print (x, y, substring (SB_ServerName (i), 0, min (sb_namewidth, 60)), selected);
};

/*
==============================================================================

THE STATUS BAR

==============================================================================
*/

// ezQuake's scan bar: its ends, 16 middles, and the knob at how far it is
void(float x, float y) SB_ProgressBar =
{
	float	sent = gethostcachevalue (SLIST_PINGSSENT), total = gethostcachevalue (SLIST_PINGSTOTAL), i;

	UI_Char (x, y, 0x80);
	for (i = 1 ; i <= 16 ; i++)
		UI_Char (x + i * 8, y, 0x81);
	UI_Char (x + 136, y, 0x82);
	UI_Char (x + 8 + floor ((total ? min (1, sent / total) : 0) * 120), y, 0x83);
};

// the status bar's last line: gamedir, map, players and limits (or the
// server's status), the gaps between them dotted
void(float x, float y, float w, float i) SB_InfoLine =
{
	float	cells = floor (w / 8), k, sx;
	string	gap = strcat (chr2str (160), " "), s = "", v, maxspec;

	if ((v = SB_Get (key_gamedir, i)) != "")
		s = strcat (substring (v, 0, 8), gap);
	if ((v = SB_Get (key_map, i)) != "")
		s = strcat (s, substring (v, 0, 8), gap);
	maxspec = SB_Get (key_maxspectators, i);
	v = SB_Get (key_maxclients, i);
	s = strcat (s, sprintf ("%d/%s", SB_Num (key_numplayers, i), v != "" ? v : "??"),
		sprintf ("-%d/%s", SB_Num (key_numspectators, i), maxspec != "" ? maxspec : "??"));
	if ((v = SB_Get (key_status, i)) != "")
		s = strcat (s, gap, v);
	else
	{
		if ((v = SB_Get (key_deathmatch, i)) != "" && strlen (s) + 7 <= cells)
			s = strcat (s, gap, "dmm", v);
		if (strlen (s) + 8 <= cells)
			s = strcat (s, gap, "fl:", ftos (SB_Num (key_fraglimit, i)));
		if (strlen (s) + 7 <= cells)
			s = strcat (s, gap, "tl:", ftos (SB_Num (key_timelimit, i)));
	}
	s = substring (s, 0, cells);
	sx = x + Trunc ((w - 8 * strlen (s)) / 2);
	UI_Print (sx, y, s, FALSE);
	for (k = 0 ; k < strlen (s) ; k++)
		if (str2chr (s, k) == 160)
			UI_Char (sx + k * 8 + 4, y, 0x85);
};

// the line with where the list is in it (and its scan's progress), the
// server's name, and its info or what is searched for
void(float x, float y, float w, float h, float i, float pos, float total) SB_Status =
{
	float	cells = floor (w / 8), k, n, scanning = gethostcachevalue (SLIST_SCANNING);
	string	count = sprintf ("%d/%d", pos + 1, total), name;

	n = strlen (count);
	for (k = 0 ; k < cells ; k++)
		if ((k < cells - 3 - n || k > cells - 3) && !(scanning && k >= 1 && k <= 18))
			UI_Char (x + k * 8, y + h - 24, k == 0 ? 0x1d : k == cells - 1 ? 0x1f : 0x1e);
	UI_Print (x + w - 8 * (3 + n) + 4, y + h - 24, count, TRUE);
	if (scanning)
		SB_ProgressBar (x + 8, y + h - 24);
	if (i < 0)
		return;

	name = SB_Get (key_name, i);
	if (strlen (name) < 2)
		name = SB_Get (key_address, i);
	UI_PrintCenter (x, y + h - 16, w, substring (name, 0, cells), FALSE);
	if (SB_Searching (&sb_search))
		UI_PrintCenter (x, y + h - 8, w, sprintf ("search for: %-7s", sb_search.text), TRUE);
	else
		SB_InfoLine (x, y + h - 8, w, i);
};

/*
==============================================================================

THE PAGE

==============================================================================
*/

void(float x, float y, float w, float h) SB_Servers_Draw =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT), r;

	SB_Columns (w);
	if (!n)
	{
		if (gethostcachevalue (SLIST_SCANNING) && !gethostcachevalue (SLIST_HOSTCACHETOTALCOUNT))
		{
			UI_PrintCenter (x, y + 8, w, "Updating, please wait", FALSE);
			SB_ProgressBar (x + Trunc ((w - 144) / 2), y + 24);
		}
		else if (sb_overlay != SBO_ADDSERVER)
		{
			UI_PrintCenter (x, y + 8, w, "No servers filtered", FALSE);
			UI_PrintCenter (x, y + 24, w, "Press [space] to refresh the list", TRUE);
			UI_PrintCenter (x, y + 40, w, "Mark some sources on the next tab", FALSE);
			UI_PrintCenter (x, y + 48, w, "or press [Insert] to add a server", FALSE);
			if (sb_search.text != "")
				UI_PrintCenter (x, y + h - 8, w, sprintf ("search for: %-7s", sb_search.text), TRUE);
		}
		return;
	}

	sb_listsize = SB_Rows (h);
	SB_Scroll (&sb_list, n, sb_listsize);
	SB_Header (x, y, w);
	for (r = 0 ; r < sb_listsize && sb_list.disp + r < n ; r++)
		SB_Row (x, y + 8 * (r + 1), w, sb_list.disp + r, sb_list.disp + r == sb_list.pos);
	if (cvar ("sb_status"))
		SB_Status (x, y, w, h, sb_list.pos, sb_list.pos, n);
};

// the keys that aren't typed: Backspace clearing the search, Insert adding a
// server, and on a list the moves; the wheel moves its rows too
float(float k) SB_Servers_Key =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT);

	if (k == K_BACKSPACE)
	{
		sb_search.text = "";
		return TRUE;
	}
	if (k == K_INS)
	{
		SB_AddServer_Open ();
		return TRUE;
	}
	if (!n)
		return FALSE;		// the tabs move on an empty list
	if (k == K_ENTER)
	{
		SB_Info_Open ();
		return TRUE;
	}
	if (k == K_MWHEELUP)
		sb_list.disp = max (0, sb_list.disp - 4);
	else if (k == K_MWHEELDOWN)
		sb_list.disp += 4;
	if (!SB_ListKey (&sb_list, k, n, 4))
		return FALSE;
	SB_Select (sb_list.pos);
	return TRUE;
};

// the typed keys: a search, or with Ctrl or Alt a command
void(float c) SB_Servers_Char =
{
	if (c == ' ' && !sb_alt)
	{
		refreshhostcache (sb_ctrl);
		return;
	}
	if (!sb_ctrl && !sb_alt)
	{
		if (c > ' ' && c <= '}')
			SB_SearchAdd (&sb_search, c);
		return;
	}
	c = Lower (c);
	if (c == 'n')
		SB_AddServer_Open ();
	else if (c >= '1' && c <= '8')
	{
		if (sb_alt)
			SB_ToggleSort ("sb_sortservers", c);
		else if (c != '1')
			cvar_set (sb_columns[c - '1'].cvar, cvar (sb_columns[c - '1'].cvar) ? "0" : "1");
	}
	else if (gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT))
	{
		if (c == 'r')
			refreshhostcacheentry (sb_list.pos);
		else
			SB_ServerCommand (sb_list.pos, c);
	}
};
