// net_rtc.c -- WebRTC over libdatachannel (net_rtc.h), as FTE's clients reach
// servers through a broker
//
// The client opens a WebSocket to the broker (subprotocol rtc_client) at
// /FTE-Quake/<room>, FTE's name for Quake's games. The broker answers with the
// server it found (NEWPEER), and the client offers a peer connection with one
// data channel, "quake", unordered and never sending again (as UDP); the
// broker passes the offer, the answer and both sides' ICE candidates between
// them (OFFER, CANDIDATE: a byte, the peer's number in two, and JSON as a
// browser's). Once the channel opens a packet is a message on it. The broker
// also answers STUN at its port, so its own address is the first ICE server.
//
// Nothing here waits: what libdatachannel's threads tell (the descriptions and
// candidates to send, the connections' states, its log) is queued for the
// game's thread, and the broker's messages and the channel's packets are
// taken as they wait. A packet sent before the channel is open is dropped, as
// a network drops it; the client asks for a challenge again in a while.

#include "cvar.h"
#include "net_rtc.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"

#include <rtc/rtc.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	RTC_BROKERPORT	27950			// FTE's (dpmaster's), unless the URL gives one
#define	RTC_PROTOCOL	"FTE-Quake"		// what FTE's brokers know Quake's games by
#define	MAX_RTCURLS		64
#define	MAX_RTCCONNS	32
#define	REOPEN_TIME		5.0				// seconds before a failed connection is tried again
#define	CONNECT_TIME	20.0			// seconds a connection may take to open
#define	IDLE_TIME		30.0			// seconds without a packet sent before one is closed
#define	MAX_RTCMESSAGE	65536

// the broker's messages (FTE's ICEMSG_*)
enum { ICEMSG_PEERLOST, ICEMSG_GREETING, ICEMSG_NEWPEER, ICEMSG_OFFER, ICEMSG_CANDIDATE, ICEMSG_ACCEPT,
	ICEMSG_SERVERINFO, ICEMSG_SERVERUPDATE, ICEMSG_NAMEINUSE };

static cvar_t	net_rtc_ignorecert = {.name = "net_rtc_ignorecert", .string = "0",
	.description = "Takes an rtcs:// broker's TLS certificate unchecked: for a broker whose certificate has run out, "
		"to test; never otherwise.",
	.values = (const cvar_value_t[]){{"0", "Certificates checked"}, {"1", "Certificates unchecked"}, {0}}};
static cvar_t	net_rtc_debug = {.name = "net_rtc_debug", .string = "0",
	.description = "Prints what WebRTC connections do: the broker's messages, ICE, and libdatachannel's log.",
	.values = (const cvar_value_t[]){{"0", "Quiet"}, {"1", "The connections' steps"},
		{"2", "And libdatachannel's log"}, {0}}};

// the servers by URL, an NA_RTC's ip[0] and ip[1] their number
typedef struct
{
	bool	tls;				// rtcs://
	char	host[128];
	char	room[128];			// after the broker's address, without the '/'
} rtcurl_t;

static rtcurl_t	rtc_urls[MAX_RTCURLS];
static int		rtc_numurls;

typedef struct
{
	bool		used;
	netadr_t	adr;			// NA_RTC
	int			ws;				// the broker's WebSocket, -1 for none
	int			pc;				// the peer connection, -1 for none
	int			dc;				// its data channel, -1 for none
	int			peer;			// the broker's number of the other side
	bool		open;			// the data channel is
	bool		failed;			// told it failed
	double		opened;			// when it was last opened
	double		sent;			// when a packet was last sent
	_Atomic int	state;			// the peer connection's (rtcState), from libdatachannel's threads
} rtcconn_t;

static rtcconn_t	rtc_conns[MAX_RTCCONNS];
static bool			rtc_initialized;

/*
===============================================================================

WHAT LIBDATACHANNEL'S THREADS TELL

===============================================================================
*/

enum { EV_DESCRIPTION, EV_CANDIDATE, EV_LOG };

typedef struct
{
	int		conn;
	int		kind;
	char	*text;
	char	*extra;		// a description's type, a candidate's mid
} rtcevent_t;

#define	MAX_RTCEVENTS	256

static atomic_flag	rtc_lock = ATOMIC_FLAG_INIT;
static rtcevent_t	rtc_events[MAX_RTCEVENTS];
static int			rtc_numevents;

