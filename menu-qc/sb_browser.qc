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
// the multiplayer menu, the page's, the tabs'
void(float k) SB_Key =
{
	if (sb_overlay)
		sb_overlays[sb_overlay].key (k);
	else if (k == K_ESCAPE)
		M_Menu_Multiplayer_f ();
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

/*
==============================================================================

THE BROWSER

==============================================================================
*/

void() SB_Open =
{
	M_Enter (M_SLIST);
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

// in the big screens' window (SB_Window): its tabs, a line, the page, what is
// over it
void(vector size) SB_Draw =
{
	SB_Window (size);
	SB_Update ();
	SB_Tabs (sb_wx, sb_wy);
	SB_TitleLine ();
	sb_pages[sb_tab].draw (sb_wx, sb_wy + 16, sb_ww, sb_wh - 16);
	if (sb_overlay)
		sb_overlays[sb_overlay].draw ();
};
