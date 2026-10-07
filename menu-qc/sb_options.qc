// sb_options.qc -- the server browser's settings, ezQuake's page: its
// sections, switches, named values and sliders, and each one's help

enum { OPT_SECTION, OPT_SWITCH, OPT_NAMED, OPT_NUMBER };

typedef struct
{
	string	label;
	string	cvar;
	float	kind;
	float	min, max, step;		// a number's
} sboption_t;

#define	OPT_COUNT	27

sboption_t	sb_options[OPT_COUNT] = {
	{"Server Filters", "", OPT_SECTION},
	{"Hide Empty", "sb_hideempty", OPT_SWITCH},
	{"Hide Full", "sb_hidefull", OPT_SWITCH},
	{"Hide Not Empty", "sb_hidenotempty", OPT_SWITCH},
	{"Hide Dead", "sb_hidedead", OPT_SWITCH},
	{"Hide High Ping", "sb_hidehighping", OPT_SWITCH},
	{"Hide QTV", "sb_hideqtv", OPT_SWITCH},
	{"Show Proxies", "sb_showproxies", OPT_NAMED},
	{"Display Columns", "", OPT_SECTION},
	{"Show Ping", "sb_showping", OPT_SWITCH},
	{"Show Map", "sb_showmap", OPT_SWITCH},
	{"Show Gamedir", "sb_showgamedir", OPT_SWITCH},
	{"Show Players", "sb_showplayers", OPT_SWITCH},
	{"Show Timelimit", "sb_showtimelimit", OPT_SWITCH},
	{"Show Fraglimit", "sb_showfraglimit", OPT_SWITCH},
	{"Show Server Address", "sb_showaddress", OPT_SWITCH},
	{"Display", "", OPT_SECTION},
	{"Server Status", "sb_status", OPT_SWITCH},
	{"Network Filters", "", OPT_SECTION},
	{"Ping Timeout", "sb_pingtimeout", OPT_NUMBER, 50, 1000, 50},
	{"Pings Per Server", "sb_pings", OPT_NUMBER, 1, 5, 1},
	{"Pings Per Second", "sb_pingspersec", OPT_NUMBER, 10, 300, 10},
	{"Master Timeout", "sb_mastertimeout", OPT_NUMBER, 50, 1000, 50},
	{"Master Retries", "sb_masterretries", OPT_NUMBER, 1, 5, 1},
	{"Info Timeout", "sb_infotimeout", OPT_NUMBER, 50, 1000, 50},
	{"Info Retries", "sb_inforetries", OPT_NUMBER, 0, 4, 1},
	{"Infos Per Second", "sb_infospersec", OPT_NUMBER, 10, 1000, 10}
};
string	sb_proxiesnames[3] = {"hide", "show", "exclusively"};		// sb_showproxies', the named one's

var float	opt_pos = 1;
float		opt_view;			// the first row shown
float		opt_help;			// the whole help up

float(float i) SB_OptHeight =
{
	return sb_options[i].kind == OPT_SECTION ? 24 : 9;
};

// the scroll bar's colours, the palette's, as ezQuake draws it without its textures
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

// a row's value: on or off, its name, or a slider and its number
void(float x, float y, sboption_t *o, float active) SB_OptValue =
{
	float	v = cvar (o->cvar);

	switch (o->kind)
	{
	case OPT_SWITCH:
		UI_Print (x, y, v ? "on" : "off", active);
		break;
	case OPT_NAMED:
		UI_Print (x, y, sb_proxiesnames[max (0, min (v, 2))], active);
		break;
	case OPT_NUMBER:
		UI_Slider (x, y, 12, (v - o->min) / (o->max - o->min));
		UI_Print (x + 112, y, sprintf ("%3d", v), active);
		break;
	}
};

// the rows from the view's on, the cursor's among them, and where they
// don't fit the scroll bar
void(float x, float y, float w, float h) SB_OptRows =
{
	float		i, ry, total = 0, lowest, room;
	sboption_t	*o;

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
		o = &sb_options[i];
		if (o->kind == OPT_SECTION)
		{
			UI_PrintCenter (x, ry + 12, w, strcat (chr2str (0x1d), " ", o->label, " ", chr2str (0x1f)), TRUE);
			continue;
		}
		if (i == opt_pos)
			UI_GrayBox (x, ry, w, 9);
		UI_Print (x + floor (w / 2) - 8 * strlen (o->label), ry, o->label, i == opt_pos);
		SB_OptValue (x + floor (w / 2) + 16, ry, o, i == opt_pos);
	}

	if (total <= h)
		return;
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
};

// the rows over the help, or the whole help (F1)
void(float x, float y, float w, float h) SB_Options_Draw =
{
	string	help = cvar_description (sb_options[opt_pos].cvar);

	w -= 8;		// the scroll bar's
	if (opt_help)
	{
		UI_TextBox (x, y, floor (w / 8) - 1, floor ((h - 32) / 8));
		UI_TextBlock (x + 8, y + 8, floor ((w - 16) / 8), floor ((h - 32) / 8), help);
		return;
	}
	UI_TextBox (x, y + h - 40, floor (w / 8) - 1, 4);
	if (UI_TextBlock (x + 8, y + h - 32, floor ((w - 16) / 8), 3, help))
		UI_Print (x + 8, y + h - 8, "Press [F1] to read more...", TRUE);
	SB_OptRows (x, y, w, h - 40);
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
		if (sb_options[i].kind != OPT_SECTION)
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
	sboption_t	*o = &sb_options[opt_pos];
	float		v = cvar (o->cvar);

	switch (o->kind)
	{
	case OPT_SWITCH:
		cvar_set (o->cvar, v ? "0" : "1");
		break;
	case OPT_NAMED:
		cvar_set (o->cvar, ftos ((v + d + 3) % 3));
		break;
	case OPT_NUMBER:
		v += d * o->step;
		if (v >= o->min && v <= o->max)
			cvar_set (o->cvar, ftos (v));
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
		cvar_set (sb_options[opt_pos].cvar, cvar_defstring (sb_options[opt_pos].cvar));
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
