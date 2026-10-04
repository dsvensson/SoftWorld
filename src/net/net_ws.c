// net_ws.c -- the server's TCP port, at its port number: WebSocket
// connections, browsers' clients, which can't send UDP (sv_websocket); and
// QTV's viewers (sv_public), handed to net_qtv.c: a connection whose first
// bytes are "QTV", or a WebSocket one whose first message is (a game client's
// is a connectionless packet, 0xffffffff)
//
// A connection is accepted, sends its upgrade request (net_wsproto.c) within
// WS_REQUESTTIME, and then a packet a binary message, as FTE's clients do. Its
// address is the client's (NA_WS: the peer's address and port), so the server
// knows it as it knows a UDP client, and its packets go back through it. A
// browser that connects again (another port) is found by its qport, as a
// client behind a NAT that changed its port is.
//
// Nothing waits: what a connection can't send at once is queued, whole
// frames, and a packet that doesn't fit is dropped as a network drops it.
// Connections are at most WS_MAXCONNS, of them at most WS_MAXHANDSHAKES from
// one address still in their handshake, and closed when quiet for
// WS_IDLETIME. wss:// is a TLS proxy's in front, which may say who its client
// is (X-Forwarded-For, taken only from this machine).

#include "mem.h"
#include "net_qtv.h"
#include "net_socket.h"
#include "net_ws.h"
#include "print.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	WS_MAXCONNS			64
#define	WS_MAXHANDSHAKES	4			// from one address, but this machine's (a proxy)
#define	WS_REQUESTTIME		5.0			// seconds to send the upgrade request in
#define	WS_IDLETIME			90.0		// seconds without a frame
#define	WS_OUTMAX			16384		// bytes queued, of whole frames
#define	WS_INMAX			(WS_REQUESTMAX + MAX_UDP_PACKET + 14)

typedef struct
{
	tcpsocket_t	*socket;
	netadr_t	adr;				// NA_WS
	bool		open;				// upgraded
	bool		passed;				// a message passed on
	double		started, heard;
	byte		in[WS_INMAX];		// the request, then frames
	int			inlength;
	byte		message[MAX_UDP_PACKET];	// a message's fragments so far
	int			messagelength;
	bool		fragmented;
	byte		out[WS_OUTMAX];
	int			outlength;
} wsconn_t;

static tcplisten_t	*ws_listen;
static int			ws_port;
static bool			ws_websocket, ws_qtv;	// what the port takes
static wsconn_t		*ws_conns[WS_MAXCONNS];
static bool			ws_round;			// accepted and expired for this round of reads

/*
===============================================================================

CONNECTIONS

===============================================================================
*/

static void WS_Close (int i)
{
	TCP_Close (ws_conns[i]->socket);
	Mem_Free (ws_conns[i]);
	ws_conns[i] = NULL;
}

// sends what is queued, as much as the socket takes; false if it failed
static bool WS_Flush (wsconn_t *c)
{
	int		sent;

	if (!c->outlength)
		return true;
	sent = TCP_Write (c->socket, c->out, c->outlength);
	if (sent < 0)
		return false;
	memmove (c->out, c->out + sent, (size_t)(c->outlength - sent));
	c->outlength -= sent;
	return true;
}

// queues a frame whole, or none of it; false if the connection failed
static bool WS_Send (wsconn_t *c, int opcode, const void *payload, int length)
{
	byte	header[10];
	int		headerlength = WS_FrameHeader (header, opcode, length);

	if (!WS_Flush (c))
		return false;
	if (c->outlength + headerlength + length > WS_OUTMAX)
		return true;		// dropped, as UDP drops what doesn't fit
	memcpy (c->out + c->outlength, header, (size_t)headerlength);
	memcpy (c->out + c->outlength + headerlength, payload, (size_t)length);
	c->outlength += headerlength + length;
	return WS_Flush (c);
}

// a close frame with its code, sent as far as it goes, and the connection closed
static void WS_CloseWith (int i, int code)
{
	byte	payload[2] = {(byte)(code >> 8), (byte)code};

	WS_Send (ws_conns[i], WS_CLOSE, payload, code ? 2 : 0);
	WS_Close (i);
}

