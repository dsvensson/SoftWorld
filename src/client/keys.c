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
/*

key up events are sent even if in console mode

*/


static int		shift_down=false;
static int		key_lastpress;

static int		history_line=0;


static int		key_count;			// incremented every key event

static char	*keybindings[256];
static bool	key_char_eaten;		// the next typed character belongs to a binding

static bool	consolekeys[256];	// if true, can't be rebound while in console
static bool	menubound[256];	// if true, can't be rebound while in menu
static int		keyshift[256];		// key to map to if shift held down in console
static int		key_repeats[256];	// if > 1, it is autorepeating
static bool	keydown[256];

typedef struct
{
	char	*name;
	int		keynum;
} keyname_t;

static keyname_t keynames[] =
{
	{"TAB", K_TAB},
	{"ENTER", K_ENTER},
	{"ESCAPE", K_ESCAPE},
	{"SPACE", K_SPACE},
	{"BACKSPACE", K_BACKSPACE},
	{"UPARROW", K_UPARROW},
	{"DOWNARROW", K_DOWNARROW},
	{"LEFTARROW", K_LEFTARROW},
	{"RIGHTARROW", K_RIGHTARROW},

	{"ALT", K_ALT},
	{"CTRL", K_CTRL},
	{"SHIFT", K_SHIFT},
	
	{"F1", K_F1},
	{"F2", K_F2},
	{"F3", K_F3},
	{"F4", K_F4},
	{"F5", K_F5},
	{"F6", K_F6},
	{"F7", K_F7},
	{"F8", K_F8},
	{"F9", K_F9},
	{"F10", K_F10},
	{"F11", K_F11},
	{"F12", K_F12},

	{"INS", K_INS},
	{"DEL", K_DEL},
	{"PGDN", K_PGDN},
	{"PGUP", K_PGUP},
	{"HOME", K_HOME},
	{"END", K_END},

	{"MOUSE1", K_MOUSE1},
	{"MOUSE2", K_MOUSE2},
	{"MOUSE3", K_MOUSE3},
	{"MOUSE4", K_MOUSE4},
	{"MOUSE5", K_MOUSE5},

	{"JOY1", K_JOY1},
	{"JOY2", K_JOY2},
	{"JOY3", K_JOY3},
	{"JOY4", K_JOY4},

	{"AUX1", K_AUX1},
	{"AUX2", K_AUX2},
	{"AUX3", K_AUX3},
	{"AUX4", K_AUX4},
	{"AUX5", K_AUX5},
	{"AUX6", K_AUX6},
	{"AUX7", K_AUX7},
	{"AUX8", K_AUX8},
	{"AUX9", K_AUX9},
	{"AUX10", K_AUX10},
	{"AUX11", K_AUX11},
	{"AUX12", K_AUX12},
	{"AUX13", K_AUX13},
	{"AUX14", K_AUX14},
	{"AUX15", K_AUX15},
	{"AUX16", K_AUX16},
	{"AUX17", K_AUX17},
	{"AUX18", K_AUX18},
	{"AUX19", K_AUX19},
	{"AUX20", K_AUX20},
	{"AUX21", K_AUX21},
	{"AUX22", K_AUX22},
	{"AUX23", K_AUX23},
	{"AUX24", K_AUX24},
	{"AUX25", K_AUX25},
	{"AUX26", K_AUX26},
	{"AUX27", K_AUX27},
	{"AUX28", K_AUX28},
	{"AUX29", K_AUX29},
	{"AUX30", K_AUX30},
	{"AUX31", K_AUX31},
	{"AUX32", K_AUX32},

	{"PAUSE", K_PAUSE},

	{"MWHEELUP", K_MWHEELUP},
	{"MWHEELDOWN", K_MWHEELDOWN},

	{"SEMICOLON", ';'},	// because a raw semicolon seperates commands

	{NULL,0}
};

/*
==============================================================================

			LINE TYPING INTO THE CONSOLE

==============================================================================
*/

