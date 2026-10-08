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
// sbar.c -- status bar code

#include "cl_local.h"



#define STAT_MINUS		10	// num frame for '-' stats digit
static qpic_t		*sb_nums[2][11];
static qpic_t		*sb_colon, *sb_slash;
static qpic_t		*sb_ibar;
static qpic_t		*sb_sbar;
static qpic_t		*sb_scorebar;

static qpic_t		*sb_weapons[7][8];	// 0 is active, 1 is owned, 2-5 are flashes
static qpic_t		*sb_ammo[4];
static qpic_t		*sb_sigil[4];
static qpic_t		*sb_armor[3];
static qpic_t		*sb_items[32];

static qpic_t	*sb_faces[7][2];		// 0 is gibbed, 1 is dead, 2-6 are alive
							// 0 is static, 1 is temporary animation
static qpic_t	*sb_face_invis;
static qpic_t	*sb_face_quad;
static qpic_t	*sb_face_invuln;
static qpic_t	*sb_face_invis_invuln;

static bool	sb_showscores;
static bool	sb_showteamscores;
static int	sbar_xofs;		// its left edge (Sbar_Draw)


static void Sbar_DeathmatchOverlay (int start);
static void Sbar_TeamOverlay (void);
static void Sbar_MiniDeathmatchOverlay (void);

static bool largegame = false;

/*
===============
Sbar_ShowTeamScores

Tab key down
===============
*/
static void Sbar_ShowTeamScores (void)
{
	if (sb_showteamscores)
		return;

	sb_showteamscores = true;
}

/*
===============
Sbar_DontShowTeamScores

Tab key up
===============
*/
static void Sbar_DontShowTeamScores (void)
{
	sb_showteamscores = false;
}

/*
===============
Sbar_ShowScores

Tab key down
===============
*/
static void Sbar_ShowScores (void)
{
	if (sb_showscores)
		return;

	sb_showscores = true;
}

/*
===============
Sbar_DontShowScores

Tab key up
===============
*/
static void Sbar_DontShowScores (void)
{
	sb_showscores = false;
}

/*
===============
Sbar_Init
===============
*/
bool Sbar_ShowingScores (void)
{
	return sb_showscores;
}

void Sbar_Init (void)
{
	Sbar_WadPics ();

	Cmd_AddCommand ("+showscores", Sbar_ShowScores, "Shows the scoreboard while held (bind to a key).");
	Cmd_AddCommand ("-showscores", Sbar_DontShowScores,
		"Hides the scoreboard (when the +showscores key is let go).");

	Cmd_AddCommand ("+showteamscores", Sbar_ShowTeamScores, "Shows the teams' scores while held (bind to a key).");
	Cmd_AddCommand ("-showteamscores", Sbar_DontShowTeamScores,
		"Hides the teams' scores (when the +showteamscores key is let go).");
}

// gfx.wad's status bar pics, again when it changes
void Sbar_WadPics (void)
{
	int		i;

	for (i=0 ; i<10 ; i++)
	{
		sb_nums[0][i] = Draw_PicFromWad (va("num_%i",i));
		sb_nums[1][i] = Draw_PicFromWad (va("anum_%i",i));
	}

	sb_nums[0][10] = Draw_PicFromWad ("num_minus");
	sb_nums[1][10] = Draw_PicFromWad ("anum_minus");
	sb_colon = Draw_PicFromWad ("num_colon");
	sb_slash = Draw_PicFromWad ("num_slash");

	sb_weapons[0][0] = Draw_PicFromWad ("inv_shotgun");
	sb_weapons[0][1] = Draw_PicFromWad ("inv_sshotgun");
	sb_weapons[0][2] = Draw_PicFromWad ("inv_nailgun");
	sb_weapons[0][3] = Draw_PicFromWad ("inv_snailgun");
	sb_weapons[0][4] = Draw_PicFromWad ("inv_rlaunch");
	sb_weapons[0][5] = Draw_PicFromWad ("inv_srlaunch");
	sb_weapons[0][6] = Draw_PicFromWad ("inv_lightng");
	
	sb_weapons[1][0] = Draw_PicFromWad ("inv2_shotgun");
	sb_weapons[1][1] = Draw_PicFromWad ("inv2_sshotgun");
	sb_weapons[1][2] = Draw_PicFromWad ("inv2_nailgun");
	sb_weapons[1][3] = Draw_PicFromWad ("inv2_snailgun");
	sb_weapons[1][4] = Draw_PicFromWad ("inv2_rlaunch");
	sb_weapons[1][5] = Draw_PicFromWad ("inv2_srlaunch");
	sb_weapons[1][6] = Draw_PicFromWad ("inv2_lightng");
	
	for (i=0 ; i<5 ; i++)
	{
		sb_weapons[2+i][0] = Draw_PicFromWad (va("inva%i_shotgun",i+1));
		sb_weapons[2+i][1] = Draw_PicFromWad (va("inva%i_sshotgun",i+1));
		sb_weapons[2+i][2] = Draw_PicFromWad (va("inva%i_nailgun",i+1));
		sb_weapons[2+i][3] = Draw_PicFromWad (va("inva%i_snailgun",i+1));
		sb_weapons[2+i][4] = Draw_PicFromWad (va("inva%i_rlaunch",i+1));
		sb_weapons[2+i][5] = Draw_PicFromWad (va("inva%i_srlaunch",i+1));
		sb_weapons[2+i][6] = Draw_PicFromWad (va("inva%i_lightng",i+1));
	}

	sb_ammo[0] = Draw_PicFromWad ("sb_shells");
	sb_ammo[1] = Draw_PicFromWad ("sb_nails");
	sb_ammo[2] = Draw_PicFromWad ("sb_rocket");
	sb_ammo[3] = Draw_PicFromWad ("sb_cells");

	sb_armor[0] = Draw_PicFromWad ("sb_armor1");
	sb_armor[1] = Draw_PicFromWad ("sb_armor2");
	sb_armor[2] = Draw_PicFromWad ("sb_armor3");

	sb_items[0] = Draw_PicFromWad ("sb_key1");
	sb_items[1] = Draw_PicFromWad ("sb_key2");
	sb_items[2] = Draw_PicFromWad ("sb_invis");
	sb_items[3] = Draw_PicFromWad ("sb_invuln");
	sb_items[4] = Draw_PicFromWad ("sb_suit");
	sb_items[5] = Draw_PicFromWad ("sb_quad");

	sb_sigil[0] = Draw_PicFromWad ("sb_sigil1");
	sb_sigil[1] = Draw_PicFromWad ("sb_sigil2");
	sb_sigil[2] = Draw_PicFromWad ("sb_sigil3");
	sb_sigil[3] = Draw_PicFromWad ("sb_sigil4");

	sb_faces[4][0] = Draw_PicFromWad ("face1");
	sb_faces[4][1] = Draw_PicFromWad ("face_p1");
	sb_faces[3][0] = Draw_PicFromWad ("face2");
	sb_faces[3][1] = Draw_PicFromWad ("face_p2");
	sb_faces[2][0] = Draw_PicFromWad ("face3");
	sb_faces[2][1] = Draw_PicFromWad ("face_p3");
	sb_faces[1][0] = Draw_PicFromWad ("face4");
	sb_faces[1][1] = Draw_PicFromWad ("face_p4");
	sb_faces[0][0] = Draw_PicFromWad ("face5");
	sb_faces[0][1] = Draw_PicFromWad ("face_p5");

	sb_face_invis = Draw_PicFromWad ("face_invis");
	sb_face_invuln = Draw_PicFromWad ("face_invul2");
	sb_face_invis_invuln = Draw_PicFromWad ("face_inv2");
	sb_face_quad = Draw_PicFromWad ("face_quad");

	sb_sbar = Draw_PicFromWad ("sbar");
	sb_ibar = Draw_PicFromWad ("ibar");
	sb_scorebar = Draw_PicFromWad ("scorebar");
}


