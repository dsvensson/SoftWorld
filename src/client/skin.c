/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "cl_local.h"

cvar_t		baseskin = {.name = "baseskin", .string = "base"};
cvar_t		noskins = {.name = "noskins", .string = "0"};

static char		allskins[128];
#define	MAX_CACHED_SKINS		128
static skin_t		skins[MAX_CACHED_SKINS];
static int			numskins;

/*
================
Skin_Find

  Determines the best skin for the given scoreboard
  slot, and sets scoreboard->skin

================
*/
void Skin_Find (player_info_t *sc)
{
	skin_t		*skin;
	int			i;
	char		skinname[128], *s;

	if (allskins[0])
		Q_strncpyz (skinname, allskins, sizeof(skinname));
	else
	{
		s = Info_ValueForKey (sc->userinfo, "skin");
		if (s && s[0])
			Q_strncpyz (skinname, s, sizeof(skinname));
		else
			Q_strncpyz (skinname, baseskin.string, sizeof(skinname));
	}

	if (strstr (skinname, "..") || *skinname == '.')
		Q_strncpyz (skinname, "base", sizeof(skinname));

	COM_StripExtension (skinname, skinname);

	for (i=0 ; i<numskins ; i++)
	{
		if (!strcmp (skinname, skins[i].name))
		{
			sc->skin = &skins[i];
			Skin_Cache (sc->skin);
			return;
		}
	}

	if (numskins == MAX_CACHED_SKINS)
	{	// ran out of spots, so flush everything
		Skin_Skins_f ();
		return;
	}

	skin = &skins[numskins];
	sc->skin = skin;
	numskins++;

	memset (skin, 0, sizeof(*skin));
	strncpy(skin->name, skinname, sizeof(skin->name) - 1);
}


/*
==========
Skin_Cache

Returns a pointer to the skin bitmap, or NULL to use the default
==========
*/
byte	*Skin_Cache (skin_t *skin)
{
	char	skinpath[1024];
	byte	*file, *raw, *end;
	byte	*out, *pix;
	pcx_t	*pcx;
	int		x, y, len;
	int		dataByte;
	int		runLength;

	if (cls.downloadtype == dl_skin)
		return NULL;		// use base until downloaded

	if (noskins.value==1) // JACK: So NOSKINS > 1 will show skins, but
		return NULL;	  // not download new ones.

	if (skin->failedload)
		return NULL;

	if (skin->data)
		return skin->data;

//
// load the pic from disk
//
	snprintf (skinpath, sizeof(skinpath), "skins/%s.pcx", skin->name);
	file = FS_LoadFile (skinpath, &len);
	if (!file)
	{
		Con_Printf ("Couldn't load skin %s\n", skinpath);
		snprintf (skinpath, sizeof(skinpath), "skins/%s.pcx", baseskin.string);
		file = FS_LoadFile (skinpath, &len);
		if (!file)
		{
			skin->failedload = true;
			return NULL;
		}
	}

//
// parse the PCX file
//
	pcx = (pcx_t *)file;
	raw = &pcx->data;
	end = file + len;

	if (len < (int)sizeof(*pcx)
		|| pcx->manufacturer != 0x0a
		|| pcx->version != 5
		|| pcx->encoding != 1
		|| pcx->bits_per_pixel != 8
		|| pcx->xmax >= 320
		|| pcx->ymax >= 200)
	{
		Con_Printf ("Bad skin %s\n", skinpath);
		goto bad;
	}

	out = Mem_Calloc (1, 320*200);
	pix = out;

	for (y=0 ; y<pcx->ymax ; y++, pix += 320)
	{
		for (x=0 ; x<=pcx->xmax ; )
		{
			if (raw >= end)
				goto malformed;
			dataByte = *raw++;

			if((dataByte & 0xC0) == 0xC0)
			{
				runLength = dataByte & 0x3F;
				if (raw >= end)
					goto malformed;
				dataByte = *raw++;
			}
			else
				runLength = 1;

			// skin sanity check
			if (runLength + x > pcx->xmax + 2)
				goto malformed;
			while(runLength-- > 0)
				pix[x++] = (byte)dataByte;
		}

	}

	Mem_Free (file);
	skin->data = out;
	skin->failedload = false;
	return out;

malformed:
	Mem_Free (out);
	Con_Printf ("Skin %s was malformed.  You should delete it.\n", skinpath);
bad:
	Mem_Free (file);
	skin->failedload = true;
	return NULL;
}


