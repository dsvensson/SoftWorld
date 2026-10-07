// sb_dialogs.qc -- the server browser's dialogs, ezQuake's: adding a server,
// adding a source, and the y/n confirmation; their edit boxes and rows

/*
==============================================================================

THEIR PARTS

==============================================================================
*/

// ezQuake's edit box: its text, its cursor, and what it shows of it
typedef struct
{
	string	text;
	float	pos;			// the cursor
	float	disp;			// the first character shown
	float	width, max;		// characters shown, and taken
} editbox_t;

void(editbox_t *e, float width, float max) SB_EditInit =
{
	e->text = "";
	e->pos = e->disp = 0;
	e->width = width;
	e->max = max;
};

void(editbox_t *e, float x, float y, float active) SB_EditDraw =
{
	while (e->pos > e->disp + e->width - 1)
		e->disp++;
	if (e->disp > e->pos)
		e->disp = e->pos;
	if (e->disp > strlen (e->text) - e->width + 1)
		e->disp = max (0, strlen (e->text) - e->width + 1);
	UI_Print (x, y, Pad (active ? substring (e->text, e->disp, 999) : e->text, e->width), FALSE);
	if (active)
		UI_Char (x + 8 * (e->pos - e->disp), y, 10 + (floor (time * 4) & 1));
};

void(editbox_t *e, float k) SB_EditKey =
{
	switch (k)
	{
	case K_LEFTARROW:
		e->pos = max (0, e->pos - 1);
		break;
	case K_RIGHTARROW:
		e->pos = min (e->pos + 1, strlen (e->text));
		break;
	case K_HOME:
		e->pos = 0;
		break;
	case K_END:
		e->pos = strlen (e->text);
		break;
	case K_DEL:
		e->text = strcat (substring (e->text, 0, e->pos), substring (e->text, e->pos + 1, 999));
		break;
	case K_BACKSPACE:
		if (e->pos > 0)
		{
			e->text = strcat (substring (e->text, 0, e->pos - 1), substring (e->text, e->pos, 999));
			e->pos--;
		}
		break;
	}
};

void(editbox_t *e, float c) SB_EditChar =
{
	if (c < ' ' || c > '}' || strlen (e->text) >= e->max)
		return;
	e->text = strcat (substring (e->text, 0, e->pos), chr2str (c), substring (e->text, e->pos, 999));
	e->pos++;
};

// a dialog's box: w by h, centered on the layout's grid; where it is
vector(float w, float h) SB_DialogBox =
{
	float	x = UI_Centered (w, sb_vw), y = UI_Centered (h, sb_vh);

	UI_TextBox (x - 16, y - 16, w / 8 + 1, h / 8 + 2);
	return [x, y];
};

// accept or cancel, red and marked where the cursor is
void(float x, float y, float w, string label, float active) SB_Button =
{
	UI_PrintCenter (x + 4, y, w, label, active);
	if (active)
		UI_Char (x + 59, y, 13);
};

// the keys that move a dialog's cursor through its rows (Ctrl+Home to the
// first, Ctrl+End to end), or close it: Escape, and Backspace off its text;
// whether the key was one of them
float(float *pos, float k, float rows, float end, float editing) SB_DialogKey =
{
	switch (k)
	{
	case K_UPARROW:
	case K_MWHEELUP:
		*pos = Wrap (*pos - 1, rows);
		return TRUE;
	case K_DOWNARROW:
	case K_MWHEELDOWN:
		*pos = Wrap (*pos + 1, rows);
		return TRUE;
	case K_TAB:
		*pos = Wrap (*pos + (sb_shift ? -1 : 1), rows);
		return TRUE;
	case K_HOME:
	case K_END:
		if (!sb_ctrl)
			return FALSE;
		*pos = k == K_HOME ? 0 : end;
		return TRUE;
	case K_BACKSPACE:
		if (editing)
			return FALSE;
		SB_CloseOverlay ();
		return TRUE;
	case K_ESCAPE:
		SB_CloseOverlay ();
		return TRUE;
	}
	return FALSE;
};

/*
==============================================================================

ADDING A SERVER

==============================================================================
*/

float		as_pos;			// address, accept, cancel
editbox_t	as_address;

void() SB_AddServer_Open =
{
	sb_overlay = SBO_ADDSERVER;
	as_pos = 0;
	SB_EditInit (&as_address, 14, 64);
};

void() SB_AddServer_Draw =
{
	vector	at = SB_DialogBox (176, 56);
	float	x = at_x - 4, y = at_y - 4, w = 184;

	UI_PrintCenter (x, y + 4, w, "Create new server", TRUE);
	UI_Print (x + 4, y + 20, "Address:", as_pos == 0);
	SB_EditDraw (&as_address, x + 70, y + 20, as_pos == 0);
	SB_Button (x, y + 40, w, "accept", as_pos == 1);
	SB_Button (x, y + 50, w, "cancel", as_pos == 2);
};