static void WS_Accept (void)
{
	tcpsocket_t	*socket;
	netadr_t	from;
	int			i, free, handshakes;

	while ((socket = TCP_Accept (ws_listen, &from)))
	{
		free = -1;
		handshakes = 0;
		for (i = 0 ; i < WS_MAXCONNS ; i++)
			if (!ws_conns[i])
			{
				if (free < 0)
					free = i;
			}
			else if (!ws_conns[i]->open && !memcmp (ws_conns[i]->adr.ip, from.ip, sizeof(from.ip)))
				handshakes++;
		if (free < 0 || (handshakes >= WS_MAXHANDSHAKES && !NET_IsLoopback (from)))
		{
			TCP_Close (socket);
			continue;
		}
		ws_conns[free] = Mem_Calloc (1, sizeof(wsconn_t));
		ws_conns[free]->socket = socket;
		ws_conns[free]->adr = from;
		ws_conns[free]->adr.type = NA_WS;
		ws_conns[free]->started = ws_conns[free]->heard = Sys_DoubleTime ();
	}
}

// the requests too slow to come, the connections quiet too long, and what
// they have queued sent
static void WS_Expire (void)
{
	double	now = Sys_DoubleTime ();
	int		i;

	for (i = 0 ; i < WS_MAXCONNS ; i++)
	{
		if (!ws_conns[i])
			continue;
		if (!ws_conns[i]->open && now - ws_conns[i]->started > WS_REQUESTTIME)
			WS_Close (i);
		else if (ws_conns[i]->open && now - ws_conns[i]->heard > WS_IDLETIME)
			WS_CloseWith (i, 1001);
		else if (!WS_Flush (ws_conns[i]))
			WS_Close (i);
	}
}

/*
===============================================================================

READING

===============================================================================
*/

// the upgrade request, answered; false when the connection is gone
static bool WS_Handshake (int i)
{
	wsconn_t	*c = ws_conns[i];
	wsrequest_t	request;
	char		reply[512];
	byte		ip[16];
	int			length;

	length = WS_ParseRequest ((const char *)c->in, c->inlength, &request);
	if (!length)
		return true;
	if (length < 0)
	{
		WS_Close (i);
		return false;
	}
	TCP_Write (c->socket, reply, WS_Reply (&request, reply, sizeof(reply)));
	if (request.status != 101)
	{
		WS_Close (i);
		return false;
	}
	// a proxy here tells whose connection it is
	if (NET_IsLoopback (c->adr) && request.forwarded[0] && NET_ParseIP (request.forwarded, ip))
		memcpy (c->adr.ip, ip, sizeof(c->adr.ip));
	c->open = true;
	c->heard = Sys_DoubleTime ();
	memmove (c->in, c->in + length, (size_t)(c->inlength - length));
	c->inlength -= length;
	return true;
}

/*
================
WS_Classify

What a connection not yet upgraded has sent: QTV's request, the connection
handed over; else a WebSocket upgrade's. False when it is gone.
================
*/
static bool WS_Classify (int i)
{
	wsconn_t	*c = ws_conns[i];

	if (memcmp (c->in, "QTV", c->inlength < 3 ? (size_t)c->inlength : 3))
	{
		if (ws_websocket)
			return WS_Handshake (i);
		WS_Close (i);
		return false;
	}
	if (c->inlength < 3)
		return true;
	if (!ws_qtv)
	{
		WS_Close (i);
		return false;
	}
	QTV_Adopt (c->socket, c->adr, false, c->in, c->inlength, NULL, 0);
	Mem_Free (c);
	ws_conns[i] = NULL;
	return false;
}

/*
================
WS_ReadMessage

The connection's next packet from what it has sent: true with it in msg;
false when there is none yet, or the connection is gone
================
*/
static bool WS_ReadMessage (int i, netadr_t *from, sizebuf_t *msg)
{
	wsconn_t	*c = ws_conns[i];
	wsframe_t	frame;
	int			length, code;

	while ((length = WS_ReadFrame (c->in, c->inlength, MAX_UDP_PACKET, &frame, &code)))
	{
		if (length < 0)
		{
			WS_CloseWith (i, code);
			return false;
		}
		c->heard = Sys_DoubleTime ();

		switch (frame.opcode)
		{
		case WS_PING:
			if (!WS_Send (c, WS_PONG, frame.payload, frame.length))
			{
				WS_Close (i);
				return false;
			}
			break;
		case WS_CLOSE:
			WS_Send (c, WS_CLOSE, frame.payload, frame.length >= 2 ? 2 : 0);
			WS_Close (i);
			return false;
		case WS_TEXT:
			WS_CloseWith (i, 1003);		// packets are binary
			return false;
		case WS_BINARY:
		case WS_CONTINUATION:
			if ((frame.opcode == WS_BINARY) == c->fragmented)
			{
				WS_CloseWith (i, 1002);		// a message begun in another's middle, or continued none
				return false;
			}
			if (c->messagelength + frame.length > MAX_UDP_PACKET)
			{
				WS_CloseWith (i, 1009);
				return false;
			}
			memcpy (c->message + c->messagelength, frame.payload, (size_t)frame.length);
			c->messagelength += frame.length;
			c->fragmented = !frame.fin;
			break;
		}

		memmove (c->in, c->in + length, (size_t)(c->inlength - length));
		c->inlength -= length;
		if ((frame.opcode == WS_BINARY || frame.opcode == WS_CONTINUATION) && frame.fin)
		{
			length = c->messagelength;
			c->messagelength = 0;
			if (!length || length > msg->maxsize)
				continue;
			// a viewer's QTV request first: the connection is QTV's
			if (!c->passed && length >= 3 && !memcmp (c->message, "QTV", 3))
			{
				if (!ws_qtv)
				{
					WS_CloseWith (i, 1008);
					return false;
				}
				QTV_Adopt (c->socket, c->adr, true, c->message, length, c->in, c->inlength);
				Mem_Free (c);
				ws_conns[i] = NULL;
				return false;
			}
			c->passed = true;
			memcpy (msg->data, c->message, (size_t)length);
			msg->cursize = length;
			*from = c->adr;
			return true;
		}
	}
	return false;
}

