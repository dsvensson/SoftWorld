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
// cl_download.c  -- fetching the files a server has and the client does not
//
// Two protocols share svc_download, told apart only by the extensions.
//
// Classic: blocks on the reliable channel, in order, each answered with
// nextdl until one says 100 percent.
//
// FTE chunked: the server announces the size, then answers requests for
// numbered 1024 byte chunks, on the datagram or out of band, in any order, and
// drops what it likes without a word. The client keeps what has not arrived
// as a list of ranges, the ones asked for tagged with the packet that asked
// (qualia's range container), and asks again for any the server has had long
// enough to answer: the server answers a request before it acknowledges any
// later packet, so that is counted in packets, not time. The request rate
// doubles every round trip until a request goes unanswered, then climbs by one
// per chunk that lands and drops by two percent when requests go unanswered.
// The requests ride on the move packets; before the client is in the game,
// what does not fit goes in packets of its own.
//
// A file the server keeps on the web too (its serverinfo's sv_dlURL, FTE's)
// comes from there first, and a map the server can't give, or a recording or
// QTV stream needs, from cl_download_mapsrc (FROM THE WEB, below).

#include "cl_local.h"

#include "net_http.h"
#include "sys.h"

#define DL_MAXREQUESTS	256		// requests in flight at most
#define DL_RETRY		10		// our packets the server acknowledges before an unanswered request is lost
#define DL_MAXPOLLGAP	0.1		// the longest gap the request rate is integrated over
#define DL_REQUESTBYTES	40		// room for one nextdl

typedef struct
{
	int		start, size;	// bytes, whole chunks
	int		asked;			// the outgoing sequence that asked for it, -1 not asked
} dlrange_t;

static struct
{
	bool		awaiting;		// a download request went out, no reply yet
	bool		chunked;		// the file in flight comes in chunks
	int			cookie;			// names the file in requests and out-of-band chunks
	int			size;
	int			received;

	// what has not arrived: sorted by start, never overlapping; two free
	// ranges that touch are merged
	dlrange_t	*ranges;
	int			numranges, maxranges;

	float		rate;			// requests per second
	float		slop;			// the fraction of a request carried to the next poll
	bool		congested;		// a request has gone unanswered: slow start is over
	double		lastpoll;

	// for the speed beside the download bar
	double		started;
	double		sampletime;
	int			samplebytes;
	double		speed;			// bytes per second
} dl = {.rate = 1};

/*
===============================================================================

RANGE CONTAINER

===============================================================================
*/

static dlrange_t *Ranges_Insert (int i)
{
	if (dl.numranges == dl.maxranges)
	{
		dl.maxranges = dl.maxranges ? dl.maxranges * 2 : 64;
		dl.ranges = Mem_Realloc (dl.ranges, dl.maxranges * sizeof(*dl.ranges));
	}
	memmove (&dl.ranges[i+1], &dl.ranges[i], (dl.numranges - i) * sizeof(*dl.ranges));
	dl.numranges++;
	return &dl.ranges[i];
}

static void Ranges_Delete (int i)
{
	dl.numranges--;
	memmove (&dl.ranges[i], &dl.ranges[i+1], (dl.numranges - i) * sizeof(*dl.ranges));
}

/*
================
Ranges_Reset

All of a file of size bytes missing, in whole chunks; an empty file is
complete
================
*/
static void Ranges_Reset (int size)
{
	dl.numranges = 0;
	if (size > 0)
		*Ranges_Insert (0) = (dlrange_t){0, (size + DL_CHUNKSIZE - 1) / DL_CHUNKSIZE * DL_CHUNKSIZE, -1};
}

/*
================
Ranges_Take

The first chunk not asked for, marked as asked in packet sequence; -1 when
all are asked for. From the front, so the holes losses leave are filled before
the tail.
================
*/
static int Ranges_Take (int sequence)
{
	dlrange_t	*r;
	int			i;

	for (i=0 ; i<dl.numranges ; i++)
		if (dl.ranges[i].asked < 0)
			break;
	if (i == dl.numranges)
		return -1;

	r = &dl.ranges[i];
	if (r->size > DL_CHUNKSIZE)
	{
		dlrange_t	rest = {r->start + DL_CHUNKSIZE, r->size - DL_CHUNKSIZE, -1};

		r->size = DL_CHUNKSIZE;
		*Ranges_Insert (i+1) = rest;
	}
	dl.ranges[i].asked = sequence;
	return dl.ranges[i].start / DL_CHUNKSIZE;
}