static char *RTC_CopyString (const char *s)
{
	size_t	length = s ? strlen (s) : 0;
	char	*copy = malloc (length + 1);

	if (copy)
	{
		if (length)
			memcpy (copy, s, length);
		copy[length] = 0;
	}
	return copy;
}

// from any thread; when there are too many a log line is dropped, or else the
// oldest
static void RTC_Push (int conn, int kind, const char *text, const char *extra)
{
	char	*t = RTC_CopyString (text), *e = RTC_CopyString (extra);

	while (atomic_flag_test_and_set_explicit (&rtc_lock, memory_order_acquire))
		;
	if (rtc_numevents == MAX_RTCEVENTS && kind == EV_LOG)
	{
		atomic_flag_clear_explicit (&rtc_lock, memory_order_release);
		free (t);
		free (e);
		return;
	}
	if (rtc_numevents == MAX_RTCEVENTS)
	{
		free (rtc_events[0].text);
		free (rtc_events[0].extra);
		memmove (rtc_events, rtc_events + 1, sizeof(rtc_events[0]) * (MAX_RTCEVENTS - 1));
		rtc_numevents--;
	}
	rtc_events[rtc_numevents++] = (rtcevent_t){conn, kind, t, e};
	atomic_flag_clear_explicit (&rtc_lock, memory_order_release);
}

// the oldest, on the game's thread; false when there is none
static bool RTC_Pop (rtcevent_t *event)
{
	bool	got = false;

	while (atomic_flag_test_and_set_explicit (&rtc_lock, memory_order_acquire))
		;
	if (rtc_numevents)
	{
		*event = rtc_events[0];
		memmove (rtc_events, rtc_events + 1, sizeof(rtc_events[0]) * (size_t)(--rtc_numevents));
		got = true;
	}
	atomic_flag_clear_explicit (&rtc_lock, memory_order_release);
	return got;
}

// a peer connection's user pointer is its connection's number, plus one
static int RTC_ConnOf (void *ptr)
{
	int		i = (int)(intptr_t)ptr - 1;

	return i >= 0 && i < MAX_RTCCONNS ? i : -1;
}

static void RTC_API RTC_OnDescription (int pc, const char *sdp, const char *type, void *ptr)
{
	(void)pc;
	RTC_Push (RTC_ConnOf (ptr), EV_DESCRIPTION, sdp, type);
}

static void RTC_API RTC_OnCandidate (int pc, const char *candidate, const char *mid, void *ptr)
{
	(void)pc;
	RTC_Push (RTC_ConnOf (ptr), EV_CANDIDATE, candidate, mid);
}

static void RTC_API RTC_OnState (int pc, rtcState state, void *ptr)
{
	int		i = RTC_ConnOf (ptr);

	(void)pc;
	if (i >= 0)
		atomic_store (&rtc_conns[i].state, (int)state);
}

// only kept when it is printed
static void RTC_API RTC_OnLog (rtcLogLevel level, const char *message)
{
	(void)level;
	if (net_rtc_debug.value >= 2)
		RTC_Push (-1, EV_LOG, message, NULL);
}

/*
===============================================================================

JSON, as browsers send descriptions and candidates

===============================================================================
*/

// text as a JSON string's contents
static void RTC_JsonEscape (char *out, size_t size, const char *text)
{
	size_t	n = 0;

	for ( ; *text && n + 7 < size ; text++)
		switch (*text)
		{
		case '"':	out[n++] = '\\'; out[n++] = '"';	break;
		case '\\':	out[n++] = '\\'; out[n++] = '\\';	break;
		case '\n':	out[n++] = '\\'; out[n++] = 'n';	break;
		case '\r':	out[n++] = '\\'; out[n++] = 'r';	break;
		case '\t':	out[n++] = '\\'; out[n++] = 't';	break;
		default:
			if ((unsigned char)*text < 32)
				n += (size_t)snprintf (out + n, size - n, "\\u%04x", (unsigned char)*text);
			else
				out[n++] = *text;
		}
	out[n] = 0;
}

