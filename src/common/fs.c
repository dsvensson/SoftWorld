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
// fs.c -- the file system: search paths, pack files and game directories

#include "fs.h"
#include "arena.h"
#include "args.h"
#include "cmd.h"
#include "cvar.h"
#include "mem.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>





static cvar_t	registered = {.name = "registered", .string = "0", .noreset = true,
	.description = "Whether the registered game's data (gfx/pop.lmp) was found; set at startup, read by the game code.",
	.values = (const cvar_value_t[]){{"0", "Shareware data"}, {"1", "Registered data"}, {0}}};

static int		static_registered = 1;	// only for startup check, then set


static void COM_InitFilesystem (const char *basedir);
static void COM_Path_f (void);


char	gamedirfile[MAX_OSPATH];

// this graphic needs to be in the pak file to use registered features
static unsigned short pop[] =
{
 0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000
,0x0000,0x0000,0x6600,0x0000,0x0000,0x0000,0x6600,0x0000
,0x0000,0x0066,0x0000,0x0000,0x0000,0x0000,0x0067,0x0000
,0x0000,0x6665,0x0000,0x0000,0x0000,0x0000,0x0065,0x6600
,0x0063,0x6561,0x0000,0x0000,0x0000,0x0000,0x0061,0x6563
,0x0064,0x6561,0x0000,0x0000,0x0000,0x0000,0x0061,0x6564
,0x0064,0x6564,0x0000,0x6469,0x6969,0x6400,0x0064,0x6564
,0x0063,0x6568,0x6200,0x0064,0x6864,0x0000,0x6268,0x6563
,0x0000,0x6567,0x6963,0x0064,0x6764,0x0063,0x6967,0x6500
,0x0000,0x6266,0x6769,0x6a68,0x6768,0x6a69,0x6766,0x6200
,0x0000,0x0062,0x6566,0x6666,0x6666,0x6666,0x6562,0x0000
,0x0000,0x0000,0x0062,0x6364,0x6664,0x6362,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0062,0x6662,0x0000,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0061,0x6661,0x0000,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0000,0x6500,0x0000,0x0000,0x0000
,0x0000,0x0000,0x0000,0x0000,0x6400,0x0000,0x0000,0x0000
};

/*


All of Quake's data access is through a hierchal file system, but the contents of the file system can be transparently merged from several sources.

The "base directory" is the path to the directory holding the quake.exe and all game directories.  The sys_* files pass this to host_init in quakeparms_t->basedir.  This can be overridden with the "-basedir" command line parm to allow code debugging in a different directory.  The base directory is
only used during filesystem initialization.

The "game directory" is the first tree on the search path and directory that all generated files (savegames, screenshots, demos, config files) will be saved to.  This can be overridden with the "-game" command line parameter.  The game directory can never be changed while quake is executing.  This is a precacution against having a malicious server instruct clients to write files over areas they shouldn't.

The "cache directory" is only used during development to save network bandwidth, especially over ISDN / T1 lines.  If there is a cache directory
specified, when a file is found by the normal search path, it will be mirrored
into the cache directory, then opened there.
	
*/

//============================================================================


// ClearLink is used for new headnodes

/*
============================================================================

					BYTE ORDER FUNCTIONS

============================================================================
*/



/*
==============================================================================

			MESSAGE IO FUNCTIONS

Handles byte ordering and avoids alignment errors
==============================================================================
*/

//
// writing functions
//

//
// reading functions
//

// returns -1 and sets msg_badread if no more characters are available

//===========================================================================

//============================================================================

//============================================================================


