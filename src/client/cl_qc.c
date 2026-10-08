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
// cl_qc.c -- what the client's QuakeC hosts share (CSQC, the menu)

#include "cl_local.h"
#include "cl_qc.h"
#include "markup.h"

static clqc_t	*clqc_hosts[4];		// those whose QuakeC registered commands
static int		clqc_numhosts;

static void CLQC_Warning (void *ctx, const qc_warning_t *w)
{
	const clqc_t	*qc = ctx;
	char			text[1024];

	Con_DPrintf ("%s: %s\n", qc->name, QC_WarningText (w, text, sizeof(text)));
}

static void CLQC_Print (void *ctx, const char *text)
{
	(void)ctx;
	Con_Printf ("%s", text);
}

static void CLQC_CenterPrint (void *ctx, const char *text)
{
	(void)ctx;
	SCR_CenterPrint ((char *)text);
}

static void CLQC_Dump (void *ctx, qc_dumpkind_t kind, const char *text)
{
	(void)kind;
	CLQC_Print (ctx, text);
}

static void CLQC_Localcmd (void *ctx, const char *text)
{
	(void)ctx;
	Cbuf_AddText ((char *)text);
}

static float CLQC_CvarFloat (void *ctx, const char *varname)
{
	(void)ctx;
	return Cvar_VariableValue ((char *)varname);
}

static const char *CLQC_CvarString (void *ctx, const char *varname)
{
	cvar_t	*var = Cvar_FindVar ((char *)varname);

	(void)ctx;
	return var ? var->string : NULL;
}

static void CLQC_CvarSet (void *ctx, const char *varname, const char *value)
{
	(void)ctx;
	if (Cvar_FindVar ((char *)varname))
		Cvar_Set ((char *)varname, (char *)value);
}

// cvar_type, cvar_defstring and cvar_description: the engine's cvars all
static bool CLQC_CvarInfo (void *ctx, const char *varname, qc_cvarinfo_t *info)
{
	cvar_t	*var = Cvar_FindVar ((char *)varname);

	(void)ctx;
	if (!var)
		return false;
	info->flags = 1 | (var->archive ? 2 : 0) | 8 | (var->description ? 16 : 0);
	info->defaultvalue = var->defaultstring;
	info->description = var->description;
	return true;
}

// checkcommand: 1 a command, 2 an alias, 3 a cvar
static uint32_t CLQC_CheckCommand (void *ctx, const char *cmd)
{
	(void)ctx;
	if (Cmd_Exists ((char *)cmd))
		return 1;
	if (Cmd_AliasExists (cmd))
		return 2;
	return Cvar_FindVar ((char *)cmd) ? 3 : 0;
}

// a command QuakeC registered: its whole line to the host whose QuakeC it is
static void CLQC_Command_f (void)
{
	clqc_t	*qc;
	int		h, i;

	for (h = 0 ; h < clqc_numhosts ; h++)
	{
		qc = clqc_hosts[h];
		for (i = 0 ; i < qc->numcommands ; i++)
			if (!strcmp (qc->commands[i], Cmd_Argv (0)))
			{
				if (qc->vm)
					qc->command (qc, va ("%s %s", Cmd_Argv (0), Cmd_Args ()));
				return;
			}
	}
}

// registercommand: a console command for QuakeC, unless the name is taken
static void CLQC_RegisterCommand (void *ctx, const char *cmd)
{
	clqc_t	*qc = ctx;
	char	*copy;
	int		h, i;

	if (!*cmd || Cmd_Exists ((char *)cmd) || Cvar_FindVar ((char *)cmd) || Cmd_AliasExists (cmd))
		return;
	for (i = 0 ; i < qc->numcommands ; i++)
		if (!strcmp (qc->commands[i], cmd))
			return;
	if (qc->numcommands == (int)(sizeof(qc->commands) / sizeof(qc->commands[0])))
	{
		Con_Printf ("%s: too many commands, %s left out\n", qc->name, cmd);
		return;
	}
	for (h = 0 ; h < clqc_numhosts && clqc_hosts[h] != qc ; h++)
		;
	if (h == clqc_numhosts)
	{
		if (clqc_numhosts == (int)(sizeof(clqc_hosts) / sizeof(clqc_hosts[0])))
			return;
		clqc_hosts[clqc_numhosts++] = qc;
	}
	copy = Mem_Alloc (strlen (cmd) + 1);
	strcpy (copy, cmd);
	qc->commands[qc->numcommands++] = copy;
	Cmd_AddCommand (copy, CLQC_Command_f, qc->description);
}

