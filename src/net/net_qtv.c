// net_qtv.c -- the server's QTV viewers, at its TCP port (net_ws.c): the game
// as an MVD (the server's sv_mvd.c), the same bytes to every viewer
//
// A viewer asks with QTV's lines (QTV, VERSION: 1, ...) and is answered
// QTVSV 1 and BEGIN, as mvdsv answers; over TCP, or over WebSocket from a
// browser, the stream then in binary messages (FTE's "faketcp"). What the
// server writes goes into one log, in order, and is let out the server's
// delay after it was written, so the stream runs behind the game; each viewer
// reads the log from its own place in it. A viewer joins at a snapshot, the
// whole state the server adds to a frame every second: it reads the level's
// gamestate, then the snapshot, then the frames after it. A level's first
// entry is a snapshot every viewer reads.
//
// Nothing waits on a viewer: a few are written to each server frame, in
// turn, as much of what is out for them as their socket takes, and one that
// falls QTV_MAXLAG behind is dropped. What every viewer has read, older than
// the newest snapshot out, is let go.

#include "mem.h"
#include "net_qtv.h"
#include "net_ws.h"
#include "print.h"
#include "q_string.h"
#include "sys.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	QTV_MAXVIEWERS	64
#define	QTV_REQUESTTIME	5.0			// seconds to send the request in
#define	QTV_MAXLAG		10.0		// seconds a viewer may fall behind what is out
#define	QTV_TURN		8			// viewers written to per NET_QTVSend
#define	QTV_GATHER		65536		// bytes taken from the log for one write
#define	QTV_INMAX		8192		// a WebSocket viewer's frames read, at most
#define	QTV_DRAIN		16			// reads of what a viewer sends up, per turn

typedef struct
{
	int		refs;				// its entries, and one while it is the level written
	int		length;
	byte	data[];				// the gamestate's fixed part
} qtvlevel_t;

typedef struct qtventry_s
{
	struct qtventry_s	*next;
	uint64_t	number;			// in the order written, from 1
	double		written;
	qtvlevel_t	*level;
	bool		start;			// the level's first: every viewer reads its gamestate
	int			readers;		// viewers whose place is in it
	int			length, snaplength;	// the frame's bytes, then the snapshot's, in data
	byte		data[];
} qtventry_t;

enum { QTV_REQUEST, QTV_WAITING, QTV_LIVE };
enum { PART_LEVEL, PART_SNAPSHOT, PART_FRAME };	// an entry's, in the order read

// a place in the log: an entry, the part of it, and how far into that
typedef struct
{
	qtventry_t	*at;
	int			part, offset;
} qtvplace_t;

typedef struct
{
	tcpsocket_t	*socket;
	netadr_t	adr;
	bool		websocket;		// its stream in binary messages
	int			state;
	double		started;
	char		name[32];
	char		request[QTV_REQUESTMAX];
	int			requestlength;
	byte		in[QTV_INMAX];	// a WebSocket viewer's frames, as far as they have come
	int			inlength;
	qtvplace_t	place;			// a live viewer's
	byte		out[10 + QTV_GATHER];	// what it was given and its socket hasn't taken yet
	int			outstart, outend;
} qtvviewer_t;

static struct
{
	qtvviewer_t	*viewers[QTV_MAXVIEWERS];
	qtventry_t	*head, *tail;
	uint64_t	numbered;		// the entries written
	uint64_t	out;			// the entries let out: those numbered up to this
	qtventry_t	*lastout;		// the last of them, NULL once let go
	qtventry_t	*join;			// the newest out with a snapshot, where viewers join
	qtvlevel_t	*level;			// the level written now
	bool		newlevel;		// the next entry is its first
	double		delay;
	int			turn;			// the viewer written to first next time
} qtv;

/*
===============================================================================

THE LOG

===============================================================================
*/

static void QTV_Release (qtvlevel_t *level)
{
	if (!--level->refs)
		Mem_Free (level);
}

// lets out the entries due, the delay after they were written
static void QTV_LetOut (double now)
{
	qtventry_t	*e;

	for (e = qtv.lastout ? qtv.lastout->next : qtv.head ; e && e->written + qtv.delay <= now ; e = e->next)
	{
		qtv.out = e->number;
		qtv.lastout = e;
		if (e->start || e->snaplength)
			qtv.join = e;
	}
}

