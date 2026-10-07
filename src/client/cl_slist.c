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
// cl_slist.c -- the server browser's list for the menu's QuakeC: FTE's
// hostcache builtins over the server list (src/slist), SoftWorld's for its
// sources, and ezQuake's sb_* cvars
//
// The list runs once the menu first asks for it, last time's servers shown
// at once. What the list's thread publishes is taken at the top of M_Draw
// only, so the indexes the menu drew with are the ones its keys mean.
//
// The sources are ezQuake's: <basedir>/qw/sb/sources.txt, else ezQuake's own
// (<basedir>/ezquake/sb/sources.txt, its lists beside it), else the masters
// everyone asks; which are marked in qw/sb/marks.txt, the servers found in
// qw/sb/servers.txt.

#include "cl_local.h"
#include "cl_qc.h"
#include "slist.h"

#define SB_DIR			"qw/sb"
#define SB_EZQUAKEDIR	"ezquake/sb"
#define SB_UNBOUND		"Unbound"			// the source of the servers added by hand
#define SB_UNBOUNDFILE	"unbound.txt"

static cvar_t	sb_status = {.name = "sb_status", .string = "1", .archive = true,
	.description = "The server browser shows the selected server's status below the list."};
static cvar_t	sb_showping = {.name = "sb_showping", .string = "1", .archive = true,
	.description = "The server browser's ping column."};
static cvar_t	sb_showaddress = {.name = "sb_showaddress", .string = "0", .archive = true,
	.description = "The server browser's address column."};
static cvar_t	sb_showmap = {.name = "sb_showmap", .string = "1", .archive = true,
	.description = "The server browser's map column."};
static cvar_t	sb_showgamedir = {.name = "sb_showgamedir", .string = "0", .archive = true,
	.description = "The server browser's game directory column."};
static cvar_t	sb_showplayers = {.name = "sb_showplayers", .string = "1", .archive = true,
	.description = "The server browser's players column."};
static cvar_t	sb_showfraglimit = {.name = "sb_showfraglimit", .string = "0", .archive = true,
	.description = "The server browser's fraglimit column."};
static cvar_t	sb_showtimelimit = {.name = "sb_showtimelimit", .string = "0", .archive = true,
	.description = "The server browser's timelimit column."};
static cvar_t	sb_pingtimeout = {.name = "sb_pingtimeout", .string = "1000", .archive = true,
	.description = "Milliseconds the server browser waits for a server's ping."};
static cvar_t	sb_pingspersec = {.name = "sb_pingspersec", .string = "150", .archive = true,
	.description = "Servers the server browser pings a second."};
static cvar_t	sb_pings = {.name = "sb_pings", .string = "3", .archive = true,
	.description = "Times the server browser pings each server, the best of them its ping."};
static cvar_t	sb_infotimeout = {.name = "sb_infotimeout", .string = "1000", .archive = true,
	.description = "Milliseconds the server browser waits for a server's status."};
static cvar_t	sb_inforetries = {.name = "sb_inforetries", .string = "3", .archive = true,
	.description = "Times the server browser asks a server that answered a ping for its status."};
static cvar_t	sb_infospersec = {.name = "sb_infospersec", .string = "100", .archive = true,
	.description = "Servers the server browser asks for their status a second."};
static cvar_t	sb_proxinfopersec = {.name = "sb_proxinfopersec", .string = "10", .archive = true,
	.description = "Proxies (qizmo, qwfwd) the server browser asks for their pings a second."};
static cvar_t	sb_proxretries = {.name = "sb_proxretries", .string = "3", .archive = true,
	.description = "Times the server browser asks a proxy for its pings."};
static cvar_t	sb_proxtimeout = {.name = "sb_proxtimeout", .string = "1000", .archive = true,
	.description = "Milliseconds the server browser waits for a proxy's pings."};
static cvar_t	sb_mastertimeout = {.name = "sb_mastertimeout", .string = "1000", .archive = true,
	.description = "Milliseconds the server browser waits for a master's list before asking again."};
static cvar_t	sb_masterretries = {.name = "sb_masterretries", .string = "3", .archive = true,
	.description = "Times the server browser asks a master for its list."};
static cvar_t	sb_liveupdate = {.name = "sb_liveupdate", .string = "2", .archive = true,
	.description = "Seconds between the server browser's updates of a server whose info is shown; 0 none."};
