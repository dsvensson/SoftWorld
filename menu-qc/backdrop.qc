// backdrop.qc -- what the menus lie on: the console's background with the
// version in its corner while the console is down (out of a game), else the
// fade, which leaves one of every four pixels of the screen showing. Both
// are images of the whole layout, made as the C menu drew them, again when
// its width changes.

#define	BACK_CONBACK	"*menuback"
#define	BACK_FADE		"*menufade"

float	back_width;		// the layout's width the images were made for

// the palette's index of a color it has (the first), 0 if none
int(int *pal, int color) PaletteIndex =
{
	int		i;

	for (i = 0 ; i < 256 ; i++)
		if (pal[i] == color)
			return i;
	return 0;
};

// the version stamped into img (imgw wide) in its bottom right corner, as
// Draw_ConsoleBackground did: a character's texels the palette's next rows on
// (0x60 + the index)
void(int *img, int imgw, int *chars, int *pal) StampVersion =
{
	string	ver = "SoftWorld " SW_VERSION;
	int		len = (int)strlen (ver);
	int		x0 = imgw - (len * 8 + 11);
	int		i, r, c, num, glyph;

	for (i = 0 ; i < len ; i++)
	{
		num = (int)str2chr (ver, (float)i);
		for (r = 0 ; r < 8 ; r++)
			for (c = 0 ; c < 8 ; c++)
			{
				glyph = chars[((num >> 4) * 8 + r) * 128 + (num & 15) * 8 + c];
				if ((glyph >> 24) & 255)
					img[(186 + r) * imgw + x0 + i * 8 + c] = pal[(0x60 + PaletteIndex (pal, glyph)) & 255];
			}
	}
};

// gfx/conback.lmp stretched to the layout's width and its 200 lines with its
// fixed point steps, the version in its corner: id's 320x200 one has it
// stamped in before, a mod's of another size (QBJ3's 854x480) after, at the
// layout's size
void(int width, int *pal) MakeConback =
{
	// one pointer a declaration: fteqcc takes int *a, *b's b as an int **
	int		*conback;
	int		*chars;
	int		*out;
	int		w, h, cw, ch;
	int		x, y, i, sy, fstep;
	int		opaque = 255 << 24;

	conback = r_readimage ("gfx/conback.lmp", w, h);
	chars = r_readimage ("gfx/conchars", cw, ch);
	if (!conback || !chars || w <= 0 || h <= 0 || cw != 128 || ch != 128)
	{
		if (conback)
			memfree (conback);
		if (chars)
			memfree (chars);
		return;
	}

	// drawn opaque: its 255 too
	for (i = 0 ; i < w * h ; i++)
		conback[i] = conback[i] | opaque;
	if (w == 320 && h == 200)
		StampVersion (conback, 320, chars, pal);

	out = memalloc (width * 200 * 4);
	fstep = w * 65536 / width;
	for (y = 0 ; y < 200 ; y++)
	{
		sy = y * h / 200;
		for (x = 0 ; x < width ; x++)
			out[y * width + x] = conback[sy * w + ((x * fstep) >> 16)];
	}
	if (w != 320 || h != 200)
		StampVersion (out, width, chars, pal);
	r_uploadimage (BACK_CONBACK, width, 200, out);
	memfree (out);
	memfree (conback);
	memfree (chars);
};

// the palette's color 0, opaque, on three of every four pixels of the layout:
// on even rows all but every fourth from the first, on odd ones from the third
void(int width, int *pal) MakeFade =
{
	int		*out = memalloc (width * 200 * 4);
	int		x, y;

	for (y = 0 ; y < 200 ; y++)
		for (x = 0 ; x < width ; x++)
			out[y * width + x] = (x & 3) == ((y & 1) << 1) ? 0 : pal[0];
	r_uploadimage (BACK_FADE, width, 200, out);
	memfree (out);
};

// the images for the layout's width, made when they aren't (the gamedir's
// change takes them)
void(float width) Backdrop_Make =
{
	int		*pal;
	int		pw, ph;

	if (width == back_width && drawgetimagesize (BACK_CONBACK) != '0 0 0'
		&& drawgetimagesize (BACK_FADE) != '0 0 0')
		return;
	back_width = width;
	pal = r_readimage ("gfx/palette.lmp", pw, ph);
	if (!pal)
		return;
	if (pw * ph == 256)
	{
		MakeConback ((int)width, pal);
		MakeFade ((int)width, pal);
	}
	memfree (pal);
};

// the console's background out of a game (where the console is down), else the fade
void(vector size) Backdrop_Draw =
{
	Backdrop_Make (size_x);
	drawpic ('0 0 0', clientstate () == 2 ? BACK_FADE : BACK_CONBACK, [size_x, size_y, 0], '1 1 1', 1, 0);
};