/*
================
Ranges_Remove

The chunk at start arrived. False when it was not missing: a duplicate. One
that comes after its request was given up on still counts (FTE). asked is the
packet that asked for it, -1 for such a late one.
================
*/
static bool Ranges_Remove (int start, int *asked)
{
	dlrange_t	*r;
	dlrange_t	rest;
	int			i, end;

	for (i=0 ; i<dl.numranges ; i++)
	{
		r = &dl.ranges[i];
		end = r->start + r->size;
		if (start < r->start)
			return false;
		if (start >= end)
			continue;

		*asked = r->asked;
		if (r->size == DL_CHUNKSIZE)
			Ranges_Delete (i);
		else if (start == r->start)
		{
			r->start += DL_CHUNKSIZE;
			r->size -= DL_CHUNKSIZE;
		}
		else if (start + DL_CHUNKSIZE == end)
			r->size -= DL_CHUNKSIZE;
		else
		{	// out of the middle of a free range
			rest = (dlrange_t){start + DL_CHUNKSIZE, end - start - DL_CHUNKSIZE, r->asked};
			r->size = start - r->start;
			*Ranges_Insert (i+1) = rest;
		}
		return true;
	}
	return false;
}

/*
================
Ranges_Prune

Gives up on the requests the server has had long enough to answer: those in
packets more than DL_RETRY before the last one it acknowledged. Returns how
many; nothing else in the protocol tells of a loss.
================
*/
static int Ranges_Prune (int acknowledged)
{
	dlrange_t	*r;
	int			i, pruned;

	pruned = 0;
	for (i=0 ; i<dl.numranges ; i++)
	{
		r = &dl.ranges[i];
		if (r->asked < 0 || r->asked > acknowledged - DL_RETRY)
			continue;
		r->asked = -1;
		pruned++;

		// merge with free neighbours, where they touch; a gap between is a
		// chunk that arrived
		if (i+1 < dl.numranges && dl.ranges[i+1].asked < 0 && r->start + r->size == dl.ranges[i+1].start)
		{
			r->size += dl.ranges[i+1].size;
			Ranges_Delete (i+1);
		}
		if (i > 0 && dl.ranges[i-1].asked < 0 && dl.ranges[i-1].start + dl.ranges[i-1].size == r->start)
		{
			dl.ranges[i-1].size += r->size;
			Ranges_Delete (i);
			i--;
		}
	}
	return pruned;
}

static int Ranges_Asked (void)
{
	int		i, n;

	for (i = n = 0 ; i<dl.numranges ; i++)
		if (dl.ranges[i].asked >= 0)
			n++;
	return n;
}

/*
===============================================================================

FILES

===============================================================================
*/

/*
================
CL_DownloadPath

Where a download goes: the game directory; skins, which all game directories
share, in qw
================
*/
static bool CL_DownloadPath (const char *file, char *path, size_t size)
{
	if (!strncmp (file, "skins/", 6))
		return Q_snprintfz (path, size, "%s/qw/%s", FS_BaseDir (), file);
	return Q_snprintfz (path, size, "%s/%s", com_gamedir, file);
}

/*
================
CL_BeginDownload

Asks the server for a file. It goes to a temp name, renamed when it is
complete, so an interrupted download leaves no runt file.
================
*/
static void CL_BeginDownload (const char *file, const char *local)
{
	Q_strncpyz (cls.downloadname, file, sizeof(cls.downloadname));
	Q_strncpyz (cls.downloadlocalname, local, sizeof(cls.downloadlocalname));
	COM_StripExtension (cls.downloadlocalname, cls.downloadtempname);
	Q_strncatz (cls.downloadtempname, ".tmp", sizeof(cls.downloadtempname));

	dl.awaiting = true;
	MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
	MSG_WriteString (&cls.netchan.message, va("download %s", cls.downloadname));
}

// the counters the download bar's speed is worked out from
static void CL_DownloadStarted (void)
{
	dl.received = 0;
	dl.started = dl.sampletime = host.realtime;
	dl.samplebytes = 0;
	dl.speed = 0;
}