static cvar_t	sb_sortservers = {.name = "sb_sortservers", .string = "32", .archive = true,
	.description = "The server browser's sort: its columns' numbers, the first first, each after a - "
		"descending (1 name, 2 address, 3 ping, 4 gamedir, 5 map, 6 players, 7 fraglimit, 8 timelimit)."};
static cvar_t	sb_sortplayers = {.name = "sb_sortplayers", .string = "92", .archive = true,
	.description = "The server browser's sort of the players' page: sb_sortservers' numbers for the players' "
		"servers, and 9 the players' names."};
static cvar_t	sb_sortsources = {.name = "sb_sortsources", .string = "3", .archive = true,
	.description = "The server browser's sort of the sources' page: 1 type, 2 name, 3 as sources.txt has them."};
static cvar_t	sb_autohide = {.name = "sb_autohide", .string = "1", .archive = true,
	.description = "The server browser closes when it connects to a server."};
static cvar_t	sb_hideempty = {.name = "sb_hideempty", .string = "1", .archive = true,
	.description = "The server browser hides servers nobody plays on."};
static cvar_t	sb_hidenotempty = {.name = "sb_hidenotempty", .string = "0", .archive = true,
	.description = "The server browser hides servers somebody plays on."};
static cvar_t	sb_hidefull = {.name = "sb_hidefull", .string = "0", .archive = true,
	.description = "The server browser hides full servers."};
static cvar_t	sb_hidedead = {.name = "sb_hidedead", .string = "1", .archive = true,
	.description = "The server browser hides servers that don't answer."};
static cvar_t	sb_hidehighping = {.name = "sb_hidehighping", .string = "0", .archive = true,
	.description = "The server browser hides servers pinging over sb_pinglimit."};
static cvar_t	sb_pinglimit = {.name = "sb_pinglimit", .string = "80", .archive = true,
	.description = "The ping over which sb_hidehighping hides a server."};
static cvar_t	sb_showproxies = {.name = "sb_showproxies", .string = "0", .archive = true,
	.description = "The server browser's proxies (qizmo, qwfwd) among the servers: 0 hidden, 1 shown, 2 alone."};

static cvar_t	sb_hideqtv = {.name = "sb_hideqtv", .string = "1", .archive = true,
	.description = "The server browser hides QTV's relays (QTV, QTVGO): a game on QTV is watched with q in its "
		"server's info."};
static cvar_t	qtv_api_url = {.name = "qtv_api_url", .string = "http://qtvapi.quakeworld.nu/api/v1/servers",
	.description = "The list of the game servers' QTV streams, as ezQuake's: the server browser's q watches a "
		"server's game through its stream. Empty for none."};

static cvar_t	sb_fetch_on_startup = {.name = "sb_fetch_on_startup", .string = "0", .archive = true,
	.description = "The server browser's list is found as the client starts (not in a timedemo), so it is ready "
		"when the browser opens; 0 when it opens first."};

static cvar_t	*const sb_cvars[] =
{
	&sb_status, &sb_showping, &sb_showaddress, &sb_showmap, &sb_showgamedir, &sb_showplayers, &sb_showfraglimit,
	&sb_showtimelimit, &sb_pingtimeout, &sb_pingspersec, &sb_pings, &sb_infotimeout, &sb_inforetries, &sb_infospersec,
	&sb_proxinfopersec, &sb_proxretries, &sb_proxtimeout, &sb_mastertimeout, &sb_masterretries, &sb_liveupdate,
	&sb_sortservers, &sb_sortplayers, &sb_sortsources, &sb_autohide, &sb_hideempty, &sb_hidenotempty, &sb_hidefull,
	&sb_hidedead, &sb_hidehighping, &sb_pinglimit, &sb_showproxies, &sb_hideqtv, &qtv_api_url, &sb_fetch_on_startup,
};

static struct
{
	bool			started;
	bool			failed;			// the list couldn't be started
	slsource_t		sources[SL_MAXSOURCES];
	int				numsources;
	char			dir[MAX_OSPATH];	// <basedir>/qw/sb
	slsnapshot_t	*snap;			// the list as the menu sees it, to the next M_Draw
	slview_t		view;
	bool			paused;
	bool			begun;			// the first frame's been, the configs run
	int				scans;			// asked for this run
} sb;

