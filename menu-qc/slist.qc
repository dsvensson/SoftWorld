// slist.qc -- the server browser, ezQuake's (EX_browser.c): its Servers,
// Sources, Players and Options pages, the info popup and the dialogs, over
// the server list the engine keeps (FTE's hostcache builtins, and SoftWorld's).
// The list fills as the scan finds it, its progress in the status bar, rather
// than behind ezQuake's box; the selection stays on its server as the list
// changes under it. ezQuake's Create tab, QuakeTV observing, the connection
// test, sb_info_filter and the mouse aren't here.

// FTE's hostcache values (gethostcachevalue), and SoftWorld's
#define	SLIST_HOSTCACHEVIEWCOUNT	0
#define	SLIST_HOSTCACHETOTALCOUNT	1
#define	SLIST_SCANNING				100
#define	SLIST_PINGSSENT				103
#define	SLIST_PINGSTOTAL			104
#define	SLIST_GENERATION			108
#define	SLIST_SOURCES				109
#define	SLIST_MARKEDSERVERS			113
#define	SLIST_SCANS					114

// FTE's mask tests (sethostcachemask*), and the OR of mask 512
#define	SLIST_TEST_CONTAINS			0
#define	SLIST_TEST_LESSEQUAL		2
#define	SLIST_TEST_EQUAL			4
#define	SLIST_TEST_GREATER			5
#define	SLIST_TEST_NOTEQUAL			7
#define	SLIST_MASK_AND				0
#define	SLIST_MASK_OR				512

// sethostcachesort's flags: FTE's, and SoftWorld's key after the others
#define	SLIST_SORT_DESCENDING		1
#define	SLIST_SORT_APPEND			8

#define	SL_DEAD		2		// a server's state (SoftWorld's key "state"): never answered

// the pages
#define	SB_SERVERS	0
#define	SB_SOURCES	1
#define	SB_PLAYERS	2
#define	SB_OPTIONS	3
#define	SB_TABS		4

// the dialogs
#define	SBD_NONE		0
#define	SBD_ADDSERVER	1
#define	SBD_ADDSOURCE	2
#define	SBD_CONFIRM		3

#define	SB_SEARCHTIME	3		// seconds the search shows after its last key
#define	SB_MAXPLAYERS	1024	// on the players' page
#define	SB_MAXROSTER	64		// a server's, in the info popup

string	sb_tabnames[SB_TABS] = {"Servers", "Sources", "Players", "Options"};

float	sb_tab;
float	sb_scanned;					// the browser asked for its first scan
float	sb_ctrl, sb_alt, sb_shift;	// held
float	sb_vw, sb_vh;				// the layout's size
float	sb_dialog;
float	sb_info;					// the info popup is up

// the servers' page
float	sb_pos, sb_disp, sb_listsize;
var string	sb_seladdr = "";			// the selected server's address, which it follows
var string	sb_search = "";				// the servers whose names have it
var float	sb_searchtime = -100;
var string	sb_settings = "";			// the filters and sort the view was arranged with
float	sb_serial;					// one more each arrange
var string	sb_pending = "";			// a server added, selected once it is listed
float	sb_pendingtime;

/*
==============================================================================

DRAWING, AS EZQUAKE'S UI DOES

==============================================================================
*/

// C's division toward zero, as ezQuake centers
float(float v) SB_Trunc =
{
	return v < 0 ? -floor (-v) : floor (v);
};

void(float x, float y, float c) SB_Char =
{
	drawcharacter ([x, y, 0], c, '8 8 0', '1 1 1', 1, 0);
};

// ezQuake's UI_Print: red as the high half of the characters, else as they are
void(float x, float y, string s, float red) SB_Print =
{
	float	i, n;

	if (!red)
	{
		drawrawstring ([x, y, 0], s, '8 8 0', '1 1 1', 1, 0);
		return;
	}
	n = strlen (s);
	for (i = 0 ; i < n ; i++)
		SB_Char (x + i * 8, y, str2chr (s, i) | 128);
};

void(float x, float y, float w, string s, float red) SB_PrintCenter =
{
	SB_Print (x + SB_Trunc ((w - 8 * strlen (s)) / 2), y, s, red);
};

// text in ezQuake's &cRGB colour, centered by what it shows
void(float x, float y, float w, string colour, string s) SB_PrintColoured =
{
	drawstring ([x + SB_Trunc ((w - 8 * strlen (s)) / 2), y, 0], strcat ("&c", colour, s), '8 8 0', '1 1 1', 1, 0);
};

void(float x, float y, float w, float h, float r, float g, float b, float a) SB_Fill =
{
	drawfill ([x, y, 0], [w, h, 0], [r, g, b] * (1 / 255), a / 255, 0);
};

// the marked row: menu_marked_bgcolor's 20 20 20 128, pulsing at menu_marked_fade's 4
void(float x, float y, float w, float h) SB_GrayBox =
{
	float	f = 0.5 + 0.25 * (1 + sin (4 * time));

	SB_Fill (x, y, w, h, f * 20, f * 20, f * 20, 128);
};

// a line of n characters: its ends and its middles
void(float x, float y, float n) SB_Line =
{
	float	i;

	for (i = 0 ; i < n ; i++)
		SB_Char (x + i * 8, y, i == 0 ? 0x1d : i == n - 1 ? 0x1f : 0x1e);
};

void(float x, float y, string pic) SB_Pic =
{
	drawpic ([x, y, 0], pic, drawgetimagesize (pic), '1 1 1', 1, 0);
};

// Draw_TextBox's box around width (in pairs of characters) by lines characters
void(float x, float y, float width, float lines) SB_TextBox =
{
	float	cx, cy, n;

	cy = y;
	SB_Pic (x, cy, "gfx/box_tl.lmp");
	for (n = 0 ; n < lines ; n++)
		SB_Pic (x, cy += 8, "gfx/box_ml.lmp");
	SB_Pic (x, cy + 8, "gfx/box_bl.lmp");
	for (cx = x + 8 ; width > 0 ; width -= 2, cx += 16)
	{
		cy = y;
		SB_Pic (cx, cy, "gfx/box_tm.lmp");
		for (n = 0 ; n < lines ; n++)
			SB_Pic (cx, cy += 8, n >= 1 ? "gfx/box_mm2.lmp" : "gfx/box_mm.lmp");
		SB_Pic (cx, cy + 8, "gfx/box_bm.lmp");
	}
	cy = y;
	SB_Pic (cx, cy, "gfx/box_tr.lmp");
	for (n = 0 ; n < lines ; n++)
		SB_Pic (cx, cy += 8, "gfx/box_mr.lmp");
	SB_Pic (cx, cy + 8, "gfx/box_br.lmp");
};

// s in n characters, cut or with spaces after (or before, right)
string(string s, float n) SB_Pad =
{
	return strpad (n, substring (s, 0, n));
};

string(string s, float n) SB_PadRight =
{
	return strpad (-n, substring (s, 0, n));
};

float(float c) SB_Lower =
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
};

// a box's place: its size centered on the layout, on the characters' grid
float(float size, float room) SB_Centered =
{
	return floor (floor ((room - size) / 2) / 8) * 8;
};

// ezQuake's scan bar: its ends, 16 middles, and the knob at how far it is
void(float x, float y) SB_ProgressBar =
{
	float	sent = gethostcachevalue (SLIST_PINGSSENT), total = gethostcachevalue (SLIST_PINGSTOTAL), i;

	SB_Char (x, y, 0x80);
	for (i = 1 ; i <= 16 ; i++)
		SB_Char (x + i * 8, y, 0x81);
	SB_Char (x + 136, y, 0x82);
	SB_Char (x + 8 + floor ((total ? min (1, sent / total) : 0) * 120), y, 0x83);
};

/*
==============================================================================

THE SERVERS

==============================================================================
*/

// the keys, by FTE's names, and the serverinfo's
float	key_ping, key_map, key_name, key_address, key_numplayers, key_numspectators, key_gamedir, key_timelimit,
	key_fraglimit, key_isproxy, key_state, key_freeplayers, key_player0, key_serverinfo, key_version, key_progs,
	key_maxclients, key_maxspectators, key_deathmatch, key_status, key_teamplay, key_qtv;

void() SB_Keys =
{
	if (key_player0)
		return;
	key_ping = gethostcacheindexforkey ("ping");
	key_map = gethostcacheindexforkey ("map");
	key_name = gethostcacheindexforkey ("name");
	key_address = gethostcacheindexforkey ("cname");
	key_numplayers = gethostcacheindexforkey ("numplayers");
	key_numspectators = gethostcacheindexforkey ("numspectators");
	key_gamedir = gethostcacheindexforkey ("gamedir");
	key_timelimit = gethostcacheindexforkey ("timelimit");
	key_fraglimit = gethostcacheindexforkey ("fraglimit");
	key_isproxy = gethostcacheindexforkey ("isproxy");
	key_state = gethostcacheindexforkey ("state");
	key_freeplayers = gethostcacheindexforkey ("freeplayers");
	key_serverinfo = gethostcacheindexforkey ("serverinfo");
	key_player0 = gethostcacheindexforkey ("player0");
	key_version = gethostcacheindexforkey ("*version");
	key_progs = gethostcacheindexforkey ("*progs");
	key_maxclients = gethostcacheindexforkey ("maxclients");
	key_maxspectators = gethostcacheindexforkey ("maxspectators");
	key_deathmatch = gethostcacheindexforkey ("deathmatch");
	key_status = gethostcacheindexforkey ("status");
	key_teamplay = gethostcacheindexforkey ("teamplay");
	key_qtv = gethostcacheindexforkey ("qtv");
};

string(float key, float i) SB_Get =
{
	return gethostcachestring (key, i);
};

float(float key, float i) SB_Num =
{
	return gethostcachenumber (key, i);
};

float(float i) SB_IsQwfwd =
{
	return !strncasecmp (SB_Get (key_version, i), "qwfwd", 5);
};

float(float i) SB_IsQizmo =
{
	return !strncasecmp (SB_Get (key_version, i), "qizmo", 5);
};

