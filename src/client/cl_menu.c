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
// cl_menu.c -- the menu, QuakeC (menu.dat) run as FTE runs menu QuakeC: the
// game directory's menu.dat, else the one built in (menu-qc); the builtins it
// draws and binds keys with, and the keys, toggling and quitting that reach it.

#include "cl_local.h"
#include "cl_qc.h"
#include "markup.h"

// menu-qc's menu.dat as the program was built with it (menu_data.c, which
// cmake/qcprogs.cmake makes)
extern const unsigned char	cl_menuprogs[];
extern const size_t			cl_menuprogs_size;

static struct
{
	clqc_t			qc;
	qc_builtins_t	*builtins;
	qc_host_t		host;
	bool			builtin;		// the VM runs the built-in menu.dat
	bool			restart;		// at the next entry: the game directory changed, or its menu failed
	bool			gamedirfailed;	// its menu.dat failed: the built-in one, until the gamedir changes

	// the entry points (0 for those the progs lacks)
	qc_func_t		init, shutdown, draw, keydown, keyup, toggle, consolecommand;
	bool			drawfloats;		// DP's m_draw(float width, float height), not FTE's vector
	double			callframe;		// host.realtime of the frame the calls in progress began in
} menu;

static void M_Load (void);
static void M_Destroy (void);

/*
==============================================================================

CALLING THE MENU

==============================================================================
*/

// a QuakeC error: what went wrong, and the menu goes; a game directory's
// menu.dat gives way to the built-in one at the next entry
static void M_Failed (void)
{
	CLQC_Failed (&menu.qc);
	Con_Printf ("Menu shut down\n");
	if (cls.key_dest == key_menu)
		cls.key_dest = key_game;
	if (!menu.builtin)
	{
		menu.gamedirfailed = true;
		menu.restart = true;
	}
	M_Destroy ();
}

// The menu's QuakeC is running, and this is the engine called from inside it:
// a print it makes draws the screen at once while the console is up. The menu
// isn't entered again, as FTE's isn't.
static bool M_Busy (void)
{
	return menu.qc.calls && menu.callframe == host.realtime;
}

// calls f, if not 0, its result in *ret (if not NULL); false (the menu gone)
// on an error. From inside the menu's QuakeC, nothing (M_Busy).
static bool M_Call (qc_func_t f, int argc, const qc_value_t *args, qc_value_t *ret)
{
	uint32_t	word, type;
	bool		ok;

	if (ret)
		memset (ret, 0, sizeof(*ret));
	if (!menu.qc.vm || !f)
		return menu.qc.vm != NULL;
	if (M_Busy ())
		return true;
	// a longjmp out of QuakeC (Host_Error's) in an earlier frame left a call
	// running: abandoned
	if (menu.qc.calls)
	{
		QC_Abandon (menu.qc.vm);
		menu.qc.calls = 0;
	}
	menu.callframe = host.realtime;
	if (QC_FindGlobal (menu.qc.vm, "time", &word, &type) && type == QC_EV_FLOAT)
		QC_Globals (menu.qc.vm)[word].f = (float)host.realtime;
	if (QC_FindGlobal (menu.qc.vm, "frametime", &word, &type) && type == QC_EV_FLOAT)
		QC_Globals (menu.qc.vm)[word].f = (float)cls.frametime;
	QC_SetTime (menu.qc.vm, host.realtime);
	menu.qc.calls++;
	ok = QC_Call (menu.qc.vm, f, argc, args, ret);
	menu.qc.calls--;
	if (!ok)
		M_Failed ();
	return ok;
}

// what changed since the menu last ran: a restart it is due, the menu up again
// if it was (a game's quake.rc opens it, menu_main, before its gamedir's
// change reaches the menu)
static void M_Check (void)
{
	qc_value_t	arg = QC_ValFloat (1);
	bool		up = cls.key_dest == key_menu;

	if (!menu.restart || M_Busy ())
		return;
	menu.restart = false;
	M_Shutdown ();
	M_Load ();
	if (up)
		M_Call (menu.toggle, 1, &arg, NULL);
}

// a command the menu's QuakeC registered: its whole line to m_consolecommand
static void M_Command (clqc_t *qc, const char *line)
{
	qc_value_t	arg = QC_ValWord (QC_TempString (qc->vm, line, strlen (line)));

	M_Call (menu.consolecommand, 1, &arg, NULL);
}

