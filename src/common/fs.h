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

extern	thread_local int	com_filesize;
extern	thread_local int	file_from_pak;	// the last file opened came from a pak (ZOID)
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
// the same, for data of the world (models, sounds): run when the search path
// changes without the game directory too (FS_SetSearchChain)
void	FS_AddWorldCallback (void (*callback)(void));
void	FS_RemoveGamedirCallback (void (*callback)(void));
void	FS_FlushGamedir (void);

// called around a change of the game directory, at once: leaving before the
// search path changes, entered after (the client's configs)
void	FS_SetGamedirHooks (void (*leaving)(void), void (*entered)(void));

// the paths under the search path that begin with partial: files with one of
// the extensions (NULL-terminated, each with its dot), and the directories
// with such files in them, ending with '/'; a path the search path has twice
// is given twice
void	FS_ListPaths (const char *partial, const char *const *extensions, void (*add) (void *ctx, const char *path),
			void *ctx);
// the same for a game directory alone, mounted or not
void	FS_ListDirFiles (const char *dir, const char *partial, const char *const *extensions,
			void (*add) (void *ctx, const char *path), void *ctx);

// a game directory's paks and directory over the base's (id1, qw), or alone,
// for a loader to read through on its own thread while the search path
// changes: opened, retained (the search path's as it is) and released on the
// main thread; FS_UseChain sets the calling thread's (NULL: the search path),
// for FS_LoadFile and FS_ListPaths
typedef struct fs_chain_s fs_chain_t;
fs_chain_t	*FS_OpenDirChain (const char *dir, bool alone);	// quiet: no "Added packfile"
fs_chain_t	*FS_RetainChain (fs_chain_t *chain);		// NULL: the search path's
void	FS_ReleaseChain (fs_chain_t *chain);
void	FS_UseChain (fs_chain_t *chain);

// a chain as the search path in place of the game directory's, which stays
// the directory written to; NULL puts its own back. A gamedir change does
// too. Only the world's data is dropped (FS_AddWorldCallback).
void	FS_SetSearchChain (fs_chain_t *chain);
// the game directory's own chain while another is the search path, else NULL
// (the search path is): what reads the game's own files uses it (2D pictures)
fs_chain_t	*FS_GameDirChain (void);

void	COM_WriteFile (char *filename, void *data, int len);
int		COM_FOpenFile (const char *filename, FILE **file);
void	COM_CreatePath (char *path);
void	COM_Gamedir (char *dir);