static bool CheckForCommand (void)
{
	char	command[128];
	char	*cmd, *s;
	int		i;

	s = key_input.lines[key_input.edit_line]+1;

	for (i=0 ; i<127 ; i++)
		if (s[i] <= ' ')
			break;
		else
			command[i] = s[i];
	command[i] = 0;

	cmd = Cmd_CompleteCommand (command);
	if (!cmd || strcmp (cmd, command))
		cmd = Cvar_CompleteVariable (command);
	if (!cmd  || strcmp (cmd, command) )
		return false;		// just a chat message
	return true;
}

/*
==============================================================================

			COMPLETION

==============================================================================
*/

#define MAX_COMPLETIONS		65536	// candidates taken at most
#define MAX_LISTED			256		// and listed

// the candidates for the word being completed
typedef struct
{
	char	**names;
	int		count, size;
	bool	overflow;		// there were more
} completions_t;

static void Key_AddCompletion (void *ctx, const char *candidate)
{
	completions_t	*c = ctx;

	if (c->count == MAX_COMPLETIONS)
	{
		c->overflow = true;
		return;
	}
	if (c->count == c->size)
	{
		c->size = c->size ? c->size * 2 : 64;
		c->names = Mem_Realloc (c->names, (size_t)c->size * sizeof(c->names[0]));
	}
	c->names[c->count] = Mem_Alloc (strlen (candidate) + 1);
	strcpy (c->names[c->count], candidate);
	c->count++;
}

// case aside, and then as they are: the same name twice is side by side
static int Key_CompareNames (const void *a, const void *b)
{
	const char	*x = *(char *const *)a, *y = *(char *const *)b;
	int			d = Q_strcasecmp (x, y);

	return d ? d : strcmp (x, y);
}

// sorted, each once
static void Key_SortCompletions (completions_t *c)
{
	int		i, n;

	if (!c->count)
		return;
	qsort (c->names, (size_t)c->count, sizeof(c->names[0]), Key_CompareNames);
	for (i = n = 1 ; i < c->count ; i++)
		if (strcmp (c->names[i], c->names[n - 1]))
			c->names[n++] = c->names[i];
		else
			Mem_Free (c->names[i]);
	c->count = n;
}

static void Key_FreeCompletions (completions_t *c)
{
	int		i;

	for (i = 0 ; i < c->count ; i++)
		Mem_Free (c->names[i]);
	Mem_Free (c->names);
}

// the first len characters of text have sub in them, case aside
static bool Key_HasText (const char *text, size_t len, const char *sub)
{
	size_t	sublen = strlen (sub), i;

	for (i = 0 ; i + sublen <= len ; i++)
		if (!Q_strncasecmp (text + i, sub, sublen))
			return true;
	return false;
}

/*
====================
Key_FilterCompletions

The candidates that begin with typed; or when none does, those with the name
typed (after the dirlen characters of its directory) in their name, as demos
named by their date are found by the rest. True for the latter.
====================
*/
static bool Key_FilterCompletions (completions_t *c, const char *typed, size_t dirlen)
{
	size_t	len = strlen (typed);
	int		i, n, begin = 0;
	bool	contains, keep;

	for (i = 0 ; i < c->count ; i++)
		if (!Q_strncasecmp (c->names[i], typed, len))
			begin++;
	contains = !begin;

	for (i = n = 0 ; i < c->count ; i++)
	{
		if (contains)
			keep = Key_HasText (c->names[i] + dirlen, strlen (c->names[i] + dirlen), typed + dirlen);
		else
			keep = !Q_strncasecmp (c->names[i], typed, len);
		if (keep)
			c->names[n++] = c->names[i];
		else
			Mem_Free (c->names[i]);
	}
	c->count = n;
	return contains;
}

// how much of the first candidate all the others begin with, case aside
static size_t Key_CommonPrefix (const completions_t *c)
{
	size_t	len = strlen (c->names[0]), j;
	int		i;

	for (i = 1 ; i < c->count ; i++)
	{
		for (j = 0 ; j < len && tolower ((byte)c->names[i][j]) == tolower ((byte)c->names[0][j]) ; j++)
			;
		len = j;
	}
	return len;
}