/*
================
M_Draw

The menu's frame, where the C menu drew: over the console and the status bar
================
*/
void M_Draw (void)
{
	qc_value_t	args[2];

	if (M_Busy ())
		return;
	M_Check ();
	if (!menu.qc.vm || cls.key_dest != key_menu || !menu.draw)
		return;
	SB_Adopt ();		// the server list, as the keys after this draw find it
	QC_RunThreads (menu.qc.vm, NULL);
	Draw_ResetClipArea ();
	if (menu.drawfloats)
	{
		args[0] = QC_ValFloat ((float)vid.conwidth);
		args[1] = QC_ValFloat ((float)vid.conheight);
		M_Call (menu.draw, 2, args, NULL);
	}
	else
	{
		args[0] = QC_ValVector ((float)vid.conwidth, (float)vid.conheight, 0);
		M_Call (menu.draw, 1, args, NULL);
	}
	Draw_ResetClipArea ();
	S_ExtraUpdate ();
}

/*
================
M_Keydown

A key down for the menu, as FTE's QuakeC numbers it, and the character it
types (0 if none)
================
*/
void M_Keydown (int key, int character)
{
	qc_value_t	args[2];
	int			scan = Key_ToFTE (key);

	M_Check ();
	if (scan < 0)
		return;
	args[0] = QC_ValFloat ((float)scan);
	args[1] = QC_ValFloat (character >= 32 && character < 127 ? (float)character : 0);
	M_Call (menu.keydown, 2, args, NULL);
}

void M_Keyup (int key)
{
	qc_value_t	args[2];
	int			scan = Key_ToFTE (key);

	if (scan < 0)
		return;
	args[0] = QC_ValFloat ((float)scan);
	args[1] = QC_ValFloat (0);
	M_Call (menu.keyup, 2, args, NULL);
}

/*
================
M_ToggleMenu_f

Escape, and togglemenu: the console closes in a game; out of one it stays
down, and the menu opens over it
================
*/
void M_ToggleMenu_f (void)
{
	qc_value_t	arg = QC_ValFloat (1);

	if (cls.key_dest == key_console && cls.state == ca_active)
	{
		Con_ToggleConsole_f ();
		return;
	}
	M_Check ();
	M_Call (menu.toggle, 1, &arg, NULL);
}

/*
================
M_QuitPrompt

quit asks the menu, as FTE's does: true if its QuakeC took menu_quit
================
*/
bool M_QuitPrompt (void)
{
	int		i;

	for (i = 0 ; i < menu.qc.numcommands ; i++)
		if (!strcmp (menu.qc.commands[i], "menu_quit"))
		{
			M_Command (&menu.qc, "menu_quit");
			return true;
		}
	return false;
}

/*
==============================================================================

THE BUILTINS

FTE's menu QuakeC builtins the menu needs, by FTE's numbers for menus

==============================================================================
*/

// int r_uploadimage(string name, int width, int height, void *pixels, optional int datasize,
// optional int format): FTE's format 1, straight RGBA bytes (the default), or 15 and 16, a byte
// a pixel and after them a palette of 256 RGB or RGBA; 1 if it was taken
static bool M_UploadImage (qcvm_t *vm)
{
	const char	*imagename = QC_ArgString (vm, 0);
	int32_t		w = QC_ArgInt (vm, 1), h = QC_ArgInt (vm, 2);
	int32_t		format = QC_Argc (vm) > 5 ? QC_ArgInt (vm, 5) : 1;
	int			palbytes = format == 15 ? 3 : format == 16 ? 4 : 0;
	byte		*data, *rgba, *out;
	const byte	*pal;
	size_t		pixels, size, i;

	QC_ReturnInt (vm, 0);
	if (QC_Argc (vm) < 4 || w <= 0 || h <= 0 || w > 4096 || h > 4096 || (format != 1 && !palbytes))
	{
		QC_Warning (vm, "r_uploadimage: %s isn't a %dx%d image of a format taken (%d)", imagename, w, h, format);
		return true;
	}
	pixels = (size_t)w * h;
	size = palbytes ? pixels + 256 * (size_t)palbytes : pixels * 4;
	if (QC_Argc (vm) > 4 && QC_ArgInt (vm, 4) < (int64_t)size)
	{
		QC_Warning (vm, "r_uploadimage: %s's %d bytes aren't %dx%d of format %d", imagename, QC_ArgInt (vm, 4), w, h,
			format);
		return true;
	}
	data = Mem_Alloc (size);
	if (!QC_ReadMemory (vm, QC_ArgWord (vm, 3), data, size))
	{
		QC_Warning (vm, "r_uploadimage: %s's pixels aren't in memory", imagename);
		Mem_Free (data);
		return true;
	}
	rgba = data;
	if (palbytes)
	{
		rgba = out = Mem_Alloc (pixels * 4);
		for (i = 0 ; i < pixels ; i++, out += 4)
		{
			pal = data + pixels + data[i] * palbytes;
			out[0] = pal[0];
			out[1] = pal[1];
			out[2] = pal[2];
			out[3] = palbytes == 4 ? pal[3] : 255;
		}
	}
	if (Draw_UploadImage (imagename, w, h, rgba))
		QC_ReturnInt (vm, 1);
	else
		QC_Warning (vm, "r_uploadimage: no room for %s", imagename);
	if (rgba != data)
		Mem_Free (rgba);
	Mem_Free (data);
	return true;
}