/*
=================
Skin_NextDownload
=================
*/
void Skin_NextDownload (void)
{
	player_info_t	*sc;
	int			i;

	if (cls.downloadnumber == 0)
		Con_Printf ("Checking skins...\n");
	cls.downloadtype = dl_skin;

	for ( 
		; cls.downloadnumber != MAX_CLIENTS
		; cls.downloadnumber++)
	{
		sc = &cl.players[cls.downloadnumber];
		if (!sc->name[0])
			continue;
		Skin_Find (sc);
		if (noskins.value)
			continue;
		if (!CL_CheckOrDownloadFile(va("skins/%s.pcx", sc->skin->name)))
			return;		// started a download
	}

	cls.downloadtype = dl_none;

	// now load them in for real
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		sc = &cl.players[i];
		if (!sc->name[0])
			continue;
		Skin_Cache (sc->skin);
	}

	if (cls.state != ca_active)
	{	// get next signon phase
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message,
			va("begin %i", cl.servercount));
	}
}


/*
==========
Skin_Skins_f

Refind all skins, downloading if needed.
==========
*/
void	Skin_Skins_f (void)
{
	int		i;

	for (i=0 ; i<numskins ; i++)
	{
		Mem_Free (skins[i].data);
		skins[i].data = NULL;
	}
	numskins = 0;

	cls.downloadnumber = 0;
	cls.downloadtype = dl_skin;
	Skin_NextDownload ();
}


/*
==========
Skin_AllSkins_f

Sets all skins to one specific one
==========
*/
void	Skin_AllSkins_f (void)
{
	Q_strncpyz (allskins, Cmd_Argv(1), sizeof(allskins));
	Skin_Skins_f ();
}

/*
==========
Skin_ForPlayer

The player's skin pixels for drawing, loading them when needed
==========
*/
byte *Skin_ForPlayer (player_info_t *info)
{
	if (!info->skin)
		Skin_Find (info);
	return Skin_Cache (info->skin);
}

/*
=============================================================================

PLAYER COLORS

A skin's shirt and pants are the palette's rows 1 and 6, and a player's
colors put other rows there. 0 to 13, the rows players dress in, go there as
painted, those past the middle of the palette backwards as they were
painted. 14 (the fire row's orange), 15 (the last row's dark red) and 16
(black, which old clients gave by running off the end of the palette) are
ramps worked out from the one color, as qualia does: shaded as the painted
rows are around the color at the ninth step, the upper steps kept from
clipping, and black lifted to grey so its folds still show. Under RGB
lighting a ramp is drawn as it is, through the colormap as the nearest
colors the palette has.

=============================================================================
*/

#define	BLACK_COLOR			16			// one past the palette's rows
#define	CLOTHING_COLORS		14			// the rows players dress in
#define	SWATCH				8			// the step that stands for a row
#define	MIN_HIGHLIGHT		72.0f		// how bright a ramp's brightest step at least gets
#define	FIRST_FULLBRIGHT	224			// the colormap leaves the rest unshaded

// the color a number stands for: its row's ninth step, or black
static void Skin_Swatch (int color, byte rgb[3])
{
	if (color >= BLACK_COLOR)
		memset (rgb, 0, 3);
	else
		memcpy (rgb, cls.basepal + (color * 16 + SWATCH) * 3, 3);
}