//=============================================================================

// drawing routines are reletive to the status bar location

/*
=============
Sbar_DrawPic
=============
*/
static void Sbar_DrawPic (int x, int y, qpic_t *pic)
{
	Draw_Pic (sbar_xofs + x, y + (vid.conheight-SBAR_HEIGHT), pic);
}

/*
=============
Sbar_DrawSubPic
=============
JACK: Draws a portion of the picture in the status bar.
*/

static void Sbar_DrawSubPic(int x, int y, qpic_t *pic, int srcx, int srcy, int width, int height) 
{
	Draw_SubPic (sbar_xofs + x, y+(vid.conheight-SBAR_HEIGHT), pic, srcx, srcy, width, height);
}


/*
=============
Sbar_DrawTransPic
=============
*/
static void Sbar_DrawTransPic (int x, int y, qpic_t *pic)
{
	Draw_TransPic (sbar_xofs + x, y + (vid.conheight-SBAR_HEIGHT), pic);
}

/*
================
Sbar_DrawCharacter

Draws one solid graphics character
================
*/
static void Sbar_DrawCharacter (int x, int y, int num)
{
	Draw_Character (sbar_xofs + x + 4, y + vid.conheight-SBAR_HEIGHT, num);
}

/*
================
Sbar_DrawString
================
*/
// in the colors a player's name in it may give
static void Sbar_DrawString (int x, int y, char *str)
{
	Draw_MarkupString (sbar_xofs + x, y+ vid.conheight-SBAR_HEIGHT, str);
}

/*
=============
Sbar_itoa
=============
*/
static int Sbar_itoa (int num, char *buf)
{
	char	*str;
	int		pow10;
	int		dig;
	
	str = buf;
	
	if (num < 0)
	{
		*str++ = '-';
		num = -num;
	}
	
	for (pow10 = 10 ; num >= pow10 ; pow10 *= 10)
	;
	
	do
	{
		pow10 /= 10;
		dig = num/pow10;
		*str++ = (char)('0'+dig);
		num -= dig*pow10;
	} while (pow10 != 1);

	*str = 0;

	return (int)(str-buf);
}


/*
=============
Sbar_DrawNum
=============
*/
static void Sbar_DrawNum (int x, int y, int num, int digits, int color)
{
	char			str[12];
	char			*ptr;
	int				l, frame;

	l = Sbar_itoa (num, str);
	ptr = str;
	if (l > digits)
		ptr += (l-digits);
	if (l < digits)
		x += (digits-l)*24;

	while (*ptr)
	{
		if (*ptr == '-')
			frame = STAT_MINUS;
		else
			frame = *ptr -'0';

		Sbar_DrawTransPic (x,y,sb_nums[color][frame]);
		x += 24;
		ptr++;
	}
}

//=============================================================================

//ZOID: this should be MAX_CLIENTS, not MAX_SCOREBOARD!!
//int		fragsort[MAX_SCOREBOARD];
static int		fragsort[MAX_CLIENTS];
static int		scoreboardlines;
typedef struct {
	char team[16+1];
	int frags;
	int players;
	int plow, phigh, ptotal;
} team_t;
static team_t teams[MAX_CLIENTS];
static int teamsort[MAX_CLIENTS];
static int scoreboardteams;

/*
===============
Sbar_SortFrags
===============
*/
static void Sbar_SortFrags (bool includespec)
{
	int		i, j, k;
		
// sort by frags
	scoreboardlines = 0;
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		if (cl.players[i].name[0] &&
			(!cl.players[i].spectator || includespec))
		{
			fragsort[scoreboardlines] = i;
			scoreboardlines++;
			if (cl.players[i].spectator)
				cl.players[i].frags = -999;
		}
	}
		
	for (i=0 ; i<scoreboardlines ; i++)
		for (j=0 ; j<scoreboardlines-1-i ; j++)
			if (cl.players[fragsort[j]].frags < cl.players[fragsort[j+1]].frags)
			{
				k = fragsort[j];
				fragsort[j] = fragsort[j+1];
				fragsort[j+1] = k;
			}
}

static void Sbar_SortTeams (void)
{
	int				i, j, k;
	player_info_t	*s;
	int				teamplay;
	char t[16+1];

// request new ping times every two second
	scoreboardteams = 0;

	teamplay = atoi(Info_ValueForKey(cl.serverinfo, "teamplay"));
	if (!teamplay)
		return;

// sort the teams
	memset(teams, 0, sizeof(teams));
	for (i = 0; i < MAX_CLIENTS; i++)
		teams[i].plow = 999;

	for (i = 0; i < MAX_CLIENTS; i++) {
		s = &cl.players[i];
		if (!s->name[0])
			continue;
		if (s->spectator)
			continue;

		// find his team in the list
		t[16] = 0;
		strncpy(t, Info_ValueForKey(s->userinfo, "team"), 16);
		if (!t[0])
			continue; // not on team
		for (j = 0; j < scoreboardteams; j++)
			if (!strcmp(teams[j].team, t)) {
				teams[j].frags += s->frags;
				teams[j].players++;
				goto addpinginfo;
			}
		if (j == scoreboardteams) { // must add him
			j = scoreboardteams++;
			Q_strncpyz(teams[j].team, t, sizeof(teams[j].team));
			teams[j].frags = s->frags;
			teams[j].players = 1;
addpinginfo:
			if (teams[j].plow > s->ping)
				teams[j].plow = s->ping;
			if (teams[j].phigh < s->ping)
				teams[j].phigh = s->ping;
			teams[j].ptotal += s->ping;
		}
	}

	// sort
	for (i = 0; i < scoreboardteams; i++)
		teamsort[i] = i;

	// good 'ol bubble sort
	for (i = 0; i < scoreboardteams - 1; i++)
		for (j = i + 1; j < scoreboardteams; j++)
			if (teams[teamsort[i]].frags < teams[teamsort[j]].frags) {
				k = teamsort[i];
				teamsort[i] = teamsort[j];
				teamsort[j] = k;
			}
}