/*
================
COM_CheckRegistered

Looks for the pop.txt file and verifies it.
Sets the "registered" cvar.
Immediately exits out if an alternate game was attempted to be started without
being registered.
================
*/
static void COM_CheckRegistered (void)
{
	FILE		*h;
	unsigned short	check[128];
	int			i;

	COM_FOpenFile("gfx/pop.lmp", &h);
	static_registered = 0;

	if (!h)
	{
		Con_Printf ("Playing shareware version.\n");
		return;
	}

	fread (check, 1, sizeof(check), h);
	fclose (h);
	
	for (i=0 ; i<128 ; i++)
		if (pop[i] != (unsigned short)BigShort (check[i]))
			Sys_Error ("Corrupted data file.");
	
	Cvar_Set ("registered", "1");
	static_registered = 1;
	Con_Printf ("Playing registered version.\n");
}

/*
================
COM_Init
================
*/
void COM_Init (const char *basedir)
{
	Con_PrintInit ();
	Cvar_RegisterVariable (&registered);
	Cmd_AddCommand ("path", COM_Path_f, "Lists the directories and pak files searched for game files, in order.");
	Cmd_AddCommand ("memstats", Arena_PrintStats, "Lists memory by arena: kilobytes used and held, and the totals.");

	COM_InitFilesystem (basedir);
	COM_CheckRegistered ();
}

/// just for debugging

/*
=============================================================================

QUAKE FILESYSTEM

=============================================================================
*/

thread_local int	com_filesize;


//
// in memory
//

typedef struct
{
	char	name[MAX_QPATH];
	int		filepos, filelen;
} packfile_t;

typedef struct pack_s
{
	char	filename[MAX_OSPATH];
	FILE	*handle;
	int		numfiles;
	packfile_t	*files;
} pack_t;

//
// on disk
//
typedef struct
{
	char	name[56];
	int		filepos, filelen;
} dpackfile_t;

typedef struct
{
	char	id[4];
	int		dirofs;
	int		dirlen;
} dpackheader_t;


char	com_gamedir[MAX_OSPATH];
static char	com_basedir[MAX_OSPATH];

/*
================
FS_BaseDir
================
*/
const char *FS_BaseDir (void)
{
	return com_basedir;
}

typedef struct searchpath_s
{
	char	filename[MAX_OSPATH];
	pack_t	*pack;		// only one of filename / pack will be used
	struct searchpath_s *next;
} searchpath_t;

static searchpath_t	*com_searchpaths;
static searchpath_t	*com_base_searchpaths;	// without gamedirs

// a game directory's paks and directory over the base's: the search path's
// (fs_live), or one a loader reads through while the search path changes
struct fs_chain_s
{
	searchpath_t	*paths;		// its own, then those of base
	searchpath_t	*base;		// com_base_searchpaths, or NULL for a directory alone
	int				refs;		// changed on the main thread only
};

static fs_chain_t	*fs_live;					// com_searchpaths is its paths
static fs_chain_t	*fs_home;					// the game directory's, while FS_SetSearchChain's is live
static thread_local fs_chain_t	*fs_thread_chain;	// FS_UseChain's, NULL: the search path

/*
================
COM_filelength
================
*/
static int COM_filelength (FILE *f)
{
	int		pos;
	int		end;

	pos = ftell (f);
	fseek (f, 0, SEEK_END);
	end = ftell (f);
	fseek (f, pos, SEEK_SET);

	return end;
}

static int COM_FileOpenRead (char *path, FILE **hndl)
{
	FILE	*f;
	int		len;

	*hndl = NULL;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	len = COM_filelength(f);
	if (len < 0)
	{	// not a file that can be read (a directory)
		fclose (f);
		return -1;
	}
	*hndl = f;
	return len;
}

/*
============
COM_Path_f

============
*/
static void COM_Path_f (void)
{
	searchpath_t	*s;
	
	Con_Printf ("Current search path:\n");
	for (s=com_searchpaths ; s ; s=s->next)
	{
		if (s == com_base_searchpaths)
			Con_Printf ("----------\n");
		if (s->pack)
			Con_Printf ("%s (%i files)\n", s->pack->filename, s->pack->numfiles);
		else
			Con_Printf ("%s\n", s->filename);
	}
}