static bool CL_OpenDownload (void)
{
	char	path[MAX_OSPATH];

	if (!CL_DownloadPath (cls.downloadtempname, path, sizeof(path)))
	{
		Con_Printf ("The path of %s doesn't fit\n", cls.downloadtempname);
		return false;
	}
	COM_CreatePath (path);
	cls.download = fopen (path, "wb");
	if (!cls.download)
	{
		Con_Printf ("Failed to open %s\n", path);
		return false;
	}
	CL_DownloadStarted ();
	return true;
}

/*
================
CL_RateText

A speed in a fixed width, kB/s or MB/s
================
*/
static const char *CL_RateText (double bytes)
{
	static char	text[16];

	if (bytes < 999950)
		snprintf (text, sizeof(text), "%5.1f kB/s", bytes / 1000);
	else
		snprintf (text, sizeof(text), "%5.1f MB/s", bytes / 1000000);
	return text;
}

/*
================
CL_DownloadSpeed

The speed of the download in flight over the last half second, for the
download bar
================
*/
const char *CL_DownloadSpeed (void)
{
	double	dt = host.realtime - dl.sampletime;

	if (dt >= 0.5)
	{
		dl.speed = (dl.received - dl.samplebytes) / dt;
		dl.samplebytes = dl.received;
		dl.sampletime = host.realtime;
	}
	return CL_RateText (dl.speed);
}

/*
================
CL_CloseDownload

Drops the file in flight, and its temp file
================
*/
static void CL_CloseDownload (void)
{
	char	path[MAX_OSPATH];

	if (cls.download)
	{
		fclose (cls.download);
		cls.download = NULL;
		if (CL_DownloadPath (cls.downloadtempname, path, sizeof(path)))
			remove (path);
	}
	dl.awaiting = false;
	dl.chunked = false;
	cls.downloadpercent = 0;
}

/*
===============================================================================

FROM THE WEB

As FTE has them: a map, a model, a sound or a skin from the server's web
address for its files (its serverinfo's sv_dlURL), <url>/<gamedir>/<path>,
before the server is asked, which it is where the web hasn't the file (a
recording or QTV stream then has only cl_download_mapsrc); and a map the
server can't give, or a recording or QTV stream needs, from
cl_download_mapsrc, <url><map>.bsp.

Over HTTP or HTTPS, a GET that runs on its own (net_http.h's HTTP_Start, a
page's fetch()), what came of it written to the temp file each frame. The
download bar follows it, and a recording waits for it. A gzip is asked for,
and one that comes is saved as it came, as <path>.gz, which the file system
reads unpacked (fs.h); a page's fetch() unpacks it before it is read.

===============================================================================
*/

static cvar_t	cl_download_mapsrc = {.name = "cl_download_mapsrc", .string = "https://maps.quakeworld.nu/all/",
	.archive = true,
	.description = "Where a map comes from that the server doesn't give (it has no downloads, or doesn't have the "
		"map), or that a demo or QTV stream needs: a URL the maps are under, as <url><map>.bsp (FTE's). Empty for "
		"none."};

// a file on its way
typedef struct
{
	httpfetch_t	*fetch;
	FILE		*file;					// the temp file
	char		url[HTTP_MAXURL];
	char		remote[MAX_OSPATH];		// the server's name for it
	bool		dlurl;					// from the server's sv_dlURL, else cl_download_mapsrc
	char		error[64];				// why it failed here, "" while it hasn't
} webdownload_t;

static webdownload_t	*web;		// the one on its way

void CL_InitDownloads (void)
{
	Cvar_RegisterVariable (&cl_download_mapsrc);
}

bool CL_Downloading (void)
{
	return cls.download || web;
}

// a URL of http:// or https://
static bool CL_IsWebURL (const char *url)
{
	return !Q_strncasecmp (url, "http://", 7) || !Q_strncasecmp (url, "https://", 8);
}