// int *r_readimage(string file, __out int width, __out int height): straight
// RGBA bytes in a block QuakeC frees (memfree), null if there is no such pic
static bool M_ReadImage (qcvm_t *vm)
{
	byte		*rgba;
	int			w, h;
	qc_ptr_t	p = 0;

	QC_SetArgWord (vm, 1, 0);
	QC_SetArgWord (vm, 2, 0);
	if ((rgba = Draw_ReadImage (QC_ArgString (vm, 0), &w, &h)))
	{
		if ((p = QC_Alloc (vm, (size_t)w * h * 4)) && QC_WriteMemory (vm, p, rgba, (size_t)w * h * 4))
		{
			QC_SetArgWord (vm, 1, (uint32_t)w);
			QC_SetArgWord (vm, 2, (uint32_t)h);
		}
		else
			p = 0;
		Mem_Free (rgba);
	}
	QC_ReturnWord (vm, p);
	return true;
}

// void localsound(string sample)
static bool M_LocalSound (qcvm_t *vm)
{
	const char	*sample = QC_ArgString (vm, 0);

	if (*sample)
		S_LocalSound ((char *)sample);
	return true;
}

// float queueaudio(int hz, int channels, int type, void *data, int frames): frames
// of 1 or 2 channels, FTE's types 8 (unsigned bytes), -8 (signed), -16 (signed 16
// bit) and 288 (floats), after those queued; 1, -1 for nonsense, -2 for a type not taken
static bool M_QueueAudio (qcvm_t *vm)
{
	int32_t		hz = QC_ArgInt (vm, 0), channels = QC_ArgInt (vm, 1), type = QC_ArgInt (vm, 2);
	int32_t		frames = QC_ArgInt (vm, 4);
	int			width = type == 8 || type == -8 ? 1 : type == -16 ? 2 : type == (256 | 32) ? 4 : 0;
	size_t		n, i;
	byte		*data;
	short		*samples;
	int16_t		s16;
	float		f;

	if (hz < 1 || (channels != 1 && channels != 2) || frames <= 0 || frames > 1 << 20)
	{
		QC_ReturnFloat (vm, -1);
		return true;
	}
	if (!width)
	{
		QC_ReturnFloat (vm, -2);
		return true;
	}
	n = (size_t)frames * channels;
	data = Mem_Alloc (n * width);
	samples = Mem_Alloc (n * sizeof(*samples));
	if (QC_ReadMemory (vm, QC_ArgWord (vm, 3), data, n * width))
	{
		for (i = 0 ; i < n ; i++)
			switch (type)
			{
			case 8:		samples[i] = (short)((data[i] - 128) << 8); break;
			case -8:	samples[i] = (short)((signed char)data[i] << 8); break;
			case -16:	memcpy (&s16, data + i*2, 2); samples[i] = s16; break;
			default:
				memcpy (&f, data + i*4, 4);
				samples[i] = (short)(f >= 1 ? 32767 : f <= -1 ? -32768 : f * 32767);
				break;
			}
		S_RawSamples (hz, channels, samples, frames);
		QC_ReturnFloat (vm, 1);
	}
	else
	{
		QC_Warning (vm, "queueaudio: the samples aren't in memory");
		QC_ReturnFloat (vm, -1);
	}
	Mem_Free (samples);
	Mem_Free (data);
	return true;
}