// the name a row shows: a qwfwd's as a proxy's, the address where it has none
string(float i) SB_ServerName =
{
	string	name = SB_Get (key_name, i);

	if (name == "")
		name = SB_Get (key_address, i);
	return SB_IsQwfwd (i) ? strcat ("proxy ", name) : name;
};

string(float i) SB_PingText =
{
	float	ping = SB_Num (key_ping, i);

	return ping >= 0xffff ? "n/a" : sprintf ("%3d", min (ping, 999));
};

// ezQuake's ping colours by the frames of a 77 fps client the ping is
string(float i) SB_PingColour =
{
	float	ping = SB_Num (key_ping, i), frames = ping >= 0xffff ? 0 : floor (ping / (1000 / 77));

	switch (frames)
	{
	case 0:		return "1f0";
	case 1:		return "3d0";
	case 2:		return "790";
	case 3:		return "880";
	case 4:		return "a60";
	}
	return frames < 10 ? "d30" : "f00";
};

// maxclients as ezQuake shows it: 99 where it is missing or too long
string(float i) SB_MaxClients =
{
	string	max = SB_Get (key_maxclients, i);

	return (max == "" || strlen (max) > 2) ? "99" : max;
};

string(float i) SB_PlayersText =
{
	return sprintf ("%2d/%-2s", min (SB_Num (key_numplayers, i), 99), SB_MaxClients (i));
};

// empty blue, playing green, full red; the spectators not counted
string(float i) SB_PlayersColour =
{
	float	players = min (SB_Num (key_numplayers, i), 99);

	if (players < 1)
		return "00f";
	return players < stof (SB_MaxClients (i)) ? "0f0" : "f00";
};

string(float i) SB_Gamedir =
{
	string	gamedir = SB_Get (key_gamedir, i);

	if (gamedir == "" && SB_Get (key_progs, i) == "666")
		return "qizmo";
	return gamedir;
};

// a limit in its two characters
string(float v) SB_Limit =
{
	string	s = ftos (v);

	return SB_PadRight (strlen (s) > 2 ? "99" : s, 2);
};

// the columns, right to left: their headers, widths and cvars
string	sb_colname[8] = {"name", "address", "png", "gamedir", "map", "plyrs", "fl", "tl"};
float	sb_colwidth[8] = {60, 21, 3, 8, 8, 5, 2, 2};
string	sb_colvar[8] = {"", "sb_showaddress", "sb_showping", "sb_showgamedir", "sb_showmap", "sb_showplayers",
	"sb_showfraglimit", "sb_showtimelimit"};
float	sb_colpos[8];		// a column's first character, -1 for one not shown
float	sb_namewidth;		// the name's, what is left

// each column from the right, the one before it a character apart, those
// with no room dropped; the name has the rest
void(float w) SB_Columns =
{
	float	pos = floor (w / 8), i;

	for (i = 7 ; i >= 1 ; i--)
	{
		sb_colpos[i] = -1;
		if (!cvar (sb_colvar[i]) || pos - sb_colwidth[i] - 1 <= 5)
			continue;
		pos -= sb_colwidth[i];
		sb_colpos[i] = pos;
		pos--;
	}
	sb_namewidth = pos;
};

void(float x, float y, float w) SB_Header =
{
	float	i;

	SB_Fill (x, y, w, 8, 10, 10, 10, 200);
	for (i = 7 ; i >= 1 ; i--)
		if (sb_colpos[i] >= 0)
			SB_PrintCenter (x + sb_colpos[i] * 8, y, sb_colwidth[i] * 8, sb_colname[i], TRUE);
	SB_Print (x, y, "name", TRUE);
};

void(float x, float y, float w, float i, float selected) SB_Row =
{
	if (selected)
	{
		SB_GrayBox (x, y, w, 8);
		SB_Char (x + 8 * sb_namewidth, y, M_Cursor ());
	}
	else if (SB_IsQizmo (i))
		SB_Fill (x, y, w, 8, 25, 25, 75, 255);
	else if (SB_IsQwfwd (i))
		SB_Fill (x, y, w, 8, 25, 50, 25, 255);
	else if (i & 1)
		SB_Fill (x, y, w, 8, 25, 25, 25, 125);
	else
		SB_Fill (x, y, w, 8, 50, 50, 50, 125);

	if (sb_colpos[7] >= 0)
		SB_PrintCenter (x + sb_colpos[7] * 8, y, 16, SB_Limit (SB_Num (key_timelimit, i)), selected);
	if (sb_colpos[6] >= 0)
		SB_PrintCenter (x + sb_colpos[6] * 8, y, 16, SB_Limit (SB_Num (key_fraglimit, i)), selected);
	if (sb_colpos[5] >= 0)
		SB_PrintColoured (x + sb_colpos[5] * 8, y, 40, SB_PlayersColour (i), SB_PlayersText (i));
	if (sb_colpos[4] >= 0)
		SB_PrintCenter (x + sb_colpos[4] * 8, y, 64, substring (SB_Get (key_map, i), 0, 8), selected);
	if (sb_colpos[3] >= 0)
		SB_PrintCenter (x + sb_colpos[3] * 8, y, 64, substring (SB_Gamedir (i), 0, 8), selected);
	if (sb_colpos[2] >= 0)
		SB_PrintColoured (x + sb_colpos[2] * 8, y, 24, SB_PingColour (i), SB_PingText (i));
	if (sb_colpos[1] >= 0)
		SB_PrintCenter (x + sb_colpos[1] * 8, y, 168, SB_Get (key_address, i), selected);
	SB_Print (x, y, substring (SB_ServerName (i), 0, min (sb_namewidth, 60)), selected);
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
	sx = x + SB_Trunc ((w - 8 * strlen (s)) / 2);
	SB_Print (sx, y, s, FALSE);
	for (k = 0 ; k < strlen (s) ; k++)
		if (str2chr (s, k) == 160)
			SB_Char (sx + k * 8 + 4, y, 0x85);
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
			SB_Char (x + k * 8, y + h - 24, k == 0 ? 0x1d : k == cells - 1 ? 0x1f : 0x1e);
	SB_Print (x + w - 8 * (3 + n) + 4, y + h - 24, count, TRUE);
	if (scanning)
		SB_ProgressBar (x + 8, y + h - 24);
	if (i < 0)
		return;

	name = SB_Get (key_name, i);
	if (strlen (name) < 2)
		name = SB_Get (key_address, i);
	SB_PrintCenter (x, y + h - 16, w, substring (name, 0, cells), FALSE);
	if (sb_search != "" && time - sb_searchtime < SB_SEARCHTIME)
		SB_PrintCenter (x, y + h - 8, w, sprintf ("search for: %-7s", sb_search), TRUE);
	else
		SB_InfoLine (x, y + h - 8, w, i);
};

// the selection on its server as the list moves under it
void(float n) SB_Follow =
{
	float	i;

	if (sb_seladdr != "" && (i = gethostcacheindexforaddress (sb_seladdr)) >= 0)
		sb_pos = i;
	if (sb_pos > n - 1)
		sb_pos = n - 1;
	if (sb_pos < 0)
		sb_pos = 0;
	sb_seladdr = n ? strzone (SB_Get (key_address, sb_pos)) : "";
};

void(float i) SB_Select =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT);

	sb_pos = max (0, min (i, n - 1));
	sb_seladdr = n ? strzone (SB_Get (key_address, sb_pos)) : "";
};

// ezQuake's scrolling: the selection in the rows, the rows full
float(float pos, float disp, float n, float rows) SB_Scroll =
{
	if (disp < 0)
		disp = 0;
	if (pos > disp + rows - 1)
		disp = pos - rows + 1;
	if (disp > n - rows)
		disp = max (n - rows, 0);
	if (pos < disp)
		disp = pos;
	return disp;
};

void(float x, float y, float w, float h) SB_Servers_Draw =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT), i;

	SB_Columns (w);
	if (!n)
	{
		if (gethostcachevalue (SLIST_SCANNING) && !gethostcachevalue (SLIST_HOSTCACHETOTALCOUNT))
		{
			SB_PrintCenter (x, y + 8, w, "Updating, please wait", FALSE);
			SB_ProgressBar (x + SB_Trunc ((w - 144) / 2), y + 24);
		}
		else if (sb_dialog != SBD_ADDSERVER)
		{
			SB_PrintCenter (x, y + 8, w, "No servers filtered", FALSE);
			SB_PrintCenter (x, y + 24, w, "Press [space] to refresh the list", TRUE);
			SB_PrintCenter (x, y + 40, w, "Mark some sources on the next tab", FALSE);
			SB_PrintCenter (x, y + 48, w, "or press [Insert] to add a server", FALSE);
			if (sb_search != "")
				SB_PrintCenter (x, y + h - 8, w, sprintf ("search for: %-7s", sb_search), TRUE);
		}
		return;
	}

	sb_listsize = floor (h / 8) - (cvar ("sb_status") ? 3 : 0) - 1;
	sb_disp = SB_Scroll (sb_pos, sb_disp, n, sb_listsize);
	SB_Header (x, y, w);
	for (i = 0 ; i < sb_listsize ; i++)
		if (sb_disp + i < n)
			SB_Row (x, y + 8 * (i + 1), w, sb_disp + i, sb_disp + i == sb_pos);
	if (cvar ("sb_status"))
		SB_Status (x, y, w, h, sb_pos, sb_pos, n);
};

// sb_sortservers' digits: 1 name, 2 address, 3 ping, 4 gamedir, 5 map,
// 6 players, 7 fraglimit, 8 timelimit
float(float digit) SB_SortKey =
{
	switch (digit)
	{
	case '1':	return key_name;
	case '2':	return key_address;
	case '3':	return key_ping;
	case '4':	return key_gamedir;
	case '5':	return key_map;
	case '6':	return key_numplayers;
	case '7':	return key_fraglimit;
	case '8':	return key_timelimit;
	}
	return -1;
};