// lets go of the entries out that no viewer reads and none will join at
static void QTV_Trim (void)
{
	qtventry_t	*e;

	while ((e = qtv.head) && e != qtv.join && e->number <= qtv.out && !e->readers)
	{
		qtv.head = e->next;
		if (!qtv.head)
			qtv.tail = NULL;
		if (qtv.lastout == e)
			qtv.lastout = NULL;
		QTV_Release (e->level);
		Mem_Free (e);
	}
}

// the log let go, all of it; no viewer has a place in it
static void QTV_Clear (void)
{
	qtventry_t	*e;

	while ((e = qtv.head))
	{
		qtv.head = e->next;
		QTV_Release (e->level);
		Mem_Free (e);
	}
	qtv.tail = qtv.lastout = qtv.join = NULL;
	qtv.out = qtv.numbered;
	if (qtv.level)
		QTV_Release (qtv.level);
	qtv.level = NULL;
}

// the place moved on to the next part, or the next entry out, the readers
// counted; false at the end of what is out
static bool QTV_Next (qtvplace_t *p)
{
	qtventry_t	*next;

	if (p->part == PART_LEVEL)
	{
		p->part = PART_SNAPSHOT;
		p->offset = 0;
		return true;
	}
	next = p->at->next;
	if (!next || next->number > qtv.out)
		return false;
	p->at->readers--;
	next->readers++;
	p->at = next;
	p->part = next->start ? PART_LEVEL : PART_FRAME;
	p->offset = 0;
	return true;
}

// the bytes at the place, to the end of its part; 0 at the end of what is out
static int QTV_Bytes (qtvplace_t *p, const byte **data)
{
	int		length;

	for (;;)
	{
		switch (p->part)
		{
		case PART_LEVEL:
			*data = p->at->level->data;
			length = p->at->level->length;
			break;
		case PART_SNAPSHOT:
			*data = p->at->data + p->at->length;
			length = p->at->snaplength;
			break;
		default:
			*data = p->at->data;
			length = p->at->length;
			break;
		}
		if (length > p->offset)
		{
			*data += p->offset;
			return length - p->offset;
		}
		if (!QTV_Next (p))
			return 0;
	}
}

/*
===============================================================================

THE VIEWERS

===============================================================================
*/

static const char *QTV_Name (const qtvviewer_t *v)
{
	return v->name[0] ? va("%s (%s)", v->name, NET_AdrToString (v->adr)) : NET_AdrToString (v->adr);
}

static void QTV_Close (int i, const char *why)
{
	qtvviewer_t	*v = qtv.viewers[i];

	if (v->place.at)
		v->place.at->readers--;
	if (why)
		Con_Printf ("QTV: %s %s\n", QTV_Name (v), why);
	TCP_Close (v->socket);
	Mem_Free (v);
	qtv.viewers[i] = NULL;
}

// what the viewer was given, as much as its socket takes; false when it failed
static bool QTV_Flush (qtvviewer_t *v)
{
	int		sent;

	if (v->outstart == v->outend)
		return true;
	sent = TCP_Write (v->socket, v->out + v->outstart, v->outend - v->outstart);
	if (sent < 0)
		return false;
	v->outstart += sent;
	return true;
}

// out to the viewer: bytes at out + 10 (of a WebSocket viewer's, a binary
// message, its header before them); false when the socket failed
static bool QTV_Give (qtvviewer_t *v, int length)
{
	byte	header[10];
	int		headerlength = 0;

	if (v->websocket)
	{
		headerlength = WS_FrameHeader (header, WS_BINARY, length);
		memcpy (v->out + 10 - headerlength, header, (size_t)headerlength);
	}
	v->outstart = 10 - headerlength;
	v->outend = 10 + length;
	return QTV_Flush (v);
}