/*
============
COM_WriteFile

The filename will be prefixed by the current game directory
============
*/
void COM_WriteFile (char *filename, void *data, int len)
{
	FILE	*f;
	char	fullpath[MAX_OSPATH];

	snprintf (fullpath, sizeof(fullpath), "%s/%s", com_gamedir, filename);

	f = fopen (fullpath, "wb");
	if (!f) {
		Sys_mkdir(com_gamedir);
		f = fopen (fullpath, "wb");
		if (!f)
			Sys_Error ("Error opening %s", filename);
	}

	Sys_Printf ("COM_WriteFile: %s\n", fullpath);
	fwrite (data, 1, len, f);
	fclose (f);
}


/*
============
COM_CreatePath

Only used for CopyFile and download
============
*/
void	COM_CreatePath (char *path)
{
	char	*ofs;
	
	for (ofs = path+1 ; *ofs ; ofs++)
	{
		if (*ofs == '/')
		{	// create the directory
			*ofs = 0;
			Sys_mkdir (path);
			*ofs = '/';
		}
	}
}

/*
===========
COM_FindFile

Finds the file in the search path.
Sets com_filesize and one of handle or file
===========
*/
thread_local int file_from_pak; // global indicating file came from pack file ZOID

static thread_local char	com_filesource[MAX_OSPATH];	// the pak or directory the last file was found in

const char *FS_FileSource (void)
{
	return com_filesource;
}

bool FS_InGameDir (const char *path)
{
	searchpath_t	*search;
	char			netpath[MAX_OSPATH];
	int				i;

	for (search = com_searchpaths ; search && search != com_base_searchpaths ; search = search->next)
	{
		if (search->pack)
		{
			for (i = 0 ; i < search->pack->numfiles ; i++)
				if (!strcmp (search->pack->files[i].name, path))
					return true;
			continue;
		}
		snprintf (netpath, sizeof(netpath), "%s/%s", search->filename, path);
		if (Sys_FileTime (netpath) != -1)
			return true;
	}
	return false;
}

int COM_FOpenFile (const char *filename, FILE **file)
{
	searchpath_t	*search;
	char		netpath[MAX_OSPATH];
	pack_t		*pak;
	int			i;
	int			findtime;

	file_from_pak = 0;
	com_filesource[0] = 0;

//
// search through the path, one element at a time: a loader's own chain
// (FS_UseChain), or the search path's
//
	for (search = fs_thread_chain ? fs_thread_chain->paths : com_searchpaths ; search ; search = search->next)
	{
	// is the element a pak file?
		if (search->pack)
		{
		// look through all the pak file elements
			pak = search->pack;
			for (i=0 ; i<pak->numfiles ; i++)
				if (!strcmp (pak->files[i].name, filename))
				{	// found it!
					Sys_Printf ("PackFile: %s : %s\n",pak->filename, filename);
				// open a new file on the pakfile; one gone since is passed by
					*file = fopen (pak->filename, "rb");
					if (!*file)
					{
						Sys_Printf ("FindFile: can't reopen %s\n", pak->filename);
						break;
					}
					fseek (*file, pak->files[i].filepos, SEEK_SET);
					com_filesize = pak->files[i].filelen;
					file_from_pak = 1;
					Q_strncpyz (com_filesource, pak->filename, sizeof(com_filesource));
					return com_filesize;
				}
		}
		else
		{		
	// check a file in the directory tree
			if (!static_registered)
			{	// if not a registered version, don't ever go beyond base
				if ( strchr (filename, '/') || strchr (filename,'\\'))
					continue;
			}
			
			snprintf (netpath, sizeof(netpath), "%s/%s",search->filename, filename);
			
			findtime = Sys_FileTime (netpath);
			if (findtime == -1)
				continue;
				
			*file = fopen (netpath, "rb");
			if (!*file)
			{	// there, but not to be read (permissions, a sandbox)
				Sys_Printf ("FindFile: can't open %s\n", netpath);
				continue;
			}
			Sys_Printf ("FindFile: %s\n",netpath);
			Q_strncpyz (com_filesource, search->filename, sizeof(com_filesource));
			return COM_filelength (*file);
		}
		
	}
	
	Sys_Printf ("FindFile: can't find %s\n", filename);
	
	*file = NULL;
	com_filesize = -1;
	return -1;
}