// the view as ezQuake filters and sorts it (Filter_Servers, Servers_Compare_Func):
// FTE's masks, each in order ANDed into the answer or ORed, so what is ORed
// comes first
void() SB_Arrange =
{
	string	sort = cvar_string ("sb_sortservers");
	float	i, c, first = TRUE, reverse = FALSE, proxies = cvar ("sb_showproxies");

	SB_Keys ();
	resethostcachemasks ();
	// the empty hidden, or those that aren't, but proxies
	if (cvar ("sb_hideempty"))
	{
		sethostcachemasknumber (SLIST_MASK_AND, key_numplayers, 0, SLIST_TEST_GREATER);
		sethostcachemasknumber (SLIST_MASK_OR, key_numspectators, 0, SLIST_TEST_GREATER);
		sethostcachemasknumber (SLIST_MASK_OR, key_isproxy, 0, SLIST_TEST_NOTEQUAL);
	}
	if (cvar ("sb_hidenotempty"))
	{
		sethostcachemasknumber (SLIST_MASK_AND, key_numplayers, 0, SLIST_TEST_EQUAL);
		sethostcachemasknumber (SLIST_MASK_AND, key_numspectators, 0, SLIST_TEST_EQUAL);
		sethostcachemasknumber (SLIST_MASK_OR, key_isproxy, 0, SLIST_TEST_NOTEQUAL);
	}
	if (sb_search != "")
		sethostcachemaskstring (SLIST_MASK_AND, key_name, sb_search, SLIST_TEST_CONTAINS);
	if (!proxies)
		sethostcachemasknumber (SLIST_MASK_AND, key_isproxy, 0, SLIST_TEST_EQUAL);
	else if (proxies == 2)
		sethostcachemasknumber (SLIST_MASK_AND, key_isproxy, 0, SLIST_TEST_NOTEQUAL);
	if (cvar ("sb_hidedead"))
		sethostcachemasknumber (SLIST_MASK_AND, key_state, SL_DEAD, SLIST_TEST_NOTEQUAL);
	if (cvar ("sb_hidehighping"))
		sethostcachemasknumber (SLIST_MASK_AND, key_ping, cvar ("sb_pinglimit"), SLIST_TEST_LESSEQUAL);
	if (cvar ("sb_hidefull"))
		sethostcachemasknumber (SLIST_MASK_AND, key_freeplayers, 0, SLIST_TEST_GREATER);

	// the digits in order, each after a - descending, and those after it too
	// (as ezQuake reads them)
	sethostcachesort (-1, 0);
	for (i = 0 ; i < strlen (sort) ; i++)
	{
		c = str2chr (sort, i);
		if (c == '-')
			reverse = TRUE;
		else if (SB_SortKey (c) >= 0)
		{
			sethostcachesort (SB_SortKey (c), (first ? 0 : SLIST_SORT_APPEND) | (reverse ? SLIST_SORT_DESCENDING : 0));
			first = FALSE;
		}
	}
	resorthostcache ();
	sb_serial++;
};

// arranged again when what arranges it changed (the console's cvars too)
void() SB_CheckSettings =
{
	string	s = strcat (cvar_string ("sb_sortservers"), " ", cvar_string ("sb_hideempty"), cvar_string ("sb_hidenotempty"),
		cvar_string ("sb_hidefull"), cvar_string ("sb_hidedead"), cvar_string ("sb_hidehighping"));

	s = strcat (s, " ", cvar_string ("sb_pinglimit"), " ", cvar_string ("sb_showproxies"), " ", sb_search);
	if (s == sb_settings)
		return;
	sb_settings = strzone (s);
	SB_Arrange ();
};

// a sort string's key first, or turned about where it was first (ezQuake's
// Alt+digit, and the sources' and players' digits)
void(string name, float digit) SB_ToggleSort =
{
	string	sort = cvar_string (name);

	if (str2chr (sort, 0) == digit)
		sort = strcat ("-", sort);
	else if (str2chr (sort, 0) == '-' && str2chr (sort, 1) == digit)
		sort = substring (sort, 1, strlen (sort) - 1);
	else
		sort = strcat (chr2str (digit), sort);
	cvar_set (name, substring (sort, 0, 31));
};

// joined, or watched; the browser closes after (sb_autohide), but for qizmo
void(float i, float spectate) SB_Connect =
{
	string	address = SB_Get (key_address, i);
	float	autohide = cvar ("sb_autohide");

	if (address == "")
		return;
	localcmd (sprintf ("spectator %s\nconnect %s\n", spectate ? "1" : "\"\"", address));
	if (autohide > 1 || (autohide && SB_Gamedir (i) != "qizmo"))
	{
		setkeydest (KEY_GAME);
		m_state = M_NONE;
	}
};

// its game watched through QTV, where the QTV list (qtv_api_url) has a stream
// of it (ezQuake's q); the browser closes after, as for joining
void(float i) SB_WatchQTV =
{
	string	stream = SB_Get (key_qtv, i);

	if (stream == "")
	{
		print (sprintf ("No QTV stream address found for '%s'\n", SB_Get (key_address, i)));
		return;
	}
	localcmd (sprintf ("qtvplay %s\n", stream));
	if (cvar ("sb_autohide"))
	{
		setkeydest (KEY_GAME);
		m_state = M_NONE;
	}
};

// the address, or with Alt the name and address
void(float i) SB_Copy =
{
	string	address = SB_Get (key_address, i), name = SB_Get (key_name, i);

	if (sb_ctrl || name == "")
		clipboard_set (0, address);
	else
		clipboard_set (0, sprintf ("%s (%s)", name, address));
};

// the name and address said, with Ctrl to the team
void(float i) SB_Say =
{
	localcmd (sprintf ("%s \"%s (%s)\"\n", sb_ctrl ? "say_team" : "say", strreplace ("\"", "", SB_Get (key_name, i)),
		SB_Get (key_address, i)));
};

/*
==============================================================================

THE INFO POPUP

The selected server's players, serverinfo and sources, kept fresh while it is
up (sb_liveupdate)

==============================================================================
*/

float	sb_infopage;		// 0 players, 1 serverinfo, 2 sources
float	sb_infopos;			// the page's top line
float	sb_infoupdate;		// when it is asked again

// the roster, as gethostcachestring gives player<N>, by frags
float	ro_count;
float	ro_ping[SB_MAXROSTER], ro_time[SB_MAXROSTER], ro_frags[SB_MAXROSTER], ro_spec[SB_MAXROSTER];
string	ro_name[SB_MAXROSTER], ro_team[SB_MAXROSTER];
vector	ro_top[SB_MAXROSTER], ro_bottom[SB_MAXROSTER];

void(float i) SB_ReadRoster =
{
	float	r, j, k, f, p, t, s;
	string	text, nm, tm;
	vector	top, bottom;

	ro_count = 0;
	for (r = 0 ; r < SB_MAXROSTER ; r++)
	{
		text = SB_Get (key_player0 + r, i);
		if (text == "")
			break;
		tokenize (text);
		// in by frags, the spectators' -9999 last
		f = stof (argv (1));
		p = stof (argv (3));
		t = stof (argv (2));
		s = f == -9999;
		nm = argv (4);
		// userid frags time ping "name" "skin" 'top' 'bottom' "team": the
		// colours are vectors, a word each to tokenize
		tm = argv (8);
		top = stov (argv (6));
		bottom = stov (argv (7));
		for (j = ro_count ; j > 0 ; j--)
			if (ro_frags[j - 1] >= f)
				break;
		for (k = ro_count ; k > j ; k--)
		{
			ro_ping[k] = ro_ping[k - 1];
			ro_time[k] = ro_time[k - 1];
			ro_frags[k] = ro_frags[k - 1];
			ro_spec[k] = ro_spec[k - 1];
			ro_name[k] = ro_name[k - 1];
			ro_team[k] = ro_team[k - 1];
			ro_top[k] = ro_top[k - 1];
			ro_bottom[k] = ro_bottom[k - 1];
		}
		ro_ping[j] = p;
		ro_time[j] = t;
		ro_frags[j] = f;
		ro_spec[j] = s;
		ro_name[j] = nm;
		ro_team[j] = tm;
		ro_top[j] = top;
		ro_bottom[j] = bottom;
		ro_count++;
	}
};

// frags as ezQuake writes them: spec, else within -99..9999, a space after under 1000
string(float r) SB_Frags =
{
	float	f = ro_frags[r];

	if (ro_spec[r])
		return "spec";
	f = max (-99, min (f, 9999));
	return sprintf ("%3d%s", f, f < 1000 ? " " : "");
};

