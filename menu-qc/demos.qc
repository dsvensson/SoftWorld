// demos.qc -- the demos recorded here, as qualia's Demos screen lists them:
// the .qwd and .mvd files (and their .gz) of the game's search path and its
// demos directory, newest first, with their size and age; typing searches
// their names, Enter plays one. Its keys are the server browser's: the list's
// moves, Space scans again, Backspace clears the search.

#define	DEMOS_MAX		1024
#define	DEMOS_PATTERNS	8

typedef struct
{
	string	name;			// under the search path: "demos/x.mvd"
	float	size;			// bytes, -1 unknown
	float	age;			// seconds since it was written, as scanned; -1 unknown
} demo_t;

demo_t		demos[DEMOS_MAX];
float		demo_order[DEMOS_MAX], demo_merge[DEMOS_MAX];	// by age, newest first
float		demo_view[DEMOS_MAX];		// of the order, those the search leaves
float		demo_count, demo_viewcount;
float		demo_scanned;				// scanned once, as the screen first opened
float		demo_scantime;				// when, so the ages go on counting
var float	demo_refilter = TRUE;		// the view is made again
string		demo_filtered;				// the search the view was made with
string		demo_selname;				// the selected demo's name, followed as the view changes
float		demo_namewidth;				// cells the names have
sblist_t	demo_list;
sbsearch_t	demo_search;

string	demo_patterns[DEMOS_PATTERNS] = {"*.qwd", "*.mvd", "*.qwd.gz", "*.mvd.gz", "demos/*.qwd", "demos/*.mvd",
	"demos/*.qwd.gz", "demos/*.mvd.gz"};

// newest first, those of no known age last, and by name where they tie
float(float a, float b) Demos_Before =
{
	float	aa = demos[a].age, ab = demos[b].age;

	if (aa < 0)
		aa = 2000000000;
	if (ab < 0)
		ab = 2000000000;
	if (aa != ab)
		return aa < ab;
	return strcasecmp (demos[a].name, demos[b].name) < 0;
};

// the demos the search path has now, each once
void() Demos_Scan =
{
	float	p, h, i, n, k;
	string	name;

	demo_count = 0;
	for (p = 0 ; p < DEMOS_PATTERNS ; p++)
	{
		h = search_begin (demo_patterns[p], TRUE, TRUE);
		if (h < 0)
			continue;
		n = search_getsize (h);
		for (i = 0 ; i < n && demo_count < DEMOS_MAX ; i++)
		{
			name = search_getfilename (h, i);
			for (k = 0 ; k < demo_count ; k++)
				if (!strcasecmp (demos[k].name, name))
					break;
			if (k < demo_count)
				continue;
			demos[demo_count].name = name;
			demos[demo_count].size = search_getfilesize (h, i);
			demos[demo_count].age = search_getfileage (h, i);
			demo_order[demo_count] = demo_count;
			demo_count++;
		}
		search_end (h);
	}
	demo_scantime = time;
	SB_Sort (demo_order, demo_merge, 0, demo_count, Demos_Before);
	demo_refilter = TRUE;
};

// the view: the demos whose names have what is searched for, the selection
// on its demo where it is still in it
void() Demos_Filter =
{
	float	k, i;

	demo_viewcount = 0;
	for (k = 0 ; k < demo_count ; k++)
	{
		i = demo_order[k];
		if (demo_search.text == "" || strstrofs (strtolower (demos[i].name), demo_search.text) >= 0)
		{
			demo_view[demo_viewcount] = i;
			demo_viewcount++;
		}
	}
	demo_filtered = demo_search.text;
	demo_refilter = FALSE;
	for (k = 0 ; k < demo_viewcount ; k++)
		if (demos[demo_view[k]].name == demo_selname)
		{
			demo_list.pos = k;
			return;
		}
	demo_list.pos = max (0, min (demo_list.pos, demo_viewcount - 1));
	demo_selname = demo_viewcount ? demos[demo_view[demo_list.pos]].name : "";
};

// the demo selected, -1 none
float() Demos_At =
{
	return demo_viewcount ? demo_view[demo_list.pos] : -1;
};

void(float i) Demos_Select =
{
	if (i >= 0)
		demo_selname = demos[i].name;
};

// a size in 5 characters at most
string(float bytes) Demos_Size =
{
	if (bytes < 0)
		return "";
	if (bytes < 1024)
		return sprintf ("%dB", bytes);
	if (bytes < 10 * 1024)
		return sprintf ("%.1fK", bytes / 1024);
	if (bytes < 1024 * 1024)
		return sprintf ("%dK", floor (bytes / 1024));
	if (bytes < 10 * 1024 * 1024)
		return sprintf ("%.1fM", bytes / (1024 * 1024));
	if (bytes < 1024 * 1024 * 1024)
		return sprintf ("%dM", floor (bytes / (1024 * 1024)));
	return sprintf ("%.1fG", bytes / (1024 * 1024 * 1024));
};

// how long ago, in 8 characters at most: qualia's
string(float age) Demos_When =
{
	if (age < 0)
		return "";
	age += time - demo_scantime;
	if (age < 60)
		return "just now";
	if (age < 3600)
		return sprintf ("%dm ago", floor (age / 60));
	if (age < 86400)
		return sprintf ("%dh ago", floor (age / 3600));
	if (age < 86400 * 30)
		return sprintf ("%dd ago", floor (age / 86400));
	if (age < 86400 * 365)
		return sprintf ("%dmo ago", floor (age / (86400 * 30)));
	return sprintf ("%dy ago", floor (age / (86400 * 365)));
};

