// qc_lib_file.c -- QuakeC's files (FTE's FRIK_FILE): fopen, fclose, fgets,
// fputs, fread, fwrite, fseek, fsize and their 64-bit forms, fremove,
// frename, whichpack
//
// The VM keeps the handles and FTE's sandbox; the host's file callbacks reach
// the disk. As FTE's: a name is refused with "..", ':' or '\', or a leading
// '/'; one without data/ in front is written there, and read from there or,
// failing that, as it is (but for configs). A file to read streams from the
// host; one to write is kept in memory (charged to limits.container_bytes)
// and written when it is closed.

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QC_FIRST_FILE	1000			// FTE's first handle
#define QC_LINE_MAX		4095			// what fgets returns of a line at most
#define QC_WINDOW		4096			// fgets reads ahead this much

// fopen's modes, FTE's numbers
enum
{
	QC_FILE_READ = 0,					// fgets a line at a time, fread
	QC_FILE_APPEND = 1,					// write, after what the file has
	QC_FILE_WRITE = 2,
	QC_FILE_INVALID = 3,				// FTE's placeholder: a handle that does nothing
	QC_FILE_READNL = 4,					// fgets the whole file at once
	QC_FILE_MMAP_READ = 5,				// the file in a heap block, which fgets returns
	QC_FILE_MMAP_RW = 6					// that, written back when closed
};

typedef struct
{
	bool		open;
	int			mode;
	char		*path;			// the sandboxed name, where a written file goes
	void		*file;			// read: the host's
	uint64_t	len;			// the file's bytes (read), or the buffer's
	uint64_t	ofs;
	uint8_t		*data;			// append, write, readnl: the bytes
	size_t		alloc;			// data's allocation, charged
	uint32_t	ptr;			// mmap: the heap block
	uint32_t	ptrsize;		// and its size
	uint8_t		*window;		// read: bytes read ahead for fgets
	uint64_t	wstart;
	size_t		wlen;
} qc_file_t;

struct qc_files_s
{
	qc_file_t	*slots;
	uint32_t	count;
};

/*
==============================================================================

NAMES

==============================================================================
*/

// a config at the top or under configs/: never read outside data/ (FTE keeps
// QuakeC from reading the player's passwords)
static bool QC_ConfigPath (const char *name)
{
	const char	*dot = strrchr (name, '.');

	return (!strchr (name, '/') || QC_LibEqualFold (name, "configs/", 8)) && dot && !strchr (dot, '/')
		&& QC_LibEqualFold (dot, ".cfg", 5);
}

// a name QuakeC may use: relative, no "..", ':' or '\'
static bool QC_NameAllowed (const char *name)
{
	return *name && *name != '/' && !strchr (name, ':') && !strchr (name, '\\') && !strstr (name, "..");
}

// FTE's QC_FixFileName: where name goes (under data/, unless it says so
// already), and where else it is read from (NULL for nowhere); false if the
// name is refused
static bool QC_FixName (const char *name, char *path, size_t size, const char **fallback)
{
	if (!QC_NameAllowed (name))
		return false;
	if (!strncmp (name, "data/", 5))
	{
		*fallback = NULL;
		return snprintf (path, size, "%s", name) < (int)size;
	}
	*fallback = QC_ConfigPath (name) ? NULL : name;
	return snprintf (path, size, "data/%s", name) < (int)size;
}

/*
==============================================================================

HANDLES

==============================================================================
*/

static qc_files_t *QC_Files (qcvm_t *vm)
{
	qc_std_t	*std = QC_LibState (vm);

	if (std && !std->files && !(std->files = calloc (1, sizeof(*std->files))))
		QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	return std ? std->files : NULL;
}

// the file a handle names, with a warning if none
static qc_file_t *QC_FileArg (qcvm_t *vm, const char *builtin)
{
	float		h = QC_ArgFloat (vm, 0);
	int64_t		n = (int64_t)QC_FloatToInt (h) - QC_FIRST_FILE;
	qc_files_t	*files = vm->std ? vm->std->files : NULL;

	if (!files || n < 0 || n >= files->count)
	{
		QC_Warning (vm, "%s: file out of range (%g)", builtin, (double)h);
		return NULL;
	}
	if (!files->slots[n].open)
	{
		QC_Warning (vm, "%s: file is not open", builtin);
		return NULL;
	}
	return &files->slots[n];
}

