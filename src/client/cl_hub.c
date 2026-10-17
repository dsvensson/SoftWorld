// cl_hub.c -- the menu's GETs (SoftWorld's webget builtins) and the hub's
// cvars: the Hub menu lists the games of hub.quakeworld.nu through its API (a
// Supabase table, read as PostgREST answers), and plays their demos from the
// URLs they have (playdemo takes a URL)
//
// A GET runs on its own (net_http.h's HTTP_Start) while the menu polls it each
// frame: its status, then its body, a header of its answer, or why it failed.
// A body is kept whole up to 1 MB. An answer that isn't a 2xx fails with its
// status, its body kept (an API says in it why).

#include "cl_local.h"
#include "cl_qc.h"
#include "menu.h"
#include "net_http.h"

#define WEB_REQUESTS	8				// GETs at once
#define WEB_MAXBODY		(1024 * 1024)	// a body's bytes

static cvar_t	hub_api_url = {.name = "hub_api_url",
	.string = "https://ncsphkjfominimxztjip.supabase.co/rest/v1/v1_games", .archive = true,
	.description = "Where the Hub menu lists hub.quakeworld.nu's games from: its API's table of them, as PostgREST "
		"answers (Supabase's). Empty for none."};
static cvar_t	hub_api_key = {.name = "hub_api_key",
	.string = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5jc3Boa2pmb21pbmlteHp0amlwIiwicm9s"
		"ZSI6ImFub24iLCJpYXQiOjE2OTY5Mzg1NjMsImV4cCI6MjAxMjUxNDU2M30.NN6hjlEW-qB4Og9hWAVlgvUdwrbBO13s8OkAJuBGVbo",
	.archive = true,
	.description = "The key the Hub menu sends its API (its apikey header): the site's own, which anyone may read."};
static cvar_t	hub_demo_url = {.name = "hub_demo_url", .string = "https://d.quake.world", .archive = true,
	.description = "Where the Hub menu plays the games' demos from: <url>/<the first three of its sha256>/<sha256>.mvd.gz."};
static cvar_t	hub_showscores = {.name = "hub_showscores", .string = "0", .archive = true,
	.description = "The Hub menu shows the games' scores; hidden, they don't tell who won.",
	.values = (const cvar_value_t[]){{"0", "Hidden"}, {"1", "Shown"}, {0}}};
static cvar_t	hub_mode = {.name = "hub_mode", .string = "", .archive = true,
	.description = "The games the Hub menu lists by mode: empty for all, else 1on1, 2on2, 4on4, wipeout or ctf."};
static cvar_t	hub_timeframe = {.name = "hub_timeframe", .string = "30", .archive = true,
	.description = "The Hub menu lists the games of the last so many days; 0 for all of them.",
	.values = (const cvar_value_t[]){{"1", "The last day"}, {"7", "The last week"}, {"30", "The last month"},
		{"91", "The last three months"}, {"365", "The last year"}, {"0", "All of them"}, {0}}};

static cvar_t	*const hub_cvars[] =
{
	&hub_api_url, &hub_api_key, &hub_demo_url, &hub_showscores, &hub_mode, &hub_timeframe,
};

typedef struct
{
	httpfetch_t	*fetch;			// NULL: no GET
	byte		*body;
	size_t		size, room;
	bool		toolong;		// its body is past WEB_MAXBODY
} webget_t;

static webget_t	webgets[WEB_REQUESTS];		// ids 1 to WEB_REQUESTS: 0 is none

// what came of its body since, kept
static void Web_Pump (webget_t *w)
{
	byte	probe;
	int		n;

	while (!w->toolong)
	{
		if (w->size == w->room)
		{
			if (w->room == WEB_MAXBODY)
			{	// full: a byte more is too long
				if (HTTP_Read (w->fetch, &probe, 1) > 0)
					w->toolong = true;
				break;
			}
			w->room = w->room ? w->room * 2 : 16384;
			if (w->room > WEB_MAXBODY)
				w->room = WEB_MAXBODY;
			w->body = Mem_Realloc (w->body, w->room);
		}
		n = HTTP_Read (w->fetch, w->body + w->size, (int)(w->room - w->size));
		if (n <= 0)
			break;
		w->size += (size_t)n;
	}
}

// the GET an id names, what it brought since kept; NULL for none
static webget_t *Web_Get (qcvm_t *vm)
{
	int		i = QC_DoubleToInt (QC_ArgFloat (vm, 0)) - 1;

	if (i < 0 || i >= WEB_REQUESTS || !webgets[i].fetch)
		return NULL;
	Web_Pump (&webgets[i]);
	return &webgets[i];
}

