// sb.qc -- the server browser, ezQuake's (EX_browser.c), over the server list
// the engine keeps (FTE's hostcache builtins, and SoftWorld's): its state, the
// servers as the list has them, how the list is filtered and sorted, what is
// done with a server, and the parts its pages share. The pages draw it
// (sb_servers, sb_sources, sb_players, sb_options), with the info popup
// (sb_info) and the dialogs (sb_dialogs) over them; sb_browser.qc puts them
// together. The list fills as the scan finds it, its progress in the status
// bar, rather than behind ezQuake's box; the selection stays on its server as
// the list changes under it. ezQuake's Create tab, the connection test,
// sb_info_filter and the mouse aren't here.

#define	SB_SEARCHTIME	3		// seconds a search shows after its last key
#define	SB_MAXPLAYERS	1024	// on the players' page
#define	SB_MAXROSTER	64		// a server's, in the info popup

// the pages, and what is over them
enum { SB_SERVERS, SB_SOURCES, SB_PLAYERS, SB_OPTIONS, SB_TABS };
enum { SBO_NONE, SBO_INFO, SBO_ADDSERVER, SBO_ADDSOURCE, SBO_CONFIRM, SBO_COUNT };

float	sb_tab;
float	sb_overlay;
float	sb_scanned;					// the browser asked for its first scan
float	sb_ctrl, sb_alt, sb_shift;	// held
float	sb_vw, sb_vh;				// the layout's size

void() SB_CloseOverlay =
{
	sb_overlay = SBO_NONE;
};

/*
==============================================================================

THE PAGES' PARTS

==============================================================================
*/

// a list's cursor, and its first row shown
typedef struct
{
	float	pos, disp;
} sblist_t;

// a page's rows under its header, the status bar's three lines kept clear
float(float h) SB_Rows =
{
	return floor (h / 8) - (cvar ("sb_status") ? 3 : 0) - 1;
};

// ezQuake's scrolling: the cursor in the rows, the rows full
void(sblist_t *l, float n, float rows) SB_Scroll =
{
	if (l->disp < 0)
		l->disp = 0;
	if (l->pos > l->disp + rows - 1)
		l->disp = l->pos - rows + 1;
	if (l->disp > n - rows)
		l->disp = max (n - rows, 0);
	if (l->pos < l->disp)
		l->disp = l->pos;
};

// the cursor moved by a key of n rows: a row, ten, to an end, or the wheel's;
// whether it was one
float(sblist_t *l, float k, float n, float wheel) SB_ListKey =
{
	switch (k)
	{
	case K_UPARROW:		l->pos -= 1; break;
	case K_DOWNARROW:	l->pos += 1; break;
	case K_MWHEELUP:	l->pos -= wheel; break;
	case K_MWHEELDOWN:	l->pos += wheel; break;
	case K_PGUP:		l->pos -= 10; break;
	case K_PGDN:		l->pos += 10; break;
	case K_HOME:		l->pos = 0; break;
	case K_END:			l->pos = n - 1; break;
	default:			return FALSE;
	}
	l->pos = max (0, min (l->pos, n - 1));
	return TRUE;
};

// what is typed to search for, shown SB_SEARCHTIME after its last key
typedef struct
{
	string	text;
	float	time;
} sbsearch_t;

float(sbsearch_t *s) SB_Searching =
{
	return s->text != "" && time - s->time < SB_SEARCHTIME;
};

// a character more, or the first of a search once the last went quiet
void(sbsearch_t *s, float c) SB_SearchAdd =
{
	if (time - s->time >= SB_SEARCHTIME)
		s->text = "";
	s->text = substring (strcat (s->text, chr2str (Lower (c))), 0, 20);
	s->time = time;
};

// a before b by a sort string's digits (ezQuake's sb_sort*), compare's for
// each in turn; a '-' turns those after it about, where they can be
float(string sort, float a, float b, float(float digit, float a, float b) compare, float reversible) SB_SortBefore =
{
	float	i, c, d, reverse = FALSE;

	for (i = 0 ; i < strlen (sort) ; i++)
	{
		c = str2chr (sort, i);
		if (c == '-')
		{
			reverse = reversible;
			continue;
		}
		d = compare (c, a, b);
		if (d)
			return reverse ? d > 0 : d < 0;
	}
	return a < b;
};