// 14 to 16, orange, dark red and black, as players have them now (skin.c)
static int	Sbar_ColorForMap (int m)
{
	return Skin_ColorIndex (m);
}


// a deathmatch's, as the serverinfo says (QuakeWorld's when it doesn't), not
// single player or coop
static bool Sbar_Deathmatch (void)
{
	const char	*dm = Info_ValueForKey (cl.serverinfo, "deathmatch");

	return !*dm || atoi (dm);
}

// hudstyle as ironwail bounds it, 0 to 3
static int Sbar_HudStyle (void)
{
	return !(hudstyle.value > 0) ? 0 : hudstyle.value >= 3 ? 3 : (int)hudstyle.value;
}

// whether the status bar takes the lines below the view (full: a view of
// viewsize 100 and over), rather than being drawn over it: the classic one
// while opaque, as ironwail has it; QuakeWorld's in a smaller view; never the
// modern ones or a game's QuakeC one
bool Sbar_Below (bool full)
{
	if (CSQC_DrawsHud ())
		return false;
	switch (Sbar_HudStyle ())
	{
	case 0:
		return scr_sbaralpha.value >= 1;
	case 3:
		return !full;
	default:
		return false;
	}
}

// QuakeWorld's heads-up bar, without its backdrops: hudstyle 3 in a view of
// the whole screen
static bool Sbar_Headsup (void)
{
	return Sbar_HudStyle () == 3 && scr_viewsize.value >= 100;
}

// a backdrop (the bar's, the inventory's, the scores'): the classic and the
// modern ones' by scr_sbaralpha, as ironwail's; QuakeWorld's opaque
static void Sbar_DrawBackdrop (int x, int y, qpic_t *pic)
{
	if (Sbar_HudStyle () != 3 && scr_sbaralpha.value < 1)
		Draw_QCPic ((float)(sbar_xofs + x), (float)(y + (int)vid.conheight - SBAR_HEIGHT), (float)pic->width,
			(float)pic->height, pic, 0, 0, 1, 1, scr_sbaralpha.value);
	else
		Sbar_DrawPic (x, y, pic);
}

/*
===============
Sbar_SoloScoreboard

The time; and outside deathmatch the monsters killed and secrets found of the
level's, and its name (FTE's Sbar_CoopScoreboard)
===============
*/
static void Sbar_SoloScoreboard (void)
{
	char	str[80];
	int		minutes, seconds, tens, units;
	double	time = Sbar_Deathmatch () ? cl.time : CL_LevelTime ();

	Sbar_DrawBackdrop (0, 0, sb_scorebar);

	if (!Sbar_Deathmatch ())
	{
		snprintf (str, sizeof(str), "Monsters:%3i /%3i", cl.stats[STAT_MONSTERS], cl.stats[STAT_TOTALMONSTERS]);
		Sbar_DrawString (8, 4, str);
		snprintf (str, sizeof(str), "Secrets :%3i /%3i", cl.stats[STAT_SECRETS], cl.stats[STAT_TOTALSECRETS]);
		Sbar_DrawString (8, 12, str);
		Sbar_DrawString (232 - (int)strlen (cl.levelname)*4, 12, cl.levelname);
	}

	// time
	minutes = (int)(time / 60);
	seconds = (int)(time - 60*minutes);
	tens = seconds / 10;
	units = seconds - 10*tens;
	snprintf (str, sizeof(str), "Time :%3i:%i%i", minutes, tens, units);
	Sbar_DrawString (184, 4, str);
}

//=============================================================================

/*
===============
Sbar_DrawInventory
===============
*/
static void Sbar_DrawInventory (void)
{	
	int		i;
	char	num[6];
	float	time;
	int		flashon;
	bool	headsup;
	bool    hudswap;

	headsup = Sbar_Headsup ();
	hudswap = cl_hudswap.value; // Get that nasty float out :)

	if (!headsup)
		Sbar_DrawBackdrop (0, -24, sb_ibar);
// weapons
	for (i=0 ; i<7 ; i++)
	{
		if (cl.stats[STAT_ITEMS] & (IT_SHOTGUN<<i) )
		{
			time = cl.item_gettime[i];
			flashon = (int)((cl.time - time)*10);
			if (flashon < 0)
				flashon = 0;
			if (flashon >= 10)
			{
				if ( cl.stats[STAT_ACTIVEWEAPON] == (IT_SHOTGUN<<i)  )
					flashon = 1;
				else
					flashon = 0;
			}
			else
				flashon = (flashon%5) + 2;

			if (headsup) {
				if (i || vid.conheight>200)
					Sbar_DrawSubPic ((hudswap) ? 0 : (vid.conwidth-24),-68-(7-i)*16 , sb_weapons[flashon][i],0,0,24,16);
			
			} else 
			Sbar_DrawPic (i*24, -16, sb_weapons[flashon][i]);
//			Sbar_DrawSubPic (0,0,20,20,i*24, -16, sb_weapons[flashon][i]);

		}
	}

// ammo counts
	for (i=0 ; i<4 ; i++)
	{
		snprintf (num, sizeof(num), "%3i",cl.stats[STAT_SHELLS+i] );
		if (headsup) {
//			Sbar_DrawSubPic(3, -24, sb_ibar, 3, 0, 42,11);
			Sbar_DrawSubPic((hudswap) ? 0 : (vid.conwidth-42), -24 - (4-i)*11, sb_ibar, 3+(i*48), 0, 42, 11);
			if (num[0] != ' ')
				Sbar_DrawCharacter ( (hudswap) ? 3 : (vid.conwidth-39), -24 - (4-i)*11, 18 + num[0] - '0');
			if (num[1] != ' ')
				Sbar_DrawCharacter ( (hudswap) ? 11 : (vid.conwidth-31), -24 - (4-i)*11, 18 + num[1] - '0');
			if (num[2] != ' ')
				Sbar_DrawCharacter ( (hudswap) ? 19 : (vid.conwidth-23), -24 - (4-i)*11, 18 + num[2] - '0');
		} else {
		if (num[0] != ' ')
			Sbar_DrawCharacter ( (6*i+1)*8 - 2, -24, 18 + num[0] - '0');
		if (num[1] != ' ')
			Sbar_DrawCharacter ( (6*i+2)*8 - 2, -24, 18 + num[1] - '0');
		if (num[2] != ' ')
			Sbar_DrawCharacter ( (6*i+3)*8 - 2, -24, 18 + num[2] - '0');
	}
	}
	
	flashon = 0;
// items
	for (i=0 ; i<6 ; i++)
		if (cl.stats[STAT_ITEMS] & (1<<(17+i)))
		{
			time = cl.item_gettime[17+i];
			if (!(time && time > cl.time - 2 && flashon))
				Sbar_DrawPic (192 + i*16, -16, sb_items[i]);
		}

// sigils
	for (i=0 ; i<4 ; i++)
		if (cl.stats[STAT_ITEMS] & (1u<<(28+i)))
		{
			time = cl.item_gettime[28+i];
			if (!(time && time > cl.time - 2 && flashon))
				Sbar_DrawPic (320-32 + i*8, -16, sb_sigil[i]);
		}
}