static float CLQC_IsDemo (void *ctx)
{
	(void)ctx;
	return cls.demoplayback ? cls.mvdplayback ? 2.0f : 1.0f : 0.0f;
}

static bool CLQC_IsServer (void *ctx)
{
	(void)ctx;
	return SV_Active ();
}

static void CLQC_Trace (void *ctx, const char *line)
{
	(void)ctx;
	Con_Printf ("%s\n", line);
}

void CLQC_InitHost (qc_host_t *h)
{
	h->warning = CLQC_Warning;
	h->print = CLQC_Print;
	h->dprint = CLQC_Print;
	h->centerprint = CLQC_CenterPrint;
	h->localcmd = CLQC_Localcmd;
	h->dump = CLQC_Dump;
	h->cvar_float = CLQC_CvarFloat;
	h->cvar_string = CLQC_CvarString;
	h->cvar_set = CLQC_CvarSet;
	h->cvar_info = CLQC_CvarInfo;
	h->check_command = CLQC_CheckCommand;
	h->register_command = CLQC_RegisterCommand;
	h->is_demo = CLQC_IsDemo;
	h->is_server = CLQC_IsServer;
	h->trace = CLQC_Trace;
}

void CLQC_Failed (const clqc_t *qc)
{
	const qc_error_t	*e = QC_LastError (qc->vm);
	char				text[1024];
	char				*trace = Mem_Alloc (16384);

	Con_Printf ("%s", QC_BacktraceText (&e->backtrace, trace, 16384));
	Mem_Free (trace);
	Con_Printf ("%s: %s\n", qc->name, QC_ErrorText (e, text, sizeof(text)));
}

void CLQC_RemoveCommands (clqc_t *qc)
{
	int		i;

	for (i = 0 ; i < qc->numcommands ; i++)
	{
		Cmd_RemoveCommand (qc->commands[i]);
		Mem_Free (qc->commands[i]);
	}
	qc->numcommands = 0;
}

bool CLQC_ReturnText (qcvm_t *vm, const char *text)
{
	if (!*text)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	return QC_ReturnString (vm, text, strlen (text));
}

/*
==============================================================================

DRAWING

FTE's 2D builtins, the menu's and CSQC's, in the layout's units: text 8 by 8
whatever size QuakeC asks, pics at the size it asks, within the clip area

==============================================================================
*/

// a coordinate of the 2D layout
static float CLQC_Coord (float f)
{
	if (!(f > -16384))
		return -16384;
	if (f > 16384)
		return 16384;
	return floorf (f);
}

// the text color of QuakeC's rgb and alpha (args rgb and rgb + 1): a tint of
// four bits a channel, half transparent below an alpha of 1; false where it
// draws nothing
static bool CLQC_TextColor (qcvm_t *vm, int rgbarg, unsigned *color)
{
	float	rgb[3] = {1, 1, 1}, alpha = 1;
	int		c[3], i;

	if (QC_Argc (vm) > rgbarg)
		QC_ArgVector (vm, rgbarg, rgb);
	if (QC_Argc (vm) > rgbarg + 1)
		alpha = QC_ArgFloat (vm, rgbarg + 1);
	if (!(alpha > 0))
		return false;
	for (i = 0 ; i < 3 ; i++)
		c[i] = !(rgb[i] > 0) ? 0 : rgb[i] >= 1 ? 15 : (int)(rgb[i] * 15 + 0.5f);
	*color = c[0] == 15 && c[1] == 15 && c[2] == 15 ? 0 : TEXT_RGB (c[0], c[1], c[2]);
	if (alpha < 1)
		*color |= TEXT_HALF;
	return true;
}

// a character at x, y, if it is whole within the clip area
static void CLQC_Character (int x, int y, int num, unsigned color)
{
	int		x0, y0, x1, y1;

	Draw_GetClipArea (&x0, &y0, &x1, &y1);
	if (x >= x0 && y >= y0 && x + 8 <= x1 && y + 8 <= y1)
		Draw_ColoredCharacter (x, y, num, color);
}

// the alpha argument, 1 when QuakeC leaves it out
static float CLQC_Alpha (qcvm_t *vm, int arg)
{
	return QC_Argc (vm) > arg ? QC_ArgFloat (vm, arg) : 1.0f;
}