/*
==============================================================================

SOURCES

==============================================================================
*/

// a file whole, malloc'd and ended; NULL where it doesn't read
static char *SB_ReadFile (const char *path)
{
	FILE	*f = fopen (path, "rb");
	char	*text = NULL;
	long	size;

	if (!f)
		return NULL;
	if (!fseek (f, 0, SEEK_END) && (size = ftell (f)) >= 0 && size < (1 << 20) && !fseek (f, 0, SEEK_SET)
		&& (text = malloc ((size_t)size + 1)))
	{
		if (fread (text, 1, (size_t)size, f) == (size_t)size)
			text[size] = 0;
		else
		{
			free (text);
			text = NULL;
		}
	}
	fclose (f);
	return text;
}

static void SB_WriteFile (const char *file, const char *text)
{
	FILE	*f;

	COM_CreatePath (va ("%s/", sb.dir));
	if (!(f = fopen (va ("%s/%s", sb.dir, file), "wb")))
	{
		Con_DPrintf ("Server browser: %s/%s can't be written\n", sb.dir, file);
		return;
	}
	fputs (text, f);
	fclose (f);
}

// sources.txt and marks.txt as the sources are now
static void SB_SaveSources (void)
{
	char	text[SL_MAXSOURCES * 340];

	SL_FormatSources (sb.sources, sb.numsources, text, sizeof(text));
	SB_WriteFile ("sources.txt", text);
	SL_FormatMarks (sb.sources, sb.numsources, text, sizeof(text));
	SB_WriteFile ("marks.txt", text);
}

// ours, else ezQuake's (its lists where they are), else the masters everyone asks
static void SB_LoadSources (void)
{
	char	*text;
	int		i;

	snprintf (sb.dir, sizeof(sb.dir), "%s/" SB_DIR, FS_BaseDir ());
	if ((text = SB_ReadFile (va ("%s/sources.txt", sb.dir))))
		sb.numsources = SL_ParseSources (text, sb.sources, SL_MAXSOURCES);
	else if ((text = SB_ReadFile (va ("%s/" SB_EZQUAKEDIR "/sources.txt", FS_BaseDir ()))))
	{
		sb.numsources = SL_ParseSources (text, sb.sources, SL_MAXSOURCES);
		for (i = 0 ; i < sb.numsources ; i++)
			if (sb.sources[i].type == SL_FILE && sb.sources[i].location[0] != '/' && !strchr (sb.sources[i].location, ':'))
				Q_strncpyz (sb.sources[i].location, va ("../../" SB_EZQUAKEDIR "/%s", sb.sources[i].location),
					sizeof(sb.sources[i].location));
	}
	free (text);
	if (!sb.numsources)
		sb.numsources = SL_DefaultSources (sb.sources, SL_MAXSOURCES);
	text = SB_ReadFile (va ("%s/marks.txt", sb.dir));
	SL_ParseMarks (text, sb.sources, sb.numsources);
	free (text);
}

static uint64_t SB_Marked (void)
{
	uint64_t	mask = 0;
	int			i;

	for (i = 0 ; i < sb.numsources ; i++)
		if (sb.sources[i].marked)
			mask |= 1ull << i;
	return mask;
}

/*
==============================================================================

THE LIST

==============================================================================
*/

static double SB_Clamp (double v, double low, double high)
{
	return v < low ? low : v > high ? high : v;
}

// how a scan asks, by the sb_* cvars
static void SB_Config (slconfig_t *c)
{
	c->pingrate = (int)SB_Clamp (sb_pingspersec.value, 1, 1000);
	c->pings = (int)SB_Clamp (sb_pings.value, 1, 10);
	c->pingtimeout = SB_Clamp (sb_pingtimeout.value, 100, 10000) / 1000;
	c->inforate = (int)SB_Clamp (sb_infospersec.value, 1, 1000);
	c->inforetries = (int)SB_Clamp (sb_inforetries.value, 1, 10);
	c->infotimeout = SB_Clamp (sb_infotimeout.value, 100, 10000) / 1000;
	c->proxyrate = (int)SB_Clamp (sb_proxinfopersec.value, 1, 1000);
	c->proxyretries = (int)SB_Clamp (sb_proxretries.value, 1, 10);
	c->proxytimeout = SB_Clamp (sb_proxtimeout.value, 100, 10000) / 1000;
	c->masterretries = (int)SB_Clamp (sb_masterretries.value, 1, 10);
	c->mastertimeout = SB_Clamp (sb_mastertimeout.value, 100, 10000) / 1000;
	Q_strncpyz (c->qtvlist, qtv_api_url.string, sizeof(c->qtvlist));
}