// float getqueuedaudiotime(): seconds of queueaudio's samples not yet played
static bool M_GetQueuedAudioTime (qcvm_t *vm)
{
	QC_ReturnFloat (vm, S_RawQueued ());
	return true;
}

// void setkeydest(float dest): 0 the game (from the menu), 2 the menu
static bool M_SetKeyDest (qcvm_t *vm)
{
	int		dest = QC_DoubleToInt (QC_ArgFloat (vm, 0));

	if (dest == 2)
		cls.key_dest = key_menu;
	else if (dest == 0 && cls.key_dest == key_menu)
		cls.key_dest = key_game;
	return true;
}

// float getkeydest(): 2 the menu, else 0
static bool M_GetKeyDest (qcvm_t *vm)
{
	QC_ReturnFloat (vm, cls.key_dest == key_menu ? 2.0f : 0.0f);
	return true;
}

// a key FTE's QuakeC numbers as one of the client's, -1 if none
static int M_KeyArg (qcvm_t *vm, int i)
{
	return Key_FromFTE (QC_DoubleToInt (QC_ArgFloat (vm, i)));
}

// string keynumtostring(float key)
static bool M_KeynumToString (qcvm_t *vm)
{
	return CLQC_ReturnText (vm, Key_KeynumToString (M_KeyArg (vm, 0)));
}

// float stringtokeynum(string name): -1 if none
static bool M_StringToKeynum (qcvm_t *vm)
{
	int		key = Key_StringToKeynum (QC_ArgString (vm, 0));

	QC_ReturnFloat (vm, key < 0 ? -1.0f : (float)Key_ToFTE (key));
	return true;
}

// string findkeysforcommand(string command, optional float bindmap): the first
// two keys bound to it, as FTE spells them (" '65' '-1'"); a binding is the
// command's when it begins with it, as the C menu found them
static bool M_FindKeysForCommand (qcvm_t *vm)
{
	const char	*command = QC_ArgString (vm, 0), *b;
	size_t		l = strlen (command);
	int			keys[2] = {-1, -1}, count = 0, k;
	char		text[32];

	for (k = 0 ; k < 256 && count < 2 ; k++)
		if ((b = Key_BindingForKey (k)) && !strncmp (b, command, l))
			keys[count++] = Key_ToFTE (k);
	snprintf (text, sizeof(text), " '%d' '%d'", keys[0], keys[1]);
	return QC_ReturnString (vm, text, strlen (text));
}

// string getkeybind(float key, optional float bindmap)
static bool M_GetKeyBind (qcvm_t *vm)
{
	const char	*b = Key_BindingForKey (M_KeyArg (vm, 0));

	return CLQC_ReturnText (vm, b ? b : "");
}

// float setkeybind(float key, string binding, optional float bindmap): "" unbinds
static bool M_SetKeyBind (qcvm_t *vm)
{
	int		key = M_KeyArg (vm, 0);

	if (key >= 0 && key < 256)
		Key_SetBinding (key, (char *)QC_ArgString (vm, 1));
	QC_ReturnFloat (vm, key >= 0 && key < 256 ? 1.0f : 0.0f);
	return true;
}

// float clientstate(): 2 in a game, 1 out of one (connecting too, while the
// console is down over it; FTE's says 2 by then)
static bool M_ClientState (qcvm_t *vm)
{
	QC_ReturnFloat (vm, cls.state == ca_active ? 2.0f : 1.0f);
	return true;
}

// void clipboard_set(float cliptype, string text): the clipboard's (cliptype 0;
// not the selection, 1), Quake's coloured characters as their plain ones
static bool M_ClipboardSet (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 1);
	char		text[1024];
	size_t		n;
	int			c;

	if (QC_DoubleToInt (QC_ArgFloat (vm, 0)) != 0)
		return true;
	for (n = 0 ; *s && n < sizeof(text) - 1 ; s++)
	{
		c = *(const unsigned char *)s & 127;
		if (c >= 0x12 && c <= 0x1b)
			c = '0' + c - 0x12;		// the gold digits
		else if (c == 0x10 || c == 0x11)
			c = c == 0x10 ? '[' : ']';
		else if (c < ' ' || c == 127)
			c = c == '\n' || c == '\t' ? c : ' ';
		text[n++] = (char)c;
	}
	text[n] = 0;
	Sys_SetClipboardText (text);
	return true;
}

