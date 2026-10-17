// cl_search.c -- the files of the game's search path for the menu's QuakeC:
// FTE's search_begin and the rest of its search builtins, and SoftWorld's
// search_getfileage
//
// A search is a pattern's files, found when it begins, sorted by name, each
// once (a file more than one directory or pak of the search path has, as
// COM_FOpenFile finds it), as paths under the search path: "demos/x.mvd". '*'
// and '?' match within a directory: the directory's part of the pattern is
// plain, and nothing below it is looked into. The files are the game's, read
// through the game directory's search path while attract mode has its own,
// as the menu's other files are (M_FileOpen). A file's size and time are
// looked up when they are first asked for.

#include "cl_local.h"
#include "cl_qc.h"

#include <stdlib.h>
#include <time.h>

#define SEARCH_MAX		16			// searches at once
#define SEARCH_FILES	65536		// a search's files, at most
#define SEARCH_DUPES	4			// FTE's flag: a file as often as the search path has it

typedef struct
{
	char		*path;
	bool		looked;				// its size and time were looked up
	bool		found;				// and are known
	long long	size, mtime;
} searchfile_t;

typedef struct
{
	bool			used;
	searchfile_t	*files;
	int				count, room;
} search_t;

static search_t	searches[SEARCH_MAX];

typedef struct
{
	const char	*pattern;
	search_t	*search;
} searchmatch_t;

static void Search_Add (void *ctx, const char *path)
{
	searchmatch_t	*m = ctx;
	search_t		*s = m->search;
	size_t			n = strlen (path);

	if (!n || path[n - 1] == '/' || s->count == SEARCH_FILES || !Q_GlobMatch (m->pattern, path))
		return;
	if (s->count == s->room)
	{
		s->room = s->room ? s->room * 2 : 64;
		s->files = Mem_Realloc (s->files, (size_t)s->room * sizeof(*s->files));
	}
	s->files[s->count].path = Mem_Alloc (n + 1);
	memcpy (s->files[s->count].path, path, n + 1);
	s->files[s->count].looked = false;
	s->count++;
}

static int Search_Compare (const void *a, const void *b)
{
	return Q_strcasecmp (((const searchfile_t *)a)->path, ((const searchfile_t *)b)->path);
}

static void Search_End (search_t *s)
{
	int		i;

	for (i = 0 ; i < s->count ; i++)
		Mem_Free (s->files[i].path);
	Mem_Free (s->files);
	memset (s, 0, sizeof(*s));
}

// the search a handle names, NULL for none
static search_t *Search_Handle (qcvm_t *vm, int arg)
{
	int		i = QC_DoubleToInt (QC_ArgFloat (vm, arg));

	return i >= 0 && i < SEARCH_MAX && searches[i].used ? &searches[i] : NULL;
}

// a search's file a handle and a number name, its size and time looked up
// once; NULL for none
static searchfile_t *Search_File (qcvm_t *vm)
{
	search_t		*s = Search_Handle (vm, 0);
	searchfile_t	*f;
	int				i = QC_DoubleToInt (QC_ArgFloat (vm, 1));

	if (!s || i < 0 || i >= s->count)
		return NULL;
	f = &s->files[i];
	if (!f->looked)
	{
		FS_UseChain (FS_GameDirChain ());
		f->found = FS_FileInfo (f->path, &f->size, &f->mtime);
		FS_UseChain (NULL);
		f->looked = true;
	}
	return f;
}

/*
==============================================================================

THE BUILTINS

==============================================================================
*/