// the players, their colours under their frags, and with two teams the score
void(float x, float y, float w, float h, float i) SB_Info_Players =
{
	float	tp, team, r, ry, rows, teams = 0, s1 = 0, s2 = 0, t;
	string	teamplay = SB_Get (key_teamplay, i), t1 = "", t2 = "", line;
	vector	top1 = '0 0 0', bottom1 = '0 0 0', top2 = '0 0 0', bottom2 = '0 0 0', swap;

	SB_ReadRoster (i);
	tp = FALSE;
	for (r = 0 ; r < ro_count ; r++)
		if (ro_team[r] != "")
			tp = TRUE;
	tp = tp && teamplay != "0";

	// two teams, by their team names or else their colours; a third undoes it
	if (!SB_IsQizmo (i) && SB_Num (key_numplayers, i) > 2 && teamplay != "0")
	{
		for (r = 0 ; r < ro_count && teams >= 0 ; r++)
		{
			if (ro_spec[r])
				continue;
			if (tp)
				team = (ro_team[r] == t1 && teams >= 1) ? 1 : (ro_team[r] == t2 && teams >= 2) ? 2 : 0;
			else
				team = (ro_top[r] == top1 && ro_bottom[r] == bottom1 && teams >= 1) ? 1
					: (ro_top[r] == top2 && ro_bottom[r] == bottom2 && teams >= 2) ? 2 : 0;
			if (!team)
			{
				if (teams == 2)
				{
					teams = -1;
					break;
				}
				team = ++teams;
				if (team == 1)
				{
					t1 = ro_team[r];
					top1 = ro_top[r];
					bottom1 = ro_bottom[r];
				}
				else
				{
					t2 = ro_team[r];
					top2 = ro_top[r];
					bottom2 = ro_bottom[r];
				}
			}
			if (team == 1)
				s1 += ro_frags[r];
			else
				s2 += ro_frags[r];
		}
		// the team ahead first
		if (teams == 2 && s2 > s1)
		{
			t = s1;
			s1 = s2;
			s2 = t;
			line = t1;
			t1 = t2;
			t2 = line;
			swap = top1;
			top1 = top2;
			top2 = swap;
			swap = bottom1;
			bottom1 = bottom2;
			bottom2 = swap;
		}
	}
	if (teams != 2)
		teams = 0;

	rows = floor (h / 8) - 1 - (teams ? 1 : 0);
	sb_infopos = max (0, min (sb_infopos, ro_count - rows));
	SB_Print (x, y, tp ? "png tm frgs team name" : "png tm frgs name", TRUE);
	for (r = sb_infopos ; r < ro_count && r < sb_infopos + rows ; r++)
	{
		ry = y + (r - sb_infopos) * 8 + 8;
		if (!ro_spec[r])
		{
			// a team's colours over its players' own
			if (tp && teams && ro_team[r] == t1)
			{
				drawfill ([x + 54, ry + 1, 0], '34 4 0', top1, 1, 0);
				drawfill ([x + 54, ry + 5, 0], '34 3 0', bottom1, 1, 0);
			}
			else if (tp && teams && ro_team[r] == t2)
			{
				drawfill ([x + 54, ry + 1, 0], '34 4 0', top2, 1, 0);
				drawfill ([x + 54, ry + 5, 0], '34 3 0', bottom2, 1, 0);
			}
			else
			{
				drawfill ([x + 54, ry + 1, 0], '34 4 0', ro_top[r], 1, 0);
				drawfill ([x + 54, ry + 5, 0], '34 3 0', ro_bottom[r], 1, 0);
			}
		}
		if (tp)
			line = sprintf ("%3d %2d %4.4s %4.4s %s", max (0, min (ro_ping[r], 999)), max (0, min (ro_time[r], 99)),
				SB_Frags (r), ro_team[r], ro_name[r]);
		else
			line = sprintf ("%3d %2d %4.4s %s", max (0, min (ro_ping[r], 999)), max (0, min (ro_time[r], 99)),
				SB_Frags (r), ro_name[r]);
		SB_Print (x, ry, substring (line, 0, 31), FALSE);
	}
	if (teams)
	{
		ry = y + rows * 8 + 8;
		drawfill ([x + 104, ry + 1, 0], '40 4 0', top1, 1, 0);
		drawfill ([x + 104, ry + 5, 0], '40 4 0', bottom1, 1, 0);
		drawfill ([x + 168, ry + 1, 0], '40 4 0', top2, 1, 0);
		drawfill ([x + 168, ry + 5, 0], '40 4 0', bottom2, 1, 0);
		SB_Print (x, ry, sprintf ("      score:  %3d  -  %3d", s1, s2), FALSE);
	}
};

// the serverinfo's keys, without case in order
float	si_count;
string	si_key[64], si_value[64];

void(float x, float y, float w, float h, float i) SB_Info_Rules =
{
	float	n = tokenizebyseparator (SB_Get (key_serverinfo, i), "\\"), k, j, rows = floor (h / 8);
	string	key, value;

	si_count = 0;
	for (k = 1 ; k + 1 < n && si_count < 64 ; k += 2)
	{
		key = argv (k);
		value = argv (k + 1);
		for (j = si_count ; j > 0 ; j--)
		{
			if (strcasecmp (si_key[j - 1], key) <= 0)
				break;
			si_key[j] = si_key[j - 1];
			si_value[j] = si_value[j - 1];
		}
		si_key[j] = key;
		si_value[j] = value;
		si_count++;
	}
	sb_infopos = max (0, min (sb_infopos, si_count - rows));
	for (k = sb_infopos ; k < si_count && k < sb_infopos + rows ; k++)
		SB_Print (x, y + (k - sb_infopos) * 8, substring (strcat (SB_Pad (si_key[k], 13), " ",
			SB_Pad (si_value[k], floor (w / 8) - 14)), 0, 31), FALSE);
};

// the sources, red those that named the server, the line chosen marked
void(float x, float y, float w, float h, float i) SB_Info_Sources =
{
	float	n = gethostcachevalue (SLIST_SOURCES), k, rows = floor (h / 8) - 1;

	sb_infopos = max (0, min (sb_infopos, n - 1));
	SB_Print (x, y, " type    name", TRUE);
	for (k = 0 ; k < n && k < rows ; k++)
	{
		SB_Print (x, y + 8 + k * 8, sprintf ("%s   %s", SB_Pad (gethostcachesource (k, "type"), 6),
			gethostcachesource (k, "name")), hostcacheinsource (i, k));
		if (k == sb_infopos)
			SB_Char (x + 56, y + 8 + k * 8, 0x8d);
	}
};

string	sb_infotabs[3] = {"players", "serverinfo", "sources"};
float	sb_infotabat[3] = {0, 8, 19};

void() SB_Info_Draw =
{
	float	w = 240, h = 112 + (sb_vh >= 240 ? 40 : 0), x, y, k;
	string	tabs = " players serverinfo sources ", name = SB_Get (key_name, sb_pos), red = "";

	x = SB_Centered (w, sb_vw);
	y = SB_Centered (h, sb_vh);
	SB_TextBox (x - 16, y - 16, 31, h / 8 + 2);
	x -= 4;
	y -= 4;
	w = 248;
	h += 8;

	SB_PrintCenter (x, y, w, substring (name != "" ? name : SB_Get (key_address, sb_pos), 0, 31), FALSE);
	SB_PrintCenter (x, y + 10, w, SB_Get (key_address, sb_pos), FALSE);
	// the page's name in brackets, red
	for (k = 0 ; k < strlen (sb_infotabs[sb_infopage]) ; k++)
		red = strcat (red, chr2str (str2chr (sb_infotabs[sb_infopage], k) | 128));
	tabs = strcat (substring (tabs, 0, sb_infotabat[sb_infopage]), chr2str (0x10), red, chr2str (0x11),
		substring (tabs, sb_infotabat[sb_infopage] + strlen (red) + 2, 28));
	SB_PrintCenter (x, y + 24, w, tabs, FALSE);
	SB_Line (x, y + 32, 31);
	switch (sb_infopage)
	{
	case 0:
		SB_Info_Players (x, y + 40, w, h - 40, sb_pos);
		break;
	case 1:
		SB_Info_Rules (x, y + 40, w, h - 40, sb_pos);
		break;
	case 2:
		SB_Info_Sources (x, y + 40, w, h - 40, sb_pos);
		break;
	}
};

void() SB_Info_Open =
{
	sb_info = TRUE;
	sb_infopos = 0;
	sb_infoupdate = 0;
};

// another server under the popup
void(float d) SB_Info_Move =
{
	SB_Select (sb_pos + d);
	sb_infopos = 0;
	sb_infoupdate = 0;
};

void(float k) SB_Info_Key =
{
	switch (k)
	{
	case K_ENTER:
		SB_Connect (sb_pos, sb_ctrl);
		break;
	case K_ESCAPE:
	case K_BACKSPACE:
		sb_info = FALSE;
		break;
	case K_TAB:
		sb_infopage = (sb_infopage + (sb_shift ? 2 : 1)) % 3;
		sb_infopos = 0;
		break;
	case K_RIGHTARROW:
		sb_infopage = (sb_infopage + 1) % 3;
		sb_infopos = 0;
		break;
	case K_LEFTARROW:
		sb_infopage = (sb_infopage + 2) % 3;
		sb_infopos = 0;
		break;
	case K_UPARROW:
	case K_PGUP:
	case K_MWHEELUP:
		if (sb_ctrl)
			sb_infopos = max (0, sb_infopos - 1);
		else
			SB_Info_Move (-1);
		break;
	case K_DOWNARROW:
	case K_PGDN:
	case K_MWHEELDOWN:
		if (sb_ctrl)
			sb_infopos++;
		else
			SB_Info_Move (1);
		break;
	case K_HOME:
		sb_infopos = 0;
		break;
	case K_END:
		sb_infopos = 999;
		break;
	}
};

void(float c) SB_Info_Char =
{
	switch (SB_Lower (c))
	{
	case 'j':
	case 'p':
	case 'n':
		SB_Connect (sb_pos, FALSE);
		break;
	case 'o':
	case 's':
		SB_Connect (sb_pos, TRUE);
		break;
	case 'q':
		SB_WatchQTV (sb_pos);
		break;
	case 'c':
		SB_Copy (sb_pos);
		break;
	case 'v':
		SB_Say (sb_pos);
		break;
	}
};

/*
==============================================================================

THE SOURCES

==============================================================================
*/

float	src_pos, src_disp;
float	src_order[64];		// by sb_sortsources
float	src_count;

// a source before another by sb_sortsources: 1 type, 2 name, 3 as listed
float(float a, float b) SB_SourceBefore =
{
	string	sort = cvar_string ("sb_sortsources");
	float	i, d, reverse = FALSE;

	for (i = 0 ; i < strlen (sort) ; i++)
	{
		switch (str2chr (sort, i))
		{
		case '-':
			reverse = TRUE;
			continue;
		case '1':
			d = strcasecmp (gethostcachesource (a, "type"), gethostcachesource (b, "type"));
			break;
		case '2':
			d = strcasecmp (gethostcachesource (a, "name"), gethostcachesource (b, "name"));
			break;
		case '3':
			d = a - b;
			break;
		default:
			d = 0;
			break;
		}
		if (d)
			return reverse ? d > 0 : d < 0;
	}
	return a < b;
};

void() SB_SortSources =
{
	float	i, j, s;

	src_count = min (gethostcachevalue (SLIST_SOURCES), 64);
	for (i = 0 ; i < src_count ; i++)
	{
		for (j = i ; j > 0 ; j--)
		{
			if (!SB_SourceBefore (i, src_order[j - 1]))
				break;
			src_order[j] = src_order[j - 1];
		}
		src_order[j] = i;
	}
	s = src_pos;
	src_pos = max (0, min (s, src_count - 1));
};

float(float s) SB_Marked =
{
	return gethostcachesource (s, "marked") != "";
};

