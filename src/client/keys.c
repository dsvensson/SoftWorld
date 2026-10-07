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

#define KEY_LINE	(key_input.lines[key_input.edit_line])

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

// the word being completed, and how a candidate is written in its place
typedef struct
{
	int			start;		// where it begins in the edit line
	bool		quoted;		// begun with a quote
	const char	*after;		// written after a whole completion
	bool		contains;	// the candidates were found by the name in them
	size_t		dirlen;		// the directory typed before the name
} word_t;

// Tab again, the line as the last Tab left it, and the next candidate
// replaces the last: the candidates, and which one is written
static struct
{
	bool			on;
	completions_t	c;
	word_t			word;
	int				index;
	char			line[MAXCMDLINE];
	int				linepos;
} key_cycle;

static void Key_EndCycle (void)
{
	if (key_cycle.on)
		Key_FreeCompletions (&key_cycle.c);
	key_cycle.on = false;
}

/*
====================
Key_WriteWord

The word before the cursor replaced by len characters of text, what is after
the cursor kept. A word with a space is quoted; whole adds the closing quote
and what goes after it.
====================
*/
static void Key_WriteWord (const word_t *w, const char *text, size_t len, bool whole)
{
	char	*line = KEY_LINE;
	char	rest[MAXCMDLINE], word[MAXCMDLINE];
	bool	quote = !w->quoted && memchr (text, ' ', len);
	const char	*after = whole ? w->after : "";

	Q_strncpyz (rest, line + key_input.linepos, sizeof(rest));
	if (after[0] == ' ' && rest[0] == ' ')
		after = "";		// a space after it already
	snprintf (word, sizeof(word), "%s%.*s%s%s", quote ? "\"" : "", (int)len, text,
		whole && (w->quoted || quote) ? "\"" : "", after);
	Q_strncpyz (line + w->start, word, (size_t)(MAXCMDLINE - w->start));
	key_input.linepos = (int)strlen (line);
	Q_strncatz (line, rest, MAXCMDLINE);
}

// the candidate the cycle is on, written in place
static void Key_WriteCycle (void)
{
	const char	*candidate = key_cycle.c.names[key_cycle.index];

	Key_WriteWord (&key_cycle.word, candidate, strlen (candidate), false);
	Q_strncpyz (key_cycle.line, KEY_LINE, sizeof(key_cycle.line));
	key_cycle.linepos = key_input.linepos;
}

/*
====================
Key_CompleteWord

The word before the cursor, typed so far, and its candidates: the only one is
written whole; else as far as they all agree, when that adds to what is typed
(candidates found by the name in them only when what they agree on has it
too). When nothing can be added they are listed, and from then on each Tab
writes the next (Shift+Tab the one before), starting after what is typed.
Takes the candidates for the cycle.
====================
*/
static void Key_CompleteWord (completions_t *c, const word_t *w, const char *typed, bool back)
{
	size_t	len;
	bool	grows;
	int		i;

	if (!c->count)
		return;
	len = Key_CommonPrefix (c);
	if (c->count == 1)
	{	// a directory, ending with '/', is only on the way
		Key_WriteWord (w, c->names[0], len, c->names[0][len - 1] != '/');
		return;
	}
	if (w->contains)
		grows = len > w->dirlen && Key_HasText (c->names[0] + w->dirlen, len - w->dirlen, typed + w->dirlen);
	else
		grows = len > strlen (typed);
	if (grows)
	{
		Key_WriteWord (w, c->names[0], len, false);
		return;
	}

	Key_ListCompletions (c);
	for (i = 0 ; i < c->count && Q_strcasecmp (c->names[i], typed) ; i++)
		;
	if (i == c->count)
		key_cycle.index = back ? c->count - 1 : 0;
	else
		key_cycle.index = (i + (back ? c->count - 1 : 1)) % c->count;
	key_cycle.c = *c;
	*c = (completions_t){0};
	key_cycle.word = *w;
	key_cycle.on = true;
	Key_WriteCycle ();
}