// the candidates (sorted) under the line typed, in columns across the console
static void Key_ListCompletions (const completions_t *c)
{
	size_t	width = 0, len;
	int		i, columns, listed = c->count < MAX_LISTED ? c->count : MAX_LISTED;

	for (i = 0 ; i < listed ; i++)
	{
		len = strlen (c->names[i]);
		if (len > width)
			width = len;
	}
	width += 2;
	columns = con.linewidth / (int)width;
	if (columns < 1)
		columns = 1;

	Con_Printf ("%s\n", key_input.lines[key_input.edit_line]);
	for (i = 0 ; i < listed ; i++)
		if (i % columns == columns - 1 || i == listed - 1)
			Con_Printf ("%s\n", c->names[i]);
		else
			Con_Printf ("%-*s", (int)width, c->names[i]);
	if (c->overflow)
		Con_Printf ("and more\n");
	else if (listed < c->count)
		Con_Printf ("and %d more\n", c->count - listed);
}

/*
====================
Key_CompleteWord

The word of the edit line from start, which the candidates complete: made as
long as they all agree, and when only one is left the whole of it and after
(a directory, ending with '/', is only on the way). Candidates found by the
name in them (contains) replace the word only when what they agree on has
it too. A word with a space is quoted (quoted says it is already). When
nothing can be added, the candidates are listed.
====================
*/
static void Key_CompleteWord (completions_t *c, int start, bool quoted, const char *after, bool contains,
	size_t dirlen)
{
	char	*line = key_input.lines[key_input.edit_line];
	char	word[MAXCMDLINE];
	size_t	len;
	bool	whole, quote, grows;

	if (!c->count)
		return;
	len = Key_CommonPrefix (c);
	if (contains)
		grows = len > dirlen && Key_HasText (c->names[0] + dirlen, len - dirlen, line + start + dirlen);
	else
		grows = len > strlen (line + start);
	if (c->count > 1 && !grows)
	{
		Key_ListCompletions (c);
		return;
	}

	whole = c->count == 1 && c->names[0][len - 1] != '/';
	quote = !quoted && memchr (c->names[0], ' ', len);
	snprintf (word, sizeof(word), "%s%.*s%s%s", quote ? "\"" : "", (int)len, c->names[0],
		whole && (quoted || quote) ? "\"" : "", whole ? after : "");
	Q_strncpyz (line + start, word, (size_t)(MAXCMDLINE - start));
	key_input.linepos = (int)strlen (line);
}

/*
====================
CompleteCommand

The word the cursor ends: a command or variable, or the argument of a command
that completes it (demos to play)
====================
*/
static void CompleteCommand (void)
{
	char			*line = key_input.lines[key_input.edit_line];
	char			command[MAXCMDLINE], dir[MAXCMDLINE];
	const char		*slash;
	completions_t	c = {0};
	int				cmd, space, arg;
	size_t			dirlen;
	bool			quoted, contains;

	line[key_input.linepos] = 0;		// what is typed, not what backspacing left after it
	cmd = line[1] == '/' || line[1] == '\\' ? 2 : 1;
	for (space = cmd ; line[space] && line[space] != ' ' ; space++)
		;

	if (!line[space])
	{	// the command's name; a line of one is a command, not chat
		Cmd_ListMatches (line + cmd, Key_AddCompletion, &c);
		Cvar_ListMatches (line + cmd, Key_AddCompletion, &c);
		Key_SortCompletions (&c);
		if (c.count && cmd == 1 && key_input.linepos < MAXCMDLINE - 1)
		{
			memmove (line + 2, line + 1, strlen (line + 1) + 1);
			line[1] = '/';
			cmd = 2;
		}
		Key_CompleteWord (&c, cmd, false, " ", false, 0);
	}
	else
	{	// its first argument, if it completes it: from what is in the directory typed
		snprintf (command, sizeof(command), "%.*s", space - cmd, line + cmd);
		for (arg = space ; line[arg] == ' ' ; arg++)
			;
		quoted = line[arg] == '"';
		if (quoted)
			arg++;
		slash = strrchr (line + arg, '/');
		dirlen = slash ? (size_t)(slash - (line + arg)) + 1 : 0;
		snprintf (dir, sizeof(dir), "%.*s", (int)dirlen, line + arg);
		if ((quoted ? !strchr (line + arg, '"') : !strchr (line + arg, ' '))
			&& Cmd_CompleteArgument (command, dir, Key_AddCompletion, &c))
		{
			Key_SortCompletions (&c);
			contains = Key_FilterCompletions (&c, line + arg, dirlen);
			Key_CompleteWord (&c, arg, quoted, "", contains, dirlen);
		}
	}

	Key_FreeCompletions (&c);
}

