// vfs_http.c -- a download as a file that grows as it comes (net_http.h's
// VFS_OpenURL): the GET's body kept whole, read and sought in as far as it has
// come. An answer that isn't a 2xx fails it, and its body is no part of it.

#include "net_http.h"
#include "mem.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define URL_FIRSTROOM	(256 * 1024)

typedef struct
{
	vfsfile_t	vfs;
	httpfetch_t	*fetch;			// NULL once it ended, or failed
	byte		*data;			// the body so far
	size_t		size, room, pos;
	bool		failed;
	char		error[256];
} vfshttp_t;

static void Url_Fail (vfshttp_t *h, const char *why)
{
	snprintf (h->error, sizeof(h->error), "%s", why);
	h->failed = true;
	HTTP_Close (h->fetch);
	h->fetch = NULL;
}

// what has come since, onto the end
static void Url_Pump (vfshttp_t *h)
{
	char	why[64];
	int		status, n;

	if (!h->fetch)
		return;
	status = HTTP_Status (h->fetch);
	if (status && (status < 200 || status >= 300))
	{
		snprintf (why, sizeof(why), "the server answered %i", status);
		Url_Fail (h, why);
		return;
	}
	for (;;)
	{
		if (h->size == h->room)
		{
			h->room = h->room ? h->room * 2 : URL_FIRSTROOM;
			h->data = Mem_Realloc (h->data, h->room);
		}
		n = HTTP_Read (h->fetch, h->data + h->size, h->room - h->size > INT_MAX ? INT_MAX : (int)(h->room - h->size));
		if (n <= 0)
			break;
		h->size += (size_t)n;
	}
	switch (HTTP_State (h->fetch))
	{
	case HTTP_DONE:
		HTTP_Close (h->fetch);
		h->fetch = NULL;
		break;
	case HTTP_FAILED:
		Url_Fail (h, HTTP_Error (h->fetch));
		break;
	default:
		break;
	}
}

static int Url_Read (vfsfile_t *f, void *buf, int size)
{
	vfshttp_t	*h = (vfshttp_t *)f;
	size_t		n;

	if (h->pos == h->size)
		Url_Pump (h);
	n = h->size - h->pos;
	if (n > (size_t)size)
		n = (size_t)size;
	memcpy (buf, h->data + h->pos, n);
	h->pos += n;
	return (int)n;
}

static bool Url_Seek (vfsfile_t *f, long long pos)
{
	vfshttp_t	*h = (vfshttp_t *)f;

	Url_Pump (h);
	if ((unsigned long long)pos > h->size)
		return false;
	h->pos = (size_t)pos;
	return true;
}

static long long Url_Tell (vfsfile_t *f)
{
	return (long long)((vfshttp_t *)f)->pos;
}

static long long Url_Length (vfsfile_t *f)
{
	vfshttp_t	*h = (vfshttp_t *)f;

	Url_Pump (h);
	if (h->failed)
		return -1;
	return h->fetch ? HTTP_Length (h->fetch) : (long long)h->size;
}

static vfsstatus_t Url_Status (vfsfile_t *f)
{
	vfshttp_t	*h = (vfshttp_t *)f;

	Url_Pump (h);
	return h->failed ? VFS_FAILED : h->fetch ? VFS_LIVE : VFS_ENDED;
}

static const char *Url_Error (vfsfile_t *f)
{
	vfshttp_t	*h = (vfshttp_t *)f;

	return h->failed ? h->error : "";
}

static void Url_Close (vfsfile_t *f)
{
	vfshttp_t	*h = (vfshttp_t *)f;

	HTTP_Close (h->fetch);
	Mem_Free (h->data);
	Mem_Free (h);
}

vfsfile_t *VFS_OpenURL (const char *url)
{
	vfshttp_t	*h = Mem_Calloc (1, sizeof(*h));

	h->vfs = (vfsfile_t){Url_Read, Url_Seek, Url_Tell, Url_Length, Url_Status, Url_Error, Url_Close};
	h->fetch = HTTP_Start (url, NULL);
	return &h->vfs;
}