/*
====================
CompleteCommand

The word the cursor ends: a command or variable, or the argument of a command
that completes it (demos to play). Tab again goes on to the next candidate.
====================
*/
static void CompleteCommand (bool back)
{
	char			*line = KEY_LINE;
	char			typed[MAXCMDLINE], command[MAXCMDLINE], dir[MAXCMDLINE];
	const char		*slash, *word;
	completions_t	c = {0};
	word_t			w = {0};
	int				cmd, space, arg;

	if (key_cycle.on && key_input.linepos == key_cycle.linepos && !strcmp (line, key_cycle.line))
	{
		key_cycle.index = (key_cycle.index + (back ? key_cycle.c.count - 1 : 1)) % key_cycle.c.count;
		Key_WriteCycle ();
		return;
	}
	Key_EndCycle ();

	// what is typed before the cursor; what is after it stays
	snprintf (typed, sizeof(typed), "%.*s", key_input.linepos, line);
	cmd = typed[1] == '/' || typed[1] == '\\' ? 2 : 1;
	for (space = cmd ; typed[space] && typed[space] != ' ' ; space++)
		;

	if (!typed[space])
	{	// the command's name; a line of one is a command, not chat
		word = typed + cmd;
		Cmd_ListMatches (typed + cmd, Key_AddCompletion, &c);
		Cvar_ListMatches (typed + cmd, Key_AddCompletion, &c);
		Key_SortCompletions (&c);
		if (c.count && cmd == 1 && strlen (line) < MAXCMDLINE - 1)
		{
			memmove (line + 2, line + 1, strlen (line + 1) + 1);
			line[1] = '/';
			key_input.linepos++;
			cmd = 2;
		}
		w = (word_t){.start = cmd, .after = " "};
		Key_CompleteWord (&c, &w, word, back);
	}
	else
	{	// its first argument, if it completes it: from what is in the directory typed
		snprintf (command, sizeof(command), "%.*s", space - cmd, typed + cmd);
		for (arg = space ; typed[arg] == ' ' ; arg++)
			;
		w.quoted = typed[arg] == '"';
		if (w.quoted)
			arg++;
		slash = strrchr (typed + arg, '/');
		w.dirlen = slash ? (size_t)(slash - (typed + arg)) + 1 : 0;
		snprintf (dir, sizeof(dir), "%.*s", (int)w.dirlen, typed + arg);
		w.start = arg;
		w.after = "";
		if ((w.quoted ? !strchr (typed + arg, '"') : !strchr (typed + arg, ' '))
			&& Cmd_CompleteArgument (command, dir, Key_AddCompletion, &c))
		{
			Key_SortCompletions (&c);
			w.contains = Key_FilterCompletions (&c, typed + arg, w.dirlen);
			Key_CompleteWord (&c, &w, typed + arg, back);
		}
	}

	Key_FreeCompletions (&c);
}

/*
====================
Key_SuggestedName

The command or variable the first word typed is the start of, fish's way: the
first by name, unless what is typed is one already; NULL for none, and unless
the cursor is at the end of the line. Found again only when the line changes.
====================
*/
static struct
{
	bool	valid;
	char	line[MAXCMDLINE];		// the line it was found for
	char	name[MAXCMDLINE];		// empty for none
} key_suggest;

static const char *Key_SuggestedName (void)
{
	char			*line = KEY_LINE;
	int				cmd = line[1] == '/' || line[1] == '\\' ? 2 : 1, i;
	completions_t	c = {0};

	if (key_input.linepos != (int)strlen (line) || !line[cmd] || strchr (line + cmd, ' '))
		return NULL;
	if (!key_suggest.valid || strcmp (key_suggest.line, line))
	{
		Q_strncpyz (key_suggest.line, line, sizeof(key_suggest.line));
		key_suggest.valid = true;
		key_suggest.name[0] = 0;
		Cmd_ListMatches (line + cmd, Key_AddCompletion, &c);
		Cvar_ListMatches (line + cmd, Key_AddCompletion, &c);
		Key_SortCompletions (&c);
		for (i = 0 ; i < c.count && Q_strcasecmp (c.names[i], line + cmd) ; i++)
			;
		if (c.count && i == c.count)
			Q_strncpyz (key_suggest.name, c.names[0], sizeof(key_suggest.name));
		Key_FreeCompletions (&c);
	}
	return key_suggest.name[0] ? key_suggest.name : NULL;
}

const char *Key_Suggestion (void)
{
	const char	*suggested = Key_SuggestedName ();
	char		*line = KEY_LINE;

	return suggested ? suggested + strlen (line + (line[1] == '/' || line[1] == '\\' ? 2 : 1)) : NULL;
}

// the suggestion taken: the name written in place of what is typed
static bool Key_AcceptSuggestion (void)
{
	const char	*suggested = Key_SuggestedName ();
	char		*line = KEY_LINE;
	int			cmd = line[1] == '/' || line[1] == '\\' ? 2 : 1;

	if (!suggested)
		return false;
	Q_strncpyz (line + cmd, suggested, (size_t)(MAXCMDLINE - cmd));
	key_input.linepos = (int)strlen (line);
	return true;
}