// the list run, the cache shown; once
static void SB_Start (void)
{
	if (sb.started || sb.failed)
		return;
	SB_LoadSources ();
	COM_CreatePath (va ("%s/", sb.dir));
	if (!SL_Start (sb.dir, va ("%s/servers.txt", sb.dir), sb.sources, sb.numsources))
	{
		Con_DPrintf ("Server browser: the list can't be started\n");
		sb.failed = true;
		return;
	}
	sb.started = true;
	SB_Adopt ();
}

static void SB_Refresh (void)
{
	slconfig_t	c;

	SB_Start ();
	if (!sb.started)
		return;
	SB_Config (&c);
	SL_Refresh (sb.sources, sb.numsources, &c);
	sb.scans++;
}

static void SB_Arrange (void)
{
	if (sb.snap)
		SL_Arrange (&sb.view, sb.snap->servers, sb.snap->numservers, SB_Marked ());
	else
		sb.view.numshown = 0;
}

/*
================
SB_Adopt

The newest list the thread published, arranged as the menu asked; at the top
of M_Draw, so the indexes it drew with are the ones its keys mean
================
*/
void SB_Adopt (void)
{
	slsnapshot_t	*snap;

	if (!sb.started || !(snap = SL_TakeSnapshot ()))
		return;
	SL_FreeSnapshot (sb.snap);
	sb.snap = snap;
	SB_Arrange ();
}

/*
================
SB_Frame

Each client frame: the first, after the configs, finds the list
(sb_fetch_on_startup); no scan while a connection is made; and what the
list's threads have to say
================
*/
void SB_Frame (bool connecting)
{
	char	message[256];

	if (!sb.begun)
	{
		sb.begun = true;
		if (sb_fetch_on_startup.value && !cls.timedemo)
			SB_Refresh ();
	}
	if (!sb.started)
		return;
	if (connecting != sb.paused)
	{
		SL_Pause (connecting);
		sb.paused = connecting;
	}
	// with developer only: a source that doesn't answer, or a file that can't
	// be written, is nothing to act on, and the Sources page shows each source
	while (SL_TakeMessage (message, sizeof(message)))
		Con_DPrintf ("%s", message);
}

void SB_Shutdown (void)
{
	SL_Shutdown ();
	SL_FreeSnapshot (sb.snap);
	sb.snap = NULL;
	SL_FreeView (&sb.view);
	sb.started = false;
}

/*
==============================================================================

THE BUILTINS

FTE's hostcache builtins (menu QuakeC's 611 to 622), and SoftWorld's by name

==============================================================================
*/

// the server at a view index, NULL past them
static const slserver_t *SB_Server (qcvm_t *vm, int arg)
{
	int		i = QC_DoubleToInt (QC_ArgFloat (vm, arg));

	if (!sb.snap || i < 0 || i >= sb.view.numshown)
		return NULL;
	return &sb.snap->servers[sb.view.shown[i]];
}

static int SB_Source (qcvm_t *vm, int arg)
{
	int		i = QC_DoubleToInt (QC_ArgFloat (vm, arg));

	return i >= 0 && i < sb.numsources ? i : -1;
}