// a string's value in a flat JSON object; false if it has none
static bool RTC_JsonField (const char *json, const char *key, char *out, size_t size)
{
	char		pattern[64];
	const char	*p;
	size_t		n = 0;

	snprintf (pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr (json, pattern);
	if (!p)
		return false;
	for (p += strlen (pattern) ; *p == ' ' || *p == '\t' || *p == ':' ; p++)
		;
	if (*p++ != '"')
		return false;
	for ( ; *p && *p != '"' && n + 1 < size ; p++)
	{
		if (*p != '\\')
		{
			out[n++] = *p;
			continue;
		}
		switch (*++p)
		{
		case 'n':	out[n++] = '\n';	break;
		case 'r':	out[n++] = '\r';	break;
		case 't':	out[n++] = '\t';	break;
		case 'u':
			if (p[1] && p[2] && p[3] && p[4])
			{
				char	hex[5] = {p[1], p[2], p[3], p[4], 0};

				out[n++] = (char)strtol (hex, NULL, 16);
				p += 4;
			}
			break;
		case 0:
			p--;
			break;
		default:	out[n++] = *p;	break;		// \" \\ \/
		}
	}
	out[n] = 0;
	return true;
}

/*
===============================================================================

URLS

===============================================================================
*/

static int RTC_UrlIndex (netadr_t a)
{
	return a.ip[0] | a.ip[1] << 8;
}

/*
=============
RTC_ResolveURL

rtc://broker[:port]/room or rtcs://, the broker's port 27950 unless given
=============
*/
bool RTC_ResolveURL (const char *s, netadr_t *a)
{
	rtcurl_t	url = {0};
	const char	*host, *end;
	int			port = RTC_BROKERPORT, i;

	if (!Q_strncasecmp (s, "rtcs://", 7))
	{
		url.tls = true;
		s += 7;
	}
	else if (!Q_strncasecmp (s, "rtc://", 6))
		s += 6;
	else
		return false;

	host = s;
	for (end = s ; *end && *end != ':' && *end != '/' ; end++)
		if (!((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z') || (*end >= '0' && *end <= '9')
			|| *end == '.' || *end == '-'))
			return false;
	if (end == host || end - host >= (int)sizeof(url.host))
		return false;
	for (i = 0 ; host + i < end ; i++)
		url.host[i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] + 'a' - 'A' : host[i]);
	s = end;
	if (*s == ':')
	{
		for (port = 0, s++ ; *s >= '0' && *s <= '9' ; s++)
			port = port * 10 + *s - '0';
		if (port <= 0 || port > 65535)
			return false;
	}
	// the room: a name, or udp/ip:port
	if (*s != '/' || !s[1] || strlen (s + 1) >= sizeof(url.room) || strpbrk (s + 1, " \t\r\n\"\\;?#"))
		return false;
	strcpy (url.room, s + 1);

	for (i = 0 ; i < rtc_numurls ; i++)
		if (rtc_urls[i].tls == url.tls && !strcmp (rtc_urls[i].host, url.host) && !strcmp (rtc_urls[i].room, url.room))
			break;
	if (i == rtc_numurls)
	{
		if (rtc_numurls == MAX_RTCURLS)
			return false;
		rtc_urls[rtc_numurls++] = url;
	}

	memset (a, 0, sizeof(*a));
	a->type = NA_RTC;
	a->ip[0] = (byte)i;
	a->ip[1] = (byte)(i >> 8);
	a->port = (unsigned short)BigShort ((short)port);
	return true;
}

// rtc[s]://broker[:port]/room, the port left out when it is the broker's own
// or not asked for
const char *RTC_AdrToString (netadr_t a, bool port)
{
	static char	s[2][320];
	static int	which;
	rtcurl_t	*url;
	int			number = (unsigned short)BigShort ((short)a.port);
	char		*out = s[which ^= 1];

	if (a.type != NA_RTC || RTC_UrlIndex (a) >= rtc_numurls)
		return "";
	url = &rtc_urls[RTC_UrlIndex (a)];
	if (port && number != RTC_BROKERPORT)
		snprintf (out, sizeof(s[0]), "%s://%s:%i/%s", url->tls ? "rtcs" : "rtc", url->host, number, url->room);
	else
		snprintf (out, sizeof(s[0]), "%s://%s/%s", url->tls ? "rtcs" : "rtc", url->host, url->room);
	return out;
}

/*
===============================================================================

CONNECTIONS

===============================================================================
*/

static void RTC_Debug (const char *fmt, ...)
{
	va_list	args;
	char	text[1024];

	if (!net_rtc_debug.value)
		return;
	va_start (args, fmt);
	vsnprintf (text, sizeof(text), fmt, args);
	va_end (args);
	Con_Printf ("%s", text);
}

static void RTC_Close (rtcconn_t *c)
{
	if (c->dc >= 0)
		rtcDeleteDataChannel (c->dc);
	if (c->pc >= 0)
		rtcDeletePeerConnection (c->pc);
	if (c->ws >= 0)
		rtcDeleteWebSocket (c->ws);
	c->ws = c->pc = c->dc = -1;
	c->open = false;
}

// told once, until it opens again
static void RTC_Fail (rtcconn_t *c, const char *why)
{
	if (!c->failed)
		Con_Printf ("WebRTC to %s: %s\n", RTC_AdrToString (c->adr, true), why);
	c->failed = true;
	RTC_Close (c);
}

static void RTC_Open (rtcconn_t *c, double now)
{
	rtcurl_t			*url = &rtc_urls[RTC_UrlIndex (c->adr)];
	const char			*protocols[] = {"rtc_client"};
	rtcWsConfiguration	config = {
		.disableTlsVerification = net_rtc_ignorecert.value != 0,
		.protocols = protocols,
		.protocolsCount = 1,
	};
	char				address[400];

	RTC_Close (c);
	snprintf (address, sizeof(address), "%s://%s:%i/%s/%s", url->tls ? "wss" : "ws", url->host,
		(unsigned short)BigShort ((short)c->adr.port), RTC_PROTOCOL, url->room);
	c->opened = now;
	c->peer = -1;
	atomic_store (&c->state, RTC_NEW);
	c->ws = rtcCreateWebSocketEx (address, &config);
	RTC_Debug ("WebRTC: broker %s\n", address);
	if (c->ws < 0)
		RTC_Fail (c, "the broker's address can't be opened");
}

// a channel's next message (size < 0 for text); one bigger than capacity is
// taken and dropped (size 0)
static bool RTC_Receive (int id, char *buffer, int capacity, int *size)
{
	char	*big;
	int		ret;

	*size = capacity;
	ret = rtcReceiveMessage (id, buffer, size);
	if (ret == RTC_ERR_TOO_SMALL)
	{
		*size = abs (*size) + 1;
		big = malloc ((size_t)*size);
		if (big)
			rtcReceiveMessage (id, big, size);
		free (big);
		*size = 0;
		return true;
	}
	return ret == RTC_ERR_SUCCESS;
}

static void RTC_SendBroker (rtcconn_t *c, int message, const char *text)
{
	static char	buffer[MAX_RTCMESSAGE];
	size_t		length = strlen (text);

	if (c->ws < 0 || length + 3 > sizeof(buffer))
		return;
	buffer[0] = (char)message;
	buffer[1] = (char)(c->peer & 0xff);
	buffer[2] = (char)((c->peer >> 8) & 0xff);
	memcpy (buffer + 3, text, length);
	rtcSendMessage (c->ws, buffer, (int)length + 3);
}

/*
================
RTC_StartPeer

The broker found the server: a peer connection to offer, with the broker as
STUN server and the relays it gives (FTE's turn:host:port?user=u?auth=p)
================
*/
static void RTC_StartPeer (rtcconn_t *c, const char *relays)
{
	rtcurl_t			*url = &rtc_urls[RTC_UrlIndex (c->adr)];
	char				servers[8][256];
	const char			*list[8];
	rtcConfiguration	config = {0};
	rtcDataChannelInit	init = {.reliability = {.unordered = true, .unreliable = true, .maxRetransmits = 0}};
	const char			*s, *user, *auth;
	char				token[256];
	int					count = 0, length;

	snprintf (servers[count], sizeof(servers[0]), "stun:%s:%i", url->host, (unsigned short)BigShort ((short)c->adr.port));
	list[count] = servers[count];
	count++;
	for (s = relays ; *s && count < 8 ; )
	{
		while (*s == ' ')
			s++;
		for (length = 0 ; s[length] && s[length] != ' ' && length < (int)sizeof(token) - 1 ; length++)
			token[length] = s[length];
		token[length] = 0;
		s += length;
		if (strncmp (token, "turn:", 5))
			continue;
		user = strstr (token, "?user=");
		auth = strstr (token, "?auth=");
		if (user)
			*(char *)strchr (token, '?') = 0;
		if (user && auth)
			snprintf (servers[count], sizeof(servers[0]), "turn:%.*s:%s@%s", (int)strcspn (user + 6, "?"), user + 6,
				auth + 6, token + 5);
		else
			snprintf (servers[count], sizeof(servers[0]), "%s", token);
		list[count] = servers[count];
		count++;
	}
	config.iceServers = list;
	config.iceServersCount = count;

	c->pc = rtcCreatePeerConnection (&config);
	if (c->pc < 0)
	{
		RTC_Fail (c, "no peer connection");
		return;
	}
	rtcSetUserPointer (c->pc, (void *)(intptr_t)(c - rtc_conns + 1));
	rtcSetLocalDescriptionCallback (c->pc, RTC_OnDescription);
	rtcSetLocalCandidateCallback (c->pc, RTC_OnCandidate);
	rtcSetStateChangeCallback (c->pc, RTC_OnState);
	// the channel makes the offer
	c->dc = rtcCreateDataChannelEx (c->pc, "quake", &init);
	if (c->dc < 0)
		RTC_Fail (c, "no data channel");
}

static void RTC_BrokerMessage (rtcconn_t *c, const char *data, int length)
{
	static char	text[MAX_RTCMESSAGE], value[MAX_RTCMESSAGE], mid[64];
	int			message, from;

	if (length < 3)
		return;
	message = (byte)data[0];
	from = (short)((byte)data[1] | (byte)data[2] << 8);
	memcpy (text, data + 3, (size_t)(length - 3));
	text[length - 3] = 0;

	switch (message)
	{
	case ICEMSG_NEWPEER:
		// the server's address, then the relays, each ending in a 0
		c->peer = from;
		RTC_Debug ("WebRTC: the broker found the server (%i)\n", from);
		if (c->pc < 0)
			RTC_StartPeer (c, data + 3 + strlen (text) + 1 < data + length ? data + 3 + strlen (text) + 1 : "");
		break;

	case ICEMSG_OFFER:		// the server's answer
		if (c->pc < 0)
			break;
		if (!RTC_JsonField (text, "sdp", value, sizeof(value)))
			strcpy (value, text);
		if (!RTC_JsonField (text, "type", mid, sizeof(mid)))
			strcpy (mid, "answer");
		RTC_Debug ("WebRTC: the server's %s\n", mid);
		rtcSetRemoteDescription (c->pc, value, mid);
		break;

	case ICEMSG_CANDIDATE:
		if (c->pc < 0)
			break;
		if (!RTC_JsonField (text, "candidate", value, sizeof(value)))
			strcpy (value, text);
		if (!RTC_JsonField (text, "sdpMid", mid, sizeof(mid)))
			strcpy (mid, "0");
		RTC_Debug ("WebRTC: the server's %s\n", value);
		rtcAddRemoteCandidate (c->pc, value, mid);
		break;

	case ICEMSG_PEERLOST:
		if (from == -1 || from == c->peer)
			RTC_Fail (c, *text ? text : "the broker lost the server");
		break;

	case ICEMSG_NAMEINUSE:
		RTC_Fail (c, "the broker refused the room");
		break;
	}
}

// what libdatachannel told, the broker's messages, and the connections' states
static void RTC_Poll (double now)
{
	static char	buffer[MAX_RTCMESSAGE], json[MAX_RTCMESSAGE];
	rtcevent_t	event;
	rtcconn_t	*c;
	int			i, size, state;

	while (RTC_Pop (&event))
	{
		c = event.conn >= 0 ? &rtc_conns[event.conn] : NULL;
		if (event.kind == EV_LOG)
			Con_Printf ("libdatachannel: %s\n", event.text);
		else if (c && c->used && c->pc >= 0 && event.kind == EV_DESCRIPTION)
		{
			RTC_JsonEscape (buffer, sizeof(buffer), event.text);
			snprintf (json, sizeof(json), "{\"type\":\"%s\",\"sdp\":\"%s\"}", event.extra, buffer);
			RTC_Debug ("WebRTC: our %s\n", event.extra);
			RTC_SendBroker (c, ICEMSG_OFFER, json);
		}
		else if (c && c->used && c->pc >= 0 && event.kind == EV_CANDIDATE)
		{
			RTC_JsonEscape (buffer, sizeof(buffer), event.text);
			snprintf (json, sizeof(json), "{\"candidate\":\"%s\",\"sdpMid\":\"%s\",\"sdpMLineIndex\":0}", buffer,
				event.extra);
			RTC_Debug ("WebRTC: our %s\n", event.text);
			RTC_SendBroker (c, ICEMSG_CANDIDATE, json);
		}
		free (event.text);
		free (event.extra);
	}

	for (i = 0, c = rtc_conns ; i < MAX_RTCCONNS ; i++, c++)
	{
		if (!c->used)
			continue;
		if (now - c->sent > IDLE_TIME)
		{
			RTC_Close (c);
			c->used = false;
			continue;
		}
		if (c->ws < 0)
			continue;

		// the broker's messages
		for (;;)
		{
			if (!RTC_Receive (c->ws, buffer, sizeof(buffer) - 1, &size))
				break;
			buffer[size > 0 ? size : 0] = 0;		// the relays' list ends
			if (size > 0)
				RTC_BrokerMessage (c, buffer, size);
		}
		if (c->ws < 0)
			continue;

		state = atomic_load (&c->state);
		if (!c->open && c->dc >= 0 && rtcIsOpen (c->dc))
		{
			c->open = true;
			c->failed = false;
			Con_Printf ("WebRTC to %s open\n", RTC_AdrToString (c->adr, true));
		}
		else if (state == RTC_FAILED || state == RTC_CLOSED || (c->open && state == RTC_DISCONNECTED))
			RTC_Fail (c, c->open ? "the connection was lost" : "the peers couldn't reach each other");
		else if (!c->open && rtcIsClosed (c->ws))
			RTC_Fail (c, "the broker's connection closed");
		else if (!c->open && now - c->opened > CONNECT_TIME)
			RTC_Fail (c, "no connection in time");
	}
}

/*
===============================================================================

PACKETS

===============================================================================
*/

bool RTC_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg)
{
	static char	buffer[MAX_RTCMESSAGE];
	rtcconn_t	*c;
	int			i, size;

	if (!rtc_initialized || sock != NS_CLIENT)
		return false;
	RTC_Poll (Sys_DoubleTime ());

	for (i = 0, c = rtc_conns ; i < MAX_RTCCONNS ; i++, c++)
	{
		if (!c->used || !c->open)
			continue;
		for (;;)
		{
			if (!RTC_Receive (c->dc, buffer, sizeof(buffer), &size))
				break;
			if (size <= 0)
				continue;		// text: not a packet
			if (size > msg->maxsize)
			{
				Con_Printf ("Oversize packet from %s\n", RTC_AdrToString (c->adr, true));
				continue;
			}
			memcpy (msg->data, buffer, (size_t)size);
			msg->cursize = size;
			*from = c->adr;
			return true;
		}
	}
	return false;
}