// float isfullscreen(): SoftWorld's, the window filling the screen
static bool M_IsFullscreen (qcvm_t *vm)
{
	QC_ReturnFloat (vm, VID_IsFullscreen () ? 1.0f : 0.0f);
	return true;
}

static const struct
{
	const char		*name;
	qc_builtin_t	func;
} menu_builtins[] =
{
	{"r_uploadimage", M_UploadImage},
	{"r_readimage", M_ReadImage},
	{"localsound", M_LocalSound},
	{"queueaudio", M_QueueAudio},
	{"getqueuedaudiotime", M_GetQueuedAudioTime},
	{"setkeydest", M_SetKeyDest},
	{"getkeydest", M_GetKeyDest},
	{"keynumtostring", M_KeynumToString},
	{"stringtokeynum", M_StringToKeynum},
	{"findkeysforcommand", M_FindKeysForCommand},
	{"getkeybind", M_GetKeyBind},
	{"setkeybind", M_SetKeyBind},
	{"clientstate", M_ClientState},
	{"clipboard_set", M_ClipboardSet},
	{"isfullscreen", M_IsFullscreen},
};

/*
==============================================================================

LOADING

==============================================================================
*/

// the VM and the commands of its QuakeC gone
static void M_Destroy (void)
{
	CLQC_RemoveCommands (&menu.qc);
	QC_Destroy (menu.qc.vm);
	menu.qc.vm = NULL;
	menu.qc.calls = 0;
	menu.init = menu.shutdown = menu.draw = menu.keydown = menu.keyup = menu.toggle = menu.consolecommand = 0;
}

// the progs: the game directory's menu.dat unless it failed, else the built-in one
static qc_progs_t *M_Progs (void)
{
	byte			*data;
	int				size;
	qc_progs_t		*p;
	qc_loaderror_t	lerr;
	char			text[1024];

	menu.builtin = false;
	if (!menu.gamedirfailed && (data = FS_LoadFile ("menu.dat", &size)))
	{
		p = QC_LoadProgs (data, (size_t)size, &lerr);
		Mem_Free (data);
		if (p)
			return p;
		Con_Printf ("menu.dat: %s\n", QC_LoadErrorText (&lerr, text, sizeof(text)));
		menu.gamedirfailed = true;
	}
	menu.builtin = true;
	Con_DPrintf ("menu.dat: the one built in\n");
	p = QC_LoadProgs (cl_menuprogs, cl_menuprogs_size, &lerr);
	if (!p)
		Con_Printf ("menu.dat (built in): %s\n", QC_LoadErrorText (&lerr, text, sizeof(text)));
	return p;
}

// m_draw's parameters: DP's two floats, or FTE's vector
static bool M_DrawTakesFloats (const qc_progs_t *p)
{
	uint32_t		index;
	qc_funcinfo_t	fn;

	return QC_ProgsFunctionIndex (p, "m_draw", &index) && QC_ProgsFunction (p, index, &fn) && fn.num_parms == 2
		&& fn.parm_sizes[0] == 1;
}

// the menu's QuakeC, up and initialised; the built-in one if a game
// directory's fails
static void M_Load (void)
{
	qc_progs_t	*p;
	qc_config_t	config;
	qc_error_t	err;
	char		text[1024];

	for (;;)
	{
		if (!(p = M_Progs ()))
			return;
		QC_DefaultConfig (&config, QC_MENU);
		config.developer = developer.value != 0;
		// what a menu needs, kept small: the web reserves it all
		config.limits.heap_bytes = 16 << 20;
		config.limits.max_edicts = 8192;
		config.limits.progs = 1;
		config.limits.progs_area_bytes = 1 << 20;
		menu.drawfloats = M_DrawTakesFloats (p);
		menu.qc.vm = QC_Create (p, menu.builtins, &config, &menu.host, &menu.qc, &err);
		QC_ReleaseProgs (p);
		if (!menu.qc.vm)
		{
			Con_Printf ("Menu: %s\n", QC_ErrorText (&err, text, sizeof(text)));
			QC_FreeError (&err);
		}
		else
		{
			menu.init = QC_FindFunction (menu.qc.vm, "m_init");
			menu.shutdown = QC_FindFunction (menu.qc.vm, "m_shutdown");
			menu.draw = QC_FindFunction (menu.qc.vm, "m_draw");
			menu.keydown = QC_FindFunction (menu.qc.vm, "m_keydown");
			menu.keyup = QC_FindFunction (menu.qc.vm, "m_keyup");
			menu.toggle = QC_FindFunction (menu.qc.vm, "m_toggle");
			menu.consolecommand = QC_FindFunction (menu.qc.vm, "m_consolecommand");
			if (M_Call (menu.init, 0, NULL, NULL))
				return;
		}
		// a game directory's that failed: the built-in one; the built-in one: none
		if (menu.builtin)
			return;
		menu.gamedirfailed = true;
		menu.restart = false;
	}
}