/*
====================
Key_Console

Interactive line editing and console scrollback
====================
*/
static void Key_Console (int key)
{
	int		i;
	char	*clipText;
	
	if (key == K_ENTER)
	{	// backslash text are commands, else chat
		if (key_input.lines[key_input.edit_line][1] == '\\' || key_input.lines[key_input.edit_line][1] == '/')
			Cbuf_AddText (key_input.lines[key_input.edit_line]+2);	// skip the >
		else if (CheckForCommand())
			Cbuf_AddText (key_input.lines[key_input.edit_line]+1);	// valid command
		else
		{	// convert to a chat message
			if (cls.state >= ca_connected)
				Cbuf_AddText ("say ");
			Cbuf_AddText (key_input.lines[key_input.edit_line]+1);	// skip the >
		}

		Cbuf_AddText ("\n");
		Con_Printf ("%s\n",key_input.lines[key_input.edit_line]);
		key_input.edit_line = (key_input.edit_line + 1) & 31;
		history_line = key_input.edit_line;
		key_input.lines[key_input.edit_line][0] = ']';
		key_input.linepos = 1;
		if (cls.state == ca_disconnected)
			SCR_UpdateScreen ();	// force an update, because the command
									// may take some time
		return;
	}

	if (key == K_TAB)
	{	// command completion
		CompleteCommand ();
		return;
	}
	
	if (key == K_BACKSPACE || key == K_LEFTARROW)
	{
		if (key_input.linepos > 1)
			key_input.linepos--;
		return;
	}

	if (key == K_UPARROW)
	{
		do
		{
			history_line = (history_line - 1) & 31;
		} while (history_line != key_input.edit_line
				&& !key_input.lines[history_line][1]);
		if (history_line == key_input.edit_line)
			history_line = (key_input.edit_line+1)&31;
		Q_strcpy(key_input.lines[key_input.edit_line], key_input.lines[history_line]);
		key_input.linepos = Q_strlen(key_input.lines[key_input.edit_line]);
		return;
	}

	if (key == K_DOWNARROW)
	{
		if (history_line == key_input.edit_line) return;
		do
		{
			history_line = (history_line + 1) & 31;
		}
		while (history_line != key_input.edit_line
			&& !key_input.lines[history_line][1]);
		if (history_line == key_input.edit_line)
		{
			key_input.lines[key_input.edit_line][0] = ']';
			key_input.linepos = 1;
		}
		else
		{
			Q_strcpy(key_input.lines[key_input.edit_line], key_input.lines[history_line]);
			key_input.linepos = Q_strlen(key_input.lines[key_input.edit_line]);
		}
		return;
	}

	if (key == K_PGUP || key==K_MWHEELUP)
	{
		con.display -= 2;
		return;
	}

	if (key == K_PGDN || key==K_MWHEELDOWN)
	{
		con.display += 2;
		if (con.display > con.current)
			con.display = con.current;
		return;
	}

	if (key == K_HOME)
	{
		con.display = con.current - con.totallines + 10;
		return;
	}

	if (key == K_END)
	{
		con.display = con.current;
		return;
	}
	
	if ((key=='V' || key=='v') && keydown[K_CTRL])
	{
		clipText = Sys_GetClipboardText ();
		if (clipText)
		{
			strtok (clipText, "\n\r\b");	// only the first line
			i = (int)strlen (clipText);
			if (i + key_input.linepos >= MAXCMDLINE)
				i = MAXCMDLINE - 1 - key_input.linepos;
			if (i > 0)
			{
				clipText[i] = 0;
				Q_strncatz (key_input.lines[key_input.edit_line], clipText, sizeof(key_input.lines[key_input.edit_line]));
				key_input.linepos += i;
			}
			free (clipText);
		}
		return;
	}

	if (key < 32 || key > 127)
		return;	// non printable
		
	if (key_input.linepos < MAXCMDLINE-1)
	{
		key_input.lines[key_input.edit_line][key_input.linepos] = (char)key;
		key_input.linepos++;
		key_input.lines[key_input.edit_line][key_input.linepos] = 0;
	}

}

