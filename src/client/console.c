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
// console.c

#include "cl_local.h"
#include "markup.h"
static void Con_ClearNotify (void);

console_t	con;


static float		con_cursorspeed = 4;


static cvar_t		con_notifytime = {.name = "con_notifytime", .string = "3",		//seconds
	.description = "Seconds the console's last lines stay over the game after they are printed."};


static void Key_ClearTyping (void)
{
	key_input.lines[key_input.edit_line][1] = 0;	// clear any typing
	key_input.linepos = 1;
}

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f (void)
{
	Key_ClearTyping ();

	if (cls.key_dest == key_console)
	{
		if (!CL_ConsoleForced ())
			cls.key_dest = key_game;
	}
	else
		cls.key_dest = key_console;
	
	Con_ClearNotify ();
}

/*
================
Con_ToggleChat_f
================
*/
static void Con_ToggleChat_f (void)
{
	Key_ClearTyping ();

	if (cls.key_dest == key_console)
	{
		if (!CL_ConsoleForced ())
			cls.key_dest = key_game;
	}
	else
		cls.key_dest = key_console;
	
	Con_ClearNotify ();
}

/*
================
Con_Clear_f
================
*/
static void Con_Clear_f (void)
{
	Q_memset (con.text, ' ', CON_TEXTSIZE);
	memset (con.colors, 0, sizeof(con.colors));
}


/*
================
Con_ClearNotify
================
*/
static void Con_ClearNotify (void)
{
	int		i;
	
	for (i=0 ; i<NUM_CON_TIMES ; i++)
		con.times[i] = 0;
}


/*
================
Con_MessageMode_f
================
*/
static void Con_MessageMode_f (void)
{
	if (CL_Attracting ())
		return;		// no one to talk to; its line is the notify lines'
	key_input.chat_team = false;
	cls.key_dest = key_message;
}

/*
================
Con_MessageMode2_f
================
*/
static void Con_MessageMode2_f (void)
{
	if (CL_Attracting ())
		return;
	key_input.chat_team = true;
	cls.key_dest = key_message;
}

/*
================
Con_Resize

================
*/
static void Con_Resize (void)
{
	static uint16_t	cbuf[CON_TEXTSIZE];
	int		i, j, width, oldwidth, oldtotallines, numlines, numchars, from, to;
	char	tbuf[CON_TEXTSIZE];

	width = (vid.conwidth >> 3) - 2;

	if (width == con.linewidth)
		return;

	if (width < 1)			// video hasn't been initialized yet
	{
		width = 38;
		con.linewidth = width;
		con.totallines = CON_TEXTSIZE / con.linewidth;
		Q_memset (con.text, ' ', CON_TEXTSIZE);
		memset (con.colors, 0, sizeof(con.colors));
	}
	else
	{
		oldwidth = con.linewidth;
		con.linewidth = width;
		oldtotallines = con.totallines;
		con.totallines = CON_TEXTSIZE / con.linewidth;
		numlines = oldtotallines;

		if (con.totallines < numlines)
			numlines = con.totallines;

		numchars = oldwidth;
	
		if (con.linewidth < numchars)
			numchars = con.linewidth;

		Q_memcpy (tbuf, con.text, CON_TEXTSIZE);
		memcpy (cbuf, con.colors, sizeof(cbuf));
		Q_memset (con.text, ' ', CON_TEXTSIZE);
		memset (con.colors, 0, sizeof(con.colors));

		for (i=0 ; i<numlines ; i++)
		{
			for (j=0 ; j<numchars ; j++)
			{
				to = (con.totallines - 1 - i) * con.linewidth + j;
				from = ((con.current - i + oldtotallines) % oldtotallines) * oldwidth + j;
				con.text[to] = tbuf[from];
				con.colors[to] = cbuf[from];
			}
		}

		Con_ClearNotify ();
	}

	con.current = con.totallines - 1;
	con.display = con.current;
}


/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize (void)
{
	Con_Resize ();
}


/*
================
Con_Init
================
*/
static void Con_PrintSink (const char *msg);

void Con_Init (void)
{
	con.debuglog = COM_CheckParm("-condebug");
	Con_AddPrintSink (Con_PrintSink);

	con.linewidth = -1;
	Con_CheckResize ();
	
	Con_Printf ("Console initialized.\n");

//
// register our commands
//
	Cvar_RegisterVariable (&con_notifytime);

	Cmd_AddCommand ("toggleconsole", Con_ToggleConsole_f, "Opens or closes the console (it stays open while not in a game).");
	Cmd_AddCommand ("togglechat", Con_ToggleChat_f, "Opens or closes the console, as toggleconsole does.");
	Cmd_AddCommand ("messagemode", Con_MessageMode_f, "Starts a chat message to everyone on the server.");
	Cmd_AddCommand ("messagemode2", Con_MessageMode2_f, "Starts a chat message to your team.");
	Cmd_AddCommand ("clear", Con_Clear_f, "Clears the console's text.");
	con.initialized = true;
}