// float gethostcachevalue(float type): FTE's 0 to 7, and SoftWorld's from 100
static bool SB_GetHostCacheValue (qcvm_t *vm)
{
	const slsnapshot_t	*s;
	float				v = 0;
	int					i, n;

	SB_Start ();
	s = sb.snap;
	switch (QC_DoubleToInt (QC_ArgFloat (vm, 0)))
	{
	case 0:		v = (float)sb.view.numshown; break;		// the view's
	case 1:		v = s ? (float)s->numservers : 0; break;	// all of them
	case 2:		// the masters asked, and those that answered
	case 3:
		for (i = n = 0 ; s && i < sb.numsources ; i++)
			if (sb.sources[i].marked && sb.sources[i].type == SL_MASTER
				&& (QC_DoubleToInt (QC_ArgFloat (vm, 0)) == 2 || s->sources[i].state == SLSRC_ANSWERED))
				n++;
		v = (float)n;
		break;
	case 4:		v = s ? (float)s->pingssent : 0; break;	// the servers asked
	case 5:		v = s ? (float)s->alive : 0; break;		// and those that answered
	case 6:		v = sb.view.numsort ? (float)sb.view.sort[0].key : -1; break;
	case 7:		v = sb.view.numsort && sb.view.sort[0].descending ? 1.0f : 0.0f; break;
	case 100:	v = s && s->scanning ? 1.0f : 0.0f; break;
	case 101:	v = s ? (float)s->round : 0; break;
	case 102:	v = s ? (float)s->rounds : 0; break;
	case 103:	v = s ? (float)s->pingssent : 0; break;
	case 104:	v = s ? (float)s->pingstotal : 0; break;
	case 105:	v = s ? (float)s->alive : 0; break;
	case 106:	v = s ? (float)s->dead : 0; break;
	case 107:	v = s ? (float)s->described : 0; break;
	case 108:	v = s ? (float)s->generation : 0; break;
	case 109:	v = (float)sb.numsources; break;
	case 110:	v = s && s->paused ? 1.0f : 0.0f; break;
	case 111:	v = s ? (float)s->relays : 0; break;
	case 112:	v = s ? (float)s->routed : 0; break;
	case 113:	// the servers of the marked sources
		for (i = n = 0 ; s && i < s->numservers ; i++)
			if (s->servers[i].sources & SB_Marked ())
				n++;
		v = (float)n;
		break;
	case 114:	v = (float)sb.scans; break;			// the scans asked for this run
	default:	break;
	}
	QC_ReturnFloat (vm, v);
	return true;
}

// float gethostcacheindexforkey(string key)
static bool SB_GetHostCacheIndexForKey (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)SL_KeyForName (&sb.view, QC_ArgString (vm, 0)));
	return true;
}

// float gethostcachenumber(float key, float hostnr)
static bool SB_GetHostCacheNumber (qcvm_t *vm)
{
	const slserver_t	*s = SB_Server (vm, 1);

	QC_ReturnFloat (vm, s ? (float)SL_KeyNumber (&sb.view, s, QC_DoubleToInt (QC_ArgFloat (vm, 0))) : 0);
	return true;
}

// a player's colour as FTE gives it: a palette's colour as rgb, or an RGB one
static void SB_Colour (int c, float rgb[3])
{
	int		index;

	if (c >= 0 && c < 16 && cls.basepal)
	{
		index = Skin_ColorIndex (c);
		rgb[0] = cls.basepal[index * 3] / 255.0f;
		rgb[1] = cls.basepal[index * 3 + 1] / 255.0f;
		rgb[2] = cls.basepal[index * 3 + 2] / 255.0f;
	}
	else
	{
		rgb[0] = ((c >> 16) & 255) / 255.0f;
		rgb[1] = ((c >> 8) & 255) / 255.0f;
		rgb[2] = (c & 255) / 255.0f;
	}
}

// string gethostcachestring(float key, float hostnr): FTE's player<N> as
// "userid frags time ping "name" "skin" 'top' 'bottom'", and its team after,
// then "b" for a bot (where the server says); a spectator's frags -9999, as
// servers give them
static bool SB_GetHostCacheString (qcvm_t *vm)
{
	const slserver_t	*s = SB_Server (vm, 1);
	const slplayer_t	*p;
	int					key = QC_DoubleToInt (QC_ArgFloat (vm, 0));
	char				text[SL_MAXINFO];
	float				top[3], bottom[3];

	text[0] = 0;
	if (s && key >= SLK_PLAYER0 && key < SLK_CUSTOM)
	{
		if (key - SLK_PLAYER0 < s->numroster)
		{
			p = &s->roster[key - SLK_PLAYER0];
			SB_Colour (p->topcolor, top);
			SB_Colour (p->bottomcolor, bottom);
			snprintf (text, sizeof(text), "%i %i %i %i \"%s\" \"%s\" '%g %g %g' '%g %g %g' \"%s\" \"%s\"", p->userid,
				p->spectator ? -9999 : p->frags, p->time, p->ping, p->name, p->skin, top[0], top[1], top[2], bottom[0],
				bottom[1], bottom[2], p->team, p->bot ? "b" : "");
		}
	}
	else if (s)
		SL_KeyString (&sb.view, s, key, text, sizeof(text));
	return CLQC_ReturnText (vm, text);
}