//=============================================================================

/*
===============
Sbar_DrawFrags
===============
*/
static void Sbar_DrawFrags (void)
{	
	int				i, k, l;
	int				top, bottom;
	int				x, y, f;
	char			num[12];
	player_info_t	*s;
	
	Sbar_SortFrags (false);

// draw the text
	l = scoreboardlines <= 4 ? scoreboardlines : 4;
	
	x = 23;
//	xofs = (vid.conwidth - 320)>>1;
	y = vid.conheight - SBAR_HEIGHT - 23;

	for (i=0 ; i<l ; i++)
	{
		k = fragsort[i];
		s = &cl.players[k];
		if (!s->name[0])
			continue;
		if (s->spectator)
			continue;

	// draw background
		top = s->topcolor;
		bottom = s->bottomcolor;
		top = (top < 0) ? 0 : ((top > 13) ? 13 : top);
		bottom = (bottom < 0) ? 0 : ((bottom > 13) ? 13 : bottom);

		top = Sbar_ColorForMap (top);
		bottom = Sbar_ColorForMap (bottom);
	
//		Draw_Fill (xofs + x*8 + 10, y, 28, 4, top);
//		Draw_Fill (xofs + x*8 + 10, y+4, 28, 3, bottom);
		Draw_Fill (x*8 + 10, y, 28, 4, top);
		Draw_Fill (x*8 + 10, y+4, 28, 3, bottom);

	// draw number
		f = s->frags;
		snprintf (num, sizeof(num), "%3i",f);

		Sbar_DrawCharacter ( (x+1)*8 , -24, num[0]);
		Sbar_DrawCharacter ( (x+2)*8 , -24, num[1]);
		Sbar_DrawCharacter ( (x+3)*8 , -24, num[2]);

		if (k == cl.playernum)
		{
			Sbar_DrawCharacter (x*8+2, -24, 16);
			Sbar_DrawCharacter ( (x+4)*8-4, -24, 17);
		}
		x+=4;
	}
}

//=============================================================================


// the face: the powerups', else by the health, its pain a moment after a hit
static qpic_t *Sbar_FacePic (void)
{
	int		items = cl.stats[STAT_ITEMS];
	int		f;

	if ((items & (IT_INVISIBILITY | IT_INVULNERABILITY)) == (IT_INVISIBILITY | IT_INVULNERABILITY))
		return sb_face_invis_invuln;
	if (items & IT_QUAD)
		return sb_face_quad;
	if (items & IT_INVISIBILITY)
		return sb_face_invis;
	if (items & IT_INVULNERABILITY)
		return sb_face_invuln;

	if (cl.stats[STAT_HEALTH] >= 100)
		f = 4;
	else
		f = cl.stats[STAT_HEALTH] / 20;
	if (f < 0)
		f = 0;
	return sb_faces[f][cl.time <= cl.faceanimtime];
}

// the ammo of the weapon in hand, NULL for none
static qpic_t *Sbar_AmmoPic (void)
{
	int		items = cl.stats[STAT_ITEMS];

	if (items & IT_SHELLS)
		return sb_ammo[0];
	if (items & IT_NAILS)
		return sb_ammo[1];
	if (items & IT_ROCKETS)
		return sb_ammo[2];
	if (items & IT_CELLS)
		return sb_ammo[3];
	return NULL;
}

// the armor worn, the disc while invulnerable, NULL for none
static qpic_t *Sbar_ArmorPic (void)
{
	int		items = cl.stats[STAT_ITEMS];

	if (items & IT_INVULNERABILITY)
		return draw_disc;
	if (items & IT_ARMOR3)
		return sb_armor[2];
	if (items & IT_ARMOR2)
		return sb_armor[1];
	if (items & IT_ARMOR1)
		return sb_armor[0];
	return NULL;
}

/*
=============
Sbar_DrawNormal
=============
*/
static void Sbar_DrawNormal (void)
{
	qpic_t	*pic;

	if (!Sbar_Headsup ())
		Sbar_DrawBackdrop (0, 0, sb_sbar);

// armor
	if (cl.stats[STAT_ITEMS] & IT_INVULNERABILITY)
		Sbar_DrawNum (24, 0, 666, 3, 1);
	else
		Sbar_DrawNum (24, 0, cl.stats[STAT_ARMOR], 3, cl.stats[STAT_ARMOR] <= 25);
	if ((pic = Sbar_ArmorPic ()))
		Sbar_DrawPic (0, 0, pic);

// face
	Sbar_DrawPic (112, 0, Sbar_FacePic ());

// health
	Sbar_DrawNum (136, 0, cl.stats[STAT_HEALTH], 3, cl.stats[STAT_HEALTH] <= 25);

// ammo icon
	if ((pic = Sbar_AmmoPic ()))
		Sbar_DrawPic (224, 0, pic);
	Sbar_DrawNum (248, 0, cl.stats[STAT_AMMO], 3, cl.stats[STAT_AMMO] <= 10);
}

/*
===============================================================================

THE MODERN STATUS BARS

ironwail's (hudstyle 1 and 2), over the view on the whole screen: the face and
the health in the bottom left corner with the armor over them, the ammo in the
bottom right corner, the weapons up the right side, the ammo counts in the
middle of the bottom (1) or over the ammo (2), the keys to the right and the
powerups to the left, the sigils a while after one is taken, and in deathmatch
the frags down the left side.

===============================================================================
*/