/*
================
Skin_Ramp

Sixteen steps from dark to bright with home at the ninth: the painted rows
are about their brightest step times (i + 1) / 16, so the steps up to home
are too; above it the steps go on to what fits under white, so that no
channel clips before the others and moves the hue. A color too dark for
that to show anything is lifted with grey.
================
*/
static void Skin_Ramp (const byte home[3], byte shades[16][3])
{
	float	full = 16.0f / (SWATCH + 1);
	float	reach, lift, scale, v;
	int		brightest, i, k;

	brightest = home[0] > home[1] ? home[0] : home[1];
	brightest = brightest > home[2] ? brightest : home[2];
	reach = brightest && 255.0f / brightest < full ? 255.0f / brightest : full;
	lift = MIN_HIGHLIGHT - brightest * reach;
	if (lift < 0)
		lift = 0;

	for (i=0 ; i<16 ; i++)
	{
		scale = i <= SWATCH ? (i + 1.0f) / (SWATCH + 1) : 1 + (reach - 1) * (i - SWATCH) / (15.0f - SWATCH);
		for (k=0 ; k<3 ; k++)
		{
			v = home[k] * scale + lift * (i + 1) / 16 + 0.5f;
			shades[i][k] = v > 255 ? 255 : (byte)v;
		}
	}
}

// the palette's nearest color the colormap shades
static byte Skin_Nearest (const byte rgb[3])
{
	const byte	*p;
	int			i, d, best, bestd;

	best = 0;
	bestd = 0x7fffffff;
	for (i=0 ; i<FIRST_FULLBRIGHT ; i++)
	{
		p = cls.basepal + i * 3;
		d = (p[0] - rgb[0]) * (p[0] - rgb[0]) + (p[1] - rgb[1]) * (p[1] - rgb[1]) + (p[2] - rgb[2]) * (p[2] - rgb[2]);
		if (d < bestd)
		{
			bestd = d;
			best = i;
		}
	}
	return (byte)best;
}

/*
================
Skin_Colors

The player's translation and palette from the player's colors; numbers past
16 are black, below 0 the first row (qualia)
================
*/
void Skin_Colors (player_info_t *player)
{
	const int	ranges[2] = {TOP_RANGE, BOTTOM_RANGE};
	const int	colors[2] = {player->topcolor, player->bottomcolor};
	const byte	*pal = cls.basepal;
	byte		home[3], shades[16][3];
	int			r, i, color, start, index;

	for (i=0 ; i<256 ; i++)
	{
		player->translate[i] = (byte)i;
		player->palette[i] = RGB30 (pal[i*3], pal[i*3+1], pal[i*3+2]);
	}

	for (r=0 ; r<2 ; r++)
	{
		color = colors[r] < 0 ? 0 : colors[r] > BLACK_COLOR ? BLACK_COLOR : colors[r];
		if (color < CLOTHING_COLORS)
		{
			start = color * 16;
			for (i=0 ; i<16 ; i++)
			{
				index = start < 128 ? start + i : start + 15 - i;
				player->translate[ranges[r] + i] = (byte)index;
				player->palette[ranges[r] + i] = RGB30 (pal[index*3], pal[index*3+1], pal[index*3+2]);
			}
			continue;
		}
		Skin_Swatch (color, home);
		Skin_Ramp (home, shades);
		for (i=0 ; i<16 ; i++)
		{
			player->translate[ranges[r] + i] = Skin_Nearest (shades[i]);
			player->palette[ranges[r] + i] = RGB30 (shades[i][0], shades[i][1], shades[i][2]);
		}
	}
}

/*
================
Skin_ColorIndex

The palette index a player's color is shown as on the scoreboard: the ninth
step of its row, or black
================
*/
int Skin_ColorIndex (int color)
{
	color = color < 0 ? 0 : color > BLACK_COLOR ? BLACK_COLOR : color;
	return color == BLACK_COLOR ? 0 : color * 16 + SWATCH;
}
