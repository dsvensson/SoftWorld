// hub.qc -- hub.quakeworld.nu's games, as its games page lists them: through
// its API (hub_api_url, a Supabase table PostgREST answers, with hub_api_key),
// newest first, filtered as the page's toolbar filters them (the mode, how
// long ago, players, teams, the map, the matchtag, the server), a page at a
// time of as many as the screen has room for. Enter plays a game's demo from
// where the hub keeps them (hub_demo_url), as it downloads. The scores are
// hidden, as the page hides them, unless hub_showscores (s) shows them.

#define	HUB_MAXROWS		50
#define	HUB_MODES		6
#define	HUB_SPANS		6
#define	HUB_FIELDS		5
#define	HUB_FIELDMAX	64
#define	HUB_DATEWIDTH	5		// "02:48" today, else "10-17"
#define	HUB_COLUMNS		3

// json_get_value_type's
#define	JSON_STRING		0
#define	JSON_ARRAY		3

// what has the keys: the list, then the choices and the fields as they are drawn
enum { HF_LIST, HF_MODE, HF_SPAN, HF_PLAYERS, HF_TEAMS, HF_MAP, HF_MATCHTAG, HF_SERVER, HF_COUNT };

// where the cursor goes on the page a request brings
enum { HC_TOP, HC_BOTTOM, HC_KEEP };

typedef struct
{
	string	date;			// its day, or its time today
	string	when;			// its day and time
	string	mode, map, vs, score, server, tag;
	string	sha;			// its demo's sha256, "" for a game without one
} hubrow_t;

hubrow_t	hub_rows[HUB_MAXROWS];
float		hub_count;				// the page's games
float		hub_page;				// the page shown, from 0
float		hub_pagesize;			// the games it was asked for in
var float	hub_total = -1;			// the games the filters find, -1 unknown
float		hub_rowsfit;			// rows the list has room for, as last drawn
sblist_t	hub_list;
float		hub_focus;
float		hub_inited;
editbox_t	hub_fields[HUB_FIELDS];
string	hub_fieldnames[HUB_FIELDS] = {"players", "teams", "map", "matchtag", "server"};
string	hub_modenames[HUB_MODES] = {"All", "1on1", "2on2", "4on4", "Wipeout", "CTF"};
string	hub_modes[HUB_MODES] = {"", "1on1", "2on2", "4on4", "wipeout", "ctf"};		// hub_mode's
string	hub_spannames[HUB_SPANS] = {"Last day", "Last week", "Last month", "Last 3 months", "Last year", "All time"};
float	hub_spans[HUB_SPANS] = {1, 7, 30, 91, 365, 0};		// hub_timeframe's

typedef struct
{
	string	name;
	float	width;
} hubcolumn_t;

// right of the players or teams, right to left as they make room
hubcolumn_t	hub_columns[HUB_COLUMNS] = {{"mode", 7}, {"map", 8}, {"score", 9}};
float		hub_colpos[HUB_COLUMNS];	// a column's first character, -1 for one not shown
float		hub_vswidth;				// the players' or teams', what is left

float		hub_req;				// the request on its way, 0 none
float		hub_reqpage, hub_reqsize, hub_reqcursor;
string		hub_applied;			// the settings the last request was made with
string		hub_error;				// why the last failed, "" none
string		hub_notice;				// a word to the player, until the next key
float		hub_tzofs;				// minutes local time is ahead of UTC
float		hub_today;				// today, local time, as a day number
float		cal_year, cal_month, cal_day;	// Civil_FromDays'

/*
==============================================================================

THE CALENDAR

Days since 1970-01-01 to and from the civil calendar (Howard Hinnant's), as
strftime has only now; the numbers stay far below a float's 2^24

==============================================================================
*/

