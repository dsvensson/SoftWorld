// net_http_async.c -- GETs that run on their own while the caller polls
// (net_http.h's HTTP_Start), the native programs': a thread each runs the GET
// that blocks (net_http.c), keeping the answer's head, and the body as it
// comes for the caller to read; a page's are fetch()'s (net_http_web.c). The
// thread uses malloc and atomics only, and wakes the main thread's wait as the
// body comes. A GET closed before it is done is left to end on its own (a read
// that waits ends at its timeout at the latest), and frees itself.

#include "net_http.h"
#include "q_string.h"
#include "sys.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_ASYNCTIMEOUT	20		// seconds without a byte, as map downloads wait

enum { FETCH_RUNNING, FETCH_DONE, FETCH_ABANDONED };

struct httpfetch_s
{
	char			url[HTTP_MAXURL];
	char			*headers;		// the request lines, each ending \r\n, or NULL
	systhread_t		*thread;
	atomic_int		state;			// FETCH_*: the thread frees an abandoned one
	atomic_bool		cancel;
	atomic_int		status;			// the answer's, 0 before it came
	atomic_flag		lock;			// the body and the head
	byte			*data;			// the body not yet read
	size_t			size, room, readpos;
	char			*head;			// the answer's head, malloc's
	// the thread's, read once it is done
	bool			ok;
	char			error[256];
	// the caller's
	bool			joined;			// the thread is gone (or never was)
};

static void HTTP_Lock (httpfetch_t *f)
{
	while (atomic_flag_test_and_set_explicit (&f->lock, memory_order_acquire))
		;
}

static void HTTP_Unlock (httpfetch_t *f)
{
	atomic_flag_clear_explicit (&f->lock, memory_order_release);
}

static void HTTP_Free (httpfetch_t *f)
{
	free (f->headers);
	free (f->data);
	free (f->head);
	free (f);
}

/*
==============================================================================

THE THREAD

==============================================================================
*/

static void HTTP_AsyncHead (void *ctx, int status, const char *head)
{
	httpfetch_t	*f = ctx;
	size_t		length = strlen (head);
	char		*copy = malloc (length + 1);

	if (copy)
		memcpy (copy, head, length + 1);
	HTTP_Lock (f);
	free (f->head);
	f->head = copy;
	HTTP_Unlock (f);
	atomic_store (&f->status, status);
}

static bool HTTP_AsyncBody (void *ctx, const void *data, size_t length, long long total)
{
	httpfetch_t	*f = ctx;
	byte		*grown;
	size_t		room;
	bool		ok = true;

	(void)total;
	if (atomic_load (&f->cancel))
		return false;
	HTTP_Lock (f);
	if (f->readpos && f->size + length > f->room)
	{	// what was read goes first
		memmove (f->data, f->data + f->readpos, f->size - f->readpos);
		f->size -= f->readpos;
		f->readpos = 0;
	}
	if (f->size + length > f->room)
	{
		for (room = f->room ? f->room * 2 : 65536 ; room < f->size + length ; room *= 2)
			;
		if ((grown = realloc (f->data, room)))
		{
			f->data = grown;
			f->room = room;
		}
		else
			ok = false;
	}
	if (ok)
	{
		memcpy (f->data + f->size, data, length);
		f->size += length;
	}
	HTTP_Unlock (f);
	if (!ok)
		snprintf (f->error, sizeof(f->error), "out of memory for the answer");
	Sys_Wake ();
	return ok;
}

static void HTTP_AsyncThread (void *arg)
{
	httpfetch_t	*f = arg;

	f->ok = HTTP_Request (f->url, f->headers, HTTP_ASYNCTIMEOUT, HTTP_AsyncHead, HTTP_AsyncBody, f, f->error,
		sizeof(f->error));
	if (!f->ok && !f->error[0])
		snprintf (f->error, sizeof(f->error), "stopped");
	if (atomic_exchange (&f->state, FETCH_DONE) == FETCH_ABANDONED)
		HTTP_Free (f);		// nothing waits for it
	else
		Sys_Wake ();
}

/*
==============================================================================

THE CALLER'S

==============================================================================
*/