void(float x, float y, float w, float h) SB_Sources_Draw =
{
	float	rows = floor (h / 8) - (cvar ("sb_status") ? 3 : 0) - 1, i, s, marked = 0;
	string	line;

	SB_SortSources ();
	src_disp = SB_Scroll (src_pos, src_disp, src_count, rows);
	SB_PrintCenter (x, y, w, " type   name             servs updated", TRUE);
	for (i = 0 ; i < rows ; i++)
	{
		if (src_disp + i >= src_count)
			break;
		s = src_order[src_disp + i];
		line = sprintf ("%s %s%s %4d  %s ", SB_Pad (gethostcachesource (s, "type"), 6),
			chr2str (src_disp + i == src_pos ? 141 : 32), SB_Pad (gethostcachesource (s, "name"), 17),
			stof (gethostcachesource (s, "servers")), gethostcachesource (s, "updated"));
		SB_PrintCenter (x, y + 8 * (i + 1), w, line, SB_Marked (s));
	}
	if (!cvar ("sb_status"))
		return;
	for (i = 0 ; i < src_count ; i++)
		marked += SB_Marked (i);
	SB_Line (x, y + h - 24, floor (w / 8));
	SB_PrintCenter (x, y + h - 16, w, sprintf ("%d sources selected (%d servers)", marked,
		gethostcachevalue (SLIST_MARKEDSERVERS)), FALSE);
	SB_PrintCenter (x, y + h - 8, w, sprintf ("of %d total (%d servers)", src_count,
		gethostcachevalue (SLIST_HOSTCACHETOTALCOUNT)), FALSE);
};

void() SB_RemoveSource =
{
	if (src_pos < src_count)
		removehostcachesource (src_order[src_pos]);
};

void(string text, void() action) SB_Confirm;
void() SB_AddSource_Open;

float(float k) SB_Sources_Key =
{
	float	i;

	switch (k)
	{
	case K_INS:
		SB_AddSource_Open ();
		return TRUE;
	case K_DEL:
		if (src_pos < src_count)
			SB_Confirm (sprintf ("Remove %.20s", gethostcachesource (src_order[src_pos], "name")), SB_RemoveSource);
		return TRUE;
	case K_UPARROW:
	case K_MWHEELUP:
		src_pos = max (0, src_pos - 1);
		return TRUE;
	case K_DOWNARROW:
	case K_MWHEELDOWN:
		src_pos = min (src_pos + 1, src_count - 1);
		return TRUE;
	case K_HOME:
		src_pos = 0;
		return TRUE;
	case K_END:
		src_pos = src_count - 1;
		return TRUE;
	case K_PGUP:
		src_pos = max (0, src_pos - 10);
		return TRUE;
	case K_PGDN:
		src_pos = min (src_pos + 10, src_count - 1);
		return TRUE;
	case K_ENTER:
		if (src_pos < src_count)
		{
			i = src_order[src_pos];
			sethostcachesourcemark (i, !SB_Marked (i));
			src_pos = min (src_pos + 1, src_count - 1);
		}
		return TRUE;
	}
	return FALSE;
};

void(float c) SB_Sources_Char =
{
	float	i;

	switch (c)
	{
	case 'n':
		SB_AddSource_Open ();
		break;
	case 'd':
		SB_Sources_Key (K_DEL);
		break;
	case 'u':
	case ' ':
		refreshhostcache (sb_ctrl);
		break;
	case ']':
		if (src_pos < src_count)
			sethostcachesourcemark (src_order[src_pos], !SB_Marked (src_order[src_pos]));
		break;
	case '+':
	case '=':
		for (i = 0 ; i < src_count ; i++)
			sethostcachesourcemark (i, TRUE);
		break;
	case '-':
		for (i = 0 ; i < src_count ; i++)
			sethostcachesourcemark (i, FALSE);
		break;
	case '*':
		for (i = 0 ; i < src_count ; i++)
			sethostcachesourcemark (i, !SB_Marked (i));
		break;
	case '1':
	case '2':
	case '3':
		SB_ToggleSort ("sb_sortsources", c);
		break;
	}
};

/*
==============================================================================

THE PLAYERS

Every player of the servers listed

==============================================================================
*/

float	pl_count, pl_pos, pl_disp;
var float	pl_generation = -1, pl_serial = -1;
float	pl_server[SB_MAXPLAYERS], pl_order[SB_MAXPLAYERS], pl_merge[SB_MAXPLAYERS];
string	pl_name[SB_MAXPLAYERS];
var string	pl_search = "";
var float	pl_searchtime = -100;

// a player before another by sb_sortplayers: sb_sortservers' digits for its
// server, 9 its name (descending never, as ezQuake has it)
float(float a, float b) SB_PlayerBefore =
{
	string	sort = cvar_string ("sb_sortplayers");
	float	i, c, d, sa = pl_server[a], sb = pl_server[b];

	for (i = 0 ; i < strlen (sort) ; i++)
	{
		c = str2chr (sort, i);
		switch (c)
		{
		case '1':
			d = strcasecmp (SB_ServerName (sa), SB_ServerName (sb));
			break;
		case '2':
		case '4':
		case '5':
			d = strcasecmp (SB_Get (SB_SortKey (c), sa), SB_Get (SB_SortKey (c), sb));
			break;
		case '3':
		case '6':
		case '7':
		case '8':
			d = SB_Num (SB_SortKey (c), sa) - SB_Num (SB_SortKey (c), sb);
			break;
		case '9':
			d = strcasecmp (pl_name[a], pl_name[b]);
			break;
		default:
			d = 0;
			break;
		}
		if (d)
			return d < 0;
	}
	return a < b;
};

// pl_order[from, to) sorted, by halves
void(float from, float to) SB_SortPlayers =
{
	float	mid = floor ((from + to) / 2), i, j, k, take;

	if (to - from < 2)
		return;
	SB_SortPlayers (from, mid);
	SB_SortPlayers (mid, to);
	for (i = from, j = mid, k = from ; k < to ; k++)
	{
		take = j >= to;
		if (!take && i < mid)
			take = !SB_PlayerBefore (pl_order[j], pl_order[i]);
		if (take)
			pl_merge[k] = pl_order[i++];
		else
			pl_merge[k] = pl_order[j++];
	}
	for (k = from ; k < to ; k++)
		pl_order[k] = pl_merge[k];
};

// the players read again when the list or its view changed
void() SB_ReadPlayers =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT), g = gethostcachevalue (SLIST_GENERATION), i, r;
	string	text;

	if (g == pl_generation && sb_serial == pl_serial)
		return;
	pl_generation = g;
	pl_serial = sb_serial;
	pl_count = 0;
	for (i = 0 ; i < n && pl_count < SB_MAXPLAYERS ; i++)
		for (r = 0 ; r < SB_MAXROSTER && pl_count < SB_MAXPLAYERS ; r++)
		{
			text = SB_Get (key_player0 + r, i);
			if (text == "")
				break;
			tokenize (text);
			pl_name[pl_count] = argv (4);
			pl_server[pl_count] = i;
			pl_order[pl_count] = pl_count;
			pl_count++;
		}
	SB_SortPlayers (0, pl_count);
	pl_pos = max (0, min (pl_pos, pl_count - 1));
};

// the player selected's server
float() SB_PlayerServer =
{
	return pl_count ? pl_server[pl_order[pl_pos]] : -1;
};

void(float x, float y, float w, float h) SB_Players_Draw =
{
	float	rows = floor (h / 8) - (cvar ("sb_status") ? 3 : 0) - 1, hw = min (floor (w / 8) - 16, 60) - 4, i, p, s;

	SB_ReadPlayers ();
	pl_disp = SB_Scroll (pl_pos, pl_disp, pl_count, rows);
	SB_PrintCenter (x, y, w, strcat ("name            ", SB_Pad ("server", hw), " png"), TRUE);
	for (i = 0 ; i < rows && pl_disp + i < pl_count ; i++)
	{
		p = pl_order[pl_disp + i];
		s = pl_server[p];
		SB_PrintCenter (x, y + 8 * (i + 1), w, strcat (strpad (15, pl_name[p]),
			chr2str (pl_disp + i == pl_pos ? 141 : 32), SB_Pad (SB_ServerName (s), hw), " ",
			SB_PadRight (SB_PingText (s), 3)), pl_disp + i == pl_pos);
	}
	if (cvar ("sb_status") && pl_count)
		SB_Status (x, y, w, h, SB_PlayerServer (), pl_pos, pl_count);
};

// the first player from the selected one on (then from the first) whose name has it
void() SB_SearchPlayer =
{
	float	i, k;

	for (k = 0 ; k < pl_count ; k++)
	{
		i = (pl_pos + k) % pl_count;
		if (strstrofs (strtolower (strdecolorize (pl_name[pl_order[i]])), pl_search) >= 0)
		{
			pl_pos = i;
			return;
		}
	}
	pl_search = "not found";
};

float(float k) SB_Players_Key =
{
	switch (k)
	{
	case K_UPARROW:
	case K_MWHEELUP:
		pl_pos = max (0, pl_pos - 1);
		return TRUE;
	case K_DOWNARROW:
	case K_MWHEELDOWN:
		pl_pos = min (pl_pos + 1, pl_count - 1);
		return TRUE;
	case K_HOME:
		pl_pos = 0;
		return TRUE;
	case K_END:
		pl_pos = pl_count - 1;
		return TRUE;
	case K_PGUP:
		pl_pos = max (0, pl_pos - 10);
		return TRUE;
	case K_PGDN:
		pl_pos = min (pl_pos + 10, pl_count - 1);
		return TRUE;
	case K_ENTER:
		if (pl_count)
		{
			SB_Select (SB_PlayerServer ());
			SB_Info_Open ();
		}
		return TRUE;
	case K_INS:
		if (pl_count)
		{
			SB_Select (SB_PlayerServer ());
			sb_tab = SB_SERVERS;
		}
		return TRUE;
	}
	return FALSE;
};