static void Web_Close (webget_t *w)
{
	HTTP_Close (w->fetch);
	Mem_Free (w->body);
	memset (w, 0, sizeof(*w));
}

/*
==============================================================================

THE BUILTINS

==============================================================================
*/

// float webget(string url, optional string headers): a GET started, with
// request lines more ("Name: value" each, apart by newlines); its id, 0 when
// as many as can be are running
static bool Web_Start (qcvm_t *vm)
{
	const char	*headers = QC_Argc (vm) > 1 ? QC_ArgString (vm, 1) : NULL;
	int			i;

	for (i = 0 ; i < WEB_REQUESTS && webgets[i].fetch ; i++)
		;
	if (i == WEB_REQUESTS)
	{
		QC_ReturnFloat (vm, 0);
		return true;
	}
	webgets[i].fetch = HTTP_Start (QC_ArgString (vm, 0), headers && *headers ? headers : NULL);
	QC_ReturnFloat (vm, (float)(i + 1));
	return true;
}

// float webget_status(float id): 0 while it runs, else the answer's status
// (200, 206, 404, ...), -1 where it failed without one or no GET has the id
static bool Web_Status (qcvm_t *vm)
{
	webget_t	*w = Web_Get (vm);
	int			status;

	if (!w || w->toolong)
		status = -1;
	else if (HTTP_State (w->fetch) == HTTP_RUNNING)
		status = 0;
	else
		status = HTTP_Status (w->fetch) ? HTTP_Status (w->fetch) : -1;
	QC_ReturnFloat (vm, (float)status);
	return true;
}

// string webget_body(float id): its body once it has all come, "" before
static bool Web_Body (qcvm_t *vm)
{
	webget_t	*w = Web_Get (vm);

	if (!w || !w->size || w->toolong || HTTP_State (w->fetch) == HTTP_RUNNING)
		return CLQC_ReturnText (vm, "");
	return QC_ReturnString (vm, (const char *)w->body, w->size);
}

// string webget_header(float id, string name): a header of its answer, ""
// where it has none
static bool Web_Header (qcvm_t *vm)
{
	webget_t	*w = Web_Get (vm);
	char		value[1024];

	if (!w || !HTTP_ResponseHeader (w->fetch, QC_ArgString (vm, 1), value, sizeof(value)))
		value[0] = 0;
	return CLQC_ReturnText (vm, value);
}

// string webget_error(float id): why it failed, "" while it hasn't
static bool Web_Error (qcvm_t *vm)
{
	webget_t	*w = Web_Get (vm);

	if (!w)
		return CLQC_ReturnText (vm, "no such request");
	if (w->toolong)
		return CLQC_ReturnText (vm, "the answer is longer than 1 MB");
	return CLQC_ReturnText (vm, HTTP_Error (w->fetch));
}

// void webget_close(float id): it stops, where it still runs, and its id is
// free for another
static bool Web_CloseBuiltin (qcvm_t *vm)
{
	webget_t	*w = Web_Get (vm);

	if (w)
		Web_Close (w);
	return true;
}

static const struct
{
	const char		*name;
	qc_builtin_t	func;
} hub_builtins[] =
{
	{"webget", Web_Start},
	{"webget_status", Web_Status},
	{"webget_body", Web_Body},
	{"webget_header", Web_Header},
	{"webget_error", Web_Error},
	{"webget_close", Web_CloseBuiltin},
};

/*
================
Hub_Builtins

The GETs' builtins into the menu's
================
*/
bool Hub_Builtins (qc_builtins_t *b)
{
	size_t	i;

	for (i = 0 ; i < sizeof(hub_builtins) / sizeof(hub_builtins[0]) ; i++)
		if (!QC_BuiltinsSet (b, hub_builtins[i].name, hub_builtins[i].func))
			return false;
	return true;
}

/*
================
Hub_Shutdown

The GETs the menu left running, stopped as it goes
================
*/
void Hub_Shutdown (void)
{
	int		i;

	for (i = 0 ; i < WEB_REQUESTS ; i++)
		if (webgets[i].fetch)
			Web_Close (&webgets[i]);
}

void Hub_Init (void)
{
	size_t	i;

	for (i = 0 ; i < sizeof(hub_cvars) / sizeof(hub_cvars[0]) ; i++)
		Cvar_RegisterVariable (hub_cvars[i]);
}