// path onto the URL in url, its bytes a URL can't hold as they are as %XX;
// false if it doesn't fit
static bool CL_URLPath (char *url, size_t size, const char *path)
{
	static const char	hex[] = "0123456789ABCDEF";
	size_t				len = strlen (url);
	unsigned char		c;

	for ( ; *path ; path++)
	{
		c = (unsigned char)*path;
		if (isalnum (c) || strchr ("-._~/", c))
		{
			if (len + 1 >= size)
				return false;
			url[len++] = (char)c;
		}
		else
		{
			if (len + 3 >= size)
				return false;
			url[len++] = '%';
			url[len++] = hex[c >> 4];
			url[len++] = hex[c & 15];
		}
	}
	url[len] = 0;
	return true;
}

/*
================
CL_StartWeb

local from url into its temp file, remote the server's name for it (asked
for where it was the server's sv_dlURL that hasn't it); false where the temp
file can't be made
================
*/
static bool CL_StartWeb (const char *url, const char *remote, const char *local, bool dlurl)
{
	char	path[MAX_OSPATH];
	FILE	*file;

	// before printing: the names may be va()'s buffer, which printing reuses
	Q_strncpyz (cls.downloadname, local, sizeof(cls.downloadname));
	Q_strncpyz (cls.downloadlocalname, local, sizeof(cls.downloadlocalname));
	COM_StripExtension (cls.downloadlocalname, cls.downloadtempname);
	Q_strncatz (cls.downloadtempname, ".tmp", sizeof(cls.downloadtempname));
	if (!CL_DownloadPath (cls.downloadtempname, path, sizeof(path)))
		return false;
	COM_CreatePath (path);
	if (!(file = fopen (path, "wb")))
	{
		Con_Printf ("Failed to open %s\n", path);
		return false;
	}
	web = Mem_Calloc (1, sizeof(*web));
	web->file = file;
	Q_strncpyz (web->url, url, sizeof(web->url));
	Q_strncpyz (web->remote, remote, sizeof(web->remote));
	web->dlurl = dlurl;
	web->fetch = HTTP_Start (url, "Accept-Encoding: gzip");
	CL_DownloadStarted ();
	Con_Printf ("Downloading %s from %s...\n", cls.downloadlocalname, web->url);
	return true;
}

/*
================
CL_DlURLDownload

local from the server's web address for its files, its serverinfo's sv_dlURL
(FTE's): <url>/<gamedir>/<remote>; false where it has none
================
*/
static bool CL_DlURLDownload (const char *remote, const char *local)
{
	const char	*base = Info_ValueForKey (cl.serverinfo, "sv_dlURL");
	char		url[HTTP_MAXURL], path[MAX_OSPATH * 2];

	if (!CL_IsWebURL (base))
		return false;
	if (!Q_snprintfz (url, sizeof(url), "%s%s", base, base[strlen (base) - 1] == '/' ? "" : "/")
		|| !Q_snprintfz (path, sizeof(path), "%s/%s", gamedirfile, remote) || !CL_URLPath (url, sizeof(url), path))
		return false;
	return CL_StartWeb (url, remote, local, true);
}

/*
================
CL_MapsrcDownload

A map from cl_download_mapsrc, <url><map>.bsp, saved as local; false where
local isn't a map or there is no such URL
================
*/
static bool CL_MapsrcDownload (const char *local)
{
	const char	*src = cl_download_mapsrc.string, *dot = strrchr (local, '.');
	char		url[HTTP_MAXURL];

	if (strncmp (local, "maps/", 5) || strchr (local + 5, '/') || !dot || Q_strcasecmp (dot, ".bsp")
		|| !CL_IsWebURL (src))
		return false;
	if (!Q_snprintfz (url, sizeof(url), "%s", src) || !CL_URLPath (url, sizeof(url), local + 5))
		return false;
	return CL_StartWeb (url, local, local, false);
}