/*
================
WS_Read

The connection's next packet: from what it has sent, else from what has
arrived, read until none waits (the waits wake for what is new)
================
*/
static bool WS_Read (int i, netadr_t *from, sizebuf_t *msg)
{
	wsconn_t	*c;
	int			got;

	for (;;)
	{
		c = ws_conns[i];
		if (c->open && WS_ReadMessage (i, from, msg))
			return true;
		if (!ws_conns[i])
			return false;
		if (c->inlength == WS_INMAX)
		{
			WS_Close (i);		// a request or a frame bigger than they may be
			return false;
		}
		got = TCP_Recv (c->socket, c->in + c->inlength, WS_INMAX - c->inlength);
		if (got < 0)
		{
			WS_Close (i);
			return false;
		}
		if (!got)
			return false;
		c->inlength += got;
		if (!c->open && !WS_Classify (i))
			return false;
	}
}

/*
===============================================================================

THE SERVER'S PORT

===============================================================================
*/

bool WS_GetPacket (netadr_t *from, sizebuf_t *msg)
{
	int		i;

	if (!ws_listen)
		return false;
	if (!ws_round)
	{
		WS_Accept ();
		WS_Expire ();
		ws_round = true;
	}
	for (i = 0 ; i < WS_MAXCONNS ; i++)
		if (ws_conns[i] && WS_Read (i, from, msg))
			return true;
	ws_round = false;
	return false;
}

void WS_SendPacket (const void *data, int length, const netadr_t *to)
{
	int		i;

	for (i = 0 ; i < WS_MAXCONNS ; i++)
		if (ws_conns[i] && ws_conns[i]->open && ws_conns[i]->adr.port == to->port
			&& !memcmp (ws_conns[i]->adr.ip, to->ip, sizeof(to->ip)))
		{
			if (!WS_Send (ws_conns[i], WS_BINARY, data, length))
				WS_Close (i);
			return;
		}
}

bool NET_ListenTCP (int port, bool websocket, bool qtv)
{
	int		i;

	if (!websocket && !qtv)
	{
		NET_CloseTCP ();
		return false;
	}
	// what the port no longer takes goes
	if (!websocket)
		for (i = 0 ; i < WS_MAXCONNS ; i++)
			if (ws_conns[i] && ws_conns[i]->open)
				WS_CloseWith (i, 1001);
	if (!qtv)
		QTV_CloseAll ();
	if (ws_listen && ws_port == port && websocket == ws_websocket && qtv == ws_qtv)
		return true;
	if (!ws_listen || ws_port != port)
	{
		NET_CloseTCP ();
		ws_listen = TCP_Listen (port);
		if (!ws_listen)
			return false;
		ws_port = port;
	}
	ws_websocket = websocket;
	ws_qtv = qtv;
	Con_Printf ("Server TCP port %i:%s%s\n", port, websocket ? " WebSocket" : "", qtv ? " QTV" : "");
	return true;
}

void NET_CloseTCP (void)
{
	int		i;

	for (i = 0 ; i < WS_MAXCONNS ; i++)
		if (ws_conns[i])
			WS_CloseWith (i, 1001);
	QTV_CloseAll ();
	if (ws_listen)
		TCP_CloseListen (ws_listen);
	ws_listen = NULL;
	ws_websocket = ws_qtv = false;
	ws_round = false;
}