void(float c) SB_Players_Char =
{
	if (c == ' ' && !sb_alt && (pl_search == "" || time - pl_searchtime >= SB_SEARCHTIME))
	{
		refreshhostcache (sb_ctrl);
		return;
	}
	// with Alt, or while a search goes on, a search
	if (c >= ' ' && c <= '}' && (sb_alt || (pl_search != "" && time - pl_searchtime < SB_SEARCHTIME)))
	{
		if (time - pl_searchtime >= SB_SEARCHTIME || pl_search == "not found")
			pl_search = "";
		pl_search = strzone (substring (strcat (pl_search, chr2str (SB_Lower (c))), 0, 20));
		pl_searchtime = time;
		SB_SearchPlayer ();
		return;
	}
	if (!pl_count)
		return;
	switch (SB_Lower (c))
	{
	case 'j':
	case 'p':
		SB_Connect (SB_PlayerServer (), FALSE);
		break;
	case 'o':
	case 's':
		SB_Connect (SB_PlayerServer (), TRUE);
		break;
	case 'c':
		SB_Copy (SB_PlayerServer ());
		break;
	case 'v':
		SB_Say (SB_PlayerServer ());
		break;
	case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
		SB_ToggleSort ("sb_sortplayers", c);
		pl_serial = -1;
		break;
	}
};

/*
==============================================================================

THE OPTIONS

ezQuake's settings page: its sections, switches, named values and sliders

==============================================================================
*/

#define	OPT_SEPARATOR	0
#define	OPT_BOOL		1
#define	OPT_NAMED		2
#define	OPT_NUMBER		3
#define	OPT_COUNT		26

string	opt_label[OPT_COUNT] = {"Server Filters", "Hide Empty", "Hide Full", "Hide Not Empty", "Hide Dead",
	"Hide High Ping", "Show Proxies", "Display Columns", "Show Ping", "Show Map", "Show Gamedir", "Show Players",
	"Show Timelimit", "Show Fraglimit", "Show Server Address", "Display", "Server Status", "Network Filters",
	"Ping Timeout", "Pings Per Server", "Pings Per Second", "Master Timeout", "Master Retries", "Info Timeout",
	"Info Retries", "Infos Per Second"};
string	opt_cvar[OPT_COUNT] = {"", "sb_hideempty", "sb_hidefull", "sb_hidenotempty", "sb_hidedead", "sb_hidehighping",
	"sb_showproxies", "", "sb_showping", "sb_showmap", "sb_showgamedir", "sb_showplayers", "sb_showtimelimit",
	"sb_showfraglimit", "sb_showaddress", "", "sb_status", "", "sb_pingtimeout", "sb_pings", "sb_pingspersec",
	"sb_mastertimeout", "sb_masterretries", "sb_infotimeout", "sb_inforetries", "sb_infospersec"};
float	opt_kind[OPT_COUNT] = {OPT_SEPARATOR, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_NAMED,
	OPT_SEPARATOR, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_SEPARATOR, OPT_BOOL,
	OPT_SEPARATOR, OPT_NUMBER, OPT_NUMBER, OPT_NUMBER, OPT_NUMBER, OPT_NUMBER, OPT_NUMBER, OPT_NUMBER, OPT_NUMBER};
float	opt_min[OPT_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 1, 10, 50, 1, 50, 0, 10};
float	opt_max[OPT_COUNT] = {0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1000, 5, 300, 1000, 5, 1000, 4, 1000};
float	opt_step[OPT_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 1, 10, 50, 1, 50, 1, 10};
string	opt_proxies[3] = {"hide", "show", "exclusively"};

var float	opt_pos = 1;
float	opt_view;
float	opt_help;			// the whole help up

float(float i) SB_OptHeight =
{
	return opt_kind[i] == OPT_SEPARATOR ? 24 : 9;
};

// the help's words in lines of width characters, from the top line on, up
// to lines; whether there was more
float(float x, float y, float width, float lines, string text) SB_TextBlock =
{
	float	n = tokenizebyseparator (text, " "), k, line = 0;
	string	s = "", word;

	for (k = 0 ; k < n ; k++)
	{
		word = argv (k);
		if (s != "" && strlen (s) + 1 + strlen (word) > width)
		{
			if (line >= lines)
				return TRUE;
			SB_Print (x, y + line++ * 8, s, FALSE);
			s = "";
		}
		s = s == "" ? substring (word, 0, width) : strcat (s, " ", word);
	}
	if (s != "")
	{
		if (line >= lines)
			return TRUE;
		SB_Print (x, y + line * 8, s, FALSE);
	}
	return FALSE;
};

// a palette's colour, for the scroll bar ezQuake draws without its textures
vector	pal4, pal40, pal72;
float	pal_read;

vector(int c) SB_PaletteColour =
{
	return [(float)(c & 255), (float)((c >> 8) & 255), (float)((c >> 16) & 255)] * (1 / 255);
};

void() SB_ReadPalette =
{
	int		*pal;
	int		pw, ph;

	if (pal_read)
		return;
	pal_read = TRUE;
	pal = r_readimage ("gfx/palette.lmp", pw, ph);
	if (!pal)
		return;
	if (pw * ph == 256)
	{
		pal4 = SB_PaletteColour (pal[4]);
		pal40 = SB_PaletteColour (pal[40]);
		pal72 = SB_PaletteColour (pal[72]);
	}
	memfree (pal);
};

void(float x, float y, float w, float h) SB_Options_Draw =
{
	float	i, ry, vx, v, total = 0, lowest, room, cvh, active;
	string	help, value;

	w -= 8;		// the scroll bar's
	if (opt_help)
	{
		SB_TextBox (x, y, floor (w / 8) - 1, floor ((h - 32) / 8));
		SB_TextBlock (x + 8, y + 8, floor ((w - 16) / 8), floor ((h - 32) / 8), cvar_description (opt_cvar[opt_pos]));
		return;
	}
	// the help below the list
	SB_TextBox (x, y + h - 40, floor (w / 8) - 1, 4);
	help = cvar_description (opt_cvar[opt_pos]);
	if (SB_TextBlock (x + 8, y + h - 32, floor ((w - 16) / 8), 3, help))
		SB_Print (x + 8, y + h - 8, "Press [F1] to read more...", TRUE);
	h -= 40;

	// the rows from the view's on, the cursor's among them
	for (i = 0 ; i < OPT_COUNT ; i++)
		total += SB_OptHeight (i);
	if (opt_view > opt_pos)
		opt_view = opt_pos;
	if (opt_view == 1)
		opt_view = 0;		// the first section's name with its rows
	for (;;)
	{
		for (room = 0, i = opt_view ; i <= opt_pos ; i++)
			room += SB_OptHeight (i);
		if (room <= h || opt_view >= opt_pos)
			break;
		opt_view++;
	}
	for (ry = y, i = opt_view ; i < OPT_COUNT ; ry += SB_OptHeight (i), i++)
	{
		if (ry + SB_OptHeight (i) > y + h)
			break;
		if (opt_kind[i] == OPT_SEPARATOR)
		{
			SB_PrintCenter (x, ry + 12, w, strcat (chr2str (0x1d), " ", opt_label[i], " ", chr2str (0x1f)), TRUE);
			continue;
		}
		if (i == opt_pos)
			SB_GrayBox (x, ry, w, 9);
		active = i == opt_pos;
		SB_Print (x + floor (w / 2) - 8 * strlen (opt_label[i]), ry, opt_label[i], active);
		vx = x + floor (w / 2) + 16;
		v = cvar (opt_cvar[i]);
		switch (opt_kind[i])
		{
		case OPT_BOOL:
			value = "off";
			if (v)
				value = "on";
			SB_Print (vx, ry, value, active);
			break;
		case OPT_NAMED:
			SB_Print (vx, ry, opt_proxies[max (0, min (v, 2))], active);
			break;
		case OPT_NUMBER:
			SB_Char (vx - 8, ry, 128);
			for (cvh = 0 ; cvh < 12 ; cvh++)
				SB_Char (vx + cvh * 8, ry, 129);
			SB_Char (vx + 96, ry, 130);
			SB_Char (vx + floor (88 * max (0, min (1, (v - opt_min[i]) / (opt_max[i] - opt_min[i])))), ry, 131);
			SB_Print (vx + 112, ry, sprintf ("%3d", v), active);
			break;
		}
	}

	// the scroll bar, where the rows don't fit
	if (total > h)
	{
		SB_ReadPalette ();
		for (lowest = 0, room = 0, i = OPT_COUNT - 1 ; i >= 0 ; i--)
		{
			room += SB_OptHeight (i);
			if (room > h)
			{
				lowest = i + 1;
				break;
			}
		}
		drawfill ([x + w, y, 0], [8, h, 0], pal4, 1, 0);
		drawfill ([x + w, y, 0], '8 8 0', pal72, 1, 0);
		drawfill ([x + w, y + h - 8, 0], '8 8 0', pal72, 1, 0);
		drawfill ([x + w, y + 8 + floor ((h - 24) * min (1, opt_view / max (lowest, 1))), 0], '8 8 0', pal40, 1, 0);
	}
};

// the cursor moved by d, past the sections
void(float d) SB_OptMove =
{
	float	i = opt_pos;

	for (;;)
	{
		i += d > 0 ? 1 : -1;
		if (i < 0 || i >= OPT_COUNT)
			return;
		if (opt_kind[i] != OPT_SEPARATOR)
		{
			opt_pos = i;
			if (fabs (d) <= 1)
				return;
			d += d > 0 ? -1 : 1;
		}
	}
};

void(float d) SB_OptChange =
{
	float	v = cvar (opt_cvar[opt_pos]);

	switch (opt_kind[opt_pos])
	{
	case OPT_BOOL:
		cvar_set (opt_cvar[opt_pos], v ? "0" : "1");
		break;
	case OPT_NAMED:
		cvar_set (opt_cvar[opt_pos], ftos ((v + d + 3) % 3));
		break;
	case OPT_NUMBER:
		v += d * opt_step[opt_pos];
		if (v >= opt_min[opt_pos] && v <= opt_max[opt_pos])
			cvar_set (opt_cvar[opt_pos], ftos (v));
		break;
	}
};