// a free slot, the list grown if need be; NULL past limits.files
static qc_file_t *QC_NewFile (qcvm_t *vm, uint32_t *index)
{
	qc_files_t	*files = QC_Files (vm);
	qc_file_t	*grown;
	uint32_t	i, count;

	if (!files)
		return NULL;
	for (i = 0 ; i < files->count ; i++)
		if (!files->slots[i].open)
			break;
	if (i == files->count)
	{
		if (files->count >= vm->config.limits.files)
			return NULL;
		count = files->count ? files->count * 2 : 8;
		if (count > vm->config.limits.files)
			count = vm->config.limits.files;
		if (!(grown = realloc (files->slots, count * sizeof(*grown))))
			return NULL;
		memset (grown + files->count, 0, (count - files->count) * sizeof(*grown));
		files->slots = grown;
		files->count = count;
	}
	*index = i;
	files->slots[i] = (qc_file_t){0};
	return &files->slots[i];
}

// a buffer of at least n bytes (to write to, FTE's growth: twice and a bit),
// zeroed past what it held; false past the budget
static bool QC_Reserve (qcvm_t *vm, qc_file_t *f, uint64_t n, bool grow)
{
	size_t	want;
	uint8_t	*grown;

	if (n <= f->alloc)
		return true;
	if (n > SIZE_MAX / 2 - 1024)
		return false;
	want = grow ? (size_t)n * 2 + 1024 : (size_t)n;
	if (!QC_LibCharge (vm, want - f->alloc))
		return false;
	if (!(grown = realloc (f->data, want)))
	{
		QC_LibRelease (vm, want - f->alloc);
		return false;
	}
	memset (grown + f->alloc, 0, want - f->alloc);
	f->data = grown;
	f->alloc = want;
	return true;
}

// a file to read, by the sandboxed path then the fallback
static void *QC_OpenRead (qcvm_t *vm, const char *path, const char *fallback, uint64_t *size)
{
	void	*file;

	if (!vm->host.file_open || !vm->host.file_read || !vm->host.file_close)
		return NULL;
	file = vm->host.file_open (vm->ctx, path, size);
	if (!file && fallback)
		file = vm->host.file_open (vm->ctx, fallback, size);
	return file;
}

// the whole of a file to read in memory; false if it is past the budget, or
// was shorter than it said
static bool QC_ReadWhole (qcvm_t *vm, qc_file_t *f, void *file, uint64_t size)
{
	if (!QC_Reserve (vm, f, size, false))
		return false;
	f->len = vm->host.file_read (vm->ctx, file, 0, f->data, (size_t)size);
	return f->len == size;
}

// the file gone: a written one written, its memory given back
static void QC_CloseFile (qcvm_t *vm, qc_file_t *f, bool warn)
{
	uint8_t	*bytes;
	bool	ok = true;

	switch (f->mode)
	{
	case QC_FILE_READ:
		if (f->file && vm->host.file_close)
			vm->host.file_close (vm->ctx, f->file);
		break;
	case QC_FILE_APPEND:
	case QC_FILE_WRITE:
		ok = vm->host.file_write && vm->host.file_write (vm->ctx, f->path, f->data ? f->data : (const uint8_t *)"",
			(size_t)f->len);
		break;
	case QC_FILE_MMAP_RW:
		// the block's bytes up to the size fsize may have cut it to
		if ((bytes = malloc (f->len ? (size_t)f->len : 1)))
		{
			ok = QC_ReadBytes (&vm->mem, f->ptr, bytes, (uint32_t)f->len) && vm->host.file_write
				&& vm->host.file_write (vm->ctx, f->path, bytes, (size_t)f->len);
			free (bytes);
		}
		else
			ok = false;
		break;
	}
	if (!ok && warn)
		QC_Warning (vm, "fclose: couldn't write %s", f->path);
	if (f->ptr)
		QC_LibHeapFree (vm, f->ptr);
	QC_LibRelease (vm, f->alloc);
	free (f->data);
	free (f->path);
	free (f->window);
	*f = (qc_file_t){0};
}

