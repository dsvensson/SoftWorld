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
// fs.h -- the file system: search paths, pack files and game directories

#include "q_types.h"

#include <stdio.h>

extern	int		com_filesize;
extern	char	com_gamedir[MAX_OSPATH];
extern	char	gamedirfile[MAX_OSPATH];

// starts the file system on basedir (overridable with -basedir)
void	COM_Init (const char *basedir);
const char	*FS_BaseDir (void);	// the directory holding the game directories

// loads a file into memory from Mem_Alloc (0-terminated); NULL if missing
byte	*FS_LoadFile (const char *path, int *length);

// called whenever the game directory changes
void	FS_AddGamedirCallback (void (*callback)(void));
void	FS_RemoveGamedirCallback (void (*callback)(void));

void	COM_WriteFile (char *filename, void *data, int len);
int		COM_FOpenFile (const char *filename, FILE **file);
void	COM_CreatePath (char *path);
void	COM_Gamedir (char *dir);