float(float k) SB_Options_Key =
{
	switch (k)
	{
	case K_DOWNARROW:
		SB_OptMove (1);
		return TRUE;
	case K_UPARROW:
		SB_OptMove (-1);
		return TRUE;
	case K_PGDN:
		SB_OptMove (5);
		return TRUE;
	case K_PGUP:
		SB_OptMove (-5);
		return TRUE;
	case K_HOME:
		opt_pos = 0;
		SB_OptMove (1);
		return TRUE;
	case K_END:
		opt_pos = OPT_COUNT;
		SB_OptMove (-1);
		return TRUE;
	case K_MWHEELUP:
		opt_view = max (0, opt_view - 1);
		return TRUE;
	case K_MWHEELDOWN:
		opt_view = min (opt_view + 1, OPT_COUNT - 1);
		return TRUE;
	case K_RIGHTARROW:
	case K_ENTER:
		SB_OptChange (1);
		return TRUE;
	case K_LEFTARROW:
	case K_BACKSPACE:
		SB_OptChange (-1);
		return TRUE;
	case K_DEL:
		cvar_set (opt_cvar[opt_pos], cvar_defstring (opt_cvar[opt_pos]));
		return TRUE;
	case K_F1:
	case K_INS:
		opt_help = !opt_help;
		return TRUE;
	}
	return FALSE;
};

void(float c) SB_Options_Char =
{
	if (c == '=' || c == '+')
		SB_OptChange (1);
	else if (c == '-')
		SB_OptChange (-1);
};

/*
==============================================================================

THE DIALOGS

==============================================================================
*/

// ezQuake's edit boxes: their text, cursor, and what they show of it
string	ed_text[2];
float	ed_pos[2], ed_disp[2], ed_width[2], ed_max[2];

void(float e, float width, float maxlength) SB_EditInit =
{
	ed_text[e] = "";
	ed_pos[e] = ed_disp[e] = 0;
	ed_width[e] = width;
	ed_max[e] = maxlength;
};

void(float e, float x, float y, float active) SB_EditDraw =
{
	string	text = ed_text[e];

	while (ed_pos[e] > ed_disp[e] + ed_width[e] - 1)
		ed_disp[e]++;
	if (ed_disp[e] > ed_pos[e])
		ed_disp[e] = ed_pos[e];
	if (ed_disp[e] > strlen (text) - ed_width[e] + 1)
		ed_disp[e] = max (0, strlen (text) - ed_width[e] + 1);
	SB_Print (x, y, SB_Pad (active ? substring (text, ed_disp[e], 999) : text, ed_width[e]), FALSE);
	if (active)
		SB_Char (x + 8 * (ed_pos[e] - ed_disp[e]), y, 10 + (floor (time * 4) & 1));
};

void(float e, float k) SB_EditKey =
{
	string	text = ed_text[e];

	switch (k)
	{
	case K_LEFTARROW:
		ed_pos[e] = max (0, ed_pos[e] - 1);
		break;
	case K_RIGHTARROW:
		ed_pos[e] = min (ed_pos[e] + 1, strlen (text));
		break;
	case K_HOME:
		ed_pos[e] = 0;
		break;
	case K_END:
		ed_pos[e] = strlen (text);
		break;
	case K_DEL:
		ed_text[e] = strzone (strcat (substring (text, 0, ed_pos[e]), substring (text, ed_pos[e] + 1, 999)));
		break;
	case K_BACKSPACE:
		if (ed_pos[e] > 0)
		{
			ed_text[e] = strzone (strcat (substring (text, 0, ed_pos[e] - 1), substring (text, ed_pos[e], 999)));
			ed_pos[e]--;
		}
		break;
	}
};

void(float e, float c) SB_EditChar =
{
	string	text = ed_text[e];

	if (c < ' ' || c > '}' || strlen (text) >= ed_max[e])
		return;
	ed_text[e] = strzone (strcat (substring (text, 0, ed_pos[e]), chr2str (c), substring (text, ed_pos[e], 999)));
	ed_pos[e]++;
};

// adding a server: its address
float	as_pos;

void() SB_AddServer_Open =
{
	sb_dialog = SBD_ADDSERVER;
	as_pos = 0;
	SB_EditInit (0, 14, 64);
};

void() SB_AddServer_Draw =
{
	float	w = 176, h = 56, x = SB_Centered (w, sb_vw), y = SB_Centered (h, sb_vh);

	SB_TextBox (x - 16, y - 16, w / 8 + 1, h / 8 + 2);
	x -= 4;
	y -= 4;
	w += 8;
	SB_PrintCenter (x, y + 4, w, "Create new server", TRUE);
	SB_Print (x + 4, y + 20, "Address:", as_pos == 0);
	SB_EditDraw (0, x + 70, y + 20, as_pos == 0);
	SB_PrintCenter (x + 4, y + 40, w, "accept", as_pos == 1);
	if (as_pos == 1)
		SB_Char (x + 59, y + 40, 13);
	SB_PrintCenter (x + 4, y + 50, w, "cancel", as_pos == 2);
	if (as_pos == 2)
		SB_Char (x + 59, y + 50, 13);
};

// added to the Unbound list, and once it is listed selected, its info up
void() SB_AddServer_Accept =
{
	string	address = ed_text[0];

	sb_dialog = SBD_NONE;
	if (address == "" || !addhostcacheserver (address))
		return;
	sb_tab = SB_SERVERS;
	sb_pending = strzone (address);
	sb_pendingtime = time;
};

void(float k) SB_AddServer_Key =
{
	switch (k)
	{
	case K_UPARROW:
	case K_MWHEELUP:
		as_pos = (as_pos + 2) % 3;
		return;
	case K_DOWNARROW:
	case K_MWHEELDOWN:
		as_pos = (as_pos + 1) % 3;
		return;
	case K_TAB:
		as_pos = (as_pos + (sb_shift ? 2 : 1)) % 3;
		return;
	case K_HOME:
		if (sb_ctrl)
		{
			as_pos = 0;
			return;
		}
		break;
	case K_END:
		if (sb_ctrl)
		{
			as_pos = 1;
			return;
		}
		break;
	case K_ENTER:
		if (as_pos == 2)
			sb_dialog = SBD_NONE;
		else
			SB_AddServer_Accept ();
		return;
	case K_ESCAPE:
		sb_dialog = SBD_NONE;
		return;
	case K_BACKSPACE:
		if (as_pos != 0)
		{
			sb_dialog = SBD_NONE;
			return;
		}
		break;
	}
	if (as_pos == 0)
		SB_EditKey (0, k);
};

// adding a source: its type, name and where it is
float	ns_pos, ns_type;
string	ns_types[3] = {"master", "file", "url"};
string	ns_where[3] = {"addr", "file", "url"};

void() SB_AddSource_Open =
{
	sb_dialog = SBD_ADDSOURCE;
	ns_pos = 0;
	ns_type = 1;
	SB_EditInit (0, 16, 25);
	SB_EditInit (1, 16, 100);
};

void() SB_AddSource_Draw =
{
	float	w = 176, h = 72, x = SB_Centered (w, sb_vw), y = SB_Centered (h, sb_vh);

	SB_TextBox (x - 16, y - 16, w / 8 + 1, h / 8 + 2);
	x -= 4;
	y -= 4;
	w += 8;
	SB_PrintCenter (x, y + 4, w, "Create new source", TRUE);
	SB_Print (x + 4, y + 20, "type", ns_pos == 0);
	SB_Print (x + 54, y + 20, SB_Pad (ns_types[ns_type], 6), ns_pos == 0);
	SB_Print (x + 4, y + 30, "name", ns_pos == 1);
	SB_EditDraw (0, x + 54, y + 30, ns_pos == 1);
	SB_Print (x + 4, y + 40, ns_where[ns_type], ns_pos == 2);
	SB_EditDraw (1, x + 54, y + 40, ns_pos == 2);
	if (ns_pos <= 2)
		SB_Char (x + 42, y + 20 + 10 * ns_pos, 13);
	SB_PrintCenter (x + 4, y + 60, w, "accept", ns_pos == 3);
	if (ns_pos == 3)
		SB_Char (x + 59, y + 60, 13);
	SB_PrintCenter (x + 4, y + 70, w, "cancel", ns_pos == 4);
	if (ns_pos == 4)
		SB_Char (x + 59, y + 70, 13);
};

void(float k) SB_AddSource_Key =
{
	switch (k)
	{
	case K_UPARROW:
	case K_MWHEELUP:
		ns_pos = (ns_pos + 4) % 5;
		return;
	case K_DOWNARROW:
	case K_MWHEELDOWN:
		ns_pos = (ns_pos + 1) % 5;
		return;
	case K_TAB:
		ns_pos = (ns_pos + (sb_shift ? 4 : 1)) % 5;
		return;
	case K_HOME:
		if (sb_ctrl)
		{
			ns_pos = 0;
			return;
		}
		break;
	case K_END:
		if (sb_ctrl)
		{
			ns_pos = 4;
			return;
		}
		break;
	case K_ENTER:
		if (ns_pos == 0)
			ns_type = (ns_type + 1) % 3;
		else if (ns_pos == 4)
			sb_dialog = SBD_NONE;
		else
		{
			addhostcachesource (ns_types[ns_type], ed_text[0], ed_text[1]);
			sb_dialog = SBD_NONE;
		}
		return;
	case K_ESCAPE:
		sb_dialog = SBD_NONE;
		return;
	case K_BACKSPACE:
		if (ns_pos != 1 && ns_pos != 2)
		{
			sb_dialog = SBD_NONE;
			return;
		}
		break;
	}
	if (ns_pos == 1 || ns_pos == 2)
		SB_EditKey (ns_pos - 1, k);
};

void(float c) SB_AddSource_Char =
{
	if (ns_pos == 0 && (c == '+' || c == '=' || c == '-'))
		ns_type = (ns_type + 1) % 3;
	else if (ns_pos == 1 || ns_pos == 2)
		SB_EditChar (ns_pos - 1, c);
};

// sure? y or n
string	cf_text;
var void()	cf_action;

void(string text, void() action) SB_Confirm =
{
	cf_text = strzone (text);
	cf_action = action;
	sb_dialog = SBD_CONFIRM;
};

void() SB_Confirm_Draw =
{
	float	w = 32 + 8 * max (strlen (cf_text), 19), h = 24, x = SB_Centered (w, sb_vw), y = SB_Centered (h, sb_vh);

	SB_TextBox (x - 16, y - 16, w / 8 + 1, 5);
	SB_PrintCenter (x, y, w, cf_text, FALSE);
	SB_PrintCenter (x, y + 16, w, "Are you sure? <y/n>", TRUE);
};

