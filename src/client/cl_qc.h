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

#pragma once
// cl_qc.h -- what the client's QuakeC hosts share (CSQC, the menu): the
// host's callbacks to the console, the cvars and the command buffer, the
// console commands QuakeC registers, and telling a QuakeC error

#include "qcvm.h"

// a host's QuakeC: the VM's host context (QC_Create's ctx)
typedef struct clqc_s
{
	const char	*name;				// in messages: "CSQC", "Menu"
	const char	*description;		// of the commands QuakeC registers
	qcvm_t		*vm;
	int			calls;				// the client's calls into QuakeC in progress

	char		*commands[256];		// QuakeC registered them
	int			numcommands;
	// a command QuakeC registered was run: its whole line
	void		(*command) (struct clqc_s *qc, const char *line);
} clqc_t;

// fills the callbacks every host of the client has
void	CLQC_InitHost (qc_host_t *h);

// QuakeC's files (fopen and the rest), into a host's: read through the search
// path, written in the game directory
void	CLQC_FileHost (qc_host_t *h);

// a QuakeC error: its backtrace and what went wrong
void	CLQC_Failed (const clqc_t *qc);

// the commands QuakeC registered, gone
void	CLQC_RemoveCommands (clqc_t *qc);

// a string result: null for empty text, as FTE returns them
bool	CLQC_ReturnText (qcvm_t *vm, const char *text);

// the server browser's hostcache builtins (cl_slist.c), into a host's; false
// when there is no room
bool	SB_Builtins (qc_builtins_t *b);

// FTE's 2D builtins (drawpic, drawstring, ...), into a host's; false when
// there is no room
bool	CLQC_DrawBuiltins (qc_builtins_t *b);