/*
================
M_Shutdown

The menu's QuakeC goes: m_shutdown first, unless a longjmp left it running
================
*/
void M_Shutdown (void)
{
	if (!menu.qc.vm)
		return;
	if (menu.qc.calls)
	{
		QC_Abandon (menu.qc.vm);
		menu.qc.calls = 0;
	}
	else if (!M_Call (menu.shutdown, 0, NULL, NULL))
		return;		// failed, and gone
	if (cls.key_dest == key_menu)
		cls.key_dest = key_game;
	M_Destroy ();
}

// a new game directory may have its own menu.dat: loaded at the next entry,
// once its paths are mounted (this runs before)
static void M_GamedirChanged (void)
{
	menu.gamedirfailed = false;
	menu.restart = true;
}

static void M_Restart_f (void)
{
	menu.gamedirfailed = false;
	M_Shutdown ();
	M_Load ();
}

/*
=================
M_Builtins_f

The builtins the menu's QuakeC calls that the client doesn't have (with
"all", those it declares)
=================
*/
static void M_Builtins_f (void)
{
	qc_unbound_t	*list;
	bool			all = Cmd_Argc () > 1 && !strcmp (Cmd_Argv (1), "all");
	uint32_t		n, i;

	if (!menu.qc.vm)
	{
		Con_Printf ("No menu is running\n");
		return;
	}
	n = QC_UnboundBuiltins (menu.qc.vm, !all, NULL, 0);
	list = Mem_Alloc (((size_t)n + 1) * sizeof(*list));
	QC_UnboundBuiltins (menu.qc.vm, !all, list, n);
	for (i = 0 ; i < n ; i++)
		if (list[i].number)
			Con_Printf ("#%-4u %s\n", list[i].number, list[i].name);
		else
			Con_Printf ("      %s\n", list[i].name);
	Con_Printf ("%u builtins %s the client lacks\n", n, all ? "declared" : "called");
	Mem_Free (list);
}

/*
================
M_Init

The engine's commands and the builtins; the QuakeC comes later (M_Start)
================
*/
void M_Init (void)
{
	size_t	i;

	Cmd_AddCommand ("togglemenu", M_ToggleMenu_f,
		"Opens or closes the main menu (from another menu, goes back to it); closes the console if it is down in a game.");
	Cmd_AddCommand ("menu_restart", M_Restart_f,
		"Loads the menu's QuakeC again: the game directory's menu.dat, else the one built in.");
	Cmd_AddCommand ("menu_builtins", M_Builtins_f,
		"Lists the builtins the menu's QuakeC calls that the client lacks; with all, those it declares. "
		"Usage: menu_builtins [all]");
	FS_AddGamedirCallback (M_GamedirChanged);

	menu.qc.name = "Menu";
	menu.qc.description = "A command of the menu's QuakeC.";
	menu.qc.command = M_Command;
	CLQC_InitHost (&menu.host);

	menu.builtins = QC_BuiltinsStandard (QC_NUMBERING_MENU);
	if (!menu.builtins)
		Sys_Error ("M_Init: out of memory");
	for (i = 0 ; i < sizeof(menu_builtins) / sizeof(menu_builtins[0]) ; i++)
		if (!QC_BuiltinsSet (menu.builtins, menu_builtins[i].name, menu_builtins[i].func))
			Sys_Error ("M_Init: out of memory");
	if (!SB_Builtins (menu.builtins) || !CLQC_DrawBuiltins (menu.builtins))
		Sys_Error ("M_Init: out of memory");
}

/*
================
M_Start

The menu's QuakeC, once the client and the server have every command and
cvar: the names its commands take are the ones left
================
*/
void M_Start (void)
{
	M_Load ();
}
