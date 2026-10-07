// sb_info.qc -- the server browser's info popup: the selected server's
// players, serverinfo and sources, kept fresh while it is up (sb_liveupdate)

float	sb_infopage;		// 0 players, 1 serverinfo, 2 sources
float	sb_infopos;			// the page's top line
float	sb_infoupdate;		// when the server is asked again

/*
==============================================================================

THE PLAYERS

==============================================================================
*/

// the server's players by frags, the spectators' -9999 last
player_t	roster[SB_MAXROSTER];
float		roster_count;

void(float i) SB_ReadRoster =
{
	player_t	p;
	float		j;

	for (roster_count = 0 ; roster_count < SB_MAXROSTER ; roster_count++)
	{
		if (!SB_ReadPlayer (i, roster_count, &p))
			break;
		for (j = roster_count ; j > 0 ; j--)
		{
			if (roster[j - 1].frags >= p.frags)
				break;
			roster[j] = roster[j - 1];
		}
		roster[j] = p;
	}
};

// a team, by its name or else its colours, and its score
typedef struct
{
	string	name;
	vector	top, bottom;
	float	score;
} team_t;

team_t	teams[2];

// the team a player is in of n found (1 or 2), 0 none of them
float(player_t *p, float n, float byname) SB_TeamOf =
{
	float	t;

	for (t = 0 ; t < n ; t++)
		if (byname ? (p->team == teams[t].name) : (p->top == teams[t].top && p->bottom == teams[t].bottom))
			return t + 1;
	return 0;
};

// two teams, by their team names or else their colours, the one ahead first;
// a third undoes them: how many there are, 2 or 0
float(float i, float byname) SB_FindTeams =
{
	float	r, t, n = 0;
	team_t	swap;

	if (SB_IsQizmo (i) || SB_Num (key_numplayers, i) <= 2 || SB_Get (key_teamplay, i) == "0")
		return 0;
	for (r = 0 ; r < roster_count ; r++)
	{
		if (roster[r].spec)
			continue;
		t = SB_TeamOf (&roster[r], n, byname);
		if (!t)
		{
			if (n == 2)
				return 0;
			teams[n].name = roster[r].team;
			teams[n].top = roster[r].top;
			teams[n].bottom = roster[r].bottom;
			teams[n].score = 0;
			t = ++n;
		}
		teams[t - 1].score += roster[r].frags;
	}
	if (n != 2)
		return 0;
	if (teams[1].score > teams[0].score)
	{
		swap = teams[0];
		teams[0] = teams[1];
		teams[1] = swap;
	}
	return 2;
};

// frags as ezQuake writes them: spec, else within -99..9999, a space after under 1000
string(player_t *p) SB_Frags =
{
	float	f = max (-99, min (p->frags, 9999));

	if (p->spec)
		return "spec";
	return sprintf ("%3d%s", f, f < 1000 ? " " : "");
};

// a player's colours: the top, and under it the bottom bh high
void(float x, float y, float w, float bh, vector top, vector bottom) SB_Colours =
{
	drawfill ([x, y + 1, 0], [w, 4, 0], top, 1, 0);
	drawfill ([x, y + 5, 0], [w, bh, 0], bottom, 1, 0);
};

// the players, their colours under their frags (a team's for its players),
// and with two teams the score
void(float x, float y, float w, float h, float i) SB_Info_Players =
{
	float		r, t, ry, rows, n, tp = FALSE;
	player_t	*p;
	string		line;

	SB_ReadRoster (i);
	for (r = 0 ; r < roster_count ; r++)
		if (roster[r].team != "")
			tp = TRUE;
	tp = tp && SB_Get (key_teamplay, i) != "0";
	n = SB_FindTeams (i, tp);

	rows = floor (h / 8) - 1 - (n ? 1 : 0);
	sb_infopos = max (0, min (sb_infopos, roster_count - rows));
	UI_Print (x, y, tp ? "png tm frgs team name" : "png tm frgs name", TRUE);
	for (r = sb_infopos ; r < roster_count && r < sb_infopos + rows ; r++)
	{
		p = &roster[r];
		ry = y + (r - sb_infopos) * 8 + 8;
		if (!p->spec)
		{
			t = (tp && n) ? SB_TeamOf (p, n, TRUE) : 0;
			if (t)
				SB_Colours (x + 54, ry, 34, 3, teams[t - 1].top, teams[t - 1].bottom);
			else
				SB_Colours (x + 54, ry, 34, 3, p->top, p->bottom);
		}
		line = sprintf ("%3d %2d %4.4s ", max (0, min (p->ping, 999)), max (0, min (p->time, 99)), SB_Frags (p));
		if (tp)
			line = strcat (line, sprintf ("%4.4s ", p->team));
		UI_Print (x, ry, substring (strcat (line, p->name), 0, 31), FALSE);
	}
	if (n)
	{
		ry = y + rows * 8 + 8;
		SB_Colours (x + 104, ry, 40, 4, teams[0].top, teams[0].bottom);
		SB_Colours (x + 168, ry, 40, 4, teams[1].top, teams[1].bottom);
		UI_Print (x, ry, sprintf ("      score:  %3d  -  %3d", teams[0].score, teams[1].score), FALSE);
	}
};