/*
===============================================================================

LISTING

===============================================================================
*/

#define FS_PROBE_DEPTH	3		// how deep a directory is looked into for a file listed

typedef struct
{
	const char			*dir;		// the partial path's directory, "" or ending with '/'
	const char			*name;		// and the start of the name in it
	const char			*diskdir;	// that directory in the search path's, on disk
	const char *const	*extensions;
	void				(*add) (void *ctx, const char *path);
	void				*ctx;
} fs_listing_t;

typedef struct
{
	const char			*dir;
	const char *const	*extensions;
	int					depth;		// directories further down to look into
	bool				found;
} fs_probe_t;

// the name ends with one of the extensions (case aside)
static bool FS_HasExtension (const char *name, const char *const *extensions)
{
	size_t	len = strlen (name), elen;

	for ( ; *extensions ; extensions++)
	{
		elen = strlen (*extensions);
		if (len > elen && !Q_strcasecmp (name + len - elen, *extensions))
			return true;
	}
	return false;
}

static void FS_ProbeEntry (void *ctx, const char *name, bool isdir)
{
	fs_probe_t	*p = ctx, sub;
	char		path[MAX_OSPATH];

	if (p->found || name[0] == '.')
		return;
	if (!isdir)
	{
		p->found = FS_HasExtension (name, p->extensions);
		return;
	}
	if (p->depth <= 0)
		return;
	snprintf (path, sizeof(path), "%s/%s", p->dir, name);
	sub = *p;
	sub.dir = path;
	sub.depth--;
	Sys_ListDir (path, FS_ProbeEntry, &sub);
	p->found = sub.found;
}

// a file with one of the extensions in the directory, or a few directories down
static bool FS_HasFiles (const char *dir, const char *const *extensions)
{
	fs_probe_t	p = {.dir = dir, .extensions = extensions, .depth = FS_PROBE_DEPTH - 1};

	Sys_ListDir (dir, FS_ProbeEntry, &p);
	return p.found;
}

// an entry of a directory of the search path, the listing's own directory
static void FS_ListEntry (void *ctx, const char *name, bool isdir)
{
	fs_listing_t	*l = ctx;
	char			path[MAX_OSPATH];

	// hidden entries only when asked for by their dot
	if (Q_strncasecmp (name, l->name, strlen (l->name)) || (name[0] == '.' && l->name[0] != '.'))
		return;
	if (!isdir && !FS_HasExtension (name, l->extensions))
		return;
	if (isdir)
	{
		snprintf (path, sizeof(path), "%s/%s", l->diskdir, name);
		if (!FS_HasFiles (path, l->extensions))
			return;
	}
	snprintf (path, sizeof(path), "%s%s%s", l->dir, name, isdir ? "/" : "");
	l->add (l->ctx, path);
}