void QC_LibCloseFiles (qcvm_t *vm)
{
	qc_files_t	*files = vm->std ? vm->std->files : NULL;
	uint32_t	i;

	if (!files)
		return;
	for (i = 0 ; i < files->count ; i++)
		if (files->slots[i].open)
			QC_CloseFile (vm, &files->slots[i], false);
	free (files->slots);
	free (files);
	vm->std->files = NULL;
}

/*
==============================================================================

OPENING AND CLOSING

==============================================================================
*/

// filestream fopen(string name, float mode, optional float mmapminsize): a
// handle (1000 and up), or -1
static bool QC_Fopen (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0), *fallback;
	int			mode = QC_Argc (vm) > 1 ? QC_LibArgInt (vm, 1) : -1;
	int32_t		minsize = QC_Argc (vm) > 2 ? QC_LibArgInt (vm, 2) : 0;
	char		path[1024], text[1100];
	qc_file_t	*f;
	uint32_t	index;
	uint64_t	size = 0;
	void		*file;

	snprintf (text, sizeof(text), "qcfopen(\"%s\", %d) called\n", name, mode);
	if (vm->config.developer && vm->host.dprint)
		vm->host.dprint (vm->ctx, text);
	QC_ReturnFloat (vm, -1);
	if (mode < 0 || mode > QC_FILE_MMAP_RW)
		return true;		// FTE's network streams, and nonsense
	if (!QC_FixName (name, path, sizeof(path), &fallback))
	{
		QC_Warning (vm, "fopen(\"%s\"): access denied", name);
		return true;
	}
	if (!(f = QC_NewFile (vm, &index)))
	{
		QC_Warning (vm, "fopen(\"%s\"): too many files open", name);
		return true;
	}
	if (!(f->path = QC_LibDup (path)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	f->mode = mode;

	switch (mode)
	{
	case QC_FILE_READ:
		if (!(f->file = QC_OpenRead (vm, path, fallback, &f->len)))
			goto fail;
		break;
	case QC_FILE_READNL:
		if (!(file = QC_OpenRead (vm, path, fallback, &size)))
			goto fail;
		if (!QC_ReadWhole (vm, f, file, size))
		{
			vm->host.file_close (vm->ctx, file);
			goto fail;
		}
		vm->host.file_close (vm->ctx, file);
		break;
	case QC_FILE_APPEND:
		// what the data/ file has, the writing after it; else a new file
		if ((file = QC_OpenRead (vm, path, NULL, &size)))
		{
			bool	whole = QC_ReadWhole (vm, f, file, size);

			vm->host.file_close (vm->ctx, file);
			if (!whole)
				goto fail;
			f->ofs = f->len;
		}
		break;
	case QC_FILE_WRITE:
	case QC_FILE_INVALID:
		break;
	case QC_FILE_MMAP_READ:
	case QC_FILE_MMAP_RW:
		file = QC_OpenRead (vm, path, fallback, &size);
		if (!file && mode == QC_FILE_MMAP_READ)
			goto fail;
		if (minsize > 0 && (uint64_t)minsize > size)
			size = (uint64_t)minsize;
		if (size >= UINT32_MAX || !(f->ptr = QC_LibHeapAlloc (vm, size ? (uint32_t)size : 1)))
		{
			if (file)
				vm->host.file_close (vm->ctx, file);
			goto fail;
		}
		f->ptrsize = (uint32_t)size;
		f->len = size;
		if (file)
		{
			uint8_t	*bytes = malloc (size ? (size_t)size : 1);
			size_t	got = bytes ? vm->host.file_read (vm->ctx, file, 0, bytes, (size_t)size) : 0;

			vm->host.file_close (vm->ctx, file);
			if (!bytes || QC_WriteBytes (&vm->mem, f->ptr, bytes, (uint32_t)got, NULL) != QC_WRITE_OK)
			{
				free (bytes);
				goto fail;
			}
			free (bytes);
		}
		break;
	}
	f->open = true;
	QC_ReturnFloat (vm, (float)(index + QC_FIRST_FILE));
	return true;

fail:
	f->mode = QC_FILE_INVALID;		// nothing to write
	QC_CloseFile (vm, f, false);
	return true;
}

// void fclose(filestream)
static bool QC_Fclose (qcvm_t *vm)
{
	qc_file_t	*f = QC_FileArg (vm, "fclose");

	if (f)
		QC_CloseFile (vm, f, true);
	return true;
}

/*
==============================================================================

READING AND WRITING

==============================================================================
*/

// the byte at the read position of a file to read (through the window), -1
// at the end
static int QC_ReadByte (qcvm_t *vm, qc_file_t *f)
{
	if (f->ofs >= f->len)
		return -1;
	if (f->ofs < f->wstart || f->ofs >= f->wstart + f->wlen)
	{
		if (!f->window && !(f->window = malloc (QC_WINDOW)))
			return -1;
		f->wstart = f->ofs;
		f->wlen = vm->host.file_read (vm->ctx, f->file, f->ofs, f->window, QC_WINDOW);
		if (!f->wlen)
			return -1;
	}
	return f->window[f->ofs++ - f->wstart];
}

// string fgets(filestream): the next line without its \n and \rs, NULs as
// C0 80; null at the end. READNL's whole file, once; mmap's pointer.
static bool QC_Fgets (qcvm_t *vm)
{
	qc_file_t	*f = QC_FileArg (vm, "fgets");
	qc_sink_t	s;
	int			c;

	QC_ReturnWord (vm, 0);
	if (!f)
		return true;
	switch (f->mode)
	{
	case QC_FILE_MMAP_READ:
	case QC_FILE_MMAP_RW:
		QC_ReturnWord (vm, f->ptr);
		return true;
	case QC_FILE_READNL:
		// FTE returns the file again at every call; once, then the end
		if (f->ofs >= f->len)
			return true;
		f->ofs = f->len;
		return QC_ReturnString (vm, (const char *)f->data, strnlen ((const char *)f->data, (size_t)f->len));
	case QC_FILE_READ:
		break;
	default:
		return true;
	}
	if (f->ofs >= f->len)
		return true;
	QC_SinkInit (&s, QC_LINE_MAX);
	while (QC_SinkRoom (&s))
	{
		if ((c = QC_ReadByte (vm, f)) < 0 || c == '\n')
			break;
		if (c == '\r')
			continue;
		if (c)
			QC_SinkPush (&s, (char)c);
		else if (QC_SinkRoom (&s) >= 2)
			QC_SinkAppend (&s, "\xC0\x80", 2);
		else
		{
			f->ofs--;			// for the next line
			break;
		}
	}
	return QC_LibReturnSink (vm, &s);
}

// the bytes written at the position of a file to write: as many as fit (an
// mmap's block doesn't grow), 0 for a file to read
static uint64_t QC_WriteAt (qcvm_t *vm, qc_file_t *f, const uint8_t *bytes, uint64_t n)
{
	if (f->ofs + n < f->ofs)
		return 0;
	switch (f->mode)
	{
	case QC_FILE_APPEND:
	case QC_FILE_WRITE:
		if (!QC_Reserve (vm, f, f->ofs + n, true))
			return 0;
		memcpy (f->data + f->ofs, bytes, (size_t)n);
		break;
	case QC_FILE_MMAP_RW:
		if (f->ofs >= f->ptrsize)
			return 0;
		if (n > f->ptrsize - f->ofs)
			n = f->ptrsize - f->ofs;
		if (QC_WriteBytes (&vm->mem, f->ptr + (uint32_t)f->ofs, bytes, (uint32_t)n, NULL) != QC_WRITE_OK)
			return 0;
		break;
	default:
		return 0;
	}
	f->ofs += n;
	if (f->len < f->ofs)
		f->len = f->ofs;
	return n;
}

// void fputs(filestream, string...): the strings, joined
static bool QC_Fputs (qcvm_t *vm)
{
	qc_file_t	*f = QC_FileArg (vm, "fputs");
	size_t		len;
	char		*text;

	if (!f)
		return true;
	if (!(text = QC_LibConcat (vm, 1, &len)))
		return false;
	QC_WriteAt (vm, f, (const uint8_t *)text, len);
	free (text);
	return true;
}

// int fwrite(filestream, void *ptr, int size, optional int srcofs): the bytes written
static bool QC_Fwrite (qcvm_t *vm)
{
	qc_file_t	*f;
	int32_t		size = QC_ArgInt (vm, 2), ofs = QC_Argc (vm) > 3 ? QC_ArgInt (vm, 3) : 0;
	uint8_t		*bytes;

	QC_ReturnInt (vm, 0);
	if (size < 0 || !(bytes = QC_LibReadRange (vm, QC_ArgWord (vm, 1), ofs, (size_t)size)))
		return QC_LibSoftError (vm, "fwrite: invalid ptr / size");
	if ((f = QC_FileArg (vm, "fwrite")))
		QC_ReturnInt (vm, (int32_t)QC_WriteAt (vm, f, bytes, (uint64_t)size));
	free (bytes);
	return true;
}

// int fread(filestream, void *ptr, int size, optional int dstofs): the bytes read
static bool QC_Fread (qcvm_t *vm)
{
	qc_file_t	*f;
	int32_t		size = QC_ArgInt (vm, 2), ofs = QC_Argc (vm) > 3 ? QC_ArgInt (vm, 3) : 0;
	qc_dest_t	d;
	uint8_t		*bytes;
	uint64_t	n;

	QC_ReturnInt (vm, 0);
	if (size < 0 || !QC_LibDest (vm, QC_ArgWord (vm, 1), ofs, (size_t)size, &d))
		return QC_LibSoftError (vm, "fread: invalid ptr / size");
	if (!(f = QC_FileArg (vm, "fread")))
		return true;
	if (f->mode != QC_FILE_READ)
	{
		QC_Warning (vm, "fread: file not opened for reading");
		return true;
	}
	n = f->ofs < f->len ? f->len - f->ofs : 0;
	if (n > (uint64_t)size)
		n = (uint64_t)size;
	if (!n)
		return true;
	if (!(bytes = malloc ((size_t)n)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HEAP, NULL);
	n = vm->host.file_read (vm->ctx, f->file, f->ofs, bytes, (size_t)n);
	if (!QC_LibWriteDest (vm, &d, bytes, (size_t)n))
	{
		free (bytes);
		return QC_LibSoftError (vm, "fread: invalid ptr / size");
	}
	free (bytes);
	f->ofs += n;
	QC_ReturnInt (vm, (int32_t)n);
	return true;
}

/*
==============================================================================

POSITIONS AND SIZES

==============================================================================
*/

// the position before, and the new one if one was passed (not negative)
static int64_t QC_Seek (qcvm_t *vm, bool wide)
{
	qc_file_t	*f = QC_FileArg (vm, "fseek");
	int64_t		was, to;

	if (!f)
		return -1;
	was = (int64_t)f->ofs;
	if (QC_Argc (vm) > 1)
	{
		to = wide ? (int64_t)QC_LibArg64 (vm, 1) : QC_ArgInt (vm, 1);
		if (to >= 0)
			f->ofs = (uint64_t)to;
	}
	return was;
}

// the size before, and a file to write resized to one passed (not negative)
static int64_t QC_Size (qcvm_t *vm, bool wide)
{
	qc_file_t	*f = QC_FileArg (vm, "fsize");
	int64_t		was, to;

	if (!f)
		return -1;
	was = (int64_t)f->len;
	if (QC_Argc (vm) < 2 || (to = wide ? (int64_t)QC_LibArg64 (vm, 1) : QC_ArgInt (vm, 1)) < 0)
		return was;
	switch (f->mode)
	{
	case QC_FILE_APPEND:
	case QC_FILE_WRITE:
		if (QC_Reserve (vm, f, (uint64_t)to, true))
		{
			if ((uint64_t)to < f->len)
				memset (f->data + to, 0, (size_t)(f->len - (uint64_t)to));
			f->len = (uint64_t)to;
		}
		break;
	case QC_FILE_MMAP_RW:
		f->len = (uint64_t)to < f->ptrsize ? (uint64_t)to : f->ptrsize;
		break;
	default:
		QC_Warning (vm, "fsize: truncation/extension is not supported for files opened to read");
		break;
	}
	return was;
}

// int fseek(filestream, optional int newpos) and __int64 fseek64(filestream,
// optional __int64 newpos): the position before
static bool QC_Fseek (qcvm_t *vm)
{
	QC_ReturnInt (vm, (int32_t)QC_Seek (vm, false));
	return true;
}

static bool QC_Fseek64 (qcvm_t *vm)
{
	QC_LibReturn64 (vm, (uint64_t)QC_Seek (vm, true));
	return true;
}

// int fsize(filestream, optional int newsize) and the __int64 fsize64: the size
// before
static bool QC_Fsize (qcvm_t *vm)
{
	QC_ReturnInt (vm, (int32_t)QC_Size (vm, false));
	return true;
}

static bool QC_Fsize64 (qcvm_t *vm)
{
	QC_LibReturn64 (vm, (uint64_t)QC_Size (vm, true));
	return true;
}

/*
==============================================================================

NAMES

==============================================================================
*/

// float fremove(string name): 0, -1 for a name refused, -5 if it couldn't be
static bool QC_Fremove (qcvm_t *vm)
{
	const char	*fallback;
	char		path[1024];

	if (!QC_FixName (QC_ArgString (vm, 0), path, sizeof(path), &fallback))
		QC_ReturnFloat (vm, -1);
	else
		QC_ReturnFloat (vm, vm->host.file_remove && vm->host.file_remove (vm->ctx, path) ? 0.0f : -5.0f);
	return true;
}

// float frename(string from, string to): 0, -1 for a name refused, -5 if it couldn't be
static bool QC_Frename (qcvm_t *vm)
{
	const char	*fallback;
	char		from[1024], to[1024];

	if (!QC_FixName (QC_ArgString (vm, 0), from, sizeof(from), &fallback)
		|| !QC_FixName (QC_ArgString (vm, 1), to, sizeof(to), &fallback))
		QC_ReturnFloat (vm, -1);
	else
		QC_ReturnFloat (vm, vm->host.file_rename && vm->host.file_rename (vm->ctx, from, to) ? 0.0f : -5.0f);
	return true;
}

// string whichpack(string name, optional float flags): the pack the search path
// finds the file in, "" for a file of its own, null if there is none
static bool QC_Whichpack (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0);
	char		pack[1024];

	if (!QC_NameAllowed (name) || !vm->host.file_pack || !vm->host.file_pack (vm->ctx, name, pack, sizeof(pack)))
		return QC_LibReturnOptString (vm, NULL);
	return QC_LibReturnOptString (vm, pack);
}

static const qc_libentry_t	qc_file[] = {
	{"fopen", QC_Fopen, NULL, 0},
	{"fclose", QC_Fclose, NULL, 0},
	{"fgets", QC_Fgets, NULL, 0},
	{"fputs", QC_Fputs, NULL, 0},
	{"fread", QC_Fread, NULL, 0},
	{"fwrite", QC_Fwrite, NULL, 0},
	{"fseek", QC_Fseek, NULL, 0},
	{"fseek64", QC_Fseek64, NULL, 0},
	{"fsize", QC_Fsize, NULL, 0},
	{"fsize64", QC_Fsize64, NULL, 0},
	{"fremove", QC_Fremove, NULL, 0},
	{"frename", QC_Frename, NULL, 0},
	{"whichpack", QC_Whichpack, NULL, 0},
};

bool QC_RegisterFile (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_file, sizeof(qc_file) / sizeof(qc_file[0]));
}