// what the viewer sends up: its request while it asks, else let go of (chat
// and commands aren't taken); false when it has gone
static bool QTV_ReadUp (qtvviewer_t *v)
{
	wsframe_t	frame;
	byte		scratch[4096];
	int			k, got, length, code;

	for (k = 0 ; k < QTV_DRAIN ; k++)
	{
		if (!v->websocket)
		{
			if (v->state == QTV_REQUEST)
				got = v->requestlength < QTV_REQUESTMAX ? TCP_Recv (v->socket,
					(byte *)v->request + v->requestlength, QTV_REQUESTMAX - v->requestlength) : 0;
			else
				got = TCP_Recv (v->socket, scratch, sizeof(scratch));
			if (got < 0)
				return false;
			if (!got)
				return true;
			if (v->state == QTV_REQUEST)
				v->requestlength += got;
			continue;
		}

		// over WebSocket, the binary messages' payloads
		while ((length = WS_ReadFrame (v->in, v->inlength, QTV_INMAX - 14, &frame, &code)))
		{
			if (length < 0 || frame.opcode == WS_CLOSE)
				return false;
			if ((frame.opcode == WS_BINARY || frame.opcode == WS_CONTINUATION) && v->state == QTV_REQUEST)
			{
				if (v->requestlength + frame.length > QTV_REQUESTMAX)
					return false;
				memcpy (v->request + v->requestlength, frame.payload, (size_t)frame.length);
				v->requestlength += frame.length;
			}
			memmove (v->in, v->in + length, (size_t)(v->inlength - length));
			v->inlength -= length;
		}
		if (v->inlength == QTV_INMAX)
			return false;		// a frame bigger than a viewer has any need of
		got = TCP_Recv (v->socket, v->in + v->inlength, QTV_INMAX - v->inlength);
		if (got < 0)
			return false;
		if (!got)
			return true;
		v->inlength += got;
	}
	return true;
}

// the request answered once it is whole
static void QTV_Request (int i, double now)
{
	qtvviewer_t		*v = qtv.viewers[i];
	qtvrequest_t	request;
	int				length;

	length = QTV_ParseRequest (v->request, v->requestlength, &request);
	if (!length)
	{
		if (now - v->started > QTV_REQUESTTIME)
			QTV_Close (i, NULL);
		return;
	}
	if (length < 0)
	{
		QTV_Close (i, NULL);
		return;
	}

	if (request.version != 1)
	{
		length = snprintf ((char *)v->out + 10, QTV_GATHER, "QTVSV 1\nPERROR: This server streams QTV version 1\n\n");
		QTV_Give (v, length);
		QTV_Close (i, NULL);
		return;
	}
	memcpy (v->name, request.name, sizeof(v->name));
	v->state = QTV_WAITING;
	Con_Printf ("QTV: %s watching\n", QTV_Name (v));
	if (request.raw)
		return;
	length = snprintf ((char *)v->out + 10, QTV_GATHER, qtv.delay > 0
		? "QTVSV 1\nPRINT: The stream is %g seconds behind the game\nBEGIN\n\n" : "QTVSV 1\nBEGIN\n\n", qtv.delay);
	if (!QTV_Give (v, length))
		QTV_Close (i, "left");
}

// a viewer's turn: what it sends up read, and what is out for it written
static void QTV_Serve (int i, double now)
{
	qtvviewer_t	*v = qtv.viewers[i];
	const byte	*data;
	int			n, length, take;

	if (!QTV_ReadUp (v))
	{
		QTV_Close (i, v->state == QTV_REQUEST ? NULL : "left");
		return;
	}
	if (v->state == QTV_REQUEST)
	{
		QTV_Request (i, now);
		return;
	}

	// what it was given before goes first, whole
	if (!QTV_Flush (v))
	{
		QTV_Close (i, "left");
		return;
	}
	if (v->outstart != v->outend)
	{
		if (v->state == QTV_LIVE && now - (v->place.at->written + qtv.delay) > QTV_MAXLAG)
			QTV_Close (i, "fell behind and was dropped");
		return;
	}

	// a viewer joins at the newest snapshot out: the level's gamestate first
	if (v->state == QTV_WAITING)
	{
		if (!qtv.join)
			return;
		v->place = (qtvplace_t){qtv.join, PART_LEVEL, 0};
		qtv.join->readers++;
		v->state = QTV_LIVE;
	}
	if (now - (v->place.at->written + qtv.delay) > QTV_MAXLAG)
	{
		QTV_Close (i, "fell behind and was dropped");
		return;
	}

	// what is out for it, as one write
	for (n = 0 ; n < QTV_GATHER && (length = QTV_Bytes (&v->place, &data)) ; n += take)
	{
		take = length < QTV_GATHER - n ? length : QTV_GATHER - n;
		memcpy (v->out + 10 + n, data, (size_t)take);
		v->place.offset += take;
	}
	if (n && !QTV_Give (v, n))
		QTV_Close (i, "left");
}