/*
==============================================================================

THE SERVERINFO AND THE SOURCES

==============================================================================
*/

// the serverinfo's keys, without case in order
typedef struct
{
	string	key, value;
} rule_t;

#define	SB_MAXRULES	64

rule_t	rules[SB_MAXRULES];

void(float x, float y, float w, float h, float i) SB_Info_Rules =
{
	float	n = tokenizebyseparator (SB_Get (key_serverinfo, i), "\\"), k, j, count = 0, rows = floor (h / 8);
	rule_t	rule;

	for (k = 1 ; k + 1 < n && count < SB_MAXRULES ; k += 2)
	{
		rule.key = argv (k);
		rule.value = argv (k + 1);
		for (j = count ; j > 0 ; j--)
		{
			if (strcasecmp (rules[j - 1].key, rule.key) <= 0)
				break;
			rules[j] = rules[j - 1];
		}
		rules[j] = rule;
		count++;
	}
	sb_infopos = max (0, min (sb_infopos, count - rows));
	for (k = sb_infopos ; k < count && k < sb_infopos + rows ; k++)
		UI_Print (x, y + (k - sb_infopos) * 8, substring (strcat (Pad (rules[k].key, 13), " ",
			Pad (rules[k].value, floor (w / 8) - 14)), 0, 31), FALSE);
};

// the sources, red those that named the server, the line chosen marked
void(float x, float y, float w, float h, float i) SB_Info_Sources =
{
	float	n = gethostcachevalue (SLIST_SOURCES), k, rows = floor (h / 8) - 1;

	sb_infopos = max (0, min (sb_infopos, n - 1));
	UI_Print (x, y, " type    name", TRUE);
	for (k = 0 ; k < n && k < rows ; k++)
	{
		UI_Print (x, y + 8 + k * 8, sprintf ("%s   %s", Pad (gethostcachesource (k, "type"), 6),
			gethostcachesource (k, "name")), hostcacheinsource (i, k));
		if (k == sb_infopos)
			UI_Char (x + 56, y + 8 + k * 8, 0x8d);
	}
};

/*
==============================================================================

THE POPUP

==============================================================================
*/

typedef struct
{
	string	name;
	float	at;			// where its name begins in the tabs' line
	void(float x, float y, float w, float h, float i)	draw;
} infopage_t;

#define	SB_INFOPAGES	3

infopage_t	sb_infopages[SB_INFOPAGES] = {
	{"players", 0, SB_Info_Players},
	{"serverinfo", 8, SB_Info_Rules},
	{"sources", 19, SB_Info_Sources}
};

void() SB_Info_Draw =
{
	float		w = 240, h = 112 + (sb_vh >= 240 ? 40 : 0), x, y;
	vector		at = SB_DialogBox (w, h);
	infopage_t	page = sb_infopages[sb_infopage];
	string		tabs = " players serverinfo sources ";

	x = at_x - 4;
	y = at_y - 4;
	w += 8;
	h += 8;
	UI_PrintCenter (x, y, w, substring (SB_NameOrAddress (sb_list.pos), 0, 31), FALSE);
	UI_PrintCenter (x, y + 10, w, SB_Get (key_address, sb_list.pos), FALSE);
	// the page's name in brackets, red
	tabs = strcat (substring (tabs, 0, page.at), chr2str (0x10), Red (page.name), chr2str (0x11),
		substring (tabs, page.at + strlen (page.name) + 2, 28));
	UI_PrintCenter (x, y + 24, w, tabs, FALSE);
	UI_Line (x, y + 32, 31);
	page.draw (x, y + 40, w, h - 40, sb_list.pos);
};

void() SB_Info_Open =
{
	sb_overlay = SBO_INFO;
	sb_infopos = 0;
	sb_infoupdate = 0;
};

// another server under the popup
void(float d) SB_Info_Move =
{
	SB_Select (sb_list.pos + d);
	sb_infopos = 0;
	sb_infoupdate = 0;
};

void(float d) SB_Info_Page =
{
	sb_infopage = Wrap (sb_infopage + d, SB_INFOPAGES);
	sb_infopos = 0;
};

void(float k) SB_Info_Key =
{
	switch (k)
	{
	case K_ENTER:
		SB_Connect (sb_list.pos, sb_ctrl);
		break;
	case K_ESCAPE:
	case K_BACKSPACE:
		SB_CloseOverlay ();
		break;
	case K_TAB:
		SB_Info_Page (sb_shift ? -1 : 1);
		break;
	case K_RIGHTARROW:
		SB_Info_Page (1);
		break;
	case K_LEFTARROW:
		SB_Info_Page (-1);
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

// the server's letters, n joining too, and q watching its game on QTV
void(float c) SB_Info_Char =
{
	if (Lower (c) == 'n')
		SB_Connect (sb_list.pos, FALSE);
	else if (Lower (c) == 'q')
		SB_WatchQTV (sb_list.pos);
	else
		SB_ServerCommand (sb_list.pos, c);
};