/*
================
CL_EndWeb

The web download ended, error why where it failed (NULL where it didn't):
renamed into place (as <local>.gz where it came gzipped), and the next file;
else, where it was the server's sv_dlURL, the server asked for it (a
recording's or a QTV stream's map from cl_download_mapsrc)
================
*/
static void CL_EndWeb (const char *error)
{
	char	why[256], encoding[32], url[HTTP_MAXURL], remote[MAX_OSPATH], local[MAX_OSPATH], saved[MAX_OSPATH];
	char	oldn[MAX_OSPATH], newn[MAX_OSPATH];
	double	took = host.realtime - dl.started;
	bool	gz = false, dlurl = web->dlurl;

	Q_strncpyz (why, error ? error : "", sizeof(why));		// error may be the GET's, closed below
	// a gzip comes as it was sent (a page's fetch() has unpacked it, and
	// tells no encoding)
	if (!why[0] && HTTP_ResponseHeader (web->fetch, "Content-Encoding", encoding, sizeof(encoding))
		&& Q_strcasecmp (encoding, "identity"))
	{
		if (Q_strcasecmp (encoding, "gzip"))
			snprintf (why, sizeof(why), "it came as %s, which isn't gzip", encoding);
		else
			gz = true;
	}
	if (fclose (web->file) && !why[0])
		Q_strncpyz (why, "can't write the file", sizeof(why));
	HTTP_Close (web->fetch);
	Q_strncpyz (url, web->url, sizeof(url));
	Q_strncpyz (remote, web->remote, sizeof(remote));
	Q_strncpyz (local, cls.downloadlocalname, sizeof(local));
	Mem_Free (web);
	web = NULL;
	cls.downloadpercent = 0;

	if (!CL_DownloadPath (cls.downloadtempname, oldn, sizeof(oldn)))
		Q_strncpyz (why, "its path doesn't fit", sizeof(why));
	else if (!why[0] && (!Q_snprintfz (saved, sizeof(saved), "%s%s", local, gz ? ".gz" : "")
		|| !CL_DownloadPath (saved, newn, sizeof(newn))))
		Q_strncpyz (why, "its path doesn't fit", sizeof(why));
	if (!why[0])
	{
		remove (newn);
		if (rename (oldn, newn))
			Con_Printf ("failed to rename %s\n", oldn);
		else
			Con_Printf ("%s: %d bytes%s in %.1f s, %s\n", saved, dl.received, gz ? ", gzipped," : "", took,
				CL_RateText (dl.received / fmax (took, 0.001)));
		CL_RequestNextDownload ();
		return;
	}

	remove (oldn);
	Con_Printf ("Couldn't download %s from %s: %s\n", local, url, why);
	if (dlurl && !cls.demoplayback)
	{	// the server, then
		CL_BeginDownload (remote, local);
		Con_Printf ("Downloading %s...\n", cls.downloadname);
		return;
	}
	if (dlurl && CL_MapsrcDownload (local))
		return;
	CL_RequestNextDownload ();
}

/*
================
CL_DownloadFrame

Once a frame: what came of the web download, into its temp file, for the bar,
and its end
================
*/
void CL_DownloadFrame (void)
{
	byte		buf[65536];
	long long	total;
	httpstate_t	state;
	int			n;

	if (!web)
		return;
	while (!web->error[0] && (n = HTTP_Read (web->fetch, buf, sizeof(buf))) > 0)
	{
		if (fwrite (buf, 1, (size_t)n, web->file) != (size_t)n)
			Q_strncpyz (web->error, "can't write the file", sizeof(web->error));
		dl.received += n;
	}
	// a page's length is the gzip's that fetch() unpacks: the bar stops full
	total = HTTP_Length (web->fetch);
	cls.downloadpercent = total > 0 ? (int)fmin (dl.received * 100.0 / (double)total, 100) : 0;
	if (web->error[0])
	{
		CL_EndWeb (web->error);
		return;
	}
	state = HTTP_State (web->fetch);
	if (state != HTTP_RUNNING)
		CL_EndWeb (state == HTTP_DONE ? NULL : HTTP_Error (web->fetch));
}

// the web download dropped, its temp file with it
static void CL_AbandonWeb (void)
{
	char	path[MAX_OSPATH];

	if (!web)
		return;
	HTTP_Close (web->fetch);
	fclose (web->file);
	if (CL_DownloadPath (cls.downloadtempname, path, sizeof(path)))
		remove (path);
	Mem_Free (web);
	web = NULL;
	cls.downloadpercent = 0;
}

/*
================
CL_StopDownload

The connection is going away
================
*/
void CL_StopDownload (void)
{
	CL_AbandonWeb ();
	CL_CloseDownload ();
	dl.rate = 1;
	dl.slop = 0;
	dl.congested = false;
}

// a map the server can't give comes from the web, where it is there
static void CL_DownloadFailed (const char *reason)
{
	char	local[MAX_OSPATH];

	Con_Printf ("Couldn't download %s: %s\n", cls.downloadname, reason);
	Q_strncpyz (local, cls.downloadlocalname, sizeof(local));
	CL_CloseDownload ();
	if (!CL_MapsrcDownload (local))
		CL_RequestNextDownload ();
}