// void resethostcachemasks(void)
static bool SB_ResetHostCacheMasks (qcvm_t *vm)
{
	(void)vm;
	SL_ClearRules (&sb.view);
	return true;
}

// void sethostcachemaskstring(float mask, float key, string text, float op): ORed with mask 512
static bool SB_SetHostCacheMaskString (qcvm_t *vm)
{
	SL_AddRule (&sb.view, (QC_DoubleToInt (QC_ArgFloat (vm, 0)) & 512) != 0, QC_DoubleToInt (QC_ArgFloat (vm, 1)),
		(sltest_t)QC_DoubleToInt (QC_ArgFloat (vm, 3)), QC_ArgString (vm, 2), 0);
	return true;
}

// void sethostcachemasknumber(float mask, float key, float number, float op)
static bool SB_SetHostCacheMaskNumber (qcvm_t *vm)
{
	SL_AddRule (&sb.view, (QC_DoubleToInt (QC_ArgFloat (vm, 0)) & 512) != 0, QC_DoubleToInt (QC_ArgFloat (vm, 1)),
		(sltest_t)QC_DoubleToInt (QC_ArgFloat (vm, 3)), NULL, QC_DoubleToInt (QC_ArgFloat (vm, 2)));
	return true;
}

// void sethostcachesort(float key, float flags): 1 descending, and SoftWorld's
// 8 a key after those before
static bool SB_SetHostCacheSort (qcvm_t *vm)
{
	SL_SetSort (&sb.view, QC_DoubleToInt (QC_ArgFloat (vm, 0)), QC_DoubleToInt (QC_ArgFloat (vm, 1)));
	return true;
}

// void resorthostcache(void)
static bool SB_ResortHostCache (qcvm_t *vm)
{
	(void)vm;
	SB_Start ();
	SB_Arrange ();
	return true;
}

// void refreshhostcache(optional float purge): a scan of the marked sources,
// from what was found before (the purge's too)
static bool SB_RefreshHostCache (qcvm_t *vm)
{
	(void)vm;
	SB_Refresh ();
	return true;
}

// float gethostcacheindexforaddress(string address): SoftWorld's, the view's
// index of a server, -1 if it isn't shown
static bool SB_GetHostCacheIndexForAddress (qcvm_t *vm)
{
	netadr_t	a;
	int			i;

	QC_ReturnFloat (vm, -1);
	if (!sb.snap || !SL_ParseAddress (QC_ArgString (vm, 0), SL_SERVERPORT, &a))
		return true;
	for (i = 0 ; i < sb.view.numshown ; i++)
		if (!memcmp (sb.snap->servers[sb.view.shown[i]].address.ip, a.ip, sizeof(a.ip))
			&& sb.snap->servers[sb.view.shown[i]].address.port == a.port)
		{
			QC_ReturnFloat (vm, (float)i);
			break;
		}
	return true;
}

// void refreshhostcacheentry(float hostnr): SoftWorld's, a server asked what
// it is ahead of the rest (the info shown kept fresh)
static bool SB_RefreshHostCacheEntry (qcvm_t *vm)
{
	const slserver_t	*s = SB_Server (vm, 0);

	if (s)
		SL_Describe (s->address);
	return true;
}

// float hostcacheinsource(float hostnr, float source): SoftWorld's, whether
// the source named the server
static bool SB_HostCacheInSource (qcvm_t *vm)
{
	const slserver_t	*s = SB_Server (vm, 0);
	int					i = SB_Source (vm, 1);

	QC_ReturnFloat (vm, s && i >= 0 && (s->sources & (1ull << i)) ? 1.0f : 0.0f);
	return true;
}