void QTV_Adopt (tcpsocket_t *socket, netadr_t from, bool websocket, const byte *request, int length,
	const byte *frames, int frameslength)
{
	static const char	full[] = "QTVSV 1\nTERROR: This server has as many viewers as it takes\n\n";
	byte		message[10 + sizeof(full)];
	qtvviewer_t	*v;
	int			i, headerlength;

	for (i = 0 ; i < QTV_MAXVIEWERS && qtv.viewers[i] ; i++)
		;
	if (i == QTV_MAXVIEWERS || length > QTV_REQUESTMAX || frameslength > QTV_INMAX)
	{
		headerlength = websocket ? WS_FrameHeader (message, WS_BINARY, (int)sizeof(full) - 1) : 0;
		memcpy (message + headerlength, full, sizeof(full) - 1);
		TCP_Write (socket, message, headerlength + (int)sizeof(full) - 1);
		TCP_Close (socket);
		return;
	}
	v = qtv.viewers[i] = Mem_Calloc (1, sizeof(*v));
	v->socket = socket;
	v->adr = from;
	v->adr.type = NA_IP;
	v->websocket = websocket;
	v->state = QTV_REQUEST;
	v->started = Sys_DoubleTime ();
	memcpy (v->request, request, (size_t)length);
	v->requestlength = length;
	memcpy (v->in, frames, (size_t)frameslength);
	v->inlength = frameslength;
	QTV_Serve (i, v->started);
}

void QTV_CloseAll (void)
{
	int		i;

	for (i = 0 ; i < QTV_MAXVIEWERS ; i++)
		if (qtv.viewers[i])
			QTV_Close (i, qtv.viewers[i]->state == QTV_REQUEST ? NULL : "was let go");
	QTV_Clear ();
}

/*
===============================================================================

THE SERVER'S

===============================================================================
*/

int NET_QTVViewers (void)
{
	int		i, count;

	for (i = count = 0 ; i < QTV_MAXVIEWERS ; i++)
		if (qtv.viewers[i] && qtv.viewers[i]->state != QTV_REQUEST)
			count++;
	return count;
}

void NET_QTVLevel (const void *data, int length)
{
	if (qtv.level)
		QTV_Release (qtv.level);
	qtv.level = Mem_Alloc (sizeof(*qtv.level) + (size_t)length);
	qtv.level->refs = 1;
	qtv.level->length = length;
	memcpy (qtv.level->data, data, (size_t)length);
	qtv.newlevel = true;
}

void NET_QTVFrame (const void *data, int length, const void *snapshot, int snaplength)
{
	qtventry_t	*e;

	if (!qtv.level)
		return;		// a frame of no level written
	e = Mem_Alloc (sizeof(*e) + (size_t)length + (size_t)snaplength);
	e->next = NULL;
	e->number = ++qtv.numbered;
	e->written = Sys_DoubleTime ();
	e->level = qtv.level;
	qtv.level->refs++;
	e->start = qtv.newlevel;
	qtv.newlevel = false;
	e->readers = 0;
	e->length = length;
	e->snaplength = snaplength;
	if (length)
		memcpy (e->data, data, (size_t)length);
	if (snaplength)
		memcpy (e->data + length, snapshot, (size_t)snaplength);
	if (qtv.tail)
		qtv.tail->next = e;
	else
		qtv.head = e;
	qtv.tail = e;
}

void NET_QTVSend (double delay)
{
	double	now = Sys_DoubleTime ();
	int		k, served;

	qtv.delay = delay;
	QTV_LetOut (now);
	for (k = served = 0 ; k < QTV_MAXVIEWERS && served < QTV_TURN ; k++)
		if (qtv.viewers[(qtv.turn + k) % QTV_MAXVIEWERS])
		{
			QTV_Serve ((qtv.turn + k) % QTV_MAXVIEWERS, now);
			served++;
		}
	qtv.turn = (qtv.turn + k) % QTV_MAXVIEWERS;
	QTV_Trim ();
}

void NET_QTVReset (void)
{
	int		i;

	// a viewer in the middle of the log has no stream to go on with
	for (i = 0 ; i < QTV_MAXVIEWERS ; i++)
		if (qtv.viewers[i] && qtv.viewers[i]->state == QTV_LIVE)
			QTV_Close (i, "was let go");
	QTV_Clear ();
}
