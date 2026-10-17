// vfs.h -- a file read through its backend's functions: one on disk or in a
// pak (FS_OpenVFS), one in memory, a gzip unpacked as it is read, a download
// (net_http.h's VFS_OpenURL). A download has only what has come so far: a read
// gives what there is, and the status tells waiting for more from the end.
// A file is one thread's at a time.

#pragma once

#include "q_types.h"

#include <stdio.h>

typedef enum
{
	VFS_LIVE,		// more may come
	VFS_ENDED,		// all of it is there
	VFS_FAILED		// it stopped short: VFS_Error says why
} vfsstatus_t;

typedef struct vfsfile_s vfsfile_t;

// a backend's file begins with one
struct vfsfile_s
{
	int			(*read) (vfsfile_t *f, void *buf, int size);
	bool		(*seek) (vfsfile_t *f, long long pos);
	long long	(*tell) (vfsfile_t *f);
	long long	(*length) (vfsfile_t *f);
	vfsstatus_t	(*status) (vfsfile_t *f);
	const char	*(*error) (vfsfile_t *f);
	void		(*close) (vfsfile_t *f);
};

// up to size bytes, those there are now: 0 while none have come yet (LIVE) and
// at the end (ENDED)
int			VFS_Read (vfsfile_t *f, void *buf, int size);
// to pos within what is there; false past it
bool		VFS_Seek (vfsfile_t *f, long long pos);
long long	VFS_Tell (vfsfile_t *f);
long long	VFS_Length (vfsfile_t *f);		// -1 while it isn't known
vfsstatus_t	VFS_Status (vfsfile_t *f);
const char	*VFS_Error (vfsfile_t *f);		// "" but for a failed file
void		VFS_Close (vfsfile_t *f);		// NULL is none

// the rest of a file that has ended, from where it is, from Mem_Alloc with a
// 0 after it (not counted in size); NULL for one still coming, or failed
byte		*VFS_ReadAll (vfsfile_t *f, size_t *size);

// length bytes of the file from offset (-1 for those to its end), the file
// the VFS's to close: a file on disk, or a pak's entry
vfsfile_t	*VFS_OpenStdio (FILE *file, long long offset, long long length);
// size bytes of memory; owned, they are Mem_Free'd when it closes
vfsfile_t	*VFS_OpenMemory (byte *data, size_t size, bool owned);
// inner unpacked as it is read where it is a gzip, else inner as it is; inner
// the VFS's to close. What is unpacked is kept, to seek back in.
vfsfile_t	*VFS_OpenGunzip (vfsfile_t *inner);
