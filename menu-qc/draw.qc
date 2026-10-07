// draw.qc -- the menus' drawing, in conchars' characters and the menus' pics:
// at the layout's pixels (UI_), as ezQuake's UI draws the server browser, and
// in id's 320 wide menus centered on the layout (M_, from menu_x), as the C
// menu drew them

float	menu_x;			// where id's menus' 320 begin: (width - 320) / 2

// C's division toward zero, as ezQuake centers
float(float v) Trunc =
{
	return v < 0 ? -floor (-v) : floor (v);
};

float(float c) Lower =
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
};

// s in n characters, cut or with spaces after (Pad), or before (PadRight)
string(string s, float n) Pad =
{
	return strpad (n, substring (s, 0, n));
};

string(string s, float n) PadRight =
{
	return strpad (-n, substring (s, 0, n));
};

// s in the red characters, the high half of conchars
string(string s) Red =
{
	float	i;
	string	red = "";

	for (i = 0 ; i < strlen (s) ; i++)
		red = strcat (red, chr2str (str2chr (s, i) | 128));
	return red;
};

/*
==============================================================================

AT THE LAYOUT'S PIXELS

==============================================================================
*/

// where a box of size is centered in room, on the characters' grid
float(float size, float room) UI_Centered =
{
	return floor (floor ((room - size) / 2) / 8) * 8;
};

void(float x, float y, float c) UI_Char =
{
	drawcharacter ([x, y, 0], c, '8 8 0', '1 1 1', 1, 0);
};

// text as it is, or red (ezQuake's UI_Print)
void(float x, float y, string s, float red) UI_Print =
{
	float	i, n;

	if (!red)
	{
		drawrawstring ([x, y, 0], s, '8 8 0', '1 1 1', 1, 0);
		return;
	}
	n = strlen (s);
	for (i = 0 ; i < n ; i++)
		UI_Char (x + i * 8, y, str2chr (s, i) | 128);
};

void(float x, float y, float w, string s, float red) UI_PrintCenter =
{
	UI_Print (x + Trunc ((w - 8 * strlen (s)) / 2), y, s, red);
};

// text in ezQuake's &cRGB colour, centered by what it shows
void(float x, float y, float w, string colour, string s) UI_PrintColoured =
{
	drawstring ([x + Trunc ((w - 8 * strlen (s)) / 2), y, 0], strcat ("&c", colour, s), '8 8 0', '1 1 1', 1, 0);
};

// a fill in ezQuake's colours: 0 to 255, alpha too
void(float x, float y, float w, float h, vector rgb, float a) UI_Fill =
{
	drawfill ([x, y, 0], [w, h, 0], rgb * (1 / 255), a / 255, 0);
};

// the marked row: menu_marked_bgcolor's 20 20 20 128, pulsing at menu_marked_fade's 4
void(float x, float y, float w, float h) UI_GrayBox =
{
	float	f = 0.5 + 0.25 * (1 + sin (4 * time));

	UI_Fill (x, y, w, h, [f, f, f] * 20, 128);
};

// a line of n characters: its ends and its middles
void(float x, float y, float n) UI_Line =
{
	float	i;

	for (i = 0 ; i < n ; i++)
		UI_Char (x + i * 8, y, i == 0 ? 0x1d : i == n - 1 ? 0x1f : 0x1e);
};

void(float x, float y, string pic) UI_Pic =
{
	drawpic ([x, y, 0], pic, drawgetimagesize (pic), '1 1 1', 1, 0);
};

// Draw_TextBox's box around width (in pairs of characters) by lines characters
void(float x, float y, float width, float lines) UI_TextBox =
{
	float	cx, cy, n;

	cy = y;
	UI_Pic (x, cy, "gfx/box_tl.lmp");
	for (n = 0 ; n < lines ; n++)
		UI_Pic (x, cy += 8, "gfx/box_ml.lmp");
	UI_Pic (x, cy + 8, "gfx/box_bl.lmp");
	for (cx = x + 8 ; width > 0 ; width -= 2, cx += 16)
	{
		cy = y;
		UI_Pic (cx, cy, "gfx/box_tm.lmp");
		for (n = 0 ; n < lines ; n++)
			UI_Pic (cx, cy += 8, n >= 1 ? "gfx/box_mm2.lmp" : "gfx/box_mm.lmp");
		UI_Pic (cx, cy + 8, "gfx/box_bm.lmp");
	}
	cy = y;
	UI_Pic (cx, cy, "gfx/box_tr.lmp");
	for (n = 0 ; n < lines ; n++)
		UI_Pic (cx, cy += 8, "gfx/box_mr.lmp");
	UI_Pic (cx, cy + 8, "gfx/box_br.lmp");
};

// a slider of cells characters, its ends either side, its knob range (0 to 1)
// of the way along
void(float x, float y, float cells, float range) UI_Slider =
{
	float	i;

	UI_Char (x - 8, y, 128);
	for (i = 0 ; i < cells ; i++)
		UI_Char (x + i * 8, y, 129);
	UI_Char (x + cells * 8, y, 130);
	UI_Char (floor (x + (cells - 1) * 8 * bound (0, range, 1)), y, 131);
};

// the blinking cursor's character
float() UI_Cursor =
{
	return 12 + (floor (time * 4) & 1);
};

// text's words in lines of width characters, up to lines; whether there was more
float(float x, float y, float width, float lines, string text) UI_TextBlock =
{
	float	n = tokenizebyseparator (text, " "), k, line = 0;
	string	s = "", word;

	for (k = 0 ; k < n ; k++)
	{
		word = argv (k);
		if (s != "" && strlen (s) + 1 + strlen (word) > width)
		{
			if (line >= lines)
				return TRUE;
			UI_Print (x, y + line++ * 8, s, FALSE);
			s = "";
		}
		s = s == "" ? substring (word, 0, width) : strcat (s, " ", word);
	}
	if (s != "")
	{
		if (line >= lines)
			return TRUE;
		UI_Print (x, y + line * 8, s, FALSE);
	}
	return FALSE;
};

/*
==============================================================================

IN ID'S MENUS

==============================================================================
*/

void(float cx, float line, float num) M_DrawCharacter =
{
	UI_Char (cx + menu_x, line, num);
};

void(float cx, float cy, string str) M_Print =
{
	UI_Print (cx + menu_x, cy, str, TRUE);
};

void(float cx, float cy, string str) M_PrintWhite =
{
	UI_Print (cx + menu_x, cy, str, FALSE);
};

void(float x, float y, string pic) M_DrawPic =
{
	UI_Pic (x + menu_x, y, pic);
};

// a title pic, centered over the menu
void(string pic) M_DrawTitle =
{
	vector	size = drawgetimagesize (pic);

	M_DrawPic (floor ((320 - size_x) / 2), 4, pic);
};

void(float x, float y, float width, float lines) M_DrawTextBox =
{
	UI_TextBox (x + menu_x, y, width, lines);
};

void(float x, float y, float range) M_DrawSlider =
{
	UI_Slider (x + menu_x, y, 10, range);
};

void(float x, float y, float on) M_DrawCheckbox =
{
	M_Print (x, y, on ? "on" : "off");
};
