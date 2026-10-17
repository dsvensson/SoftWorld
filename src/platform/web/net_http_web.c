// net_http_web.c -- a page's GETs that run on their own (net_http.h's
// HTTP_Start): the browser's fetch() (net_http_web.js), polled as the native
// programs' threads are. A page has no GETs that block (net_http_none.c).

#include "mem.h"
#include "net_http.h"
#include "q_types.h"

#include <stdlib.h>

// net_http_web.js
int		web_http_start (const char *url, const char *headers);
int		web_http_read (int id, byte *buf, int max);
int		web_http_state (int id);
int		web_http_status (int id);
int		web_http_header (int id, const char *name, char *out, int size);
void	web_http_error (int id, char *out, int size);
void	web_http_close (int id);

struct httpfetch_s
{
	int		id;
	char	error[256];
};

httpfetch_t *HTTP_Start (const char *url, const char *headers)
{
	httpfetch_t	*f = Mem_Calloc (1, sizeof(*f));

	f->id = web_http_start (url, headers);
	return f;
}

int HTTP_Read (httpfetch_t *f, void *buf, int size)
{
	return size > 0 ? web_http_read (f->id, buf, size) : 0;
}

httpstate_t HTTP_State (httpfetch_t *f)
{
	switch (web_http_state (f->id))
	{
	case 1:		return HTTP_DONE;
	case 2:		return HTTP_FAILED;
	default:	return HTTP_RUNNING;
	}
}

int HTTP_Status (httpfetch_t *f)
{
	return web_http_status (f->id);
}

bool HTTP_ResponseHeader (httpfetch_t *f, const char *name, char *value, size_t size)
{
	return web_http_header (f->id, name, value, (int)size) >= 0;
}

long long HTTP_Length (httpfetch_t *f)
{
	char	value[32];

	return HTTP_ResponseHeader (f, "Content-Length", value, sizeof(value)) ? atoll (value) : -1;
}

const char *HTTP_Error (httpfetch_t *f)
{
	if (HTTP_State (f) != HTTP_FAILED)
		return "";
	web_http_error (f->id, f->error, (int)sizeof(f->error));
	return f->error;
}

void HTTP_Close (httpfetch_t *f)
{
	if (!f)
		return;
	web_http_close (f->id);
	Mem_Free (f);
}