/*
==============================================================================

THE SCREEN

==============================================================================
*/

void(float x, float y, float w) Demos_Header =
{
	float	cells = floor (w / 8);

	UI_Fill (x, y, w, 8, '10 10 10', 200);
	UI_Print (x, y, "name", TRUE);
	UI_PrintCenter (x + (cells - 14) * 8, y, 40, "size", TRUE);
	UI_PrintCenter (x + (cells - 8) * 8, y, 64, "when", TRUE);
};

// a demo's row: the selected one marked, the rest striped
void(float x, float y, float w, float i, float r, float selected) Demos_Row =
{
	float	cells = floor (w / 8);

	if (selected)
	{
		UI_GrayBox (x, y, w, 8);
		UI_Char (x + 8 * demo_namewidth, y, UI_Cursor ());
	}
	else if (r & 1)
		UI_Fill (x, y, w, 8, '25 25 25', 125);
	else
		UI_Fill (x, y, w, 8, '50 50 50', 125);
	UI_Print (x, y, substring (demos[i].name, 0, demo_namewidth), selected);
	UI_Print (x + (cells - 14) * 8, y, PadRight (Demos_Size (demos[i].size), 5), selected);
	UI_PrintCenter (x + (cells - 8) * 8, y, 64, Demos_When (demos[i].age), selected);
};

// where the list is, the selected demo's name whole, and what is searched for
// or the keys
void(float x, float y, float w, float h) Demos_Status =
{
	float	cells = floor (w / 8), i = Demos_At ();

	SB_CountLine (x, y + h - 24, w, sprintf ("%d/%d", demo_list.pos + 1, demo_viewcount));
	if (i >= 0)
		UI_PrintCenter (x, y + h - 16, w, substring (demos[i].name, 0, cells), FALSE);
	if (SB_Searching (&demo_search))
		UI_PrintCenter (x, y + h - 8, w, sprintf ("search for: %-7s", demo_search.text), TRUE);
	else
		UI_PrintCenter (x, y + h - 8, w, substring ("[enter] plays  [space] scans again", 0, cells), FALSE);
};

void(vector size) Demos_Draw =
{
	float	x, y, w, h, rows, r;

	SB_Window (size);
	SB_Title ("Demos");
	if (demo_refilter || demo_search.text != demo_filtered)
		Demos_Filter ();

	x = sb_wx;
	y = sb_wy + 16;
	w = sb_ww;
	h = sb_wh - 16;
	demo_namewidth = floor (w / 8) - 15;
	if (!demo_viewcount)
	{
		if (!demo_count)
		{
			UI_PrintCenter (x, y + 8, w, "No demos found", FALSE);
			UI_PrintCenter (x, y + 24, w, "Press [space] to scan again", TRUE);
			UI_PrintCenter (x, y + 40, w, "record <name> writes one", FALSE);
		}
		else
		{
			UI_PrintCenter (x, y + 8, w, substring (sprintf ("No demo is named like %s", demo_search.text), 0,
				floor (w / 8)), FALSE);
			UI_PrintCenter (x, y + 24, w, "Press [backspace] to clear the search", TRUE);
		}
		return;
	}

	rows = SB_Rows (h);
	SB_Scroll (&demo_list, demo_viewcount, rows);
	Demos_Header (x, y, w);
	for (r = 0 ; r < rows && demo_list.disp + r < demo_viewcount ; r++)
		Demos_Row (x, y + 8 * (r + 1), w, demo_view[demo_list.disp + r], demo_list.disp + r,
			demo_list.disp + r == demo_list.pos);
	if (cvar ("sb_status"))
		Demos_Status (x, y, w, h);
};

void(float i) Demos_Play =
{
	if (i < 0)
		return;
	localcmd (sprintf ("playdemo \"%s\"\n", demos[i].name));
	M_Close ();
};

// the keys that aren't typed: Escape back to the multiplayer menu, Backspace
// clearing the search, Enter playing, and the list's moves
void(float k) Demos_Key =
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Multiplayer_f ();
		return;
	case K_BACKSPACE:
		demo_search.text = "";
		return;
	case K_ENTER:
		Demos_Play (Demos_At ());
		return;
	case K_MWHEELUP:
		demo_list.disp = max (0, demo_list.disp - 4);
		break;
	case K_MWHEELDOWN:
		demo_list.disp += 4;
		break;
	}
	if (SB_ListKey (&demo_list, k, demo_viewcount, 4))
		Demos_Select (Demos_At ());
};

// the typed keys: Space scans again, the rest search; with Ctrl, c copies
// the selected demo's name
void(float c) Demos_Char =
{
	if (c == ' ' && !sb_alt)
	{
		Demos_Scan ();
		return;
	}
	if (!sb_ctrl && !sb_alt)
	{
		if (c > ' ' && c <= '}')
			SB_SearchAdd (&demo_search, c);
		return;
	}
	if (Lower (c) == 'c' && Demos_At () >= 0)
		clipboard_set (0, demos[Demos_At ()].name);
};

// the demos, scanned for the first time they show
void() Demos_Open =
{
	M_Enter (M_DEMOS);
	if (!demo_scanned)
	{
		demo_scanned = TRUE;
		Demos_Scan ();
	}
};