// order[from, to) sorted by before, stably, by halves through merge
void(float *order, float *merge, float from, float to, float(float a, float b) before) SB_Sort =
{
	float	mid = floor ((from + to) / 2), i, j, k, left;

	if (to - from < 2)
		return;
	SB_Sort (order, merge, from, mid, before);
	SB_Sort (order, merge, mid, to, before);
	for (i = from, j = mid, k = from ; k < to ; k++)
	{
		// the left half's next unless the right's comes before it (fteqcc's
		// || doesn't stop where it is decided)
		left = j >= to;
		if (!left && i < mid)
			left = !before (order[j], order[i]);
		if (left)
			merge[k] = order[i++];
		else
			merge[k] = order[j++];
	}
	for (k = from ; k < to ; k++)
		order[k] = merge[k];
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

// the address where it has no name
string(float i) SB_NameOrAddress =
{
	string	name = SB_Get (key_name, i);

	return name != "" ? name : SB_Get (key_address, i);
};

// the name a row shows: a qwfwd's as a proxy's
string(float i) SB_ServerName =
{
	return SB_IsQwfwd (i) ? strcat ("proxy ", SB_NameOrAddress (i)) : SB_NameOrAddress (i);
};

string(float i) SB_Gamedir =
{
	string	gamedir = SB_Get (key_gamedir, i);

	if (gamedir == "" && SB_Get (key_progs, i) == "666")
		return "qizmo";
	return gamedir;
};

// what the servers' columns show of a server, and the colours they show it in
string(float i) SB_AddressText = { return SB_Get (key_address, i); };
string(float i) SB_MapText = { return substring (SB_Get (key_map, i), 0, 8); };
string(float i) SB_GamedirText = { return substring (SB_Gamedir (i), 0, 8); };

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

// a limit in its two characters
string(float v) SB_Limit =
{
	string	s = ftos (v);

	return PadRight (strlen (s) > 2 ? "99" : s, 2);
};

string(float i) SB_FraglimitText = { return SB_Limit (SB_Num (key_fraglimit, i)); };
string(float i) SB_TimelimitText = { return SB_Limit (SB_Num (key_timelimit, i)); };

// a player as the list gives player<N> (FTE's spelling, its team after):
// userid frags time ping "name" "skin" 'top' 'bottom' "team", the colours
// vectors a word each to tokenize
typedef struct
{
	float	frags, time, ping;
	float	spec;				// frags -9999
	string	name, team;
	vector	top, bottom;		// 0 to 1
} player_t;

// server i's player r into p; FALSE past its last
float(float i, float r, player_t *p) SB_ReadPlayer =
{
	string	text = SB_Get (key_player0 + r, i);

	if (text == "")
		return FALSE;
	tokenize (text);
	p->frags = stof (argv (1));
	p->time = stof (argv (2));
	p->ping = stof (argv (3));
	p->spec = p->frags == -9999;
	p->name = argv (4);
	p->top = stov (argv (6));
	p->bottom = stov (argv (7));
	p->team = argv (8);
	return TRUE;
};

/*
==============================================================================

THE SELECTION

==============================================================================
*/

sblist_t	sb_list;				// the servers' page's
float		sb_listsize;			// its rows
string		sb_seladdr;				// the selected server's address, which it follows
sbsearch_t	sb_search;				// the servers whose names have it
string		sb_pending;				// a server added, selected once it is listed
float		sb_pendingtime;

// the selection on its server as the list moves under it
void(float n) SB_Follow =
{
	float	i;

	if (sb_seladdr != "" && (i = gethostcacheindexforaddress (sb_seladdr)) >= 0)
		sb_list.pos = i;
	sb_list.pos = max (0, min (sb_list.pos, n - 1));
	sb_seladdr = n ? SB_Get (key_address, sb_list.pos) : "";
};

void(float i) SB_Select =
{
	float	n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT);

	sb_list.pos = max (0, min (i, n - 1));
	sb_seladdr = n ? SB_Get (key_address, sb_list.pos) : "";
};

/*
==============================================================================

THE VIEW: ITS FILTERS AND ITS SORT

==============================================================================
*/

string	sb_settings;		// the filters and sort the view was arranged with
float	sb_serial;			// one more each arrange

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
	if (sb_search.text != "")
		sethostcachemaskstring (SLIST_MASK_AND, key_name, sb_search.text, SLIST_TEST_CONTAINS);
	if (!proxies)
		sethostcachemasknumber (SLIST_MASK_AND, key_isproxy, 0, SLIST_TEST_EQUAL);
	else if (proxies == 2)
		sethostcachemasknumber (SLIST_MASK_AND, key_isproxy, 0, SLIST_TEST_NOTEQUAL);
	// QTV's relays (QTV, QTVGO) by what they answer: SoftWorld's, ezQuake lists them
	if (cvar ("sb_hideqtv"))
		sethostcachemaskstring (SLIST_MASK_AND, key_version, "QTV", SLIST_TEST_NOTSTARTSWITH);
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

#define	SB_ARRANGEDBY	9

// the cvars the view is arranged by
string	sb_arrangedby[SB_ARRANGEDBY] = {"sb_sortservers", "sb_hideempty", "sb_hidenotempty", "sb_hidefull",
	"sb_hidedead", "sb_hidehighping", "sb_pinglimit", "sb_showproxies", "sb_hideqtv"};

// arranged again when what arranges it changed (the console's cvars too)
void() SB_CheckSettings =
{
	string	s = sb_search.text;
	float	i;

	for (i = 0 ; i < SB_ARRANGEDBY ; i++)
		s = strcat (s, " ", cvar_string (sb_arrangedby[i]));
	if (s == sb_settings)
		return;
	sb_settings = s;
	SB_Arrange ();
};

/*
==============================================================================

WHAT IS DONE WITH A SERVER

==============================================================================
*/

// joined, or watched; the browser closes after (sb_autohide), but for qizmo
void(float i, float spectate) SB_Connect =
{
	string	address = SB_Get (key_address, i);
	float	autohide = cvar ("sb_autohide");

	if (address == "")
		return;
	localcmd (sprintf ("spectator %s\nconnect %s\n", spectate ? "1" : "\"\"", address));
	if (autohide > 1 || (autohide && SB_Gamedir (i) != "qizmo"))
		M_Close ();
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
		M_Close ();
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

// the letters a server takes on each page: j, p join it, o, s watch it, c
// copies its address, v says it; whether c was one
float(float i, float c) SB_ServerCommand =
{
	switch (Lower (c))
	{
	case 'j':
	case 'p':
		SB_Connect (i, FALSE);
		return TRUE;
	case 'o':
	case 's':
		SB_Connect (i, TRUE);
		return TRUE;
	case 'c':
		SB_Copy (i);
		return TRUE;
	case 'v':
		SB_Say (i);
		return TRUE;
	}
	return FALSE;
};