// string gethostcachesource(float source, string field): SoftWorld's, a
// source's name, type (master, file, url, server), location, marked ("1" or
// ""), servers, state (idle, asking, answered, failed), age (seconds since
// it answered, "" if not this run), or updated (when, as ezQuake has it:
// HH:MM today, MM-DD another day, the year another year, never)
static bool SB_GetHostCacheSource (qcvm_t *vm)
{
	static const char	*const types[] = {"master", "file", "url", "server"};
	static const char	*const states[] = {"idle", "asking", "answered", "failed"};
	const char			*field = QC_ArgString (vm, 1);
	const slsource_t	*src;
	int					i = SB_Source (vm, 0);
	char				text[64];
	time_t				now = time (NULL), then;
	struct tm			today, day;

	if (i < 0)
		return CLQC_ReturnText (vm, "");
	src = &sb.sources[i];
	text[0] = 0;
	if (!strcmp (field, "name"))
		return CLQC_ReturnText (vm, src->name);
	if (!strcmp (field, "location"))
		return CLQC_ReturnText (vm, src->location);
	if (!strcmp (field, "type"))
		return CLQC_ReturnText (vm, types[src->type]);
	if (!strcmp (field, "marked"))
		return CLQC_ReturnText (vm, src->marked ? "1" : "");
	if (!strcmp (field, "servers") && sb.snap)
		snprintf (text, sizeof(text), "%i", sb.snap->sources[i].servers);
	else if (!strcmp (field, "state") && sb.snap)
		return CLQC_ReturnText (vm, states[sb.snap->sources[i].state]);
	else if (!strcmp (field, "age") && sb.snap && sb.snap->sources[i].updated)
		snprintf (text, sizeof(text), "%lld", (long long)(now - sb.snap->sources[i].updated));
	else if (!strcmp (field, "updated"))
	{
		then = sb.snap ? (time_t)sb.snap->sources[i].updated : 0;
		if (!then || !localtime (&now))
			return CLQC_ReturnText (vm, "never");
		today = *localtime (&now);
		day = *localtime (&then);
		if (day.tm_year != today.tm_year)
			snprintf (text, sizeof(text), "%4dy", day.tm_year + 1900);
		else if (day.tm_yday != today.tm_yday)
			snprintf (text, sizeof(text), "%02d-%02d", day.tm_mon + 1, day.tm_mday);
		else
			snprintf (text, sizeof(text), "%2d:%02d", day.tm_hour, day.tm_min);
	}
	return CLQC_ReturnText (vm, text);
}

// void sethostcachesourcemark(float source, float marked): SoftWorld's; the
// view's servers are the marked sources'
static bool SB_SetHostCacheSourceMark (qcvm_t *vm)
{
	int		i = SB_Source (vm, 0);

	if (i < 0)
		return true;
	sb.sources[i].marked = QC_ArgFloat (vm, 1) != 0;
	SB_SaveSources ();
	SB_Arrange ();
	return true;
}

// a source, marked, saved, and scanned for; its index, -1 if there is no room
static int SB_AddSource (slsourcetype_t type, const char *title, const char *location)
{
	slsource_t	*s;
	char		text[256];

	SB_Start ();
	if (!*title || !*location || sb.numsources == SL_MAXSOURCES)
		return -1;
	s = &sb.sources[sb.numsources];
	memset (s, 0, sizeof(*s));
	s->type = type;
	s->marked = true;
	Q_strncpyz (s->name, title, sizeof(s->name));
	if (type == SL_MASTER || type == SL_SERVER)
		SL_WithPort (location, type == SL_MASTER ? SL_MASTERPORT : SL_SERVERPORT, s->location, sizeof(s->location));
	else
		Q_strncpyz (s->location, location, sizeof(s->location));
	// written as it would be read, so it reads back the same
	SL_FormatSources (s, 1, text, sizeof(text));
	if (SL_ParseSources (text, s, 1) != 1)
		return -1;
	s->marked = true;
	sb.numsources++;
	SB_SaveSources ();
	SB_Refresh ();
	return sb.numsources - 1;
}

// float addhostcachesource(string type, string name, string location):
// SoftWorld's, a source of ezQuake's kinds (and server); its index, -1 if it
// can't be
static bool SB_AddHostCacheSource (qcvm_t *vm)
{
	static const char	*const types[] = {"master", "file", "url", "server"};
	const char			*type = QC_ArgString (vm, 0);
	int					i;

	for (i = 0 ; i < 4 && Q_strcasecmp (type, types[i]) ; i++)
		;
	QC_ReturnFloat (vm, i < 4 ? (float)SB_AddSource ((slsourcetype_t)i, QC_ArgString (vm, 1), QC_ArgString (vm, 2)) : -1);
	return true;
}

