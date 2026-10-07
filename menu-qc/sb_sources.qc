// sb_sources.qc -- the server browser's sources: masters, files and lists of
// servers, marked for the scan, by sb_sortsources

#define	SB_MAXSOURCES	64

sblist_t	src_list;
float		src_order[SB_MAXSOURCES], src_merge[SB_MAXSOURCES];
float		src_count;

// sb_sortsources' digits: 1 type, 2 name, 3 as listed
float(float digit, float a, float b) SB_SourceCompare =
{
	switch (digit)
	{
	case '1':	return strcasecmp (gethostcachesource (a, "type"), gethostcachesource (b, "type"));
	case '2':	return strcasecmp (gethostcachesource (a, "name"), gethostcachesource (b, "name"));
	case '3':	return a - b;
	}
	return 0;
};

float(float a, float b) SB_SourceBefore =
{
	return SB_SortBefore (cvar_string ("sb_sortsources"), a, b, SB_SourceCompare, TRUE);
};

void() SB_SortSources =
{
	float	i;

	src_count = min (gethostcachevalue (SLIST_SOURCES), SB_MAXSOURCES);
	for (i = 0 ; i < src_count ; i++)
		src_order[i] = i;
	SB_Sort (src_order, src_merge, 0, src_count, SB_SourceBefore);
	src_list.pos = max (0, min (src_list.pos, src_count - 1));
};

float(float s) SB_Marked =
{
	return gethostcachesource (s, "marked") != "";
};

// the source under the cursor, -1 none
float() SB_SourceAt =
{
	return src_list.pos < src_count ? src_order[src_list.pos] : -1;
};

void(float x, float y, float w, float h) SB_Sources_Draw =
{
	float	rows = SB_Rows (h), r, s, marked = 0;

	SB_SortSources ();
	SB_Scroll (&src_list, src_count, rows);
	UI_PrintCenter (x, y, w, " type   name             servs updated", TRUE);
	for (r = 0 ; r < rows && src_list.disp + r < src_count ; r++)
	{
		s = src_order[src_list.disp + r];
		UI_PrintCenter (x, y + 8 * (r + 1), w, sprintf ("%s %s%s %4d  %s ", Pad (gethostcachesource (s, "type"), 6),
			chr2str (src_list.disp + r == src_list.pos ? 141 : 32), Pad (gethostcachesource (s, "name"), 17),
			stof (gethostcachesource (s, "servers")), gethostcachesource (s, "updated")), SB_Marked (s));
	}
	if (!cvar ("sb_status"))
		return;
	for (s = 0 ; s < src_count ; s++)
		marked += SB_Marked (s);
	UI_Line (x, y + h - 24, floor (w / 8));
	UI_PrintCenter (x, y + h - 16, w, sprintf ("%d sources selected (%d servers)", marked,
		gethostcachevalue (SLIST_MARKEDSERVERS)), FALSE);
	UI_PrintCenter (x, y + h - 8, w, sprintf ("of %d total (%d servers)", src_count,
		gethostcachevalue (SLIST_HOSTCACHETOTALCOUNT)), FALSE);
};

void() SB_RemoveSource =
{
	if (SB_SourceAt () >= 0)
		removehostcachesource (SB_SourceAt ());
};

// the source under the cursor's mark turned; whether there was one
float() SB_ToggleMark =
{
	float	s = SB_SourceAt ();

	if (s < 0)
		return FALSE;
	sethostcachesourcemark (s, !SB_Marked (s));
	return TRUE;
};

float(float k) SB_Sources_Key =
{
	switch (k)
	{
	case K_INS:
		SB_AddSource_Open ();
		return TRUE;
	case K_DEL:
		if (SB_SourceAt () >= 0)
			SB_Confirm (sprintf ("Remove %.20s", gethostcachesource (SB_SourceAt (), "name")), SB_RemoveSource);
		return TRUE;
	case K_ENTER:
		if (SB_ToggleMark ())
			src_list.pos = min (src_list.pos + 1, src_count - 1);
		return TRUE;
	}
	return SB_ListKey (&src_list, k, src_count, 1);
};

// letters, and with Ctrl or Alt only Space
void(float c) SB_Sources_Char =
{
	float	i;

	if (c != ' ' && (sb_ctrl || sb_alt))
		return;
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
		SB_ToggleMark ();
		break;
	case '+':
	case '=':
	case '-':
	case '*':
		for (i = 0 ; i < src_count ; i++)
			sethostcachesourcemark (i, (c == '*') ? !SB_Marked (i) : (c != '-'));
		break;
	case '1':
	case '2':
	case '3':
		SB_ToggleSort ("sb_sortsources", c);
		break;
	}
};