// string precache_pic(string name, optional float flags): the name, or null if there is none
static bool CLQC_PrecachePic (qcvm_t *vm)
{
	QC_ReturnWord (vm, Draw_TryCachePic (QC_ArgString (vm, 0)) ? QC_ArgWord (vm, 0) : 0);
	return true;
}

// float iscachedpic(string name)
static bool CLQC_IsCachedPic (qcvm_t *vm)
{
	const char	*picname = QC_ArgString (vm, 0);

	QC_ReturnFloat (vm, Draw_FindImage (picname) || Draw_TryCachePic (picname) ? 1.0f : 0.0f);
	return true;
}

// float drawpic(vector pos, string pic, vector size, vector rgb, float alpha, optional float flag):
// an image QuakeC made (r_uploadimage) as it is, else a pic at the size (its own for '0 0')
static bool CLQC_DrawPic (qcvm_t *vm)
{
	const char			*picname = QC_ArgString (vm, 1);
	const drawimage_t	*img;
	qpic_t				*pic = NULL;
	float				pos[3], size[3];

	QC_ArgVector (vm, 0, pos);
	QC_ArgVector (vm, 2, size);
	if ((img = Draw_FindImage (picname)))
		Draw_ClippedImage ((int)CLQC_Coord (pos[0]), (int)CLQC_Coord (pos[1]), img);
	else if ((pic = Draw_TryCachePic (picname)))
	{
		if (!size[0] && !size[1])
		{
			size[0] = (float)pic->width;
			size[1] = (float)pic->height;
		}
		Draw_QCPic (CLQC_Coord (pos[0]), CLQC_Coord (pos[1]), size[0], size[1], pic, 0, 0, 1, 1,
			CLQC_Alpha (vm, 4));
	}
	QC_ReturnFloat (vm, img || pic ? 1.0f : 0.0f);
	return true;
}

// void drawsubpic(vector pos, vector size, string pic, vector srcpos, vector srcsize, vector rgb,
// float alpha, optional float flag): the part of the pic from srcpos, srcsize big (fractions of it)
static bool CLQC_DrawSubPic (qcvm_t *vm)
{
	qpic_t	*pic;
	float	pos[3], size[3], src[3], srcsize[3];

	if (!(pic = Draw_TryCachePic (QC_ArgString (vm, 2))))
		return true;
	QC_ArgVector (vm, 0, pos);
	QC_ArgVector (vm, 1, size);
	QC_ArgVector (vm, 3, src);
	QC_ArgVector (vm, 4, srcsize);
	Draw_QCPic (CLQC_Coord (pos[0]), CLQC_Coord (pos[1]), size[0], size[1], pic, src[0], src[1], srcsize[0],
		srcsize[1], CLQC_Alpha (vm, 6));
	return true;
}

// float drawcharacter(vector pos, float char, vector scale, vector rgb, float alpha, optional float flag)
static bool CLQC_DrawCharacter (qcvm_t *vm)
{
	float		pos[3];
	unsigned	color;

	QC_ArgVector (vm, 0, pos);
	if (CLQC_TextColor (vm, 3, &color))
		CLQC_Character ((int)CLQC_Coord (pos[0]), (int)CLQC_Coord (pos[1]),
			QC_DoubleToInt (QC_ArgFloat (vm, 1)) & 255, color);
	QC_ReturnFloat (vm, 1);
	return true;
}

// float drawrawstring(vector pos, string text, vector scale, vector rgb, float alpha, optional float flag)
static bool CLQC_DrawRawString (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 1);
	float		pos[3];
	unsigned	color;
	int			x, y;

	QC_ArgVector (vm, 0, pos);
	x = (int)CLQC_Coord (pos[0]);
	y = (int)CLQC_Coord (pos[1]);
	if (CLQC_TextColor (vm, 3, &color))
		for ( ; *s && x < (int)vid.conwidth ; s++, x += 8)
			CLQC_Character (x, y, (unsigned char)*s, color);
	QC_ReturnFloat (vm, 1);
	return true;
}

