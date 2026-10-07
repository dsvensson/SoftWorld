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

// the pak (<basedir>/id1/pak0.pak) or directory the last file opened was found
// in; empty when it wasn't
const char	*FS_FileSource (void);

// whether the game directory has the file itself, above the base's (id1, qw)
bool	FS_InGameDir (const char *path);

// sees each file FS_LoadFile loads, as it is loaded (f_modified's checks)
void	FS_SetLoadHook (void (*hook) (const char *path, const byte *data, int length));

// called after the game directory changes, from FS_FlushGamedir: by the
// client at a level's start, or while it has none
void	FS_AddGamedirCallback (void (*callback)(void));
void	FS_RemoveGamedirCallback (void (*callback)(void));
void	FS_FlushGamedir (void);

// the paths under the search path that begin with partial: files with one of
// the extensions (NULL-terminated, each with its dot), and the directories
// with such files in them, ending with '/'; a path the search path has twice
// is given twice
void	FS_ListPaths (const char *partial, const char *const *extensions, void (*add) (void *ctx, const char *path),
			void *ctx);

void	COM_WriteFile (char *filename, void *data, int len);
int		COM_FOpenFile (const char *filename, FILE **file);
void	COM_CreatePath (char *path);
void	COM_Gamedir (char *dir);