/*
============
FS_ListPaths

The paths under the search path that begin with partial (case aside): the
files with one of the extensions, and the directories on the way to others,
ending with '/' (those with such files in them: on disk a few directories
deep), each as often as the search path has it
============
*/
void FS_ListPaths (const char *partial, const char *const *extensions, void (*add) (void *ctx, const char *path),
	void *ctx)
{
	searchpath_t	*search;
	pack_t			*pak;
	fs_listing_t	l = {.extensions = extensions, .add = add, .ctx = ctx};
	char			dir[MAX_OSPATH], full[MAX_OSPATH * 2], path[MAX_QPATH];
	const char		*slash = strrchr (partial, '/'), *next;
	size_t			len = strlen (partial), dirlen;
	int				i;

	dirlen = slash ? (size_t)(slash - partial) + 1 : 0;
	if (dirlen >= sizeof(dir))
		return;
	memcpy (dir, partial, dirlen);
	dir[dirlen] = 0;
	l.dir = dir;
	l.name = partial + dirlen;

	for (search = fs_thread_chain ? fs_thread_chain->paths : com_searchpaths ; search ; search = search->next)
	{
		if (!search->pack)
		{
			snprintf (full, sizeof(full), "%s/%s", search->filename, dir);
			l.diskdir = full;
			Sys_ListDir (full, FS_ListEntry, &l);
			continue;
		}
		// a pak's files have whole paths: a directory is where one of them goes
		// on past the partial's
		pak = search->pack;
		for (i = 0 ; i < pak->numfiles ; i++)
		{
			if (Q_strncasecmp (pak->files[i].name, partial, len) || !FS_HasExtension (pak->files[i].name, extensions))
				continue;
			next = strchr (pak->files[i].name + dirlen, '/');
			if (next)
			{
				snprintf (path, sizeof(path), "%.*s", (int)(next - pak->files[i].name) + 1, pak->files[i].name);
				add (ctx, path);
			}
			else
				add (ctx, pak->files[i].name);
		}
	}
}

/*
============
FS_ListDirFiles

FS_ListPaths for a game directory alone, mounted or not: its loose files
and its paks'
============
*/
void FS_ListDirFiles (const char *dir, const char *partial, const char *const *extensions,
	void (*add) (void *ctx, const char *path), void *ctx)
{
	fs_chain_t	*chain = FS_OpenDirChain (dir, true), *was = fs_thread_chain;

	FS_UseChain (chain);
	FS_ListPaths (partial, extensions, add, ctx);
	FS_UseChain (was);
	FS_ReleaseChain (chain);
}

/*
============
FS_LoadFile

Loads a file from the search path into memory from Mem_Alloc, with a 0 byte
appended. Returns NULL if the file doesn't exist; the caller frees the data
with Mem_Free. length (if not NULL) receives the file size.
============
*/
static void	(*fs_loadhook) (const char *path, const byte *data, int length);

void FS_SetLoadHook (void (*hook) (const char *path, const byte *data, int length))
{
	fs_loadhook = hook;
}

byte *FS_LoadFile (const char *path, int *length)
{
	FILE	*h;
	byte	*buf;
	int		len;

// look for it in the filesystem or pack files
	len = com_filesize = COM_FOpenFile (path, &h);
	if (!h)
		return NULL;

	buf = Mem_Alloc ((size_t)len + 1);
	buf[len] = 0;
	if (fread (buf, 1, (size_t)len, h) != (size_t)len)
	{
		fclose (h);
		Mem_Free (buf);
		Con_Printf ("FS_LoadFile: error reading %s\n", path);
		return NULL;
	}
	fclose (h);

	// the hook is the main thread's: a loader reads through a chain of its own
	if (fs_loadhook && !fs_thread_chain)
		fs_loadhook (path, buf, len);
	if (length)
		*length = len;
	return buf;
}

/*
============
FS_AddGamedirCallback

The callbacks run after the game directory changes, so data loaded from the
old directory can be dropped: when nothing uses it (FS_FlushGamedir)
============
*/
#define MAX_GAMEDIR_CALLBACKS	8
static void	(*gamedir_callbacks[MAX_GAMEDIR_CALLBACKS])(void);
static bool	gamedir_world[MAX_GAMEDIR_CALLBACKS];	// FS_AddWorldCallback's
static int	num_gamedir_callbacks;
static bool	gamedir_changed;		// the callbacks are owed a run
static bool	search_changed;			// the world's are (FS_SetSearchChain)