// float drawstring(vector pos, string text, vector scale, vector rgb, float alpha, optional float flag):
// the colors written in it (ezQuake's &cRGB, FTE's ^), the rgb where none are
static bool CLQC_DrawString (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 1);
	markup_t	m;
	float		pos[3];
	unsigned	color;
	int			x, y, c;

	QC_ArgVector (vm, 0, pos);
	x = (int)CLQC_Coord (pos[0]);
	y = (int)CLQC_Coord (pos[1]);
	if (CLQC_TextColor (vm, 3, &color))
		for (Markup_Begin (&m) ; (c = Markup_Next (&s, &m)) >= 0 && x < (int)vid.conwidth ; x += 8)
			CLQC_Character (x, y, c, m.color ? m.color | (color & TEXT_HALF) : color);
	QC_ReturnFloat (vm, 1);
	return true;
}

// float stringwidth(string text, float usecolours, optional vector fontsize):
// 8 a character, the colors' codes none where usecolours
static bool CLQC_StringWidth (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);

	QC_ReturnFloat (vm, 8.0f * (float)(QC_ArgFloat (vm, 1) != 0 ? Markup_Length (s) : (int)strlen (s)));
	return true;
}

// float drawfill(vector pos, vector size, vector rgb, float alpha, optional float flag):
// over what is there by the alpha
static bool CLQC_DrawFill (qcvm_t *vm)
{
	float	pos[3], size[3], rgb[3];

	QC_ArgVector (vm, 0, pos);
	QC_ArgVector (vm, 1, size);
	QC_ArgVector (vm, 2, rgb);
	Draw_BlendFill ((int)CLQC_Coord (pos[0]), (int)CLQC_Coord (pos[1]), (int)CLQC_Coord (size[0]),
		(int)CLQC_Coord (size[1]), (int)(rgb[0] * 255 + 0.5f), (int)(rgb[1] * 255 + 0.5f),
		(int)(rgb[2] * 255 + 0.5f), (int)(CLQC_Alpha (vm, 3) * 255 + 0.5f));
	QC_ReturnFloat (vm, 1);
	return true;
}

// void drawsetcliparea(float x, float y, float width, float height)
static bool CLQC_DrawSetClipArea (qcvm_t *vm)
{
	Draw_SetClipArea (CLQC_Coord (QC_ArgFloat (vm, 0)), CLQC_Coord (QC_ArgFloat (vm, 1)),
		CLQC_Coord (QC_ArgFloat (vm, 2)), CLQC_Coord (QC_ArgFloat (vm, 3)));
	return true;
}

// void drawresetcliparea(void)
static bool CLQC_DrawResetClipArea (qcvm_t *vm)
{
	(void)vm;
	Draw_ResetClipArea ();
	return true;
}

// vector drawgetimagesize(string pic): '0 0 0' if there is none
static bool CLQC_DrawGetImageSize (qcvm_t *vm)
{
	const char			*picname = QC_ArgString (vm, 0);
	const drawimage_t	*img;
	qpic_t				*pic;
	float				size[3] = {0, 0, 0};
	int					w, h;

	if ((img = Draw_FindImage (picname)))
	{
		Draw_ImageSize (img, &w, &h);
		size[0] = (float)w;
		size[1] = (float)h;
	}
	else if ((pic = Draw_TryCachePic (picname)))
	{
		size[0] = (float)pic->width;
		size[1] = (float)pic->height;
	}
	QC_ReturnVector (vm, size);
	return true;
}

bool CLQC_DrawBuiltins (qc_builtins_t *b)
{
	static const struct
	{
		const char		*name;
		qc_builtin_t	func;
	} draw[] = {
		{"precache_pic", CLQC_PrecachePic},
		{"iscachedpic", CLQC_IsCachedPic},
		{"drawpic", CLQC_DrawPic},
		{"drawsubpic", CLQC_DrawSubPic},
		{"drawcharacter", CLQC_DrawCharacter},
		{"drawrawstring", CLQC_DrawRawString},
		{"drawstring", CLQC_DrawString},
		{"stringwidth", CLQC_StringWidth},
		{"drawfill", CLQC_DrawFill},
		{"drawsetcliparea", CLQC_DrawSetClipArea},
		{"drawresetcliparea", CLQC_DrawResetClipArea},
		{"drawgetimagesize", CLQC_DrawGetImageSize},
	};
	size_t	i;

	for (i = 0 ; i < sizeof(draw) / sizeof(draw[0]) ; i++)
		if (!QC_BuiltinsSet (b, draw[i].name, draw[i].func))
			return false;
	return true;
}
