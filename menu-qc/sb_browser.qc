// sb_browser.qc -- the server browser put together: its pages under their
// tabs, what is over them, the keys given them, and opening it

// a page: its tab's name, its drawing, and the keys it takes
typedef struct
{
	string	name;
	void(float x, float y, float w, float h)	draw;
	float(float k)	key;			// whether it took the key, else the tabs do
	void(float c)	typed;
} sbpage_t;

sbpage_t	sb_pages[SB_TABS] = {
	{"Servers", SB_Servers_Draw, SB_Servers_Key, SB_Servers_Char},
	{"Sources", SB_Sources_Draw, SB_Sources_Key, SB_Sources_Char},
	{"Players", SB_Players_Draw, SB_Players_Key, SB_Players_Char},
	{"Options", SB_Options_Draw, SB_Options_Key, SB_Options_Char}
};

// what is over the page, which takes every key: the info popup or a dialog
typedef struct
{
	void()			draw;
	void(float k)	key;
	void(float c)	typed;
} sboverlay_t;

sboverlay_t	sb_overlays[SBO_COUNT] = {
	{__NULL__, __NULL__, __NULL__},
	{SB_Info_Draw, SB_Info_Key, SB_Info_Char},
	{SB_AddServer_Draw, SB_AddServer_Key, SB_AddServer_Char},
	{SB_AddSource_Draw, SB_AddSource_Key, SB_AddSource_Char},
	{SB_Confirm_Draw, SB_Confirm_Key, SB_Confirm_Char}
};

/*
==============================================================================

THE KEYS

==============================================================================
*/

void(float tab) SB_ShowTab =
{
	sb_tab = Wrap (tab, SB_TABS);
	localsound ("misc/menu1.wav");
};

// the tabs', where the page didn't take the key
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

// a key that types nothing: what is over the page's, else Escape back to
// the main menu, the page's, the tabs'
void(float k) SB_Key =
{
	if (sb_overlay)
		sb_overlays[sb_overlay].key (k);
	else if (k == K_ESCAPE)
		M_Menu_Main_f ();
	else if (!sb_pages[sb_tab].key (k))
		SB_TabKey (k);
};

// a key that types: a character, or with Ctrl or Alt the key's own
void(float c) SB_Typed =
{
	if (sb_overlay)
		sb_overlays[sb_overlay].typed (c);
	else
		sb_pages[sb_tab].typed (c);
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

/*
==============================================================================

THE BROWSER

==============================================================================
*/

void() SB_Open =
{
	M_Enter (M_SLIST);
	sb_ctrl = sb_alt = sb_shift = FALSE;
	SB_CloseOverlay ();
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

// what follows the list each frame: the selection, a server added once it
// is listed (10 s at most), the info popup's server asked again (sb_liveupdate)
void() SB_Update =
{
	float	i;

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
	if (sb_overlay == SBO_INFO && cvar ("sb_liveupdate") > 0 && time >= sb_infoupdate)
	{
		refreshhostcacheentry (sb_list.pos);
		sb_infoupdate = time + cvar ("sb_liveupdate");
	}
};

// the tabs, the current one bracketed and red
void(float x, float y) SB_Tabs =
{
	float	i;
	string	name;

	for (i = 0 ; i < SB_TABS ; i++, x += (strlen (name) + 1) * 8)
	{
		name = sb_pages[i].name;
		if (i == sb_tab)
			UI_Print (x, y, strcat (chr2str (0x10), name, chr2str (0x11)), TRUE);
		else
			UI_Print (x, y, strcat (" ", name, " "), FALSE);
	}
};

// on the console's background (out of a game), else on ezQuake's fade: its
// window min (640, w) - 8 wide, its tabs, a line, the page, what is over it
void(vector size) SB_Draw =
{
	float	x, y, w, h;

	sb_vw = size_x;
	sb_vh = size_y;
	if (clientstate () == 2)
		drawfill ('0 0 0', size, '0 0 0', 0.7, 0);
	else
		Backdrop_Draw (size);
	SB_Update ();

	w = min (640, sb_vw) - 8;
	h = min (480, sb_vh) - 8;
	x = floor ((sb_vw - w) / 2);
	y = floor ((sb_vh - h) / 2);
	SB_Tabs (x, y);
	UI_Line (x + Trunc ((w - 8 * floor (w / 8)) / 2), y + 8, floor (w / 8));
	sb_pages[sb_tab].draw (x, y + 16, w, h - 16);
	if (sb_overlay)
		sb_overlays[sb_overlay].draw ();
};