#define	SBAR2_MARGIN_X	16		// from the screen's edges
#define	SBAR2_MARGIN_Y	10

// a pic anywhere on the screen, its 255 transparent, clipped
static void Sbar_ScreenPic (int x, int y, qpic_t *pic)
{
	if (pic)
		Draw_ClippedPic (x, y, pic);
}

// the big numbers right aligned in digits places, at most 999, as ironwail's
static void Sbar_ScreenNum (int x, int y, int num, int digits, int color)
{
	char	str[12], *ptr;
	int		l;

	l = Sbar_itoa (num > 999 ? 999 : num, str);
	ptr = str;
	if (l > digits)
		ptr += l - digits;
	if (l < digits)
		x += (digits - l) * 24;
	for ( ; *ptr ; ptr++, x += 24)
		Sbar_ScreenPic (x, y, sb_nums[color][*ptr == '-' ? STAT_MINUS : *ptr - '0']);
}

// the small gold numbers, three places, 0 to 999
static void Sbar_ScreenSmallNum (int x, int y, int num)
{
	char	str[12];
	int		i;

	snprintf (str, sizeof(str), "%3i", num < 0 ? 0 : num > 999 ? 999 : num);
	for (i = 0 ; i < 3 ; i++)
		if (str[i] != ' ')
			Draw_Character (x + i*8, y, 18 + str[i] - '0');
}

// a stretch of the inventory bar (fractions of it) over w by h, by an alpha
static void Sbar_InventoryBackdrop (int x, int y, int w, int h, float s, float t, float sw, float th, float alpha)
{
	Draw_QCPic ((float)x, (float)y, (float)w, (float)h, sb_ibar, s, t, sw, th, alpha);
}

// the sigils over the middle of the bottom, a while after one was taken (and
// with the scores), the new one flashing in
static void Sbar_ModernSigils (int style)
{
	int		items = cl.stats[STAT_ITEMS];
	int		i, x, y;
	double	t = 0;
	float	a;

	if (!(items & (15u<<28)) || cl.stats[STAT_HEALTH] <= 0)
		return;
	for (i = 0 ; i < 4 ; i++)
		if ((items & (1u<<(28+i))) && cl.item_gettime[28+i] > t)
			t = cl.item_gettime[28+i];
	if (!sb_showscores && (cl.time - t > 3 || scr_viewsize.value >= 120))
		return;

	x = sbar_xofs + 160 - 16;
	y = (int)vid.conheight - SBAR_HEIGHT + (sb_showscores ? -20 : style == 1 ? -8 : -4);
	Sbar_InventoryBackdrop (x, y, 32, 16, 1 - 32/320.0f, 8/24.0f, 32/320.0f, 16/24.0f, 1);
	for (i = 0 ; i < 4 ; i++)
	{
		if (!(items & (1u<<(28+i))))
			continue;
		a = (float)(cl.time - cl.item_gettime[28+i]);
		if (a < 0)
			a = 0;
		a = a >= 1 ? 1 : 1 - floorf (fabsf (fmodf (a * 5, 2) - 1) * 3 + 0.5f) / 3;
		Draw_QCPic ((float)(x + 8*i), (float)y, (float)sb_sigil[i]->width, (float)sb_sigil[i]->height, sb_sigil[i],
			0, 0, 1, 1, a);
	}
}

// the weapons up the right side, the one in hand further out; the ammo
// counts; the keys to the right and the powerups to the left
static void Sbar_ModernInventory (int style)
{
	int		w = (int)vid.conwidth, h = (int)vid.conheight;
	int		items = cl.stats[STAT_ITEMS];
	int		i, x, y, flashon;
	bool	active;

	if (scr_viewsize.value < 110)
	{
		x = w + 1;
		y = (h - 148) / 2 + 16*7 / 2;
		for (i = 0 ; i < 7 ; i++)
		{
			if (!(items & (IT_SHOTGUN<<i)))
				continue;
			active = cl.stats[STAT_ACTIVEWEAPON] == (IT_SHOTGUN<<i);
			flashon = (int)((cl.time - cl.item_gettime[i]) * 10);
			if (flashon < 0)
				flashon = 0;
			flashon = flashon >= 10 ? active : flashon % 5 + 2;
			Sbar_ScreenPic (x - (active ? 24 : 18), y + 24 - 16*i, sb_weapons[flashon][i]);
		}

		if (style == 2)
		{	// over the ammo, two by two
			x = w - SBAR2_MARGIN_X - 52*2;
			y = h - SBAR2_MARGIN_Y - 60 + 24;
			for (i = 0 ; i < 2 ; i++)
				Sbar_InventoryBackdrop (x, y - 10*i, 52*2, 10, i * (2*48/320.0f), 0, 2*48/320.0f, 10/24.0f,
					scr_sbaralpha.value);
			for (i = 0 ; i < 4 ; i++)
				Sbar_ScreenSmallNum (x + 11 + 52 * (i&1), y - 10 * (i>>1), cl.stats[STAT_SHELLS+i]);
		}
		else
		{	// the middle of the bottom, four in a row
			x = w/2 - 96;
			y = h - 9;
			Sbar_InventoryBackdrop (x, y, 192, 10, 0, 0, 192/320.0f, 10/24.0f, scr_sbaralpha.value);
			for (i = 0 ; i < 4 ; i++)
				Sbar_ScreenSmallNum (x + 10 + 48*i, y, cl.stats[STAT_SHELLS+i]);
		}
	}

	if (scr_viewsize.value < 110 && style == 2)
	{
		x = w - SBAR2_MARGIN_X - 16;
		y = h - SBAR2_MARGIN_Y - 68 - 20 + 24;
	}
	else
	{
		x = w - SBAR2_MARGIN_X - 20;
		y = h - SBAR2_MARGIN_Y - 68 + 24;
	}
	for (i = 0 ; i < 6 ; i++)
	{
		if (i == 2)
		{
			if (scr_viewsize.value >= 110)
				break;		// the keys alone in the smaller one
			x = SBAR2_MARGIN_X + 4;
			y = h - SBAR2_MARGIN_Y - 66 + 24;
			if ((items & IT_INVULNERABILITY) || cl.stats[STAT_ARMOR] > 0)
				y -= 24;	// over the armor
		}
		if (items & (1<<(17+i)))
		{
			Sbar_ScreenPic (x, y, sb_items[i]);
			y -= 16;
		}
	}
}

