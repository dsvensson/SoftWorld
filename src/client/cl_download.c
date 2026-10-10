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
// A map the server can't give, or a recording or QTV stream needs, comes
// from the web (cl_download_mapsrc).

#include "cl_local.h"

#include "net_http.h"
#include "sys.h"

#include <stdatomic.h>

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

A map the server can't give, or a recording or QTV stream needs, as FTE's
cl_download_mapsrc has it: <url><map>.bsp, over HTTP or HTTPS on a thread of
its own, which writes the temp file. The download bar follows it, and a
recording waits for it.

===============================================================================
*/

#define WEB_TIMEOUT		20		// seconds without a byte

static cvar_t	cl_download_mapsrc = {.name = "cl_download_mapsrc", .string = "https://maps.quakeworld.nu/all/",
	.archive = true,
	.description = "Where a map comes from that the server doesn't give (it has no downloads, or doesn't have the "
		"map), or that a demo or QTV stream needs: a URL the maps are under, as <url><map>.bsp (FTE's). Empty for "
		"none."};

enum { WEB_RUNNING, WEB_DONE, WEB_ABANDONED };

// a map on its way; its thread frees it where it was abandoned
typedef struct
{
	char			url[MAX_OSPATH * 2];
	char			path[MAX_OSPATH];		// the temp file
	FILE			*file;
	systhread_t		*thread;
	atomic_llong	received;
	atomic_llong	total;					// -1 where the answer gives none
	atomic_int		state;
	atomic_bool		cancel;
	bool			ok;						// once it is done
	char			error[256];
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

static bool CL_WebBody (void *ctx, const void *data, size_t length, long long total)
{
	webdownload_t	*w = ctx;

	if (atomic_load (&w->cancel))
		return false;
	if (fwrite (data, 1, length, w->file) != length)
	{
		snprintf (w->error, sizeof(w->error), "can't write the file");
		return false;
	}
	atomic_store (&w->total, total);
	atomic_fetch_add (&w->received, (long long)length);
	return true;
}

static void CL_WebThread (void *arg)
{
	webdownload_t	*w = arg;

	if (!(w->file = fopen (w->path, "wb")))
		snprintf (w->error, sizeof(w->error), "can't create the file");
	else
	{
		w->ok = HTTP_Get (w->url, WEB_TIMEOUT, CL_WebBody, w, w->error, sizeof(w->error));
		if (fclose (w->file) && w->ok)
		{
			w->ok = false;
			snprintf (w->error, sizeof(w->error), "can't write the file");
		}
	}
	if (!w->ok)
		remove (w->path);
	if (atomic_exchange (&w->state, WEB_DONE) == WEB_ABANDONED)
	{	// nothing waits for it
		remove (w->path);
		free (w);
	}
}

/*
================
CL_WebDownload

A map from cl_download_mapsrc, saved as local; false where local isn't a map,
there is no such URL, or no thread for it
================
*/
static bool CL_WebDownload (const char *local)
{
	const char		*src = cl_download_mapsrc.string, *dot = strrchr (local, '.');
	webdownload_t	*w;

	if (strncmp (local, "maps/", 5) || strchr (local + 5, '/') || !dot || Q_strcasecmp (dot, ".bsp")
		|| (Q_strncasecmp (src, "http://", 7) && Q_strncasecmp (src, "https://", 8)))
		return false;
	if (!(w = calloc (1, sizeof(*w))))
		return false;

	Q_strncpyz (cls.downloadname, local, sizeof(cls.downloadname));
	Q_strncpyz (cls.downloadlocalname, local, sizeof(cls.downloadlocalname));
	COM_StripExtension (cls.downloadlocalname, cls.downloadtempname);
	Q_strncatz (cls.downloadtempname, ".tmp", sizeof(cls.downloadtempname));
	snprintf (w->url, sizeof(w->url), "%s%s", src, local + 5);
	if (!CL_DownloadPath (cls.downloadtempname, w->path, sizeof(w->path)))
	{
		free (w);
		return false;
	}
	COM_CreatePath (w->path);
	atomic_store (&w->total, -1);
	atomic_store (&w->state, WEB_RUNNING);
	if (!(w->thread = Sys_StartThread ("download", CL_WebThread, w)))
	{
		free (w);
		return false;
	}
	web = w;
	CL_DownloadStarted ();
	Con_Printf ("Downloading %s from %s...\n", local, w->url);
	return true;
}

/*
================
CL_DownloadFrame

Once a frame: the web download's bar, and its end, as one from the server
ends: renamed into place, then the next file
================
*/
void CL_DownloadFrame (void)
{
	char		oldn[MAX_OSPATH], newn[MAX_OSPATH], error[256];
	long long	total;
	double		took;
	bool		ok;

	if (!web)
		return;
	dl.received = (int)atomic_load (&web->received);
	total = atomic_load (&web->total);
	cls.downloadpercent = total > 0 ? (int)(dl.received * 100LL / total) : 0;
	if (atomic_load (&web->state) != WEB_DONE)
		return;

	Sys_JoinThread (web->thread);
	ok = web->ok;
	Q_strncpyz (error, web->error, sizeof(error));
	free (web);
	web = NULL;
	cls.downloadpercent = 0;

	took = host.realtime - dl.started;
	if (!CL_DownloadPath (cls.downloadtempname, oldn, sizeof(oldn))
		|| !CL_DownloadPath (cls.downloadlocalname, newn, sizeof(newn)))
	{
		ok = false;
		Q_strncpyz (error, "its path doesn't fit", sizeof(error));
	}
	if (!ok)
		Con_Printf ("Couldn't download %s: %s\n", cls.downloadname, error);
	else
	{
		remove (newn);
		if (rename (oldn, newn))
			Con_Printf ("failed to rename %s\n", oldn);
		else
			Con_Printf ("%s: %d bytes in %.1f s, %s\n", cls.downloadname, dl.received, took,
				CL_RateText (dl.received / fmax (took, 0.001)));
	}
	CL_RequestNextDownload ();
}

// the web download dropped: its thread ends on its own, and cleans up after
static void CL_AbandonWeb (void)
{
	systhread_t	*thread;

	if (!web)
		return;
	thread = web->thread;
	atomic_store (&web->cancel, true);
	if (atomic_exchange (&web->state, WEB_ABANDONED) == WEB_DONE)
	{
		Sys_JoinThread (thread);
		remove (web->path);
		free (web);
	}
	else
		Sys_DetachThread (thread);
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
	if (!CL_WebDownload (local))
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
CL_CheckOrDownloadFile

Returns true if the file exists, otherwise it attempts
to start a download from the server.
=================
*/
bool	CL_CheckOrDownloadFile (char *filename)
{
	return CL_CheckOrDownloadFileAs (filename, filename);
}

/*
=================
CL_CheckOrDownloadFileAs

The same, saving the server's file remote as local
=================
*/
bool	CL_CheckOrDownloadFileAs (const char *remote, const char *local)
{
	FILE	*f;

	if (strstr (remote, "..") || strstr (local, ".."))
	{
		Con_Printf ("Refusing to download a path with ..\n");
		return true;
	}

	COM_FOpenFile (local, &f);
	if (f)
	{	// it exists, no need to download
		fclose (f);
		return true;
	}
	if (CL_Attracting ())
		return true;	// attract mode shows what is here

	//ZOID - can't download when recording
	if (cls.demorecording) {
		Con_Printf("Unable to download %s in record mode.\n", remote);
		return true;
	}
	// a recording or a QTV stream has no server to ask: a map comes from the
	// web, where it is there, the recording waiting for it
	if (cls.demoplayback)
	{
		if (!CL_WebDownload (local))
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