static void FS_AddCallback (void (*callback)(void), bool world)
{
	if (num_gamedir_callbacks == MAX_GAMEDIR_CALLBACKS)
		Sys_Error ("FS_AddGamedirCallback: too many callbacks");
	gamedir_world[num_gamedir_callbacks] = world;
	gamedir_callbacks[num_gamedir_callbacks++] = callback;
}

void FS_AddGamedirCallback (void (*callback)(void))
{
	FS_AddCallback (callback, false);
}

void FS_AddWorldCallback (void (*callback)(void))
{
	FS_AddCallback (callback, true);
}

void FS_RemoveGamedirCallback (void (*callback)(void))
{
	int		i;

	for (i=0 ; i<num_gamedir_callbacks ; i++)
	{
		if (gamedir_callbacks[i] == callback)
		{
			num_gamedir_callbacks--;
			gamedir_callbacks[i] = gamedir_callbacks[num_gamedir_callbacks];
			gamedir_world[i] = gamedir_world[num_gamedir_callbacks];
			return;
		}
	}
}

/*
============
FS_FlushGamedir

The callbacks, where the game directory changed since they last ran. Not
while a level is on: they free its models and its sounds (a server's gamedir
command mid-level changes the search path, and the next level reloads)
============
*/
void FS_FlushGamedir (void)
{
	int		i;

	if (!gamedir_changed && !search_changed)
		return;
	for (i = 0 ; i < num_gamedir_callbacks ; i++)
		if (gamedir_changed || gamedir_world[i])
			gamedir_callbacks[i] ();
	gamedir_changed = search_changed = false;
}

/*
============
FS_SetSearchChain

A chain (FS_OpenDirChain's, held as long as it is) as the search path, in
place of the game directory's, or the game directory's back (NULL): the
directory itself (gamedirfile, com_gamedir, where files are written) stays,
and nothing runs as it would for a change of it (FS_SetGamedirHooks); the
world's data loaded is dropped (FS_AddWorldCallback), the rest kept
============
*/
void FS_SetSearchChain (fs_chain_t *chain)
{
	fs_chain_t	*to = chain ? chain : fs_home;

	if (!to || to == fs_live)
		return;
	if (chain)
		chain->refs++;
	if (!fs_home)
		fs_home = fs_live;
	else
		FS_ReleaseChain (fs_live);
	if (!chain)
		fs_home = NULL;
	if (to->paths != com_searchpaths)
		search_changed = true;
	fs_live = to;
	com_searchpaths = to->paths;
}

static void	(*gamedir_leaving)(void), (*gamedir_entered)(void);	// FS_SetGamedirHooks

void FS_SetGamedirHooks (void (*leaving)(void), void (*entered)(void))
{
	gamedir_leaving = leaving;
	gamedir_entered = entered;
}