// void removehostcachesource(float source): SoftWorld's
static bool SB_RemoveHostCacheSource (qcvm_t *vm)
{
	int		i = SB_Source (vm, 0);

	if (i < 0)
		return true;
	memmove (sb.sources + i, sb.sources + i + 1, (size_t)(sb.numsources - i - 1) * sizeof(sb.sources[0]));
	sb.numsources--;
	SB_SaveSources ();
	SB_Refresh ();		// the list's sources matched again by name
	return true;
}

// float addhostcacheserver(string address): SoftWorld's, a server added by
// hand, as ezQuake's are: to the list of the "Unbound" source (made where
// there is none), scanned for; false if it isn't an address
static bool SB_AddHostCacheServer (qcvm_t *vm)
{
	const char	*address = QC_ArgString (vm, 0);
	char		hostname[256], *text;
	int			port, i;
	FILE		*f;

	QC_ReturnFloat (vm, 0);
	if (!SL_SplitAddress (address, SL_SERVERPORT, hostname, sizeof(hostname), &port) || strpbrk (address, " \t\"\\"))
		return true;
	SB_Start ();
	COM_CreatePath (va ("%s/", sb.dir));
	text = SB_ReadFile (va ("%s/" SB_UNBOUNDFILE, sb.dir));
	if (!text || !strstr (text, address))
	{
		if (!(f = fopen (va ("%s/" SB_UNBOUNDFILE, sb.dir), "ab")))
		{
			free (text);
			return true;
		}
		fprintf (f, "%s\n", address);
		fclose (f);
	}
	free (text);
	for (i = 0 ; i < sb.numsources && !(sb.sources[i].type == SL_FILE && !strcmp (sb.sources[i].name, SB_UNBOUND)) ; i++)
		;
	if (i == sb.numsources)
		SB_AddSource (SL_FILE, SB_UNBOUND, SB_UNBOUNDFILE);
	else
	{
		sb.sources[i].marked = true;
		SB_SaveSources ();
		SB_Refresh ();
	}
	QC_ReturnFloat (vm, 1);
	return true;
}

static const struct
{
	const char		*name;
	qc_builtin_t	func;
} sb_builtins[] =
{
	{"gethostcachevalue", SB_GetHostCacheValue},
	{"gethostcacheindexforkey", SB_GetHostCacheIndexForKey},
	{"gethostcachenumber", SB_GetHostCacheNumber},
	{"gethostcachestring", SB_GetHostCacheString},
	{"resethostcachemasks", SB_ResetHostCacheMasks},
	{"sethostcachemaskstring", SB_SetHostCacheMaskString},
	{"sethostcachemasknumber", SB_SetHostCacheMaskNumber},
	{"sethostcachesort", SB_SetHostCacheSort},
	{"resorthostcache", SB_ResortHostCache},
	{"refreshhostcache", SB_RefreshHostCache},
	{"gethostcacheindexforaddress", SB_GetHostCacheIndexForAddress},
	{"refreshhostcacheentry", SB_RefreshHostCacheEntry},
	{"hostcacheinsource", SB_HostCacheInSource},
	{"gethostcachesource", SB_GetHostCacheSource},
	{"sethostcachesourcemark", SB_SetHostCacheSourceMark},
	{"addhostcachesource", SB_AddHostCacheSource},
	{"removehostcachesource", SB_RemoveHostCacheSource},
	{"addhostcacheserver", SB_AddHostCacheServer},
};

/*
================
SB_Builtins

The browser's builtins into the menu's
================
*/
bool SB_Builtins (qc_builtins_t *b)
{
	size_t	i;

	for (i = 0 ; i < sizeof(sb_builtins) / sizeof(sb_builtins[0]) ; i++)
		if (!QC_BuiltinsSet (b, sb_builtins[i].name, sb_builtins[i].func))
			return false;
	return true;
}

void SB_Init (void)
{
	size_t	i;

	for (i = 0 ; i < sizeof(sb_cvars) / sizeof(sb_cvars[0]) ; i++)
		Cvar_RegisterVariable (sb_cvars[i]);
}