static void CL_FinishDownload (void)
{
	char	oldn[MAX_OSPATH];
	char	newn[MAX_OSPATH];

	fclose (cls.download);
	cls.download = NULL;
	cls.downloadpercent = 0;

	if (!CL_DownloadPath (cls.downloadtempname, oldn, sizeof(oldn))
		|| !CL_DownloadPath (cls.downloadlocalname, newn, sizeof(newn)))
		Con_Printf ("failed to name %s\n", cls.downloadname);
	else
	{
		remove (newn);
		if (rename (oldn, newn))
			Con_Printf ("failed to rename %s\n", oldn);
	}

	if (dl.chunked)
	{
		Con_Printf ("%s: %d bytes in %.1f s, %s\n", cls.downloadname, dl.size, host.realtime - dl.started,
			CL_RateText (dl.size / fmax (host.realtime - dl.started, 0.001)));

		// nothing acknowledges a chunk: the server keeps the file open until
		// it hears chunk -1
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message, va("nextdl -1 100 %d", dl.cookie));
		dl.chunked = false;
	}

	// get another file if needed
	CL_RequestNextDownload ();
}

/*
=================
CL_CheckOrDownload

Returns true if the file exists (stored gzipped too), otherwise it attempts to
start a download of the server's file remote as local: from the server's
sv_dlURL first where fromweb, then from the server
=================
*/
static bool CL_CheckOrDownload (const char *remote, const char *local, bool fromweb)
{
	long long	size, mtime;

	if (strstr (remote, "..") || strstr (local, ".."))
	{
		Con_Printf ("Refusing to download a path with ..\n");
		return true;
	}

	if (FS_FileInfo (local, &size, &mtime))
		return true;	// it exists, no need to download
	if (CL_Attracting ())
		return true;	// attract mode shows what is here

	//ZOID - can't download when recording
	if (cls.demorecording) {
		Con_Printf("Unable to download %s in record mode.\n", remote);
		return true;
	}
	// the server's web address for its files first, a recording's and a QTV
	// stream's too
	if (fromweb && CL_DlURLDownload (remote, local))
	{
		cls.downloadnumber++;
		return false;
	}
	// a recording or a QTV stream has no server to ask: a map comes from the
	// web, where it is there, the recording waiting for it
	if (cls.demoplayback)
	{
		if (!CL_MapsrcDownload (local))
			return true;
		cls.downloadnumber++;
		return false;
	}

	// before printing: the names may be va()'s buffer, which printing reuses
	CL_BeginDownload (remote, local);
	Con_Printf ("Downloading %s...\n", cls.downloadname);
	cls.downloadnumber++;

	return false;
}

/*
=================
CL_CheckOrDownloadFile

Returns true if the file exists, otherwise it attempts
to start a download: a map, a model, a sound or a skin, from the server's
sv_dlURL first, as FTE has it
=================
*/
bool	CL_CheckOrDownloadFile (char *filename)
{
	return CL_CheckOrDownload (filename, filename, true);
}

/*
=================
CL_CheckOrDownloadFileAs

The same, saving the server's file remote as local, from the server alone
(a csprogs, which must be the one its checksum names)
=================
*/
bool	CL_CheckOrDownloadFileAs (const char *remote, const char *local)
{
	return CL_CheckOrDownload (remote, local, false);
}

/*
=====================
CL_Download_f
=====================
*/
void CL_Download_f (void)
{
	if (cls.state == ca_disconnected)
	{
		Con_Printf ("Must be connected.\n");
		return;
	}

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("Usage: download <datafile>\n");
		return;
	}

	if (CL_Downloading () || dl.awaiting)
	{
		Con_Printf ("Already downloading %s\n", cls.downloadname);
		return;
	}

	cls.downloadtype = dl_single;
	CL_BeginDownload (Cmd_Argv(1), Cmd_Argv(1));
}

/*
===============================================================================

CHUNKED

===============================================================================
*/