void(float k, float c) SB_Confirm_Key =
{
	if (c == 'y' || c == 'Y')
	{
		sb_dialog = SBD_NONE;
		cf_action ();
	}
	else if (c == 'n' || c == 'N' || k == K_ESCAPE || k == K_BACKSPACE)
		sb_dialog = SBD_NONE;
};

/*
==============================================================================

THE BROWSER

==============================================================================
*/

// the servers' keys that aren't typed
float(float k) SB_Servers_Key =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT);

	if (k == K_BACKSPACE)
	{
		sb_search = "";
		return TRUE;
	}
	if (k == K_INS)
	{
		SB_AddServer_Open ();
		return TRUE;
	}
	if (!n)
		return FALSE;		// the tabs move on an empty list
	switch (k)
	{
	case K_UPARROW:
		SB_Select (sb_pos - 1);
		break;
	case K_DOWNARROW:
		SB_Select (sb_pos + 1);
		break;
	case K_MWHEELUP:
		sb_disp = max (0, sb_disp - 4);
		SB_Select (sb_pos - 4);
		break;
	case K_MWHEELDOWN:
		sb_disp += 4;
		SB_Select (sb_pos + 4);
		break;
	case K_HOME:
		SB_Select (0);
		break;
	case K_END:
		SB_Select (n - 1);
		break;
	case K_PGUP:
		SB_Select (sb_pos - 10);
		break;
	case K_PGDN:
		SB_Select (sb_pos + 10);
		break;
	case K_ENTER:
		SB_Info_Open ();
		break;
	default:
		return FALSE;
	}
	return TRUE;
};

// the servers' typed keys: a search, or with Ctrl or Alt a command
void(float c) SB_Servers_Char =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT);

	if (c == ' ' && !sb_alt)
	{
		refreshhostcache (sb_ctrl);
		return;
	}
	if (!sb_ctrl && !sb_alt)
	{
		if (c > ' ' && c <= '}')
		{
			if (time - sb_searchtime >= SB_SEARCHTIME)
				sb_search = "";
			sb_search = strzone (substring (strcat (sb_search, chr2str (SB_Lower (c))), 0, 20));
			sb_searchtime = time;
		}
		return;
	}
	c = SB_Lower (c);
	if (c == 'n')
	{
		SB_AddServer_Open ();
		return;
	}
	if (c >= '1' && c <= '8')
	{
		if (sb_alt)
			SB_ToggleSort ("sb_sortservers", c);
		else if (c != '1')
			cvar_set (sb_colvar[c - '1'], cvar (sb_colvar[c - '1']) ? "0" : "1");
		return;
	}
	if (!n)
		return;
	switch (c)
	{
	case 'j':
	case 'p':
		SB_Connect (sb_pos, FALSE);
		break;
	case 'o':
	case 's':
		SB_Connect (sb_pos, TRUE);
		break;
	case 'r':
		refreshhostcacheentry (sb_pos);
		break;
	case 'c':
		SB_Copy (sb_pos);
		break;
	case 'v':
		SB_Say (sb_pos);
		break;
	}
};

void(float tab) SB_ShowTab =
{
	sb_tab = (tab + SB_TABS) % SB_TABS;
	localsound ("misc/menu1.wav");
};

// the tabs, where the page didn't take the key
void(float k) SB_TabKey =
{
	switch (k)
	{
	case K_PGUP:
	case K_LEFTARROW:
		SB_ShowTab (sb_tab - 1);
		break;
	case K_PGDN:
	case K_RIGHTARROW:
		SB_ShowTab (sb_tab + 1);
		break;
	case K_TAB:
		SB_ShowTab (sb_tab + (sb_shift ? -1 : 1));
		break;
	}
};

// a key that types nothing: the confirmation's, Escape, the popup's, the
// dialogs', the page's, the tabs'
void(float k) SB_Key =
{
	float	handled = FALSE;

	if (sb_dialog == SBD_CONFIRM)
	{
		SB_Confirm_Key (k, 0);
		return;
	}
	if (k == K_ESCAPE && sb_dialog == SBD_NONE && !sb_info)
	{
		M_Menu_Main_f ();
		return;
	}
	if (sb_info)
	{
		SB_Info_Key (k);
		return;
	}
	if (sb_dialog == SBD_ADDSOURCE)
	{
		SB_AddSource_Key (k);
		return;
	}
	if (sb_dialog == SBD_ADDSERVER)
	{
		SB_AddServer_Key (k);
		return;
	}
	switch (sb_tab)
	{
	case SB_SERVERS:
		handled = SB_Servers_Key (k);
		break;
	case SB_SOURCES:
		handled = SB_Sources_Key (k);
		break;
	case SB_PLAYERS:
		handled = SB_Players_Key (k);
		break;
	case SB_OPTIONS:
		handled = SB_Options_Key (k);
		if (k == K_LEFTARROW || k == K_RIGHTARROW || k == K_PGUP || k == K_PGDN)
			handled = TRUE;
		break;
	}
	if (!handled)
		SB_TabKey (k);
};

// a key that types: a character, or with Ctrl or Alt the key's own
void(float c) SB_Typed =
{
	if (sb_dialog == SBD_CONFIRM)
	{
		SB_Confirm_Key (0, c);
		return;
	}
	if (sb_info)
	{
		SB_Info_Char (c);
		return;
	}
	if (sb_dialog == SBD_ADDSOURCE)
	{
		SB_AddSource_Char (c);
		return;
	}
	if (sb_dialog == SBD_ADDSERVER)
	{
		if (as_pos == 0 && !sb_ctrl && !sb_alt)
			SB_EditChar (0, c);
		return;
	}
	switch (sb_tab)
	{
	case SB_SERVERS:
		SB_Servers_Char (c);
		break;
	case SB_SOURCES:
		if (c == ' ' || (!sb_ctrl && !sb_alt))
			SB_Sources_Char (c);
		break;
	case SB_PLAYERS:
		SB_Players_Char (c);
		break;
	case SB_OPTIONS:
		SB_Options_Char (c);
		break;
	}
};

void(float scan, float chr) SB_KeyDown =
{
	if (scan == K_CTRL)
		sb_ctrl = TRUE;
	else if (scan == K_ALT)
		sb_alt = TRUE;
	else if (scan == K_SHIFT)
		sb_shift = TRUE;

	// the characters typed come on their own (the layout's); with Ctrl or
	// Alt, which type none, the key is the character
	if (!scan)
	{
		if (!sb_ctrl && !sb_alt)
			SB_Typed (chr);
	}
	else if (scan >= ' ' && scan < 127)
	{
		if (sb_ctrl || sb_alt)
			SB_Typed (scan);
	}
	else
		SB_Key (scan);
};

void(float scan) SB_KeyUp =
{
	if (scan == K_CTRL)
		sb_ctrl = FALSE;
	else if (scan == K_ALT)
		sb_alt = FALSE;
	else if (scan == K_SHIFT)
		sb_shift = FALSE;
};

void() SB_Open =
{
	setkeydest (KEY_MENU);
	m_state = M_SLIST;
	m_entersound = TRUE;
	sb_ctrl = sb_alt = sb_shift = FALSE;
	sb_info = FALSE;
	sb_dialog = SBD_NONE;
	SB_Keys ();
	sb_settings = "";
	// the cache shows at once, and the first time it opens the list is scanned,
	// where it wasn't as the client started (sb_fetch_on_startup)
	if (!sb_scanned)
	{
		sb_scanned = TRUE;
		if (!gethostcachevalue (SLIST_SCANS))
			refreshhostcache ();
	}
};

// the browser on the console's background (out of a game), else on ezQuake's
// fade: its window min (640, w) - 8 wide, its tabs, a line, and the page
void(vector size) SB_Draw =
{
	float	x, y, w, h, i, cx = 0, ww;
	string	label;

	sb_vw = size_x;
	sb_vh = size_y;
	if (clientstate () == 2)
		drawfill ('0 0 0', size, '0 0 0', 0.7, 0);
	else
		Backdrop_Draw (size);
	SB_Keys ();
	SB_CheckSettings ();
	SB_Follow (gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT));
	if (sb_pending != "")
	{
		if ((i = gethostcacheindexforaddress (sb_pending)) >= 0)
		{
			SB_Select (i);
			SB_Info_Open ();
			sb_pending = "";
		}
		else if (time - sb_pendingtime > 10)
			sb_pending = "";
	}
	if (sb_info && cvar ("sb_liveupdate") > 0 && time >= sb_infoupdate)
	{
		refreshhostcacheentry (sb_pos);
		sb_infoupdate = time + cvar ("sb_liveupdate");
	}

	w = min (640, sb_vw) - 8;
	h = min (480, sb_vh) - 8;
	x = floor ((sb_vw - w) / 2);
	y = floor ((sb_vh - h) / 2);
	for (i = 0 ; i < SB_TABS ; i++)
	{
		label = sb_tabnames[i];
		ww = (strlen (label) + 2) * 8;
		if (i == sb_tab)
			SB_Print (x + cx, y, strcat (chr2str (0x10), label, chr2str (0x11)), TRUE);
		else
			SB_Print (x + cx, y, strcat (" ", label, " "), FALSE);
		cx += ww - 8;
	}
	SB_Line (x + SB_Trunc ((w - 8 * floor (w / 8)) / 2), y + 8, floor (w / 8));
	y += 16;
	h -= 16;
	switch (sb_tab)
	{
	case SB_SERVERS:
		SB_Servers_Draw (x, y, w, h);
		break;
	case SB_SOURCES:
		SB_Sources_Draw (x, y, w, h);
		break;
	case SB_PLAYERS:
		SB_Players_Draw (x, y, w, h);
		break;
	case SB_OPTIONS:
		SB_Options_Draw (x, y, w, h);
		break;
	}
	if (sb_info)
		SB_Info_Draw ();
	if (sb_dialog == SBD_ADDSERVER)
		SB_AddServer_Draw ();
	else if (sb_dialog == SBD_ADDSOURCE)
		SB_AddSource_Draw ();
	else if (sb_dialog == SBD_CONFIRM)
		SB_Confirm_Draw ();
};