void RTC_SendPacket (netsrc_t sock, const void *data, int length, const netadr_t *to)
{
	double		now = Sys_DoubleTime ();
	rtcconn_t	*c, *unused = NULL;
	int			i;

	if (!rtc_initialized || sock != NS_CLIENT || to->type != NA_RTC)
		return;
	for (i = 0, c = rtc_conns ; i < MAX_RTCCONNS ; i++, c++)
	{
		if (c->used && NET_CompareAdr (c->adr, *to))
			break;
		if (!c->used && !unused)
			unused = c;
	}
	if (i == MAX_RTCCONNS)
	{
		if (!unused)
			return;
		c = unused;
		memset (c, 0, sizeof(*c));
		c->used = true;
		c->adr = *to;
		c->ws = c->pc = c->dc = -1;
		RTC_Open (c, now);
	}
	c->sent = now;

	if (c->open)
	{
		rtcSendMessage (c->dc, data, length);
		return;
	}
	// not open: the packet is dropped, and one that failed is tried again in a while
	if (c->ws < 0 && now - c->opened >= REOPEN_TIME)
		RTC_Open (c, now);
}

/*
===============================================================================

INIT

===============================================================================
*/

void RTC_Init (void)
{
	Cvar_RegisterVariable (&net_rtc_ignorecert);
	Cvar_RegisterVariable (&net_rtc_debug);
	rtcInitLogger (RTC_LOG_INFO, RTC_OnLog);
	rtc_initialized = true;
}

void RTC_Shutdown (void)
{
	rtcevent_t	event;
	int			i;

	if (!rtc_initialized)
		return;
	for (i = 0 ; i < MAX_RTCCONNS ; i++)
		if (rtc_conns[i].used)
			RTC_Close (&rtc_conns[i]);
	memset (rtc_conns, 0, sizeof(rtc_conns));
	rtcCleanup ();
	while (RTC_Pop (&event))
	{
		free (event.text);
		free (event.extra);
	}
	rtc_initialized = false;
}