// deathmatch's frags down the left side, in the players' colors, the player's
// own bracketed, with their names in the bigger one
static void Sbar_ModernFrags (void)
{
	int				i, k, y, top, bottom;
	char			num[12];
	player_info_t	*s;

	Sbar_SortFrags (false);
	y = (int)vid.conheight / 4 - ((scoreboardlines >> 2) << 3);
	if (y < 40)
		y = 40;
	for (i = 0 ; i < scoreboardlines ; i++, y += 8)
	{
		k = fragsort[i];
		s = &cl.players[k];
		if (!s->name[0] || s->spectator)
			continue;

		top = s->topcolor < 0 ? 0 : s->topcolor > 13 ? 13 : s->topcolor;
		bottom = s->bottomcolor < 0 ? 0 : s->bottomcolor > 13 ? 13 : s->bottomcolor;
		Draw_Fill (6, y + 1, 28, 4, Sbar_ColorForMap (top));
		Draw_Fill (6, y + 5, 28, 3, Sbar_ColorForMap (bottom));

		snprintf (num, sizeof(num), "%3i", s->frags);
		Draw_Character (8, y, num[0]);
		Draw_Character (16, y, num[1]);
		Draw_Character (24, y, num[2]);
		if (k == cl.playernum)
		{
			Draw_Character (2, y, 16);
			Draw_Character (28, y, 17);
		}
		if (scr_viewsize.value < 110)
			Draw_MarkupString (40, y, s->name);
	}
}

static void Sbar_DrawModern (int style)
{
	int		w = (int)vid.conwidth, h = (int)vid.conheight;
	bool	invuln = (cl.stats[STAT_ITEMS] & IT_INVULNERABILITY) != 0;
	int		armor = invuln ? 666 : cl.stats[STAT_ARMOR];
	int		x, y;
	qpic_t	*pic;

	if (sb_showscores || cl.stats[STAT_HEALTH] <= 0)
		Sbar_SoloScoreboard ();
	else if (scr_viewsize.value < 120)
	{
		x = SBAR2_MARGIN_X;
		y = h - SBAR2_MARGIN_Y - 24;
		Sbar_ScreenPic (x, y, Sbar_FacePic ());
		Sbar_ScreenNum (x + 32, y, cl.stats[STAT_HEALTH], 3, cl.stats[STAT_HEALTH] <= 25);
		if (armor > 0)
		{
			Sbar_ScreenNum (x + 32, y - 24, armor, 3, invuln || armor <= 25);
			Sbar_ScreenPic (x, y - 24, Sbar_ArmorPic ());
		}

		x = w - SBAR2_MARGIN_X - 24;
		if ((pic = Sbar_AmmoPic ()))
		{
			Sbar_ScreenPic (x, y, pic);
			x -= 32;
		}
		Sbar_ScreenNum (x - 48, y, cl.stats[STAT_AMMO], 3, cl.stats[STAT_AMMO] <= 10);

		Sbar_ModernInventory (style);
		if (Sbar_Deathmatch ())
			Sbar_ModernFrags ();
	}
	Sbar_ModernSigils (style);
}

//=============================================================================

// the main line: the scores or the bar; the modern ones' all of theirs
static void Sbar_DrawMain (int style)
{
	if (style == 1 || style == 2)
		Sbar_DrawModern (style);
	else if (sb_showscores || cl.stats[STAT_HEALTH] <= 0)
		Sbar_SoloScoreboard ();
	else
		Sbar_DrawNormal ();
}

/*
===============
Sbar_Draw
===============
*/
void Sbar_Draw (void)
{
	int		style;
	bool	modern, headsup;
	char	st[512];

	if (scr.con_current == vid.conheight)
		return;		// console is full screen

	style = Sbar_HudStyle ();
	modern = style == 1 || style == 2;
	headsup = Sbar_Headsup ();
	// the classic and the modern ones in the middle, as id's and ironwail's
	// (the classic one at the left in deathmatch); QuakeWorld's at the left
	sbar_xofs = style == 3 || (!style && Sbar_Deathmatch ()) ? 0 : ((int)vid.conwidth - 320) / 2;

		
// top line
	if (scr.sb_lines > 24 && !modern)
	{
		if (!cl.spectator || Cam_TrackNum () >= 0)
			Sbar_DrawInventory ();
		if ((!headsup || vid.conwidth<512) && Sbar_Deathmatch ())
			Sbar_DrawFrags ();
	}	

// main area
	if (scr.sb_lines > 0)
	{
		if (cl.spectator) {
			if (Cam_TrackNum () < 0) {
				Sbar_DrawPic (0, 0, sb_scorebar);
				Sbar_DrawString (160-7*8,4, "SPECTATOR MODE");
				Sbar_DrawString(160-14*8+4, 12, "Press [ATTACK] for AutoCamera");
			} else {
				Sbar_DrawMain (style);

//					Sbar_DrawString (160-14*8+4,4, "SPECTATOR MODE - TRACK CAMERA");
				if (CL_MVDFlying ())
					snprintf (st, sizeof(st), "Flying, [ATTACK] back to %-.13s",
						cl.players[Cam_TrackNum ()].name);
				else
					snprintf(st, sizeof(st), "Tracking %-.13s, [JUMP] for next",
						cl.players[Cam_TrackNum ()].name);
				Sbar_DrawString(0, -8, st);
			}
		} else
			Sbar_DrawMain (style);
	}

// main screen deathmatch rankings
	if (!Sbar_Deathmatch ())
		return;		// single player or coop: the scoreboard above says it
	// if we're dead show team scores in team games
	if (cl.stats[STAT_HEALTH] <= 0 && !cl.spectator)
		if (atoi(Info_ValueForKey(cl.serverinfo, "teamplay")) > 0 &&
			!sb_showscores)
			Sbar_TeamOverlay();
		else
			Sbar_DeathmatchOverlay (0);
	else if (sb_showscores)
		Sbar_DeathmatchOverlay (0);
	else if (sb_showteamscores)
		Sbar_TeamOverlay();


	if (scr.sb_lines > 0 && !modern)
		Sbar_MiniDeathmatchOverlay ();
}

//=============================================================================