/*
=================
COM_LoadPackFile

Takes an explicit (not game tree related) path to a pak file.

Loads the header and directory; NULL, said on the console when loud, if it
isn't a pak that can be read
=================
*/
static pack_t *COM_LoadPackFile (const char *packfile, bool loud)
{
	dpackheader_t	header;
	int				i;
	packfile_t		*newfiles;
	int				numpackfiles;
	pack_t			*pack;
	FILE			*packhandle;
	dpackfile_t		*info;
	const char		*error = NULL;

	if (COM_FileOpenRead ((char *)packfile, &packhandle) == -1)
		return NULL;

	if (fread (&header, 1, sizeof(header), packhandle) != sizeof(header)
		|| header.id[0] != 'P' || header.id[1] != 'A' || header.id[2] != 'C' || header.id[3] != 'K')
		error = "is not a packfile";
	header.dirofs = LittleLong (header.dirofs);
	header.dirlen = LittleLong (header.dirlen);
	if (!error && (header.dirlen < 0 || header.dirofs < 0))
		error = "has a bad directory";
	if (error)
	{
		fclose (packhandle);
		if (loud)
			Con_Printf ("%s %s\n", packfile, error);
		return NULL;
	}
	numpackfiles = header.dirlen / (int)sizeof(dpackfile_t);

	info = Mem_Alloc ((size_t)header.dirlen + 1);
	fseek (packhandle, header.dirofs, SEEK_SET);
	if (fread (info, 1, (size_t)header.dirlen, packhandle) != (size_t)header.dirlen)
	{
		Mem_Free (info);
		fclose (packhandle);
		if (loud)
			Con_Printf ("%s: couldn't read the directory\n", packfile);
		return NULL;
	}

// parse the directory
	newfiles = Mem_Calloc ((size_t)numpackfiles, sizeof(packfile_t));
	for (i=0 ; i<numpackfiles ; i++)
	{
		Q_strncpyz (newfiles[i].name, info[i].name, sizeof(newfiles[i].name));
		newfiles[i].filepos = LittleLong(info[i].filepos);
		newfiles[i].filelen = LittleLong(info[i].filelen);
	}

	Mem_Free (info);

	pack = Mem_Calloc (1, sizeof (pack_t));
	Q_strncpyz (pack->filename, packfile, sizeof(pack->filename));
	pack->handle = packhandle;
	pack->numfiles = numpackfiles;
	pack->files = newfiles;
	
	if (loud)
		Con_Printf ("Added packfile %s (%i files)\n", packfile, numpackfiles);
	return pack;
}

/*
================
COM_PushDirectory

The directory dir (a full path), then its pak0.pak, pak1.pak ... over paths:
the head of a search path, the paks before the directory
================
*/
static searchpath_t *COM_PushDirectory (const char *dir, searchpath_t *paths, bool loud)
{
	int				i;
	searchpath_t	*search;
	pack_t			*pak;
	char			pakfile[MAX_OSPATH];

	search = Mem_Calloc (1, sizeof(searchpath_t));
	Q_strncpyz (search->filename, dir, sizeof(search->filename));
	search->next = paths;
	paths = search;

	// the paks numbered from 0 on, to the first missing; one that can't be
	// read is passed by
	for (i=0 ; ; i++)
	{
		snprintf (pakfile, sizeof(pakfile), "%s/pak%i.pak", dir, i);
		if (Sys_FileTime (pakfile) == -1)
			break;
		pak = COM_LoadPackFile (pakfile, loud);
		if (!pak)
			continue;
		search = Mem_Calloc (1, sizeof(searchpath_t));
		search->pack = pak;
		search->next = paths;
		paths = search;
	}
	return paths;
}

/*
================
COM_AddGameDirectory

Sets com_gamedir, adds the directory to the head of the path,
then loads and adds pak1.pak pak2.pak ... 
================
*/
static void COM_AddGameDirectory (char *dir)
{
	char			*p;

	if ((p = strrchr(dir, '/')) != NULL)
		Q_strncpyz(gamedirfile, ++p, sizeof(gamedirfile));
	else
		Q_strncpyz(gamedirfile, dir, sizeof(gamedirfile));
	Q_strncpyz (com_gamedir, dir, sizeof(com_gamedir));

	com_searchpaths = COM_PushDirectory (dir, com_searchpaths, true);
}

/*
================
FS_IsBaseDir

id1 and qw, which the base search path always has
================
*/
static bool FS_IsBaseDir (const char *dir)
{
	return !strcmp (dir, "id1") || !strcmp (dir, "qw");
}

/*
================
FS_NewChain

A game directory's chain over the base's: nothing above it for id1 and qw
================
*/
static fs_chain_t *FS_NewChain (const char *dir, bool alone, bool loud)
{
	fs_chain_t	*chain = Mem_Calloc (1, sizeof(*chain));
	char		path[MAX_OSPATH];

	chain->refs = 1;
	chain->base = alone ? NULL : com_base_searchpaths;
	chain->paths = chain->base;
	if (alone || !FS_IsBaseDir (dir))
	{
		snprintf (path, sizeof(path), "%s/%s", com_basedir, dir);
		chain->paths = COM_PushDirectory (path, chain->base, loud);
	}
	return chain;
}