//============================================================================

key_input_t	key_input;

static void Key_Message (int key)
{

	if (key == K_ENTER)
	{
		if (key_input.chat_team)
			Cbuf_AddText ("say_team \"");
		else
			Cbuf_AddText ("say \"");
		Cbuf_AddText(key_input.chat_buffer);
		Cbuf_AddText("\"\n");

		cls.key_dest = key_game;
		key_input.chat_bufferlen = 0;
		key_input.chat_buffer[0] = 0;
		return;
	}

	if (key == K_ESCAPE)
	{
		cls.key_dest = key_game;
		key_input.chat_bufferlen = 0;
		key_input.chat_buffer[0] = 0;
		return;
	}

	if (key < 32 || key > 127)
		return;	// non printable

	if (key == K_BACKSPACE)
	{
		if (key_input.chat_bufferlen)
		{
			key_input.chat_bufferlen--;
			key_input.chat_buffer[key_input.chat_bufferlen] = 0;
		}
		return;
	}

	if (key_input.chat_bufferlen == sizeof(key_input.chat_buffer)-1)
		return; // all full

	key_input.chat_buffer[key_input.chat_bufferlen++] = (char)key;
	key_input.chat_buffer[key_input.chat_bufferlen] = 0;
}

//============================================================================


/*
===================
Key_StringToKeynum

Returns a key number to be used to index keybindings[] by looking at
the given string.  Single ascii characters return themselves, while
the K_* names are matched up.
===================
*/
static int Key_StringToKeynum (char *str)
{
	keyname_t	*kn;
	
	if (!str || !str[0])
		return -1;
	if (!str[1])
		return str[0];

	for (kn=keynames ; kn->name ; kn++)
	{
		if (!Q_strcasecmp(str,kn->name))
			return kn->keynum;
	}
	return -1;
}

/*
===================
Key_KeynumToString

Returns a string (either a single ascii char, or a K_* name) for the
given keynum.
FIXME: handle quote special (general escape sequence?)
===================
*/
char *Key_KeynumToString (int keynum)
{
	keyname_t	*kn;	
	static	char	tinystr[2];
	
	if (keynum == -1)
		return "<KEY NOT FOUND>";
	if (keynum > 32 && keynum < 127)
	{	// printable ascii
		tinystr[0] = (char)keynum;
		tinystr[1] = 0;
		return tinystr;
	}
	
	for (kn=keynames ; kn->name ; kn++)
		if (keynum == kn->keynum)
			return kn->name;

	return "<UNKNOWN KEYNUM>";
}


/*
===================
Key_BindingForKey
===================
*/
const char *Key_BindingForKey (int keynum)
{
	if (keynum < 0 || keynum > 255)
		return NULL;
	return keybindings[keynum];
}

/*
===================
Key_SetBinding
===================
*/
void Key_SetBinding (int keynum, char *binding)
{
	char	*new;
	int		l;
			
	if (keynum == -1)
		return;

// free old bindings
	if (keybindings[keynum])
	{
		Mem_Free (keybindings[keynum]);
		keybindings[keynum] = NULL;
	}
			
// allocate memory for new binding
	l = Q_strlen (binding);	
	new = Mem_Alloc ((size_t)l+1);
	Q_strcpy (new, binding);
	new[l] = 0;
	keybindings[keynum] = new;	
}

/*
===================
Key_Unbind_f
===================
*/
static void Key_Unbind_f (void)
{
	int		b;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("unbind <key> : remove commands from a key\n");
		return;
	}
	
	b = Key_StringToKeynum (Cmd_Argv(1));
	if (b==-1)
	{
		Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	Key_SetBinding (b, "");
}

static void Key_Unbindall_f (void)
{
	int		i;
	
	for (i=0 ; i<256 ; i++)
		if (keybindings[i])
			Key_SetBinding (i, "");
}


