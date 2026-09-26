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
// args.h -- the command line

#define MAX_NUM_ARGVS	50

extern	int		com_argc;
extern	char	**com_argv;

void	COM_InitArgv (int argc, char **argv);

// returns the position (1 to argc-1) of parm in the argument list, or 0
int		COM_CheckParm (char *parm);

void	COM_AddParm (char *parm);