/*
==================
Sbar_TeamOverlay

team frags
added by Zoid
==================
*/
static void Sbar_TeamOverlay (void)
{
	qpic_t			*pic;
	int				i, k;
	int				x, y;
	char			num[12];
	int				teamplay;
	char			team[5];
	team_t *tm;
	int plow, phigh, pavg;

// request new ping times every two second
	teamplay = atoi(Info_ValueForKey(cl.serverinfo, "teamplay"));

	if (!teamplay) {
		Sbar_DeathmatchOverlay(0);
		return;
	}


	pic = Draw_CachePic ("gfx/ranking.lmp");
	Draw_Pic (160-pic->width/2, 0, pic);

	y = 24;
	x = 36;
	Draw_String(x, y, "low/avg/high team total players");
	y += 8;
//	Draw_String(x, y, "------------ ---- ----- -------");
	Draw_String(x, y, "\x1d\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1f \x1d\x1e\x1e\x1f \x1d\x1e\x1e\x1e\x1f \x1d\x1e\x1e\x1e\x1e\x1e\x1f");
	y += 8;

// sort the teams
	Sbar_SortTeams();

// draw the text
	for (i=0 ; i < scoreboardteams && (unsigned)y <= vid.conheight-10 ; i++)
	{
		k = teamsort[i];
		tm = teams + k;

	// draw pings
		plow = tm->plow;
		if (plow < 0 || plow > 999)
			plow = 999;
		phigh = tm->phigh;
		if (phigh < 0 || phigh > 999)
			phigh = 999;
		if (!tm->players)
			pavg = 999;
		else
			pavg = tm->ptotal / tm->players;
		if (pavg < 0 || pavg > 999)
			pavg = 999;

		snprintf (num, sizeof(num), "%3i/%3i/%3i", plow, pavg, phigh);
		Draw_String ( x, y, num);

	// draw team
		team[4] = 0;
		strncpy (team, tm->team, 4);
		Draw_String (x + 104, y, team);

	// draw total
		snprintf (num, sizeof(num), "%5i", tm->frags);
		Draw_String (x + 104 + 40, y, num);

	// draw players
		snprintf (num, sizeof(num), "%5i", tm->players);
		Draw_String (x + 104 + 88, y, num);
		
		if (!strncmp(Info_ValueForKey(cl.players[cl.playernum].userinfo,
			"team"), tm->team, 16)) {
			Draw_Character ( x + 104 - 8, y, 16);
			Draw_Character ( x + 104 + 32, y, 17);
		}
		
		y += 8;
	}
	y += 8;
	Sbar_DeathmatchOverlay(y);
}

/*
==================
Sbar_DeathmatchOverlay

ping time frags name
==================
*/
static void Sbar_DeathmatchOverlay (int start)
{
	qpic_t			*pic;
	int				i, k, l;
	int				top, bottom;
	int				x, y, f;
	char			num[12];
	player_info_t	*s;
	int				total;
	int				minutes;
	int				p;
	int				teamplay;
	char			team[5];
	int				skip = 10;

	if (largegame)
		skip = 8;

// request new ping times every two second
	if (host.realtime - cl.last_ping_request > 2)
	{
		cl.last_ping_request = host.realtime;
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, "pings");
	}

	teamplay = atoi(Info_ValueForKey(cl.serverinfo, "teamplay"));


	if (!start) {
		pic = Draw_CachePic ("gfx/ranking.lmp");
		Draw_Pic (160-pic->width/2, 0, pic);
	}

// scores	
	Sbar_SortFrags (true);

// draw the text
	l = scoreboardlines;

	if (start)
		y = start;
	else
		y = 24;
	if (teamplay)
	{
		x = 4;
//                            0    40 64   104   152  192 
		Draw_String ( x , y, "ping pl time frags team name");
		y += 8;
//		Draw_String ( x , y, "---- -- ---- ----- ---- ----------------");
		Draw_String ( x , y, "\x1d\x1e\x1e\x1f \x1d\x1f \x1d\x1e\x1e\x1f \x1d\x1e\x1e\x1e\x1f \x1d\x1e\x1e\x1f \x1d\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1f");
		y += 8;
	}
	else
	{
		x = 16;
//                            0    40 64   104   152
		Draw_String ( x , y, "ping pl time frags name");
		y += 8;
//		Draw_String ( x , y, "---- -- ---- ----- ----------------");
		Draw_String ( x , y, "\x1d\x1e\x1e\x1f \x1d\x1f \x1d\x1e\x1e\x1f \x1d\x1e\x1e\x1e\x1f \x1d\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1e\x1f");
		y += 8;
	}

	for (i=0 ; i<l && (unsigned)y <= vid.conheight-10 ; i++)
	{
		k = fragsort[i];
		s = &cl.players[k];
		if (!s->name[0])
			continue;

		// draw ping
		p = s->ping;
		if (p < 0 || p > 999)
			p = 999;
		snprintf (num, sizeof(num), "%4i", p);
		Draw_String ( x, y, num);

		// draw pl
		p = s->pl;
		snprintf (num, sizeof(num), "%3i", p);
		if (p > 25)
			Draw_Alt_String ( x+32, y, num);
		else
			Draw_String ( x+32, y, num);

		if (s->spectator)
		{
			Draw_String (x+40, y, "(spectator)");
			// draw name
			if (teamplay)
				Draw_MarkupString (x+152+40, y, s->name);
			else
				Draw_MarkupString (x+152, y, s->name);
			y += skip;
			continue;
		}


		// draw time
		if (cl.intermission)
			total = (int)(cl.completed_time - s->entertime);
		else
			total = (int)(CL_ScoreClock () - s->entertime);
		minutes = (int)total/60;
		snprintf (num, sizeof(num), "%4i", minutes);
		Draw_String ( x+64 , y, num);

		// draw background
		top = s->topcolor;
		bottom = s->bottomcolor;
		top = Sbar_ColorForMap (top);
		bottom = Sbar_ColorForMap (bottom);
	
		if (largegame)
			Draw_Fill ( x+104, y+1, 40, 3, top);
		else
			Draw_Fill ( x+104, y, 40, 4, top);
		Draw_Fill ( x+104, y+4, 40, 4, bottom);

	// draw number
		f = s->frags;
		snprintf (num, sizeof(num), "%3i",f);

		Draw_Character ( x+112 , y, num[0]);
		Draw_Character ( x+120 , y, num[1]);
		Draw_Character ( x+128 , y, num[2]);

		if (k == cl.playernum)
		{
			Draw_Character ( x + 104, y, 16);
			Draw_Character ( x + 136, y, 17);
		}
		
		// team
		if (teamplay)
		{
			team[4] = 0;
			strncpy (team, Info_ValueForKey(s->userinfo, "team"), 4);
			Draw_String (x+152, y, team);
		}

		// draw name
		if (teamplay)
			Draw_MarkupString (x+152+40, y, s->name);
		else
			Draw_MarkupString (x+152, y, s->name);
		
		y += skip;
	}

	if ((unsigned)y >= vid.conheight-10) // we ran over the screen size, squish
		largegame = true;
}