/*
===============
Con_Linefeed
===============
*/
static void Con_Linefeed (void)
{
	con.x = 0;
	if (con.display == con.current)
		con.display++;
	con.current++;
	Q_memset (&con.text[(con.current%con.totallines)*con.linewidth]
	, ' ', con.linewidth);
	memset (&con.colors[(con.current%con.totallines)*con.linewidth], 0, con.linewidth * sizeof(con.colors[0]));
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the notify window will pop up.
The colors ezQuake's and FTE's markup give are kept with the characters;
each print starts with none (ezQuake).
================
*/
void Con_Print (char *txt)
{
	int		y;
	int		c, l;
	static int	cr;
	int		mask;
	markup_t	m, ahead;
	const char	*s, *word;

	if (txt[0] == 1 || txt[0] == 2)
	{
		mask = 128;		// go to colored text
		txt++;
	}
	else
		mask = 0;

	Markup_Begin (&m);
	for (s = txt ; ; )
	{
	// count word length, in characters shown
		ahead = m;
		word = s;
		for (l=0 ; l< con.linewidth ; l++)
		{
			c = Markup_Next (&word, &ahead);
			if (c < 0 || (c & 127) <= ' ')
				break;
		}

		c = Markup_Next (&s, &m);
		if (c < 0)
			break;

	// word wrap
		if (l != con.linewidth && (con.x + l > con.linewidth) )
			con.x = 0;

		if (cr)
		{
			con.current--;
			cr = false;
		}


		if (!con.x)
		{
			Con_Linefeed ();
		// mark time for transparent overlay
			if (con.current >= 0)
				con.times[con.current % NUM_CON_TIMES] = (float)host.realtime;
		}

		switch (c)
		{
		case '\n':
			con.x = 0;
			break;

		case '\r':
			con.x = 0;
			cr = 1;
			break;

		default:	// display character and advance
			y = con.current % con.totallines;
			con.text[y*con.linewidth+con.x] = (char)(c | mask);
			con.colors[y*con.linewidth+con.x] = m.color;
			con.x++;
			if (con.x >= con.linewidth)
				con.x = 0;
			break;
		}
		
	}
}


/*
================
Con_Printf

Handles cursor positioning, line wrapping, etc
================
*/
#define	MAXPRINTMSG	4096
// FIXME: make a buffer size safe vsprintf?
static void Con_PrintSink (const char *msg)
{
	static bool	inupdate;
	static double	lastupdate;

// log all messages to file
	if (con.debuglog)
		Sys_DebugLog(va("%s/qconsole.log",com_gamedir), "%s", msg);
		
	if (!con.initialized)
		return;
		
// write it to the scrollable buffer
	Con_Print ((char *)msg);
	
// update the screen if the console is displayed, at most 20 times a second:
// a map whose entities print hundreds of warnings while loading drew a frame
// for each line
	if (CL_ConsoleForced () && Sys_DoubleTime () - lastupdate >= 0.05)
	{
	// protect against infinite loop if something in SCR_UpdateScreen calls
	// Con_Printd
		if (!inupdate)
		{
			inupdate = true;
			SCR_UpdateScreen ();
			lastupdate = Sys_DoubleTime ();
			inupdate = false;
		}
	}
}

/*
==============================================================================

DRAWING

==============================================================================
*/


/*
================
Con_DrawInput

The input line scrolls horizontally to keep the cursor in view. After the
text, what completing the first word would add is drawn faded (fish's
suggestion); the cursor blinks over the character it is on.
================
*/
static void Con_DrawInput (void)
{
	const char	*text, *suggestion;
	int			i, at, len, suggestlen, start, y;

	if (cls.key_dest != key_console && !CL_ConsoleForced ())
		return;		// don't draw anything (allways draw if not active)

	text = key_input.lines[key_input.edit_line];
	len = (int)strlen (text);
	suggestion = Key_Suggestion ();
	suggestlen = suggestion ? (int)strlen (suggestion) : 0;
	y = con.vislines - 22;

//	prestep if horizontally scrolling
	start = key_input.linepos >= con.linewidth ? key_input.linepos - con.linewidth + 1 : 0;

	for (i = 0 ; i < con.linewidth ; i++)
	{
		at = start + i;
		if (at < len)
			Draw_Character ((i+1)<<3, y, text[at]);
		else if (at - len < suggestlen)
			Draw_ColoredCharacter ((i+1)<<3, y, suggestion[at - len], TEXT_HALF);
	}

	if ((int)(host.realtime*con_cursorspeed) & 1)
		Draw_Character ((key_input.linepos - start + 1)<<3, y, 11);
}


/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify (void)
{
	int		x, v;
	char	*text;
	uint16_t	*colors;
	int		i;
	float	time;
	char	*s;
	int		skip;

	v = 0;
	for (i= con.current-NUM_CON_TIMES+1 ; i<=con.current ; i++)
	{
		if (i < 0)
			continue;
		time = con.times[i % NUM_CON_TIMES];
		if (time == 0)
			continue;
		time = (float)(host.realtime - time);
		if (time > con_notifytime.value)
			continue;
		text = con.text + (i % con.totallines)*con.linewidth;
		colors = con.colors + (i % con.totallines)*con.linewidth;

		for (x = 0 ; x < con.linewidth ; x++)
			Draw_ColoredCharacter ( (x+1)<<3, v, text[x], colors[x]);

		v += 8;
	}


	if (cls.key_dest == key_message)
	{
	
		if (key_input.chat_team)
		{
			Draw_String (8, v, "say_team:");
			skip = 11;
		}
		else
		{
			Draw_String (8, v, "say:");
			skip = 5;
		}

		s = key_input.chat_buffer;
		if ((unsigned)key_input.chat_bufferlen > (vid.conwidth>>3)-(skip+1))
			s += key_input.chat_bufferlen - ((vid.conwidth>>3)-(skip+1));
		x = 0;
		while(s[x])
		{
			Draw_Character ( (x+skip)<<3, v, s[x]);
			x++;
		}
		Draw_Character ( (x+skip)<<3, v, 10+((int)(host.realtime*con_cursorspeed)&1));
		v += 8;
	}
	
}

/*
================
Con_DrawConsole

Draws the console with the solid background
================
*/
void Con_DrawConsole (int lines)
{
	int				i, j, x, y, n;
	int				rows;
	char			*text;
	uint16_t		*colors;
	int				row;
	char			dlbar[1024];
	
	if (lines <= 0)
		return;

// draw the background
	Draw_ConsoleBackground (lines, CL_Downloading ());

// draw the text
	con.vislines = lines;
	
// changed to line things up better
	rows = (lines-22)>>3;		// rows of text to draw

	y = lines - 30;

// draw from the bottom up
	if (con.display != con.current)
	{
	// draw arrows to show the buffer is backscrolled
		for (x=0 ; x<con.linewidth ; x+=4)
			Draw_Character ( (x+1)<<3, y, '^');
	
		y -= 8;
		rows--;
	}
	
	row = con.display;
	for (i=0 ; i<rows ; i++, y-=8, row--)
	{
		if (row < 0)
			break;
		if (con.current - row >= con.totallines)
			break;		// past scrollback wrap point
			
		text = con.text + (row % con.totallines)*con.linewidth;
		colors = con.colors + (row % con.totallines)*con.linewidth;

		for (x=0 ; x<con.linewidth ; x++)
			Draw_ColoredCharacter ( (x+1)<<3, y, text[x], colors[x]);
	}

	// draw the download bar
	// figure out width
	if (CL_Downloading ()) {
		if ((text = strrchr(cls.downloadname, '/')) != NULL)
			text++;
		else
			text = cls.downloadname;

		// the name, cut to a third of the line, then the bar, then the
		// percent and the speed: " 100%  12.3 MB/s", 16 wide
		i = con.linewidth/3;
		if (strlen(text) > (size_t)i) {
			strncpy(dlbar, text, i);
			dlbar[i] = 0;
			Q_strncatz(dlbar, "...", sizeof(dlbar));
		} else
			Q_strncpyz(dlbar, text, sizeof(dlbar));
		Q_strncatz(dlbar, ": ", sizeof(dlbar));
		i = (int)strlen(dlbar);
		y = con.linewidth - i - 2 - 16;
		if (y < 1)
			y = 1;
		dlbar[i++] = '\x80';
		// where's the dot go?
		if (cls.downloadpercent == 0)
			n = 0;
		else
			n = y * cls.downloadpercent / 100;
			
		for (j = 0; j < y; j++)
			if (j == n)
				dlbar[i++] = '\x83';
			else
				dlbar[i++] = '\x81';
		dlbar[i++] = '\x82';
		dlbar[i] = 0;

		snprintf(dlbar + strlen(dlbar), sizeof(dlbar) - strlen(dlbar), " %3d%% %s", cls.downloadpercent,
			CL_DownloadSpeed ());

		// draw it
		y = con.vislines-22 + 8;
		for (i = 0; i < (int)strlen(dlbar); i++)
			Draw_Character ( (i+1)<<3, y, dlbar[i]);
	}


// draw the input prompt, user text, and cursor if desired
	Con_DrawInput ();
}
