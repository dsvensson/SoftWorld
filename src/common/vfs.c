// vfs.c -- files read through their backends' functions (vfs.h): the calls,
// and the files on disk and in memory

#include "vfs.h"
#include "mem.h"

#include <limits.h>
#include <string.h>

/*
==============================================================================

THE CALLS

==============================================================================
*/

int VFS_Read (vfsfile_t *f, void *buf, int size)
{
	return size > 0 ? f->read (f, buf, size) : 0;
}

bool VFS_Seek (vfsfile_t *f, long long pos)
{
	return pos >= 0 && f->seek (f, pos);
}

long long VFS_Tell (vfsfile_t *f)
{
	return f->tell (f);
}

long long VFS_Length (vfsfile_t *f)
{
	return f->length (f);
}

vfsstatus_t VFS_Status (vfsfile_t *f)
{
	return f->status (f);
}

const char *VFS_Error (vfsfile_t *f)
{
	return f->error (f);
}

void VFS_Close (vfsfile_t *f)
{
	if (f)
		f->close (f);
}

byte *VFS_ReadAll (vfsfile_t *f, size_t *size)
{
	long long	length, at;
	byte		*data, more[4096];
	size_t		have = 0, room;
	int			n;

	if (VFS_Status (f) != VFS_ENDED)
		return NULL;
	length = VFS_Length (f);
	at = VFS_Tell (f);
	room = length > at ? (size_t)(length - at) : 0;
	data = Mem_Alloc (room + 1);
	for (;;)
	{
		if (have < room)
		{
			n = VFS_Read (f, data + have, room - have > INT_MAX ? INT_MAX : (int)(room - have));
			if (n <= 0)
				break;
			have += (size_t)n;
			continue;
		}
		// full: the length was short, or not known
		n = VFS_Read (f, more, sizeof(more));
		if (n <= 0)
			break;
		room = (room ? room * 2 : sizeof(more)) + (size_t)n;
		data = Mem_Realloc (data, room + 1);
		memcpy (data + have, more, (size_t)n);
		have += (size_t)n;
	}
	if (VFS_Status (f) != VFS_ENDED)
	{
		Mem_Free (data);
		return NULL;
	}
	data[have] = 0;
	*size = have;
	return data;
}

static const char *VFS_NoError (vfsfile_t *f)
{
	(void)f;
	return "";
}

static vfsstatus_t VFS_Ended (vfsfile_t *f)
{
	(void)f;
	return VFS_ENDED;
}

/*
==============================================================================

ON DISK

==============================================================================
*/

typedef struct
{
	vfsfile_t	vfs;
	FILE		*file;
	long long	base, length;	// its bytes in the file
	long long	pos;			// within them
	long long	at;				// where the file is, -1 unknown
} vfsstdio_t;

static int VFS_StdioRead (vfsfile_t *f, void *buf, int size)
{
	vfsstdio_t	*s = (vfsstdio_t *)f;
	size_t		n;

	if (s->pos >= s->length)
		return 0;
	if (size > s->length - s->pos)
		size = (int)(s->length - s->pos);
	if (s->at != s->base + s->pos)
	{
		if (fseek (s->file, (long)(s->base + s->pos), SEEK_SET))
		{
			s->at = -1;
			return 0;
		}
		s->at = s->base + s->pos;
	}
	n = fread (buf, 1, (size_t)size, s->file);
	s->pos += (long long)n;
	s->at += (long long)n;
	return (int)n;
}

static bool VFS_StdioSeek (vfsfile_t *f, long long pos)
{
	vfsstdio_t	*s = (vfsstdio_t *)f;

	if (pos > s->length)
		return false;
	s->pos = pos;
	return true;
}

static long long VFS_StdioTell (vfsfile_t *f)
{
	return ((vfsstdio_t *)f)->pos;
}

static long long VFS_StdioLength (vfsfile_t *f)
{
	return ((vfsstdio_t *)f)->length;
}

static void VFS_StdioClose (vfsfile_t *f)
{
	vfsstdio_t	*s = (vfsstdio_t *)f;

	fclose (s->file);
	Mem_Free (s);
}

vfsfile_t *VFS_OpenStdio (FILE *file, long long offset, long long length)
{
	vfsstdio_t	*s = Mem_Calloc (1, sizeof(*s));
	long long	end;

	s->vfs = (vfsfile_t){VFS_StdioRead, VFS_StdioSeek, VFS_StdioTell, VFS_StdioLength, VFS_Ended, VFS_NoError,
		VFS_StdioClose};
	s->file = file;
	s->base = offset;
	if (length < 0)
	{
		end = !fseek (file, 0, SEEK_END) ? ftell (file) : -1;
		length = end > offset ? end - offset : 0;
	}
	s->length = length;
	s->at = -1;
	return &s->vfs;
}

/*
==============================================================================

IN MEMORY

==============================================================================
*/

typedef struct
{
	vfsfile_t	vfs;
	byte		*data;
	size_t		size, pos;
	bool		owned;
} vfsmemory_t;

static int VFS_MemoryRead (vfsfile_t *f, void *buf, int size)
{
	vfsmemory_t	*m = (vfsmemory_t *)f;
	size_t		n = m->size - m->pos;

	if (n > (size_t)size)
		n = (size_t)size;
	memcpy (buf, m->data + m->pos, n);
	m->pos += n;
	return (int)n;
}

static bool VFS_MemorySeek (vfsfile_t *f, long long pos)
{
	vfsmemory_t	*m = (vfsmemory_t *)f;

	if ((unsigned long long)pos > m->size)
		return false;
	m->pos = (size_t)pos;
	return true;
}

static long long VFS_MemoryTell (vfsfile_t *f)
{
	return (long long)((vfsmemory_t *)f)->pos;
}

static long long VFS_MemoryLength (vfsfile_t *f)
{
	return (long long)((vfsmemory_t *)f)->size;
}

static void VFS_MemoryClose (vfsfile_t *f)
{
	vfsmemory_t	*m = (vfsmemory_t *)f;

	if (m->owned)
		Mem_Free (m->data);
	Mem_Free (m);
}

vfsfile_t *VFS_OpenMemory (byte *data, size_t size, bool owned)
{
	vfsmemory_t	*m = Mem_Calloc (1, sizeof(*m));

	m->vfs = (vfsfile_t){VFS_MemoryRead, VFS_MemorySeek, VFS_MemoryTell, VFS_MemoryLength, VFS_Ended, VFS_NoError,
		VFS_MemoryClose};
	m->data = data;
	m->size = size;
	m->owned = owned;
	return &m->vfs;
}