/*
================
CL_ReceiveChunk

A chunk, from the datagram or out of band
================
*/
static void CL_ReceiveChunk (int chunk, const byte *data)
{
	int		start, len, asked;
	double	rtt;

	if (!cls.download || !dl.chunked || chunk < 0 || chunk >= (dl.size + DL_CHUNKSIZE - 1) / DL_CHUNKSIZE)
		return;
	start = chunk * DL_CHUNKSIZE;
	if (!Ranges_Remove (start, &asked))
		return;

	// the last chunk is padded on the wire
	len = dl.size - start;
	if (len > DL_CHUNKSIZE)
		len = DL_CHUNKSIZE;
	if (fseek (cls.download, start, SEEK_SET) || (int)fwrite (data, 1, len, cls.download) != len)
	{
		CL_DownloadFailed ("can't write the file");
		return;
	}
	dl.received += len;

	// until a request goes unanswered each chunk adds one request per round
	// trip, doubling the rate every round trip (TCP's slow start); after, one
	// request per second (qualia, FTE), which alone would take seconds to
	// reach what a fast link carries
	if (!dl.congested && asked >= 0 && cls.netchan.outgoing_sequence - asked < UPDATE_BACKUP)
	{
		rtt = host.realtime - cl.frames[asked & UPDATE_MASK].senttime;
		dl.rate += 1 / (float)(rtt < 0.005 ? 0.005 : rtt > 1 ? 1 : rtt);
	}
	else
		dl.rate += 1;
	cls.downloadpercent = (int)((long long)dl.received * 100 / dl.size);

	if (!dl.numranges)
		CL_FinishDownload ();
}

/*
================
CL_ParseChunkedDownload

A chunk, or the header: chunk -1, then the size or why there is none, and the
name
================
*/
static void CL_ParseChunkedDownload (void)
{
	const char	*svname;
	byte		*data;
	int			chunk, size;

	chunk = MSG_ReadLong ();
	if (chunk != -1)
	{
		data = cls.net_message.data + msg_readcount;
		msg_readcount += DL_CHUNKSIZE;
		if (msg_readcount > cls.net_message.cursize)
			msg_badread = true;
		else if (!cls.demoplayback)
			CL_ReceiveChunk (chunk, data);
		return;
	}

	size = MSG_ReadLong ();
	if (size == (int)0x80000000)
	{	// FTE's escape for files past 2 GB: the size as two longs
		MSG_ReadLong ();
		MSG_ReadLong ();
	}
	svname = MSG_ReadString ();
	if (cls.demoplayback || msg_badread)
		return;
	if (!*svname)
		return;		// mvdsv acknowledging a stop
	if (!dl.awaiting && !cls.download)
		return;		// not asked for

	if (size < 0 && size != (int)0x80000000)
	{
		CL_DownloadFailed (size == -1 ? "not found on the server"
			: size == -2 ? "the server doesn't allow it"
			: size == -3 ? "the server stopped sending it"
			: "the server redirected it");
		return;
	}
	if (cls.download)
		return;		// already started
	dl.awaiting = false;

	// a different name would put one file's bytes at another's path
	if (Q_strcasecmp (svname, cls.downloadname))
	{
		CL_DownloadFailed (va("the server sent %s instead", svname));
		return;
	}
	if (size == (int)0x80000000 || size > INT_MAX - DL_CHUNKSIZE)
	{
		CL_DownloadFailed ("too big");
		return;
	}
	if (!CL_OpenDownload ())
	{
		CL_DownloadFailed ("can't create the file");
		return;
	}

	dl.chunked = true;
	dl.cookie++;
	dl.size = size;
	dl.lastpoll = host.realtime;
	Ranges_Reset (size);
	if (!dl.numranges)
		CL_FinishDownload ();
}

/*
================
CL_ParseChunkPacket

An out-of-band print starting "\chunk" is a chunk, with the number of the
file it belongs to. Returns false for other prints.
================
*/
bool CL_ParseChunkPacket (void)
{
	int		cookie, chunk;

	if (cls.net_message.cursize - msg_readcount < 6
	 || memcmp (cls.net_message.data + msg_readcount, "\\chunk", 6))
		return false;
	msg_readcount += 6;

	cookie = MSG_ReadLong ();
	if (MSG_ReadByte () != svc_download)
		return true;
	chunk = MSG_ReadLong ();
	if (msg_badread || cls.net_message.cursize - msg_readcount < DL_CHUNKSIZE)
		return true;
	if (cookie == dl.cookie && NET_CompareAdr (cls.net_from, cls.netchan.remote_address))
		CL_ReceiveChunk (chunk, cls.net_message.data + msg_readcount);
	return true;
}