float(float y, float m, float d) Days_FromCivil =
{
	float	era, yoe, doy, doe;

	if (m <= 2)
		y -= 1;
	era = floor (y / 400);
	yoe = y - era * 400;
	doy = floor ((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + d - 1;
	doe = yoe * 365 + floor (yoe / 4) - floor (yoe / 100) + doy;
	return era * 146097 + doe - 719468;
};

// into cal_year, cal_month and cal_day
void(float z) Civil_FromDays =
{
	float	era, doe, yoe, doy, mp;

	z += 719468;
	era = floor (z / 146097);
	doe = z - era * 146097;
	yoe = floor ((doe - floor (doe / 1460) + floor (doe / 36524) - floor (doe / 146096)) / 365);
	doy = doe - (365 * yoe + floor (yoe / 4) - floor (yoe / 100));
	mp = floor ((5 * doy + 2) / 153);
	cal_day = doy - floor ((153 * mp + 2) / 5) + 1;
	cal_month = mp < 10 ? mp + 3 : mp - 9;
	cal_year = yoe + era * 400 + (cal_month <= 2 ? 1 : 0);
};

// today, and how far local time is from UTC
void() Hub_Clock =
{
	float	today, minutes;

	tokenize (strftime (1, "%Y %m %d %H %M"));
	today = Days_FromCivil (stof (argv (0)), stof (argv (1)), stof (argv (2)));
	minutes = stof (argv (3)) * 60 + stof (argv (4));
	tokenize (strftime (0, "%Y %m %d %H %M"));
	hub_tzofs = (today - Days_FromCivil (stof (argv (0)), stof (argv (1)), stof (argv (2)))) * 1440 + minutes
		- stof (argv (3)) * 60 - stof (argv (4));
	hub_today = today;
};

// the moment so many days ago, as PostgREST compares times: UTC, ISO 8601
// (tokenize takes a ':' for a word of its own)
string(float days) Hub_Since =
{
	tokenize (strftime (0, "%Y %m %d %H %M %S"));
	Civil_FromDays (Days_FromCivil (stof (argv (0)), stof (argv (1)), stof (argv (2))) - days);
	return strcat (sprintf ("%04d-%02d-%02dT", cal_year, cal_month, cal_day),
		sprintf ("%02d:%02d:%02dZ", stof (argv (3)), stof (argv (4)), stof (argv (5))));
};

float	hub_isoday, hub_isominutes;		// Hub_ISOTime's

// an ISO 8601 time ("2026-10-17T02:48:30+00:00") in local time, into
// hub_isoday and hub_isominutes; false where it isn't one
float(string iso) Hub_ISOTime =
{
	float	days, minutes, zone = 0, k = 19, c;

	if (strlen (iso) < 16)
		return FALSE;
	// past the seconds' fraction, the zone: Z, or +hh:mm or -hh:mm
	for (c = str2chr (iso, k) ; c == '.' || (c >= '0' && c <= '9') ; c = str2chr (iso, k))
		k++;
	if (c == '+' || c == '-')
		zone = (c == '-' ? -1 : 1) * (stof (substring (iso, k + 1, 2)) * 60 + stof (substring (iso, k + 4, 2)));
	days = Days_FromCivil (stof (substring (iso, 0, 4)), stof (substring (iso, 5, 2)), stof (substring (iso, 8, 2)));
	minutes = stof (substring (iso, 11, 2)) * 60 + stof (substring (iso, 14, 2)) - zone + hub_tzofs;
	hub_isoday = days + floor (minutes / 1440);
	hub_isominutes = minutes - floor (minutes / 1440) * 1440;
	return TRUE;
};

// a time's day and time as the browser's sources have theirs: 10-17 02:48,
// and today's 02:48
string(float day, float minutes) Hub_When =
{
	string	hm = sprintf ("%02d:%02d", floor (minutes / 60), minutes - floor (minutes / 60) * 60);

	if (day == hub_today)
		return hm;
	Civil_FromDays (day);
	return sprintf ("%02d-%02d %s", cal_month, cal_day, hm);
};

// in a row: today's time, another day's day
string(float day, float minutes) Hub_Day =
{
	if (day == hub_today)
		return sprintf ("%02d:%02d", floor (minutes / 60), minutes - floor (minutes / 60) * 60);
	Civil_FromDays (day);
	return sprintf ("%02d-%02d", cal_month, cal_day);
};

/*
==============================================================================

THE FILTERS

==============================================================================
*/

// the mode tab's, 0 for all (and for one it doesn't name)
float() Hub_ModeIndex =
{
	string	mode = strtolower (cvar_string ("hub_mode"));
	float	i;

	for (i = 0 ; i < HUB_MODES ; i++)
		if (hub_modes[i] == mode)
			return i;
	return 0;
};

string() Hub_ModeName =
{
	string	mode = cvar_string ("hub_mode");

	if (mode != "" && !Hub_ModeIndex ())
		return mode;		// one of the console's
	return hub_modenames[Hub_ModeIndex ()];
};

// the time's, the nearest of them for a day count the console set
float() Hub_SpanIndex =
{
	float	days = cvar ("hub_timeframe"), i;

	if (days <= 0)
		return HUB_SPANS - 1;
	for (i = 0 ; i < HUB_SPANS - 1 ; i++)
		if (hub_spans[i] >= days)
			return i;
	return HUB_SPANS - 2;
};

string() Hub_SpanName =
{
	float	days = cvar ("hub_timeframe");

	if (days > 0 && hub_spans[Hub_SpanIndex ()] != days)
		return sprintf ("Last %d days", days);
	return hub_spannames[Hub_SpanIndex ()];
};

// the mode or the time on by one, or back
void(float d) Hub_Step =
{
	if (hub_focus == HF_MODE)
		cvar_set ("hub_mode", hub_modes[Wrap (Hub_ModeIndex () + d, HUB_MODES)]);
	else if (hub_focus == HF_SPAN)
		cvar_set ("hub_timeframe", ftos (hub_spans[Wrap (Hub_SpanIndex () + d, HUB_SPANS)]));
};

// the field that has the keys, if one does
editbox_t*() Hub_Edit =
{
	if (hub_focus < HF_PLAYERS || hub_focus >= HF_COUNT)
		return __NULL__;
	return &hub_fields[hub_focus - HF_PLAYERS];
};

string(string s) Hub_Trim =
{
	float	a = 0, b = strlen (s);

	while (a < b && str2chr (s, a) == ' ')
		a++;
	while (b > a && str2chr (s, b - 1) == ' ')
		b--;
	return substring (s, a, b - a);
};

// players' names as the hub searches them: 'a' & 'b', all of them in a game
string(string text) Hub_FTS =
{
	float	n = tokenizebyseparator (text, " "), k;
	string	out = "", word;

	for (k = 0 ; k < n ; k++)
	{
		word = strreplace ("'", "", argv (k));
		if (word == "")
			continue;
		out = strcat (out, out == "" ? "" : " & ", "'", word, "'");
	}
	return out;
};

// teams' names as the hub's arrays have them: {"a","b"}, lower case
string(string text) Hub_Set =
{
	float	n = tokenizebyseparator (strtolower (text), " "), k;
	string	out = "", word;

	for (k = 0 ; k < n ; k++)
	{
		word = strreplace ("\"", "", strreplace ("\\", "", argv (k)));
		if (word == "")
			continue;
		out = strcat (out, out == "" ? "" : ",", "\"", word, "\"");
	}
	return out == "" ? "" : strcat ("{", out, "}");
};

// what the games are asked by: when it changes, the first page is asked again
string() Hub_Settings =
{
	return strcat (strcat (cvar_string ("hub_api_url"), "\n", cvar_string ("hub_api_key"), "\n", cvar_string ("hub_mode"),
		"\n", cvar_string ("hub_timeframe")), "\n", strcat (hub_fields[0].text, "\n", hub_fields[1].text, "\n",
		hub_fields[2].text, "\n", hub_fields[3].text, "\n"), hub_fields[4].text);
};

// a page of size games, as PostgREST asks: the columns, newest first, the
// filters each an operator on a column
string(float page, float size) Hub_URL =
{
	string	url, f, mode = strtolower (cvar_string ("hub_mode"));
	float	days = cvar ("hub_timeframe");

	url = strcat (cvar_string ("hub_api_url"), "?select=id,timestamp,mode,matchtag,map,teams,players,demo_sha256,",
		"server_hostname&order=timestamp.desc", sprintf ("&offset=%d&limit=%d", page * size, size));
	if (mode != "")
		url = strcat (url, "&mode=eq.", uri_escape (mode));
	if (days > 0)
		url = strcat (url, "&timestamp=gte.", uri_escape (Hub_Since (days)));
	f = Hub_FTS (hub_fields[0].text);
	if (f != "")
		url = strcat (url, "&players_fts=fts.", uri_escape (f));
	f = Hub_Set (hub_fields[1].text);
	if (f != "")
		url = strcat (url, "&team_names=cs.", uri_escape (f));
	f = strtolower (Hub_Trim (hub_fields[2].text));
	if (f != "")
		url = strcat (url, "&map=eq.", uri_escape (f));
	f = Hub_Trim (hub_fields[3].text);
	if (f != "")
		url = strcat (url, "&matchtag=ilike.", uri_escape (strcat ("*", f, "*")));
	f = Hub_Trim (hub_fields[4].text);
	if (f != "")
		url = strcat (url, "&server_hostname=ilike.", uri_escape (strcat ("*", f, "*")));
	return url;
};

/*
==============================================================================

THE REQUESTS

One at a time: a newer one drops the one on its way

==============================================================================
*/

// a page asked for, the settings it is asked with remembered
void(float page, float cursor) Hub_Fetch =
{
	if (hub_req)
		webget_close (hub_req);
	hub_req = 0;
	hub_applied = Hub_Settings ();
	hub_error = "";
	if (cvar_string ("hub_api_url") == "")
	{
		hub_error = "There's no hub to ask: hub_api_url is empty";
		return;
	}
	hub_reqpage = page;
	hub_reqsize = bound (1, hub_rowsfit, HUB_MAXROWS);
	hub_reqcursor = cursor;
	// with the count of the games the filters find, in Content-Range
	hub_req = webget (Hub_URL (page, hub_reqsize), strcat ("apikey: ", cvar_string ("hub_api_key"),
		"\nPrefer: count=exact"));
	if (!hub_req)
		hub_error = "The request couldn't be made";
};

// a string of an object's, copied out of the document json_free frees
string(jsonnode node, string key) Hub_Str =
{
	jsonnode	c = json_find_object_child (node, key);

	if (!c || json_get_value_type (c) != JSON_STRING)
		return "";
	return strcat (json_get_string (c), "");
};

// why the hub didn't answer with games: PostgREST's message
string(float status, string body) Hub_Refused =
{
	jsonnode	root = json_parse (body);
	string		message = "";

	if (root)
	{
		message = Hub_Str (root, "message");
		json_free (root);
	}
	if (message == "")
		return sprintf ("The hub answered %d", status);
	return sprintf ("The hub answered %d: %s", status, message);
};

// a page of games, and how many there are (Content-Range: 0-14/940)
void(string body, string range) Hub_Parse =
{
	jsonnode	root = json_parse (body), row, sides, side;
	int			n, i, ns;
	string		a, b, vs;
	float		fa, fb, at, slash, pos;

	hub_count = 0;
	if (!root)
	{
		hub_error = "The hub's answer isn't JSON";
		return;
	}
	if (json_get_value_type (root) != JSON_ARRAY)
	{
		hub_error = strcat ("The hub answered: ", Hub_Str (root, "message"));
		json_free (root);
		return;
	}
	Hub_Clock ();
	n = json_get_length (root);
	for (i = 0 ; i < n ; i++)
	{
		if (hub_count >= HUB_MAXROWS)
			break;
		row = json_get_child_at_index (root, i);
		at = hub_count;
		hub_rows[at].date = "";
		hub_rows[at].when = "";
		if (Hub_ISOTime (Hub_Str (row, "timestamp")))
		{
			hub_rows[at].date = Hub_Day (hub_isoday, hub_isominutes);
			hub_rows[at].when = Hub_When (hub_isoday, hub_isominutes);
		}
		hub_rows[at].mode = Hub_Str (row, "mode");
		hub_rows[at].map = Hub_Str (row, "map");
		hub_rows[at].server = Hub_Str (row, "server_hostname");
		hub_rows[at].tag = Hub_Str (row, "matchtag");
		hub_rows[at].sha = Hub_Str (row, "demo_sha256");

		// the sides: the teams, or the players where there are none (duels)
		sides = json_find_object_child (row, "teams");
		if (json_get_length (sides) < 2)
			sides = json_find_object_child (row, "players");
		ns = json_get_length (sides);
		a = "";
		b = "";
		fa = 0;
		fb = 0;
		if (ns > 0)
		{
			side = json_get_child_at_index (sides, 0);
			a = Hub_Str (side, "name");
			fa = json_get_float (json_find_object_child (side, "frags"));
		}
		if (ns > 1)
		{
			side = json_get_child_at_index (sides, 1);
			b = Hub_Str (side, "name");
			fb = json_get_float (json_find_object_child (side, "frags"));
		}
		vs = a;
		if (ns > 1)
			vs = strcat (a, " vs ", b);
		if (ns > 2)
			vs = sprintf ("%s +%d", vs, (float)(ns - 2));
		hub_rows[at].vs = vs;
		hub_rows[at].score = ns == 2 ? sprintf ("%d : %d", fa, fb) : "";
		hub_count++;
	}
	json_free (root);

	hub_total = -1;
	slash = strstrofs (range, "/");
	if (slash >= 0)
		if (substring (range, slash + 1, 1) != "*")
			hub_total = stof (substring (range, slash + 1, 99));
	hub_page = hub_reqpage;
	hub_pagesize = hub_reqsize;
	pos = 0;
	if (hub_reqcursor == HC_BOTTOM)
		pos = hub_count - 1;
	else if (hub_reqcursor == HC_KEEP)
		pos = hub_list.pos;
	hub_list.pos = max (0, min (pos, hub_count - 1));
	hub_list.disp = 0;
};

// each frame: the first page asked again where the filters changed (not while
// a field is typed in), and the request on its way taken when it has come
void() Hub_Update =
{
	float	status, again = FALSE;

	if (hub_focus < HF_PLAYERS)
		if (Hub_Settings () != hub_applied)
			Hub_Fetch (0, HC_TOP);
	if (!hub_req)
		return;
	status = webget_status (hub_req);
	if (!status)
		return;
	if (status >= 200 && status < 300)		// a page of the count is a 206
		Hub_Parse (webget_body (hub_req), webget_header (hub_req, "Content-Range"));
	else if (status == 416 && hub_reqpage > 0)
		again = TRUE;						// past the end: the count shrank
	else if (status < 0)
		hub_error = webget_error (hub_req);
	else
		hub_error = Hub_Refused (status, webget_body (hub_req));
	webget_close (hub_req);
	hub_req = 0;
	if (again)
		Hub_Fetch (0, HC_TOP);
};

// the page on, or back, with the cursor on its first row or its last
void(float d) Hub_Page =
{
	float	from = hub_page, size = hub_pagesize, p;

	if (hub_req)
	{	// on from the one on its way
		from = hub_reqpage;
		size = hub_reqsize;
	}
	p = from + d;
	if (p < 0 || size <= 0)
		return;
	if (hub_total >= 0 && p * size >= hub_total)
		return;
	localsound ("misc/menu1.wav");
	Hub_Fetch (p, d < 0 ? HC_BOTTOM : HC_TOP);
};

string(float i) Hub_DemoURL =
{
	string	sha = hub_rows[i].sha;

	return sprintf ("%s/%s/%s.mvd.gz", cvar_string ("hub_demo_url"), substring (sha, 0, 3), sha);
};

void(float i) Hub_Play =
{
	if (i < 0 || i >= hub_count)
		return;
	if (hub_rows[i].sha == "")
	{
		hub_notice = "The hub has no demo of this game";
		return;
	}
	localcmd (sprintf ("playdemo \"%s\"\n", Hub_DemoURL (i)));
	M_Close ();
};

void(float i) Hub_Copy =
{
	if (i < 0 || i >= hub_count || hub_rows[i].sha == "")
		return;
	clipboard_set (0, Hub_DemoURL (i));
	hub_notice = "The demo's URL is on the clipboard";
};

/*
==============================================================================

THE SCREEN

==============================================================================
*/

// a control's name, red with an arrow while it has the keys
void(float x, float y, float f, string label) Hub_Label =
{
	UI_Print (x, y, label, hub_focus == f);
	if (hub_focus == f)
		UI_Char (x + 72, y, 13);
};

// a choice: bracketed and red while it has the keys
void(float x, float y, string text, float active) Hub_Choice =
{
	if (active)
		UI_Print (x, y, strcat (chr2str (0x10), text, chr2str (0x11)), TRUE);
	else
		UI_Print (x + 8, y, text, FALSE);
};

// the filters, a line each: the mode, the time, the fields
void(float x, float y, float w) Hub_Filters =
{
	float	i, ly, width = max (8, min (floor (w / 8) - 11, 40));

	Hub_Label (x, y, HF_MODE, "mode");
	Hub_Choice (x + 80, y, Hub_ModeName (), hub_focus == HF_MODE);
	Hub_Label (x, y + 8, HF_SPAN, "when");
	Hub_Choice (x + 80, y + 8, Hub_SpanName (), hub_focus == HF_SPAN);
	for (i = 0 ; i < HUB_FIELDS ; i++)
	{
		ly = y + 16 + i * 8;
		Hub_Label (x, ly, HF_PLAYERS + i, hub_fieldnames[i]);
		hub_fields[i].width = width;
		UI_Fill (x + 80, ly, width * 8, 8, '40 40 40', 160);
		SB_EditDraw (&hub_fields[i], x + 80, ly, hub_focus == HF_PLAYERS + i);
	}
};

// each column from the right, the one before it a character apart, those
// with no room dropped (the score never); the players or teams have the rest
void(float w) Hub_Columns =
{
	float	pos = floor (w / 8), c;

	for (c = HUB_COLUMNS - 1 ; c >= 0 ; c--)
	{
		hub_colpos[c] = -1;
		if (c < HUB_COLUMNS - 1 && pos - hub_columns[c].width - 1 - (HUB_DATEWIDTH + 1) < 10)
			continue;
		pos -= hub_columns[c].width;
		hub_colpos[c] = pos;
		pos--;
	}
	hub_vswidth = pos - (HUB_DATEWIDTH + 1);
};

void(float x, float y, float w) Hub_Header =
{
	float	c;

	UI_Fill (x, y, w, 8, '10 10 10', 200);
	UI_Print (x, y, "date", TRUE);
	UI_Print (x + (HUB_DATEWIDTH + 1) * 8, y, substring ("players/teams", 0, hub_vswidth), TRUE);
	for (c = 0 ; c < HUB_COLUMNS ; c++)
		if (hub_colpos[c] >= 0)
			UI_PrintCenter (x + hub_colpos[c] * 8, y, hub_columns[c].width * 8, hub_columns[c].name, TRUE);
};

// a row's text: red where it is selected, gray for a game without a demo
void(float x, float y, string s, float selected, float playable) Hub_Text =
{
	if (selected || playable)
		UI_Print (x, y, s, selected);
	else
		drawstring ([x, y, 0], strcat ("&c777", s), '8 8 0', '1 1 1', 1, 0);
};

void(float x, float y, float w, float i, float selected) Hub_Row =
{
	float	vx = x + (HUB_DATEWIDTH + 1) * 8, c, playable = hub_rows[i].sha != "", cw;
	string	text;

	if (selected)
	{
		UI_GrayBox (x, y, w, 8);
		UI_Char (vx + 8 * hub_vswidth, y, UI_Cursor ());
	}
	else if (i & 1)
		UI_Fill (x, y, w, 8, '25 25 25', 125);
	else
		UI_Fill (x, y, w, 8, '50 50 50', 125);
	Hub_Text (x, y, hub_rows[i].date, selected, playable);
	Hub_Text (vx, y, substring (hub_rows[i].vs, 0, hub_vswidth), selected, playable);
	for (c = 0 ; c < HUB_COLUMNS ; c++)
	{
		if (hub_colpos[c] < 0)
			continue;
		if (c == 0)
			text = hub_rows[i].mode;
		else if (c == 1)
			text = hub_rows[i].map;
		else if (cvar ("hub_showscores") || hub_rows[i].score == "")
			text = hub_rows[i].score;
		else
			text = chr2str (0x85, 0x85);		// hidden: it would tell who won
		cw = hub_columns[c].width;
		text = substring (text, 0, cw);
		Hub_Text (x + hub_colpos[c] * 8 + Trunc ((cw - strlen (text)) / 2) * 8, y, text, selected, playable);
	}
};

// where the list is, what is going on or the game's server, and the keys
void(float x, float y, float w, float h) Hub_Status =
{
	float	cells = floor (w / 8), red = TRUE;
	string	count, line, keys;

	if (hub_total >= 0 && hub_pagesize > 0)
		count = sprintf ("page %d/%d, %d games", hub_page + 1, max (1, ceil (hub_total / hub_pagesize)), hub_total);
	else
		count = sprintf ("page %d", hub_page + 1);
	SB_CountLine (x, y + h - 24, w, count);

	if (hub_req)
		line = "Fetching...";
	else if (hub_error != "")
		line = hub_error;
	else if (hub_notice != "")
		line = hub_notice;
	else
	{
		red = FALSE;
		line = "";
		if (hub_count)
		{	// the game's day and time, its server, its matchtag
			line = strcat (hub_rows[hub_list.pos].when, " ", hub_rows[hub_list.pos].server);
			if (hub_rows[hub_list.pos].tag != "")
				line = strcat (line, " ", chr2str (0x85), " ", hub_rows[hub_list.pos].tag);
		}
	}
	UI_PrintCenter (x, y + h - 16, w, substring (line, 0, cells), red);

	if (hub_focus == HF_LIST)
		keys = "[enter] plays [pgdn] pages [tab] filters [s] scores";
	else if (hub_focus < HF_PLAYERS)
		keys = "[left/right] changes it [enter] back to the list";
	else
		keys = "type to filter [enter] back to the list";
	UI_PrintCenter (x, y + h - 8, w, substring (keys, 0, cells), FALSE);
};

void(vector size) Hub_Draw =
{
	float	x, y, w, h, rows, r;

	SB_Window (size);
	SB_Title ("Hub");
	x = sb_wx;
	y = sb_wy + 16;
	w = sb_ww;
	h = sb_wh - 16;

	// the filters, a line apart, the header, the rows, the status bar's three
	Hub_Filters (x, y, w);
	rows = max (1, floor (h / 8) - 12);
	hub_rowsfit = min (rows, HUB_MAXROWS);
	Hub_Update ();
	Hub_Columns (w);
	if (hub_count)
	{
		Hub_Header (x, y + 64, w);
		SB_Scroll (&hub_list, hub_count, rows);
		for (r = 0 ; r < rows && hub_list.disp + r < hub_count ; r++)
			Hub_Row (x, y + 72 + r * 8, w, hub_list.disp + r, hub_list.disp + r == hub_list.pos);
	}
	else if (!hub_req && hub_error == "")
	{
		UI_PrintCenter (x, y + 72, w, "No games found", FALSE);
		UI_PrintCenter (x, y + 88, w, "Change the filters above", TRUE);
	}
	Hub_Status (x, y, w, h);
};

/*
==============================================================================

THE KEYS

==============================================================================
*/

// the keys that aren't typed: Escape back to the list, or from it to the
// multiplayer menu; Tab through the list and the filters; on the list its
// moves, the pages and Enter to play; on a filter, Up and Down to the next,
// Left and Right to change a choice, a field's edits
void(float k) Hub_Key =
{
	editbox_t	*edit = Hub_Edit ();

	hub_notice = "";
	if (k == K_ESCAPE)
	{
		if (hub_focus == HF_LIST)
			M_Menu_Multiplayer_f ();
		else
			hub_focus = HF_LIST;
		return;
	}
	if (k == K_TAB)
	{
		hub_focus = Wrap (hub_focus + (sb_shift ? -1 : 1), HF_COUNT);
		return;
	}

	if (hub_focus == HF_LIST)
	{
		switch (k)
		{
		case K_UPARROW:
			if (hub_list.pos > 0)
				break;
			hub_focus = HF_COUNT - 1;		// above the list, the filters
			return;
		case K_PGUP:
		case K_LEFTARROW:
			Hub_Page (-1);
			return;
		case K_PGDN:
		case K_RIGHTARROW:
			Hub_Page (1);
			return;
		case K_ENTER:
			Hub_Play (hub_list.pos);
			return;
		}
		SB_ListKey (&hub_list, k, hub_count, 4);
		return;
	}

	switch (k)
	{
	case K_UPARROW:
		hub_focus = Wrap (hub_focus - 1, HF_COUNT);
		return;
	case K_DOWNARROW:
		hub_focus = Wrap (hub_focus + 1, HF_COUNT);
		return;
	case K_ENTER:
		hub_focus = HF_LIST;		// what was typed is asked for (Hub_Update)
		return;
	case K_LEFTARROW:
	case K_RIGHTARROW:
		if (!edit)
		{
			Hub_Step (k == K_RIGHTARROW ? 1 : -1);
			return;
		}
		break;
	}
	if (edit)
		SB_EditKey (edit, k);
};

// the typed keys: a field's text; else Space asks for the page again, s shows
// or hides the scores, c copies the demo's URL
void(float c) Hub_Char =
{
	editbox_t	*edit = Hub_Edit ();

	hub_notice = "";
	if (edit && !sb_ctrl && !sb_alt)
	{
		SB_EditChar (edit, c);
		return;
	}
	if (hub_focus == HF_MODE || hub_focus == HF_SPAN)
	{
		if (c == '+' || c == '=' || c == '-')
			Hub_Step (c == '-' ? -1 : 1);
		return;
	}
	switch (Lower (c))
	{
	case ' ':
		Hub_Fetch (hub_page, HC_KEEP);
		break;
	case 's':
		cvar_set ("hub_showscores", cvar ("hub_showscores") ? "0" : "1");
		break;
	case 'c':
		Hub_Copy (hub_list.pos);
		break;
	}
};

// the filters and the page as they were left; the first page is asked for as
// the screen first draws, in as many rows as it has room for
void() Hub_Open =
{
	float	i;

	M_Enter (M_HUB);
	if (!hub_inited)
	{
		hub_inited = TRUE;
		for (i = 0 ; i < HUB_FIELDS ; i++)
			SB_EditInit (&hub_fields[i], 40, HUB_FIELDMAX);
	}
	hub_notice = "";
};