/*
==============================================================================

			LINE EDITING

==============================================================================
*/

static char		key_killed[MAXCMDLINE];		// what Ctrl+U, Ctrl+K and Ctrl+W cut, for Ctrl+Y

// len characters of text put in at the cursor, as many as fit
static void Key_InsertText (const char *text, size_t len)
{
	char	*line = KEY_LINE;
	size_t	end = strlen (line), room = MAXCMDLINE - 1 - end;

	if (len > room)
		len = room;
	if (!len)
		return;
	memmove (line + key_input.linepos + len, line + key_input.linepos, end - (size_t)key_input.linepos + 1);
	memcpy (line + key_input.linepos, text, len);
	key_input.linepos += (int)len;
}

// the characters from .. to taken out, the cursor where they were; cut keeps
// them for Ctrl+Y
static void Key_DeleteText (int from, int to, bool cut)
{
	char	*line = KEY_LINE;

	if (to <= from)
		return;
	if (cut)
		snprintf (key_killed, sizeof(key_killed), "%.*s", to - from, line + from);
	memmove (line + from, line + to, strlen (line + to) + 1);
	key_input.linepos = from;
}

/*
====================
Key_Console

Interactive line editing and console scrollback: the keys (and Ctrl with a
letter), as bash has them. Typed text comes by Key_ConsoleText.
====================
*/
static void Key_Console (int key)
{
	char	*line = KEY_LINE, *clipText;
	int		end = (int)strlen (line), i;
	bool	ctrl = keydown[K_CTRL];
	int		letter = ctrl && !keydown[K_ALT] && key >= 'A' && key <= 'z' ? tolower (key) : 0;	// not AltGr's text

	if (key == K_ENTER)
	{	// backslash text are commands, else chat
		Key_EndCycle ();
		if (line[1] == '\\' || line[1] == '/')
			Cbuf_AddText (line+2);	// skip the >
		else if (CheckForCommand())
			Cbuf_AddText (line+1);	// valid command
		else
		{	// convert to a chat message
			if (cls.state >= ca_connected)
				Cbuf_AddText ("say ");
			Cbuf_AddText (line+1);	// skip the >
		}

		Cbuf_AddText ("\n");
		Con_Printf ("%s\n", line);
		key_input.edit_line = (key_input.edit_line + 1) & 31;
		history_line = key_input.edit_line;
		KEY_LINE[0] = ']';
		KEY_LINE[1] = 0;
		key_input.linepos = 1;
		if (cls.state == ca_disconnected)
			SCR_UpdateScreen ();	// force an update, because the command
									// may take some time
		return;
	}

	if (key == K_TAB)
	{	// completion; Shift+Tab goes back through the candidates
		CompleteCommand (shift_down);
		return;
	}

	if (key == K_BACKSPACE)
	{
		if (key_input.linepos > 1)
			Key_DeleteText (key_input.linepos - 1, key_input.linepos, false);
		return;
	}

	if (key == K_DEL || letter == 'd')
	{
		if (key_input.linepos < end)
			Key_DeleteText (key_input.linepos, key_input.linepos + 1, false);
		return;
	}

	if (key == K_LEFTARROW || letter == 'b')
	{
		if (key_input.linepos > 1)
			key_input.linepos--;
		return;
	}

	// at the end of the line, right takes the suggestion, as in fish
	if (key == K_RIGHTARROW || letter == 'f')
	{
		if (key_input.linepos < end)
			key_input.linepos++;
		else
			Key_AcceptSuggestion ();
		return;
	}

	if ((key == K_HOME && !ctrl) || letter == 'a')
	{
		key_input.linepos = 1;
		return;
	}

	if ((key == K_END && !ctrl) || letter == 'e')
	{
		if (key_input.linepos < end)
			key_input.linepos = end;
		else
			Key_AcceptSuggestion ();
		return;
	}

	if (letter == 'u')
	{	// cuts from the start of the line
		Key_DeleteText (1, key_input.linepos, true);
		return;
	}

	if (letter == 'k')
	{	// cuts to the end of the line
		Key_DeleteText (key_input.linepos, end, true);
		return;
	}

	if (letter == 'w')
	{	// cuts the word before the cursor
		for (i = key_input.linepos ; i > 1 && line[i - 1] == ' ' ; i--)
			;
		for ( ; i > 1 && line[i - 1] != ' ' ; i--)
			;
		Key_DeleteText (i, key_input.linepos, true);
		return;
	}

	if (letter == 'y')
	{	// puts back what was cut
		Key_InsertText (key_killed, strlen (key_killed));
		return;
	}

	if (letter == 'v')
	{
		clipText = Sys_GetClipboardText ();
		if (clipText)
		{
			strtok (clipText, "\n\r\b");	// only the first line
			for (i = 0 ; clipText[i] ; i++)
				if ((byte)clipText[i] < 32)
					clipText[i] = ' ';
			Key_InsertText (clipText, strlen (clipText));
			free (clipText);
		}
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
		Q_strcpy(KEY_LINE, key_input.lines[history_line]);
		key_input.linepos = Q_strlen(KEY_LINE);
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
			KEY_LINE[0] = ']';
			KEY_LINE[1] = 0;
			key_input.linepos = 1;
		}
		else
		{
			Q_strcpy(KEY_LINE, key_input.lines[history_line]);
			key_input.linepos = Q_strlen(KEY_LINE);
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

	// Ctrl+Home and Ctrl+End: the top and the bottom of the scrollback
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
}

// a character typed into the console's line, at the cursor
static void Key_ConsoleText (int ch)
{
	char	c = (char)ch;

	Key_InsertText (&c, 1);
}

//============================================================================

key_input_t	key_input;

static void Key_Message (int key)
{

	if (key == K_ENTER)
	{
		if (key_input.chat_bufferlen)	// an empty line says nothing
		{
			if (key_input.chat_team)
				Cbuf_AddText ("say_team \"");
			else
				Cbuf_AddText ("say \"");
			Cbuf_AddText(key_input.chat_buffer);
			Cbuf_AddText("\"\n");
		}

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
	consolekeys[K_CTRL] = true;		// Ctrl with a letter edits the line, and runs no binding
	consolekeys[K_DEL] = true;
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
	Cmd_AddCommand ("bind",Key_Bind_f, "Binds a command to a key, or shows the key's binding. Usage: bind <key> [command]");
	Cmd_AddCommand ("unbind",Key_Unbind_f, "Removes a key's binding. Usage: unbind <key>");
	Cmd_AddCommand ("unbindall",Key_Unbindall_f, "Removes every key's binding.");

	// this client's binds in place of the ones of id's default.cfg (its cvars
	// and aliases stay), not of a mod's own: WASD moves, space jumps (and swims
	// up), c swims down, the arrows play an MVD
	Cmd_SetExecSuffix ("default.cfg", "id1/pak0.pak",
		"unbindall\n"
		"bind w +forward\n"
		"bind s +back\n"
		"bind a +moveleft\n"
		"bind d +moveright\n"
		"bind c +movedown\n"
		"bind UPARROW \"demo_speed 1\"\n"
		"bind DOWNARROW \"demo_speed 0\"\n"
		"bind LEFTARROW \"demo_jump +10\"\n"
		"bind RIGHTARROW \"demo_jump -10\"\n"
		"bind SPACE +jump\n"
		"bind TAB +showteamscores\n"
		"bind 1 \"impulse 1\"\n"
		"bind 2 \"impulse 2\"\n"
		"bind 3 \"impulse 3\"\n"
		"bind 4 \"impulse 4\"\n"
		"bind 5 \"impulse 5\"\n"
		"bind 6 \"impulse 6\"\n"
		"bind 7 \"impulse 7\"\n"
		"bind 8 \"impulse 8\"\n"
		"bind F1 help\n"
		"bind F4 menu_options\n"
		"bind F5 menu_multiplayer\n"
		"bind F10 quit\n"
		"bind F12 screenshot\n"
		"bind ESCAPE togglemenu\n"
		"bind ~ toggleconsole\n"
		"bind ` toggleconsole\n"
		"bind ENTER messagemode\n"
		"bind + sizeup\n"
		"bind = sizeup\n"
		"bind - sizedown\n"
		"bind MOUSE1 +attack\n");

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
			&& !(cls.key_dest == key_console && (key == K_LEFTARROW || key == K_RIGHTARROW || key == K_DEL
				|| (keydown[K_CTRL] && key != K_CTRL)))
			&& key_repeats[key] > 1)
			return;	// ignore most autorepeats; the line's editing keys repeat
			
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
		Key_ConsoleText (ch);
		break;
	case key_game:
		if (cls.state != ca_active)
			Key_ConsoleText (ch);		// the console fills the screen
		break;
	default:
		break;
	}
}