/*
================
CL_DownloadRequests

How many chunks a chunked download asks for this frame: as many as the rate
allows, no more than DL_MAXREQUESTS in flight. The requests the server has had
long enough to answer are given up on first.
================
*/
int CL_DownloadRequests (void)
{
	double	gap;
	int		want;

	if (!cls.download || !dl.chunked)
		return 0;

	if (Ranges_Prune (cls.netchan.incoming_acknowledged) > 0)
	{
		dl.rate *= 0.98f;
		dl.congested = true;
	}

	gap = host.realtime - dl.lastpoll;
	if (gap < 0)
		gap = 0;
	if (gap > DL_MAXPOLLGAP)
		gap = DL_MAXPOLLGAP;
	dl.lastpoll = host.realtime;
	dl.slop += dl.rate * (float)gap;
	want = (int)dl.slop;
	dl.slop -= want;

	// a rate this poll can't use is taken back (FTE), all of it, or a loss
	// would take long to bring it down to what the link carries; one below
	// the window always asks for something, or a transfer that lost every
	// request would never earn the rate back
	if (want > DL_MAXREQUESTS)
	{
		dl.rate = (float)(DL_MAXREQUESTS / gap);
		dl.slop = 0;
		want = DL_MAXREQUESTS;
	}
	if (want < 1 && dl.rate < DL_MAXREQUESTS)
		want = 1;
	if (want > DL_MAXREQUESTS - Ranges_Asked ())
		want = DL_MAXREQUESTS - Ranges_Asked ();
	return want;
}

/*
================
CL_WriteDownloadRequests

Up to want chunk requests into the packet about to go, unreliable, as many as
fit beside the reliable part: over it the netchan would drop the whole
unreliable part, a move with it. Returns how many.
================
*/
int CL_WriteDownloadRequests (sizebuf_t *buf, int want)
{
	int		room, chunk, i;

	room = MAX_MSGLEN - (cls.netchan.reliable_length ? cls.netchan.reliable_length : cls.netchan.message.cursize);
	if (room > buf->maxsize)
		room = buf->maxsize;
	for (i=0 ; i<want && buf->cursize + DL_REQUESTBYTES <= room ; i++)
	{
		chunk = Ranges_Take (cls.netchan.outgoing_sequence);
		if (chunk < 0)
			break;
		MSG_WriteByte (buf, clc_stringcmd);
		MSG_WriteString (buf, va("nextdl %d %d %d", chunk, cls.downloadpercent, dl.cookie));
	}
	return i;
}

/*
===============================================================================

CLASSIC

===============================================================================
*/

/*
=====================
CL_ParseDownload

A download message has been received from the server
=====================
*/
void CL_ParseDownload (void)
{
	int		size, percent;

	if (cls.fteext & FTE_PEXT_CHUNKEDDOWNLOADS)
	{
		CL_ParseChunkedDownload ();
		return;
	}

	// read the data
	size = MSG_ReadShort ();
	percent = MSG_ReadByte ();

	if (cls.demoplayback) {
		if (size > 0)
			msg_readcount += size;
		return; // not in demo playback
	}

	if (size == -1)
	{
		CL_DownloadFailed ("not found on the server");
		return;
	}
	if (size < 0 || msg_readcount + size > cls.net_message.cursize)
	{
		msg_badread = true;
		return;
	}

	// open the file if not opened yet
	if (!cls.download)
	{
		if (!dl.awaiting)
		{	// not asked for
			msg_readcount += size;
			return;
		}
		dl.awaiting = false;
		if (!CL_OpenDownload ())
		{
			msg_readcount += size;
			CL_DownloadFailed ("can't create the file");
			return;
		}
	}

	fwrite (cls.net_message.data + msg_readcount, 1, size, cls.download);
	msg_readcount += size;
	dl.received += size;

	if (percent != 100)
	{
// change display routines by zoid
		// request next block
		cls.downloadpercent = percent;

		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		SZ_Print (&cls.netchan.message, "nextdl");
	}
	else
		CL_FinishDownload ();
}