/*
==================
Sbar_MiniDeathmatchOverlay

frags name
frags team name
displayed to right of status bar if there's room
==================
*/
static void Sbar_MiniDeathmatchOverlay (void)
{
	int				i, k;
	int				top, bottom;
	int				x, y, f;
	char			num[12];
	player_info_t	*s;
	int				teamplay;
	char			team[5];
	int				numlines;
	char			shortname[16+1];
	team_t			*tm;

	if (vid.conwidth < 512 || !scr.sb_lines)
		return; // not enuff room

	teamplay = atoi(Info_ValueForKey(cl.serverinfo, "teamplay"));


// scores	
	Sbar_SortFrags (false);
	if (vid.conwidth >= 640)
		Sbar_SortTeams();

	if (!scoreboardlines)
		return; // no one there?

// draw the text
	y = vid.conheight - scr.sb_lines - 1;
	numlines = scr.sb_lines/8;
	if (numlines < 3)
		return; // not enough room

	// find us
	for (i=0 ; i < scoreboardlines; i++)
		if (fragsort[i] == cl.playernum)
			break;

	if (i == scoreboardlines) // we're not there, we are probably a spectator, just display top
		i = 0;
	else // figure out start
		i = i - numlines/2;

	if (i > scoreboardlines - numlines)
		i = scoreboardlines - numlines;
	if (i < 0)
		i = 0;

	x = 324;

	for (/* */ ; i < scoreboardlines && (unsigned)y < vid.conheight - 8 + 1; i++)
	{
		k = fragsort[i];
		s = &cl.players[k];
		if (!s->name[0])
			continue;

	// draw ping
		top = s->topcolor;
		bottom = s->bottomcolor;
		top = Sbar_ColorForMap (top);
		bottom = Sbar_ColorForMap (bottom);
	
		Draw_Fill ( x, y+1, 40, 3, top);
		Draw_Fill ( x, y+4, 40, 4, bottom);

	// draw number
		f = s->frags;
		snprintf (num, sizeof(num), "%3i",f);

		Draw_Character ( x+8 , y, num[0]);
		Draw_Character ( x+16, y, num[1]);
		Draw_Character ( x+24, y, num[2]);

		if (k == cl.playernum)
		{
			Draw_Character ( x, y, 16);
			Draw_Character ( x + 32, y, 17);
		}
		
	// team
		if (teamplay)
		{
			team[4] = 0;
			strncpy (team, Info_ValueForKey(s->userinfo, "team"), 4);
			Draw_String (x+48, y, team);
		}

	// draw name
		shortname[16] = 0;
		strncpy(shortname, s->name, 16);
		if (teamplay)
			Draw_MarkupString (x+48+40, y, shortname);
		else
			Draw_MarkupString (x+48, y, shortname);
		y += 8;
	}

	// draw teams if room
	if (vid.conwidth < 640 || !teamplay)
		return;

	// draw seperator
	x += 208;
	for (y = vid.conheight - scr.sb_lines; (unsigned)y < vid.conheight - 6; y += 2)
		Draw_Character(x, y, 14);

	x += 16;

	y = vid.conheight - scr.sb_lines;
	for (i=0 ; i < scoreboardteams && (unsigned)y <= vid.conheight; i++)
	{
		k = teamsort[i];
		tm = teams + k;

	// draw pings
		team[4] = 0;
		strncpy (team, tm->team, 4);
		Draw_String (x, y, team);

	// draw total
		snprintf (num, sizeof(num), "%5i", tm->frags);
		Draw_String (x + 40, y, num);
		
		if (!strncmp(Info_ValueForKey(cl.players[cl.playernum].userinfo,
			"team"), tm->team, 16)) {
			Draw_Character ( x - 8, y, 16);
			Draw_Character ( x + 32, y, 17);
		}
		
		y += 8;
	}

}


// a number in the big digits, digits wide, at x, y of the screen
static void Sbar_IntermissionNumber (int x, int y, int num, int digits)
{
	char	str[12], *ptr = str;
	int		l;

	l = snprintf (str, sizeof(str), "%i", num);
	if (l > digits)
		ptr += l - digits;
	if (l < digits)
		x += (digits - l) * 24;
	for ( ; *ptr ; ptr++, x += 24)
		Draw_TransPic (x, y, sb_nums[0][*ptr == '-' ? 10 : *ptr - '0']);
}

/*
==================
Sbar_CoopIntermission

The level completed outside deathmatch: its time, secrets and monsters, as
NetQuake draws them, in the middle of the screen (FTE's)
==================
*/
static void Sbar_CoopIntermission (void)
{
	int		x = (vid.conwidth - 320) / 2, y = (vid.conheight - 200) / 2;
	int		dig, num;

	Draw_Pic (x + 64, y + 24, Draw_CachePic ("gfx/complete.lmp"));
	Draw_TransPic (x, y + 56, Draw_CachePic ("gfx/inter.lmp"));

// time
	dig = (int)cl.completed_leveltime / 60;
	Sbar_IntermissionNumber (x + 160, y + 64, dig, 3);
	num = (int)cl.completed_leveltime - dig*60;
	Draw_TransPic (x + 234, y + 64, sb_colon);
	Draw_TransPic (x + 246, y + 64, sb_nums[0][num/10]);
	Draw_TransPic (x + 266, y + 64, sb_nums[0][num%10]);

	Sbar_IntermissionNumber (x + 160, y + 104, cl.stats[STAT_SECRETS], 3);
	Draw_TransPic (x + 232, y + 104, sb_slash);
	Sbar_IntermissionNumber (x + 240, y + 104, cl.stats[STAT_TOTALSECRETS], 3);

	Sbar_IntermissionNumber (x + 160, y + 144, cl.stats[STAT_MONSTERS], 3);
	Draw_TransPic (x + 232, y + 144, sb_slash);
	Sbar_IntermissionNumber (x + 240, y + 144, cl.stats[STAT_TOTALMONSTERS], 3);
}

/*
==================
Sbar_IntermissionOverlay

==================
*/
void Sbar_IntermissionOverlay (void)
{
	if (!Sbar_Deathmatch ())
		Sbar_CoopIntermission ();
	else if (atoi(Info_ValueForKey(cl.serverinfo, "teamplay")) > 0 && !sb_showscores)
		Sbar_TeamOverlay ();
	else
		Sbar_DeathmatchOverlay (0);
}


/*
==================
Sbar_FinaleOverlay

==================
*/
void Sbar_FinaleOverlay (void)
{
	qpic_t	*pic;


	pic = Draw_CachePic ("gfx/finale.lmp");
	Draw_TransPic ( (vid.conwidth-pic->width)/2, 16, pic);
}


