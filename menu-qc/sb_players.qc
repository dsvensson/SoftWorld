// sb_players.qc -- the server browser's players: every player of the servers
// listed, by sb_sortplayers, and a search through their names

typedef struct
{
	float	server;
	string	name;
} sbplayer_t;

sbplayer_t	pl_players[SB_MAXPLAYERS];
float		pl_order[SB_MAXPLAYERS], pl_merge[SB_MAXPLAYERS];
float		pl_count;
sblist_t	pl_list;
var float	pl_generation = -1, pl_serial = -1;		// of the list and its view, as read
sbsearch_t	pl_search;

// sb_sortplayers' digits: sb_sortservers' for their servers, 9 their names
float(float digit, float a, float b) SB_PlayerCompare =
{
	float	sa = pl_players[a].server, sb = pl_players[b].server;

	switch (digit)
	{
	case '1':
		return strcasecmp (SB_ServerName (sa), SB_ServerName (sb));
	case '2':
	case '4':
	case '5':
		return strcasecmp (SB_Get (SB_SortKey (digit), sa), SB_Get (SB_SortKey (digit), sb));
	case '3':
	case '6':
	case '7':
	case '8':
		return SB_Num (SB_SortKey (digit), sa) - SB_Num (SB_SortKey (digit), sb);
	case '9':
		return strcasecmp (pl_players[a].name, pl_players[b].name);
	}
	return 0;
};

// descending never, as ezQuake has it
float(float a, float b) SB_PlayerBefore =
{
	return SB_SortBefore (cvar_string ("sb_sortplayers"), a, b, SB_PlayerCompare, FALSE);
};

// the players read again when the list or its view changed
void() SB_ReadPlayers =
{
	float		n = gethostcachevalue (SLIST_HOSTCACHEVIEWCOUNT), g = gethostcachevalue (SLIST_GENERATION), i, r;
	player_t	p;

	if (g == pl_generation && sb_serial == pl_serial)
		return;
	pl_generation = g;
	pl_serial = sb_serial;
	pl_count = 0;
	for (i = 0 ; i < n && pl_count < SB_MAXPLAYERS ; i++)
		for (r = 0 ; r < SB_MAXROSTER && pl_count < SB_MAXPLAYERS ; r++)
		{
			if (!SB_ReadPlayer (i, r, &p))
				break;
			pl_players[pl_count].server = i;
			pl_players[pl_count].name = p.name;
			pl_order[pl_count] = pl_count;
			pl_count++;
		}
	SB_Sort (pl_order, pl_merge, 0, pl_count, SB_PlayerBefore);
	pl_list.pos = max (0, min (pl_list.pos, pl_count - 1));
};

// the player selected's server
float() SB_PlayerServer =
{
	return pl_count ? pl_players[pl_order[pl_list.pos]].server : -1;
};

void(float x, float y, float w, float h) SB_Players_Draw =
{
	float		rows = SB_Rows (h), hw = min (floor (w / 8) - 16, 60) - 4, r, at;
	sbplayer_t	p;

	SB_ReadPlayers ();
	SB_Scroll (&pl_list, pl_count, rows);
	UI_PrintCenter (x, y, w, strcat ("name            ", Pad ("server", hw), " png"), TRUE);
	for (r = 0 ; r < rows && pl_list.disp + r < pl_count ; r++)
	{
		at = pl_list.disp + r;
		p = pl_players[pl_order[at]];
		UI_PrintCenter (x, y + 8 * (r + 1), w, strcat (strpad (15, p.name), chr2str (at == pl_list.pos ? 141 : 32),
			Pad (SB_ServerName (p.server), hw), " ", PadRight (SB_PingText (p.server), 3)), at == pl_list.pos);
	}
	if (cvar ("sb_status") && pl_count)
		SB_Status (x, y, w, h, SB_PlayerServer (), pl_list.pos, pl_count);
};

// the first player from the selected one on (then from the first) whose name has it
void() SB_SearchPlayer =
{
	float	i, k;

	for (k = 0 ; k < pl_count ; k++)
	{
		i = (pl_list.pos + k) % pl_count;
		if (strstrofs (strtolower (strdecolorize (pl_players[pl_order[i]].name)), pl_search.text) >= 0)
		{
			pl_list.pos = i;
			return;
		}
	}
	pl_search.text = "not found";
};

float(float k) SB_Players_Key =
{
	switch (k)
	{
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
	return SB_ListKey (&pl_list, k, pl_count, 1);
};

// a search, with Alt or while one goes on; else Space scans, and the letters
// and the sort's digits
void(float c) SB_Players_Char =
{
	float	searching = SB_Searching (&pl_search);

	if (c == ' ' && !sb_alt && !searching)
	{
		refreshhostcache (sb_ctrl);
		return;
	}
	if (c >= ' ' && c <= '}' && (sb_alt || searching))
	{
		if (pl_search.text == "not found")
			pl_search.text = "";
		SB_SearchAdd (&pl_search, c);
		SB_SearchPlayer ();
		return;
	}
	if (!pl_count)
		return;
	if (c >= '1' && c <= '9')
	{
		SB_ToggleSort ("sb_sortplayers", c);
		pl_serial = -1;
	}
	else
		SB_ServerCommand (SB_PlayerServer (), c);
};