// added to the Unbound list, and once it is listed selected, its info up
void() SB_AddServer_Accept =
{
	SB_CloseOverlay ();
	if (as_address.text == "" || !addhostcacheserver (as_address.text))
		return;
	sb_tab = SB_SERVERS;
	sb_pending = as_address.text;
	sb_pendingtime = time;
};

void(float k) SB_AddServer_Key =
{
	if (k == K_ENTER)
	{
		if (as_pos == 2)
			SB_CloseOverlay ();
		else
			SB_AddServer_Accept ();
	}
	else if (!SB_DialogKey (&as_pos, k, 3, 1, as_pos == 0) && as_pos == 0)
		SB_EditKey (&as_address, k);
};

void(float c) SB_AddServer_Char =
{
	if (as_pos == 0 && !sb_ctrl && !sb_alt)
		SB_EditChar (&as_address, c);
};

/*
==============================================================================

ADDING A SOURCE

==============================================================================
*/

#define	NS_TYPES	3

float		ns_pos;				// type, name, where, accept, cancel
float		ns_type;
editbox_t	ns_name, ns_where;
string	ns_types[NS_TYPES] = {"master", "file", "url"};
string	ns_wherelabel[NS_TYPES] = {"addr", "file", "url"};

void() SB_AddSource_Open =
{
	sb_overlay = SBO_ADDSOURCE;
	ns_pos = 0;
	ns_type = 1;
	SB_EditInit (&ns_name, 16, 25);
	SB_EditInit (&ns_where, 16, 100);
};

// the edit box the cursor is on, if any
editbox_t*() SB_AddSource_Edit =
{
	if (ns_pos == 1)
		return &ns_name;
	if (ns_pos == 2)
		return &ns_where;
	return __NULL__;
};

void() SB_AddSource_Draw =
{
	vector	at = SB_DialogBox (176, 72);
	float	x = at_x - 4, y = at_y - 4, w = 184;

	UI_PrintCenter (x, y + 4, w, "Create new source", TRUE);
	UI_Print (x + 4, y + 20, "type", ns_pos == 0);
	UI_Print (x + 54, y + 20, Pad (ns_types[ns_type], 6), ns_pos == 0);
	UI_Print (x + 4, y + 30, "name", ns_pos == 1);
	SB_EditDraw (&ns_name, x + 54, y + 30, ns_pos == 1);
	UI_Print (x + 4, y + 40, ns_wherelabel[ns_type], ns_pos == 2);
	SB_EditDraw (&ns_where, x + 54, y + 40, ns_pos == 2);
	if (ns_pos <= 2)
		UI_Char (x + 42, y + 20 + 10 * ns_pos, 13);
	SB_Button (x, y + 60, w, "accept", ns_pos == 3);
	SB_Button (x, y + 70, w, "cancel", ns_pos == 4);
};

void(float k) SB_AddSource_Key =
{
	editbox_t	*edit = SB_AddSource_Edit ();

	if (k == K_ENTER)
	{
		if (ns_pos == 0)
			ns_type = Wrap (ns_type + 1, NS_TYPES);
		else
		{
			if (ns_pos != 4)
				addhostcachesource (ns_types[ns_type], ns_name.text, ns_where.text);
			SB_CloseOverlay ();
		}
	}
	else if (!SB_DialogKey (&ns_pos, k, 5, 4, edit != __NULL__) && edit)
		SB_EditKey (edit, k);
};

void(float c) SB_AddSource_Char =
{
	editbox_t	*edit = SB_AddSource_Edit ();

	if (ns_pos == 0 && (c == '+' || c == '=' || c == '-'))
		ns_type = Wrap (ns_type + 1, NS_TYPES);
	else if (edit)
		SB_EditChar (edit, c);
};

/*
==============================================================================

SURE? Y OR N

==============================================================================
*/

string	cf_text;
var void()	cf_action;

void(string text, void() action) SB_Confirm =
{
	cf_text = text;
	cf_action = action;
	sb_overlay = SBO_CONFIRM;
};

void() SB_Confirm_Draw =
{
	float	w = 32 + 8 * max (strlen (cf_text), 19);
	vector	at = SB_DialogBox (w, 24);

	UI_PrintCenter (at_x, at_y, w, cf_text, FALSE);
	UI_PrintCenter (at_x, at_y + 16, w, "Are you sure? <y/n>", TRUE);
};

void(float k) SB_Confirm_Key =
{
	if (k == K_ESCAPE || k == K_BACKSPACE)
		SB_CloseOverlay ();
};

void(float c) SB_Confirm_Char =
{
	if (c == 'y' || c == 'Y')
	{
		SB_CloseOverlay ();
		cf_action ();
	}
	else if (c == 'n' || c == 'N')
		SB_CloseOverlay ();
};
