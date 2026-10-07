// draw.qc -- the menus' drawing: id's 320 wide menus, centered on the layout
// (menu_x), in its characters and pics, as the C menu drew them

float	menu_x;			// where the menus' 320 begin: (width - 320) / 2

// one character of conchars
void(float cx, float line, float num) M_DrawCharacter =
{
	drawcharacter([cx + menu_x, line, 0], num, '8 8 0', '1 1 1', 1, 0);
};

// text in the red characters (the high half of conchars)
void(float cx, float cy, string str) M_Print =
{
	float	i, n = strlen (str);

	for (i = 0 ; i < n ; i++, cx += 8)
		M_DrawCharacter (cx, cy, str2chr (str, i) + 128);
};

void(float cx, float cy, string str) M_PrintWhite =
{
	drawrawstring ([cx + menu_x, cy, 0], str, '8 8 0', '1 1 1', 1, 0);
};

// a pic, 255 transparent
void(float x, float y, string pic) M_DrawPic =
{
	drawpic ([x + menu_x, y, 0], pic, drawgetimagesize (pic), '1 1 1', 1, 0);
};

// a title pic, centered over the menu
void(string pic) M_DrawTitle =
{
	vector	size = drawgetimagesize (pic);

	M_DrawPic (floor ((320 - size_x) / 2), 4, pic);
};

// the box of gfx/box_*.lmp around width (in pairs of characters) by lines characters
void(float x, float y, float width, float lines) M_DrawTextBox =
{
	float	cx, cy, n;

	// draw left side
	cx = x;
	cy = y;
	M_DrawPic (cx, cy, "gfx/box_tl.lmp");
	for (n = 0 ; n < lines ; n++)
	{
		cy += 8;
		M_DrawPic (cx, cy, "gfx/box_ml.lmp");
	}
	M_DrawPic (cx, cy + 8, "gfx/box_bl.lmp");

	// draw middle
	cx += 8;
	while (width > 0)
	{
		cy = y;
		M_DrawPic (cx, cy, "gfx/box_tm.lmp");
		for (n = 0 ; n < lines ; n++)
		{
			cy += 8;
			M_DrawPic (cx, cy, n >= 1 ? "gfx/box_mm2.lmp" : "gfx/box_mm.lmp");
		}
		M_DrawPic (cx, cy + 8, "gfx/box_bm.lmp");
		width -= 2;
		cx += 16;
	}

	// draw right side
	cy = y;
	M_DrawPic (cx, cy, "gfx/box_tr.lmp");
	for (n = 0 ; n < lines ; n++)
	{
		cy += 8;
		M_DrawPic (cx, cy, "gfx/box_mr.lmp");
	}
	M_DrawPic (cx, cy + 8, "gfx/box_br.lmp");
};

#define	SLIDER_RANGE	10

void(float x, float y, float range) M_DrawSlider =
{
	float	i;

	if (range < 0)
		range = 0;
	if (range > 1)
		range = 1;
	M_DrawCharacter (x - 8, y, 128);
	for (i = 0 ; i < SLIDER_RANGE ; i++)
		M_DrawCharacter (x + i * 8, y, 129);
	M_DrawCharacter (x + i * 8, y, 130);
	M_DrawCharacter (floor (x + (SLIDER_RANGE - 1) * 8 * range), y, 131);
};

void(float x, float y, float on) M_DrawCheckbox =
{
	if (on)
		M_Print (x, y, "on");
	else
		M_Print (x, y, "off");
};

// the blinking cursor's character
float() M_Cursor =
{
	return 12 + (floor (time * 4) & 1);
};