/*
===================
Key_Bind_f
===================
*/
static void Key_Bind_f (void)
{
	int			i, c, b;
	char		cmd[1024];
	
	c = Cmd_Argc();

	if (c != 2 && c != 3)
	{
		Con_Printf ("bind <key> [command] : attach a command to a key\n");
		return;
	}
	b = Key_StringToKeynum (Cmd_Argv(1));
	if (b==-1)
	{
		Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	if (c == 2)
	{
		if (keybindings[b])
			Con_Printf ("\"%s\" = \"%s\"\n", Cmd_Argv(1), keybindings[b] );
		else
			Con_Printf ("\"%s\" is not bound\n", Cmd_Argv(1) );
		return;
	}
	
// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	for (i=2 ; i< c ; i++)
	{
		Q_strncatz (cmd, Cmd_Argv(i), sizeof(cmd));
		if (i != (c-1))
			Q_strncatz (cmd, " ", sizeof(cmd));
	}

	Key_SetBinding (b, cmd);
}

/*
============
Key_WriteBindings

Writes lines containing "bind key value"
============
*/
void Key_WriteBindings (FILE *f)
{
	int		i;

	for (i=0 ; i<256 ; i++)
		if (keybindings[i])
			fprintf (f, "bind %s \"%s\"\n", Key_KeynumToString(i), keybindings[i]);
}


/*
===================
Key_Init
===================
*/
void Key_Init (void)
{
	int		i;

	for (i=0 ; i<32 ; i++)
	{
		key_input.lines[i][0] = ']';
		key_input.lines[i][1] = 0;
	}
	key_input.linepos = 1;
	
//
// init ascii characters in console mode
//
	for (i=32 ; i<128 ; i++)
		consolekeys[i] = true;
	consolekeys[K_ENTER] = true;
	consolekeys[K_TAB] = true;
	consolekeys[K_LEFTARROW] = true;
	consolekeys[K_RIGHTARROW] = true;
	consolekeys[K_UPARROW] = true;
	consolekeys[K_DOWNARROW] = true;
	consolekeys[K_BACKSPACE] = true;
	consolekeys[K_HOME] = true;
	consolekeys[K_END] = true;
	consolekeys[K_PGUP] = true;
	consolekeys[K_PGDN] = true;
	consolekeys[K_SHIFT] = true;
	consolekeys[K_MWHEELUP] = true;
	consolekeys[K_MWHEELDOWN] = true;
	consolekeys['`'] = false;
	consolekeys['~'] = false;

	for (i=0 ; i<256 ; i++)
		keyshift[i] = i;
	for (i='a' ; i<='z' ; i++)
		keyshift[i] = i - 'a' + 'A';
	keyshift['1'] = '!';
	keyshift['2'] = '@';
	keyshift['3'] = '#';
	keyshift['4'] = '$';
	keyshift['5'] = '%';
	keyshift['6'] = '^';
	keyshift['7'] = '&';
	keyshift['8'] = '*';
	keyshift['9'] = '(';
	keyshift['0'] = ')';
	keyshift['-'] = '_';
	keyshift['='] = '+';
	keyshift[','] = '<';
	keyshift['.'] = '>';
	keyshift['/'] = '?';
	keyshift[';'] = ':';
	keyshift['\''] = '"';
	keyshift['['] = '{';
	keyshift[']'] = '}';
	keyshift['`'] = '~';
	keyshift['\\'] = '|';

	menubound[K_ESCAPE] = true;
	for (i=0 ; i<12 ; i++)
		menubound[K_F1+i] = true;

//
// register our functions
//
	Cmd_AddCommand ("bind",Key_Bind_f);
	Cmd_AddCommand ("unbind",Key_Unbind_f);
	Cmd_AddCommand ("unbindall",Key_Unbindall_f);


}

/*
===================
Key_Event

Called by the system between frames for both key up and key down events
Should NOT be called during an interrupt!
===================
*/
void Key_Event (int key, bool down)
{
	char	*kb;
	char	cmd[1024];

//	Con_Printf ("%i : %i\n", key, down); //@@@

	keydown[key] = down;

	if (!down)
		key_repeats[key] = 0;

	key_lastpress = key;
	key_count++;
	if (key_count <= 0)
	{
		return;		// just catching keys for Con_NotifyBox
	}

// update auto-repeat status
	if (down)
	{
		key_repeats[key]++;
		if (key != K_BACKSPACE 
			&& key != K_PAUSE 
			&& key != K_PGUP 
			&& key != K_PGDN
			&& key_repeats[key] > 1)
			return;	// ignore most autorepeats
			
		if (key >= 200 && !keybindings[key])
			Con_Printf ("%s is unbound, hit F4 to set.\n", Key_KeynumToString (key) );
	}

	if (key == K_SHIFT)
		shift_down = down;
	if (down)
		key_char_eaten = false;

//
// handle escape specialy, so the user can never unbind it
//
	if (key == K_ESCAPE)
	{
		if (!down)
			return;
		switch (cls.key_dest)
		{
		case key_message:
			Key_Message (key);
			break;
		case key_menu:
			M_Keydown (key);
			break;
		case key_game:
		case key_console:
			M_ToggleMenu_f ();
			break;
		default:
			Sys_Error ("Bad key_dest");
		}
		return;
	}

//
// key up events only generate commands if the game key binding is
// a button command (leading + sign).  These will occur even in console mode,
// to keep the character from continuing an action started before a console
// switch.  Button commands include the kenum as a parameter, so multiple
// downs can be matched with ups
//
	if (!down)
	{
		kb = keybindings[key];
		if (kb && kb[0] == '+')
		{
			snprintf (cmd, sizeof(cmd), "-%s %i\n", kb+1, key);
			Cbuf_AddText (cmd);
		}
		if (keyshift[key] != key)
		{
			kb = keybindings[keyshift[key]];
			if (kb && kb[0] == '+')
			{
				snprintf (cmd, sizeof(cmd), "-%s %i\n", kb+1, key);
				Cbuf_AddText (cmd);
			}
		}
		return;
	}

//
// if not a consolekey, send to the interpreter no matter what mode is
//
	if ( (cls.key_dest == key_menu && menubound[key])
	|| (cls.key_dest == key_console && !consolekeys[key])
	|| (cls.key_dest == key_game && ( cls.state == ca_active || !consolekeys[key] ) ) )
	{
		kb = keybindings[key];
		if (kb)
		{
			key_char_eaten = true;
			if (kb[0] == '+')
			{	// button commands add keynum as a parm
				snprintf (cmd, sizeof(cmd), "%s %i\n", kb, key);
				Cbuf_AddText (cmd);
			}
			else
			{
				Cbuf_AddText (kb);
				Cbuf_AddText ("\n");
			}
		}
		return;
	}

	if (!down)
		return;		// other systems only care about key down events

	if (shift_down)
		key = keyshift[key];

	// text for the console and message line comes from Key_CharEvent
	if (cls.key_dest != key_menu && key >= 32 && key < 127 && !keydown[K_CTRL])
		return;

	switch (cls.key_dest)
	{
	case key_message:
		Key_Message (key);
		break;
	case key_menu:
		M_Keydown (key);
		break;

	case key_game:
	case key_console:
		Key_Console (key);
		break;
	default:
		Sys_Error ("Bad key_dest");
	}
}

/*
===================
Key_ClearStates
===================
*/
void Key_ClearStates (void)
{
	int		i;

	// send the key ups, so held button commands (+forward) are released
	for (i=0 ; i<256 ; i++)
	{
		if (keydown[i])
			Key_Event (i, false);
		keydown[i] = false;
		key_repeats[i] = 0;
	}
	IN_ClearStates ();
}

/*
===================
Key_CharEvent

Typed text for the console and the message line. Key_Event handles their
editing keys; printable characters come from here, after the keyboard layout.
===================
*/
void Key_CharEvent (int ch)
{
	if (key_char_eaten)
	{	// the key that typed this ran a binding (toggleconsole, messagemode)
		key_char_eaten = false;
		return;
	}
	if (ch < 32 || ch >= 127)
		return;		// control characters are keys, and the font is ASCII

	switch (cls.key_dest)
	{
	case key_message:
		Key_Message (ch);
		break;
	case key_console:
		Key_Console (ch);
		break;
	case key_game:
		if (cls.state != ca_active)
			Key_Console (ch);		// the console fills the screen
		break;
	default:
		break;
	}
}