/*
================
FS_OpenDirChain / FS_RetainChain / FS_ReleaseChain / FS_UseChain

Chains a loader reads through on its thread while the search path changes:
a game directory's (opened quietly), or the search path's as it is. They are
held and let go on the main thread; the last let go closes its paks.
================
*/
fs_chain_t *FS_OpenDirChain (const char *dir, bool alone)
{
	return FS_NewChain (dir, alone, false);
}

fs_chain_t *FS_RetainChain (fs_chain_t *chain)
{
	if (!chain)
		chain = fs_live;
	chain->refs++;
	return chain;
}

void FS_ReleaseChain (fs_chain_t *chain)
{
	searchpath_t	*next;

	if (--chain->refs > 0)
		return;
	while (chain->paths != chain->base)
	{
		if (chain->paths->pack)
		{
			fclose (chain->paths->pack->handle);
			Mem_Free (chain->paths->pack->files);
			Mem_Free (chain->paths->pack);
		}
		next = chain->paths->next;
		Mem_Free (chain->paths);
		chain->paths = next;
	}
	Mem_Free (chain);
}

fs_chain_t *FS_GameDirChain (void)
{
	return fs_home;
}

void FS_UseChain (fs_chain_t *chain)
{
	fs_thread_chain = chain;
}

/*
================
COM_Gamedir

Sets the gamedir and path to a different directory, the hooks around it
(FS_SetGamedirHooks).
================
*/
void COM_Gamedir (char *dir)
{
	if (strstr(dir, "..") || strstr(dir, "/")
		|| strstr(dir, "\\") || strstr(dir, ":") )
	{
		Con_Printf ("Gamedir should be a single filename, not a path\n");
		return;
	}

	if (!strcmp(gamedirfile, dir))
		return;		// still the same (whatever FS_SetSearchChain set)
	FS_SetSearchChain (NULL);	// the game directory's own search path back
	if (gamedir_leaving)
		gamedir_leaving ();
	Q_strncpyz (gamedirfile, dir, sizeof(gamedirfile));

	// the old directory's paths go once no loader reads through them, and
	// the data loaded from it once nothing uses it
	FS_ReleaseChain (fs_live);
	fs_live = FS_NewChain (dir, false, true);
	com_searchpaths = fs_live->paths;
	gamedir_changed = true;

	// the base's (id1 and qw) is qw's directory
	if (FS_IsBaseDir (dir))
		snprintf (com_gamedir, sizeof(com_gamedir), "%s/qw", com_basedir);
	else
		snprintf (com_gamedir, sizeof(com_gamedir), "%s/%s", com_basedir, dir);

	if (gamedir_entered)
		gamedir_entered ();
}

/*
================
COM_InitFilesystem
================
*/
static void COM_InitFilesystem (const char *basedir)
{
	int		i;

//
// -basedir <path>
// Overrides the system supplied base directory (under id1)
//
	i = COM_CheckParm ("-basedir");
	if (i && i < com_argc-1)
		Q_strncpyz (com_basedir, com_argv[i+1], sizeof(com_basedir));
	else
		Q_strncpyz (com_basedir, basedir, sizeof(com_basedir));

//
// start up with id1 by default
//
	COM_AddGameDirectory (va("%s/id1", com_basedir) );
	COM_AddGameDirectory (va("%s/qw", com_basedir) );

	// any set gamedirs will be freed up to here
	com_base_searchpaths = com_searchpaths;
	fs_live = Mem_Calloc (1, sizeof(*fs_live));
	fs_live->refs = 1;
	fs_live->paths = fs_live->base = com_searchpaths;
}