// float search_begin(string pattern, float flags, float quiet): a search's
// handle, -1 where nothing matches (or there is no room); flags 4 lists a file
// as often as the search path has it (FTE's SB_ALLOWDUPES); case is always
// aside, as FTE's flag 1 has it
static bool Search_Begin (qcvm_t *vm)
{
	const char		*pattern = QC_ArgString (vm, 0), *wild, *slash, *star;
	int				flags = QC_DoubleToInt (QC_ArgFloat (vm, 1)), i, j, k;
	char			partial[MAX_OSPATH], extension[MAX_OSPATH];
	const char		*extensions[2] = {extension, NULL};
	searchmatch_t	m;
	search_t		*s;

	QC_ReturnFloat (vm, -1);
	for (i = 0 ; i < SEARCH_MAX && searches[i].used ; i++)
		;
	// a path out of the game's directories, or no name at all, finds nothing
	if (i == SEARCH_MAX || !*pattern || pattern[0] == '/' || strstr (pattern, "..") || strpbrk (pattern, ":\\"))
		return true;

	// the directory's part, plain: to the last '/' before the first wildcard
	wild = pattern + strcspn (pattern, "*?");
	for (slash = wild ; slash > pattern && slash[-1] != '/' ; slash--)
		;
	if ((size_t)(slash - pattern) >= sizeof(partial))
		return true;
	memcpy (partial, pattern, (size_t)(slash - pattern));
	partial[slash - pattern] = 0;
	// what follows the last '*' ends each file's name: the listing keeps to it
	star = strrchr (pattern, '*');
	Q_strncpyz (extension, star && !strpbrk (star + 1, "?/") ? star + 1 : "", sizeof(extension));

	s = &searches[i];
	s->used = true;
	m.pattern = pattern;
	m.search = s;
	FS_UseChain (FS_GameDirChain ());
	FS_ListPaths (partial, extensions, Search_Add, &m);
	FS_UseChain (NULL);
	if (!s->count)
	{
		Search_End (s);
		return true;
	}
	qsort (s->files, (size_t)s->count, sizeof(*s->files), Search_Compare);
	if (!(flags & SEARCH_DUPES))
	{
		for (j = k = 0 ; j < s->count ; j++)
		{
			if (k && !Q_strcasecmp (s->files[k - 1].path, s->files[j].path))
			{
				Mem_Free (s->files[j].path);
				continue;
			}
			s->files[k++] = s->files[j];
		}
		s->count = k;
	}
	QC_ReturnFloat (vm, (float)i);
	return true;
}

// void search_end(float handle)
static bool Search_EndBuiltin (qcvm_t *vm)
{
	search_t	*s = Search_Handle (vm, 0);

	if (s)
		Search_End (s);
	return true;
}

// float search_getsize(float handle): its files, 0 for no search
static bool Search_GetSize (qcvm_t *vm)
{
	search_t	*s = Search_Handle (vm, 0);

	QC_ReturnFloat (vm, s ? (float)s->count : 0);
	return true;
}

// string search_getfilename(float handle, float num)
static bool Search_GetFilename (qcvm_t *vm)
{
	search_t	*s = Search_Handle (vm, 0);
	int			i = QC_DoubleToInt (QC_ArgFloat (vm, 1));

	return CLQC_ReturnText (vm, s && i >= 0 && i < s->count ? s->files[i].path : "");
}

// float search_getfilesize(float handle, float num): bytes, -1 unknown
static bool Search_GetFileSize (qcvm_t *vm)
{
	searchfile_t	*f = Search_File (vm);

	QC_ReturnFloat (vm, f && f->found ? (float)f->size : -1);
	return true;
}

// string search_getfilemtime(float handle, float num): when it was written,
// local time as FTE has it ("2026-10-17 14:05:09"), "" unknown
static bool Search_GetFileMtime (qcvm_t *vm)
{
	searchfile_t	*f = Search_File (vm);
	time_t			t;
	struct tm		*tm;
	char			text[32];

	text[0] = 0;
	if (f && f->found && f->mtime > 0)
	{
		t = (time_t)f->mtime;
		if ((tm = localtime (&t)))
			strftime (text, sizeof(text), "%Y-%m-%d %H:%M:%S", tm);
	}
	return CLQC_ReturnText (vm, text);
}

// float search_getfileage(float handle, float num): SoftWorld's, the seconds
// since it was written (0 for a time to come), -1 unknown
static bool Search_GetFileAge (qcvm_t *vm)
{
	searchfile_t	*f = Search_File (vm);
	long long		age;

	if (!f || !f->found || f->mtime <= 0)
	{
		QC_ReturnFloat (vm, -1);
		return true;
	}
	age = (long long)time (NULL) - f->mtime;
	QC_ReturnFloat (vm, age > 0 ? (float)age : 0);
	return true;
}

static const struct
{
	const char		*name;
	qc_builtin_t	func;
} search_builtins[] =
{
	{"search_begin", Search_Begin},
	{"search_end", Search_EndBuiltin},
	{"search_getsize", Search_GetSize},
	{"search_getfilename", Search_GetFilename},
	{"search_getfilesize", Search_GetFileSize},
	{"search_getfilemtime", Search_GetFileMtime},
	{"search_getfileage", Search_GetFileAge},
};

/*
================
Search_Builtins

The searches' builtins into the menu's
================
*/
bool Search_Builtins (qc_builtins_t *b)
{
	size_t	i;

	for (i = 0 ; i < sizeof(search_builtins) / sizeof(search_builtins[0]) ; i++)
		if (!QC_BuiltinsSet (b, search_builtins[i].name, search_builtins[i].func))
			return false;
	return true;
}

/*
================
Search_Shutdown

The searches the menu left open, gone with it
================
*/
void Search_Shutdown (void)
{
	int		i;

	for (i = 0 ; i < SEARCH_MAX ; i++)
		if (searches[i].used)
			Search_End (&searches[i]);
}