// a header's name: letters, digits, - and _
static bool HTTP_NameChar (int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// the request lines: each "Name: value" of text, apart by \n or \r\n, as
// lines ending \r\n (malloc's); NULL with why in error where one isn't a header
static char *HTTP_Headers (const char *text, char *error, size_t errorsize)
{
	char		*out = malloc (strlen (text) * 2 + 1), *o = out;
	const char	*line, *end, *c;
	size_t		n;

	if (!out)
	{
		snprintf (error, errorsize, "out of memory");
		return NULL;
	}
	for (line = text ; *line ; line = *end ? end + 1 : end)
	{
		end = line + strcspn (line, "\n");
		n = (size_t)(end - line);
		if (n && line[n - 1] == '\r')
			n--;
		if (!n)
			continue;
		// a name, a colon, and a value without control characters
		for (c = line ; c < line + n && HTTP_NameChar (*c) ; c++)
			;
		if (c == line || c == line + n || *c != ':')
		{
			snprintf (error, errorsize, "\"%.*s\" isn't a header", (int)(n > 64 ? 64 : n), line);
			free (out);
			return NULL;
		}
		for ( ; c < line + n ; c++)
			if ((unsigned char)*c < ' ' || *c == 127)
			{
				snprintf (error, errorsize, "a header has a control character in it");
				free (out);
				return NULL;
			}
		memcpy (o, line, n);
		o += n;
		*o++ = '\r';
		*o++ = '\n';
	}
	*o = 0;
	return out;
}

httpfetch_t *HTTP_Start (const char *url, const char *headers)
{
	httpfetch_t	*f = calloc (1, sizeof(*f));

	if (!f)
		Sys_Error ("HTTP_Start: out of memory");
	atomic_init (&f->state, FETCH_DONE);
	atomic_init (&f->cancel, false);
	atomic_init (&f->status, 0);
	atomic_flag_clear (&f->lock);
	f->joined = true;
	if (strlen (url) >= sizeof(f->url))
	{
		snprintf (f->error, sizeof(f->error), "the URL is longer than %i characters", HTTP_MAXURL - 1);
		return f;
	}
	Q_strncpyz (f->url, url, sizeof(f->url));
	if (headers && !(f->headers = HTTP_Headers (headers, f->error, sizeof(f->error))))
		return f;
	atomic_store (&f->state, FETCH_RUNNING);
	if (!(f->thread = Sys_StartThread ("http", HTTP_AsyncThread, f)))
	{
		atomic_store (&f->state, FETCH_DONE);
		snprintf (f->error, sizeof(f->error), "no thread for it");
		return f;
	}
	f->joined = false;
	return f;
}

int HTTP_Read (httpfetch_t *f, void *buf, int size)
{
	size_t	n;

	if (size <= 0)
		return 0;
	HTTP_Lock (f);
	n = f->size - f->readpos;
	if (n > (size_t)size)
		n = (size_t)size;
	if (n)
		memcpy (buf, f->data + f->readpos, n);
	f->readpos += n;
	if (f->readpos == f->size)
		f->readpos = f->size = 0;
	HTTP_Unlock (f);
	return (int)n;
}

// the thread's end seen, and the thread gone; false while it runs
static bool HTTP_Ended (httpfetch_t *f)
{
	if (atomic_load (&f->state) != FETCH_DONE)
		return false;
	if (!f->joined)
	{
		Sys_JoinThread (f->thread);
		f->thread = NULL;
		f->joined = true;
	}
	return true;
}

httpstate_t HTTP_State (httpfetch_t *f)
{
	bool	unread;

	if (!HTTP_Ended (f))
		return HTTP_RUNNING;
	if (!f->ok)
		return HTTP_FAILED;
	HTTP_Lock (f);
	unread = f->size > f->readpos;
	HTTP_Unlock (f);
	return unread ? HTTP_RUNNING : HTTP_DONE;
}

int HTTP_Status (httpfetch_t *f)
{
	return atomic_load (&f->status);
}

bool HTTP_ResponseHeader (httpfetch_t *f, const char *name, char *value, size_t size)
{
	bool	found;

	HTTP_Lock (f);
	found = f->head && HTTP_HeaderValue (f->head, name, value, size);
	HTTP_Unlock (f);
	return found;
}

long long HTTP_Length (httpfetch_t *f)
{
	char	value[32];

	return HTTP_ResponseHeader (f, "Content-Length", value, sizeof(value)) ? atoll (value) : -1;
}

const char *HTTP_Error (httpfetch_t *f)
{
	return HTTP_Ended (f) && !f->ok ? f->error : "";
}

void HTTP_Close (httpfetch_t *f)
{
	if (!f)
		return;
	atomic_store (&f->cancel, true);
	if (f->joined)
	{
		HTTP_Free (f);
		return;
	}
	if (atomic_exchange (&f->state, FETCH_ABANDONED) == FETCH_DONE)
	{
		Sys_JoinThread (f->thread);
		HTTP_Free (f);
	}
	else
		Sys_DetachThread (f->thread);		// it frees it when its GET ends
}
