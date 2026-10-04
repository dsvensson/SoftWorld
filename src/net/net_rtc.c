// net_rtc.c -- WebRTC over libdatachannel (net_rtc.h), as FTE's clients reach
// servers through a broker, and as FTE's servers take clients through one
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
// A server keeps a WebSocket to its broker as a room's host (rtc_host), the
// room it is given or else its invitation code (eight digits made once a run,
// others while another server has them): the broker greets it (GREETING),
// tells it of each client that comes (NEWPEER)
// and passes the client's offer, which the server answers. The server opens a
// "quake" channel of its own too, and sends on the client's when there is
// one, or else on its own: FTE's native clients open none, and take the
// other side's (as FTE's servers in a browser send on theirs, which the
// client reads beside its own). Every 30 s the server tells the broker
// what it is (SERVERINFO), for the broker's list of servers. Its clients are
// by the address their packets come from, IPv4's or IPv6's (NA_RTCCLIENT), as
// its UDP clients are; a lost broker loses no client already in.
//
// Nothing here waits: what libdatachannel's threads tell (the descriptions and
// candidates to send, the connections' states, the clients' channels, its
// log) is queued or stored for the game's thread, and the broker's messages
// and the channels' packets are taken as they wait. A packet sent before the
// channel is open is dropped, as a network drops it; the client asks for a
// challenge again in a while.

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
#define	MAX_RTCCONNS	32				// the client's connections to servers
#define	MAX_RTCPEERS	32				// the server's clients, connecting or in
#define	REOPEN_TIME		5.0				// seconds before a failed connection is tried again
#define	HOST_REOPEN_TIME	10.0		// and the server's broker connection
#define	CONNECT_TIME	20.0			// seconds a connection may take to open
#define	IDLE_TIME		30.0			// seconds without a packet sent (or come) before one is closed
#define	INFO_TIME		30.0			// seconds between the server's SERVERINFO, as FTE's
#define	MAX_RTCMESSAGE	65536
#define	RTC_NOPEER		-65536			// no broker's number (theirs are shorts)

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

// the rooms by URL, an NA_RTC's ip[0] and ip[1] their number
typedef struct
{
	bool	tls;				// rtcs://
	char	host[128];
	char	room[128];			// after the broker's address, without the '/'
} rtcurl_t;

static rtcurl_t	rtc_urls[MAX_RTCURLS];
static int		rtc_numurls;

// the client's connection to a server
typedef struct
{
	bool		used;
	netadr_t	adr;			// NA_RTC
	int			ws;				// the broker's WebSocket, -1 for none
	int			pc;				// the peer connection, -1 for none
	int			dc;				// its data channel, -1 for none
	_Atomic int	extra;			// a channel the server opened, -1 for none
	int			peer;			// the broker's number of the other side
	bool		open;			// the data channel is
	bool		failed;			// told it failed
	double		opened;			// when it was last opened
	double		sent;			// when a packet was last sent
	_Atomic int	state;			// the peer connection's (rtcState), from libdatachannel's threads
} rtcconn_t;

static rtcconn_t	rtc_conns[MAX_RTCCONNS];

// a client of the server's
typedef struct
{
	bool		used;
	int			peer;			// the broker's number of it
	int			pc;				// the peer connection
	int			own;			// the server's channel to it, -1 for none
	bool		open;			// a channel is, and adr known
	netadr_t	adr;			// NA_RTCCLIENT, once open
	double		started;		// when the broker told of it
	double		heard;			// when a packet last came
	_Atomic int	state;			// the peer connection's (rtcState)
	_Atomic int	channel;		// the client's data channel, -1 until it comes
} rtcpeer_t;

// the room the server hosts
static struct
{
	netadr_t	adr;			// NA_RTC; NA_INVALID for none
	char		broker[128];	// rtc://broker[:port] or rtcs://
	bool		code;			// the room is the invitation code
	int			ws;				// the broker's WebSocket, -1 for none
	bool		greeted;		// the broker took the room
	bool		failed;			// told it failed
	double		opened;			// when the WebSocket was last opened
	double		infosent;
	char		info[2048];		// what the server tells the broker it is
	rtcpeer_t	peers[MAX_RTCPEERS];
} rtc_host = {.ws = -1};

static char	rtc_code[9];		// this run's invitation code, once a room is

static bool	rtc_initialized;

/*
===============================================================================

WHAT LIBDATACHANNEL'S THREADS TELL

===============================================================================
*/

enum { EV_DESCRIPTION, EV_CANDIDATE, EV_LOG };

typedef struct
{
	int		conn;		// RTC_ConnOf's number, -1 for none
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

// a peer connection's user pointer is its number, plus one: the client's
// connections, then the server's clients
static int RTC_ConnOf (void *ptr)
{
	int		i = (int)(intptr_t)ptr - 1;

	return i >= 0 && i < MAX_RTCCONNS + MAX_RTCPEERS ? i : -1;
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
	if (i >= 0 && i < MAX_RTCCONNS)
		atomic_store (&rtc_conns[i].state, (int)state);
	else if (i >= MAX_RTCCONNS)
		atomic_store (&rtc_host.peers[i - MAX_RTCCONNS].state, (int)state);
}

// a channel the other side opened: a client's to the server, or a server's
// of its own to the client; the first is taken
static void RTC_API RTC_OnDataChannel (int pc, int dc, void *ptr)
{
	int		i = RTC_ConnOf (ptr), none = -1;

	(void)pc;
	if (i >= 0 && i < MAX_RTCCONNS)
		atomic_compare_exchange_strong (&rtc_conns[i].extra, &none, dc);
	else if (i >= MAX_RTCCONNS)
		atomic_compare_exchange_strong (&rtc_host.peers[i - MAX_RTCCONNS].channel, &none, dc);
}

// only kept when it is printed
static void RTC_API RTC_OnLog (rtcLogLevel level, const char *message)
{
	(void)level;
	if (net_rtc_debug.value >= 2)
		RTC_Push (-1, EV_LOG, message, NULL);
}

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

// the broker's WebSocket for a room, as a client or as its host
static int RTC_OpenBroker (netadr_t adr, const char *protocol)
{
	rtcurl_t			*url = &rtc_urls[RTC_UrlIndex (adr)];
	const char			*protocols[] = {protocol};
	rtcWsConfiguration	config = {
		.disableTlsVerification = net_rtc_ignorecert.value != 0,
		.protocols = protocols,
		.protocolsCount = 1,
	};
	char				address[400];

	snprintf (address, sizeof(address), "%s://%s:%i/%s/%s", url->tls ? "wss" : "ws", url->host,
		(unsigned short)BigShort ((short)adr.port), RTC_PROTOCOL, url->room);
	RTC_Debug ("WebRTC: broker %s (%s)\n", address, protocol);
	return rtcCreateWebSocketEx (address, &config);
}

/*
===============================================================================

CONNECTIONS

===============================================================================
*/

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

// the next packet on a data channel, from adr; false when none waits
static bool RTC_ChannelPacket (int dc, netadr_t adr, netadr_t *from, sizebuf_t *msg)
{
	static char	buffer[MAX_RTCMESSAGE];
	int			size;

	while (RTC_Receive (dc, buffer, sizeof(buffer), &size))
	{
		if (size <= 0)
			continue;		// text: not a packet
		if (size > msg->maxsize)
		{
			Con_Printf ("Oversize packet from %s\n", NET_AdrToString (adr));
			continue;
		}
		memcpy (msg->data, buffer, (size_t)size);
		msg->cursize = size;
		*from = adr;
		return true;
	}
	return false;
}

// a message to the broker: what, to whom, and its text
static void RTC_SendBroker (int ws, int peer, int message, const char *text)
{
	static char	buffer[MAX_RTCMESSAGE];
	size_t		length = strlen (text);

	if (ws < 0 || length + 3 > sizeof(buffer))
		return;
	buffer[0] = (char)message;
	buffer[1] = (char)(peer & 0xff);
	buffer[2] = (char)((peer >> 8) & 0xff);
	memcpy (buffer + 3, text, length);
	rtcSendMessage (ws, buffer, (int)length + 3);
}

// a broker's message: what, from whom, and its text (the text ends in a 0,
// and NEWPEER's has the relays after it)
static bool RTC_BrokerText (const char *data, int length, int *message, int *from, char *text)
{
	if (length < 3)
		return false;
	*message = (byte)data[0];
	*from = (short)((byte)data[1] | (byte)data[2] << 8);
	memcpy (text, data + 3, (size_t)(length - 3));
	text[length - 3] = 0;
	return true;
}

// a NEWPEER's relays, after its text
static const char *RTC_Relays (const char *data, int length)
{
	const char	*relays = data + 3 + strlen (data + 3) + 1;

	return relays < data + length ? relays : "";
}

// a description's or candidate's text and its second field from the JSON a
// browser sends, or as it is
static void RTC_JsonPair (const char *text, const char *key, const char *key2, const char *default2, char *value,
	size_t size, char *value2, size_t size2)
{
	if (!RTC_JsonField (text, key, value, size))
		Q_strncpyz (value, text, size);
	if (!RTC_JsonField (text, key2, value2, size2))
		Q_strncpyz (value2, default2, size2);
}

// the ICE servers for a peer connection: the broker as STUN server, and the
// relays it gives (FTE's turn:host:port?user=u?auth=p, between spaces)
typedef struct
{
	char		servers[8][256];
	const char	*list[8];
} rtcservers_t;

static void RTC_IceServers (rtcConfiguration *config, rtcservers_t *s, netadr_t broker, const char *relays)
{
	const char	*user, *auth;
	char		token[256];
	int			count = 0, length;

	snprintf (s->servers[count], sizeof(s->servers[0]), "stun:%s:%i", rtc_urls[RTC_UrlIndex (broker)].host,
		(unsigned short)BigShort ((short)broker.port));
	s->list[count] = s->servers[count];
	count++;
	while (*relays && count < 8)
	{
		while (*relays == ' ')
			relays++;
		for (length = 0 ; relays[length] && relays[length] != ' ' && length < (int)sizeof(token) - 1 ; length++)
			token[length] = relays[length];
		token[length] = 0;
		relays += length;
		if (strncmp (token, "turn:", 5))
			continue;
		user = strstr (token, "?user=");
		auth = strstr (token, "?auth=");
		if (user)
			*strchr (token, '?') = 0;
		if (user && auth)
			snprintf (s->servers[count], sizeof(s->servers[0]), "turn:%.*s:%s@%s", (int)strcspn (user + 6, "?"),
				user + 6, auth + 6, token + 5);
		else
			snprintf (s->servers[count], sizeof(s->servers[0]), "%s", token);
		s->list[count] = s->servers[count];
		count++;
	}
	config->iceServers = s->list;
	config->iceServersCount = count;
}

// the descriptions and candidates libdatachannel made, through the broker to
// the other side; its log
static void RTC_PollEvents (void)
{
	static char	buffer[MAX_RTCMESSAGE], json[MAX_RTCMESSAGE];
	rtcevent_t	event;
	rtcpeer_t	*p;
	int			ws, peer;

	while (RTC_Pop (&event))
	{
		ws = peer = -1;
		if (event.conn >= 0 && event.conn < MAX_RTCCONNS && rtc_conns[event.conn].used
			&& rtc_conns[event.conn].pc >= 0)
		{
			ws = rtc_conns[event.conn].ws;
			peer = rtc_conns[event.conn].peer;
		}
		else if (event.conn >= MAX_RTCCONNS)
		{
			p = &rtc_host.peers[event.conn - MAX_RTCCONNS];
			if (p->used && p->peer != RTC_NOPEER)
			{
				ws = rtc_host.ws;
				peer = p->peer;
			}
		}

		if (event.kind == EV_LOG)
			Con_Printf ("libdatachannel: %s\n", event.text);
		else if (ws >= 0 && event.kind == EV_DESCRIPTION)
		{
			RTC_JsonEscape (buffer, sizeof(buffer), event.text);
			snprintf (json, sizeof(json), "{\"type\":\"%s\",\"sdp\":\"%s\"}", event.extra, buffer);
			RTC_Debug ("WebRTC: our %s (%i)\n", event.extra, peer);
			RTC_SendBroker (ws, peer, ICEMSG_OFFER, json);
		}
		else if (ws >= 0 && event.kind == EV_CANDIDATE)
		{
			RTC_JsonEscape (buffer, sizeof(buffer), event.text);
			snprintf (json, sizeof(json), "{\"candidate\":\"%s\",\"sdpMid\":\"%s\",\"sdpMLineIndex\":0}", buffer,
				event.extra);
			RTC_Debug ("WebRTC: our %s\n", event.text);
			RTC_SendBroker (ws, peer, ICEMSG_CANDIDATE, json);
		}
		free (event.text);
		free (event.extra);
	}
}

/*
===============================================================================

THE CLIENT'S CONNECTIONS

===============================================================================
*/

static void RTC_Close (rtcconn_t *c)
{
	int		extra = atomic_exchange (&c->extra, -1);

	if (extra >= 0)
		rtcDeleteDataChannel (extra);
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
	RTC_Close (c);
	c->opened = now;
	c->peer = -1;
	atomic_store (&c->state, RTC_NEW);
	c->ws = RTC_OpenBroker (c->adr, "rtc_client");
	if (c->ws < 0)
		RTC_Fail (c, "the broker's address can't be opened");
}

/*
================
RTC_StartPeer

The broker found the server: a peer connection to offer
================
*/
static void RTC_StartPeer (rtcconn_t *c, const char *relays)
{
	rtcConfiguration	config = {0};
	rtcservers_t		servers;
	rtcDataChannelInit	init = {.reliability = {.unordered = true, .unreliable = true, .maxRetransmits = 0}};

	RTC_IceServers (&config, &servers, c->adr, relays);
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
	rtcSetDataChannelCallback (c->pc, RTC_OnDataChannel);
	// the channel makes the offer
	c->dc = rtcCreateDataChannelEx (c->pc, "quake", &init);
	if (c->dc < 0)
		RTC_Fail (c, "no data channel");
}

static void RTC_BrokerMessage (rtcconn_t *c, const char *data, int length)
{
	static char	text[MAX_RTCMESSAGE], value[MAX_RTCMESSAGE];
	char		mid[64];
	int			message, from;

	if (!RTC_BrokerText (data, length, &message, &from, text))
		return;

	switch (message)
	{
	case ICEMSG_NEWPEER:	// the server's address, then the relays
		c->peer = from;
		RTC_Debug ("WebRTC: the broker found the server (%i)\n", from);
		if (c->pc < 0)
			RTC_StartPeer (c, RTC_Relays (data, length));
		break;

	case ICEMSG_OFFER:		// the server's answer
		if (c->pc < 0)
			break;
		RTC_JsonPair (text, "sdp", "type", "answer", value, sizeof(value), mid, sizeof(mid));
		RTC_Debug ("WebRTC: the server's %s\n", mid);
		rtcSetRemoteDescription (c->pc, value, mid);
		break;

	case ICEMSG_CANDIDATE:
		if (c->pc < 0)
			break;
		RTC_JsonPair (text, "candidate", "sdpMid", "0", value, sizeof(value), mid, sizeof(mid));
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

// the broker's messages, and the connections' states
static void RTC_Poll (double now)
{
	static char	buffer[MAX_RTCMESSAGE];
	rtcconn_t	*c;
	int			i, size, state;

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

THE SERVER'S ROOM

===============================================================================
*/

static void RTC_ClosePeer (rtcpeer_t *p)
{
	int		dc = atomic_exchange (&p->channel, -1);

	if (dc >= 0)
		rtcDeleteDataChannel (dc);
	if (p->own >= 0)
		rtcDeleteDataChannel (p->own);
	p->own = -1;
	if (p->pc >= 0)
		rtcDeletePeerConnection (p->pc);
	p->pc = -1;
	p->open = false;
	p->used = false;
}

// the broker's connection; the clients in stay
static void RTC_CloseBroker (void)
{
	if (rtc_host.ws >= 0)
		rtcDeleteWebSocket (rtc_host.ws);
	rtc_host.ws = -1;
	rtc_host.greeted = false;
}

// told once, until the broker greets it again; opened again in a while
static void RTC_HostFail (const char *why)
{
	if (!rtc_host.failed)
		Con_Printf ("WebRTC room %s: %s\n", RTC_AdrToString (rtc_host.adr, true), why);
	rtc_host.failed = true;
	RTC_CloseBroker ();
}

static void RTC_OpenHost (double now)
{
	RTC_CloseBroker ();
	rtc_host.opened = now;
	rtc_host.ws = RTC_OpenBroker (rtc_host.adr, "rtc_host");
	if (rtc_host.ws < 0)
		RTC_HostFail ("the broker's address can't be opened");
}

static void RTC_SendInfo (double now)
{
	if (rtc_host.greeted && *rtc_host.info)
		RTC_SendBroker (rtc_host.ws, -1, ICEMSG_SERVERINFO, rtc_host.info);
	rtc_host.infosent = now;
}

static void RTC_NewCode (void)
{
	snprintf (rtc_code, sizeof(rtc_code), "%08u", Sys_Seed () % 100000000u);
}

// the room at the broker as an address; told when it isn't one
static bool RTC_RoomAddress (const char *broker, const char *room, netadr_t *a)
{
	char	url[256];

	if (NET_RoomURL (broker, room, url, sizeof(url)) && RTC_ResolveURL (url, a))
		return true;
	Con_Printf ("WebRTC: room %s at %s isn't a broker's room (rtc://broker[:port]/room)\n", room, broker);
	a->type = NA_INVALID;
	return false;
}

// "address:port", IPv6's address in [ ] or not (its zone left out:
// fe80::1%eth0), as an NA_RTCCLIENT
static bool RTC_ClientAddress (const char *s, netadr_t *a)
{
	char		host[64];
	const char	*colon = strrchr (s, ':'), *zone;
	int			length, port;

	if (!colon)
		return false;
	length = (int)(colon - s);
	if (*s == '[' && length >= 2 && s[length - 1] == ']')
	{
		s++;
		length -= 2;
	}
	zone = memchr (s, '%', (size_t)(length > 0 ? length : 0));
	if (zone)
		length = (int)(zone - s);
	if (length <= 0 || length >= (int)sizeof(host))
		return false;
	memcpy (host, s, (size_t)length);
	host[length] = 0;
	port = atoi (colon + 1);
	if (port <= 0 || port > 65535)
		return false;
	memset (a, 0, sizeof(*a));
	if (!NET_ParseIP (host, a->ip))
		return false;
	a->type = NA_RTCCLIENT;
	a->port = (unsigned short)BigShort ((short)port);
	return true;
}

// the channel to send a client its packets on: its own, or else the server's;
// -1 while neither is open
static int RTC_PeerChannel (rtcpeer_t *p)
{
	int		dc = atomic_load (&p->channel);

	if (dc >= 0 && rtcIsOpen (dc))
		return dc;
	return p->own >= 0 && rtcIsOpen (p->own) ? p->own : -1;
}

// a client the broker told of, to answer
static void RTC_NewPeer (int from, const char *relays, double now)
{
	rtcConfiguration	config = {0};
	rtcservers_t		servers;
	rtcpeer_t			*p;
	int					i;

	for (i = 0 ; i < MAX_RTCPEERS && rtc_host.peers[i].used ; i++)
		;
	if (i == MAX_RTCPEERS)
	{
		RTC_Debug ("WebRTC: no room for another client (%i)\n", from);
		return;
	}
	p = &rtc_host.peers[i];
	memset (&p->adr, 0, sizeof(p->adr));
	p->used = true;
	p->peer = from;
	p->own = -1;
	p->open = false;
	p->started = now;
	atomic_store (&p->state, RTC_NEW);
	atomic_store (&p->channel, -1);

	RTC_IceServers (&config, &servers, rtc_host.adr, relays);
	p->pc = rtcCreatePeerConnection (&config);
	if (p->pc < 0)
	{
		p->used = false;
		return;
	}
	rtcSetUserPointer (p->pc, (void *)(intptr_t)(MAX_RTCCONNS + i + 1));
	rtcSetLocalDescriptionCallback (p->pc, RTC_OnDescription);
	rtcSetLocalCandidateCallback (p->pc, RTC_OnCandidate);
	rtcSetStateChangeCallback (p->pc, RTC_OnState);
	rtcSetDataChannelCallback (p->pc, RTC_OnDataChannel);
	RTC_Debug ("WebRTC: a client comes (%i)\n", from);
}

static void RTC_HostMessage (const char *data, int length, double now)
{
	static char	text[MAX_RTCMESSAGE], value[MAX_RTCMESSAGE];
	char		mid[64];
	rtcpeer_t	*p = NULL;
	rtcDataChannelInit	init = {.reliability = {.unordered = true, .unreliable = true, .maxRetransmits = 0}};
	int			message, from, i;

	if (!RTC_BrokerText (data, length, &message, &from, text))
		return;
	for (i = 0 ; i < MAX_RTCPEERS ; i++)
		if (rtc_host.peers[i].used && rtc_host.peers[i].peer == from)
			p = &rtc_host.peers[i];

	switch (message)
	{
	case ICEMSG_GREETING:	// the broker took the room
		if (!rtc_host.greeted && rtc_host.code)
			Con_Printf ("Invitation code: %.4s-%.4s\n", rtc_code, rtc_code + 4);
		else if (!rtc_host.greeted)
			Con_Printf ("WebRTC room %s is this server's\n", RTC_AdrToString (rtc_host.adr, true));
		rtc_host.greeted = true;
		rtc_host.failed = false;
		RTC_SendInfo (now);
		break;

	case ICEMSG_NEWPEER:	// a client: its address, then the relays
		// a number the broker gives again: one still connecting with it is
		// gone, one in keeps its channel
		if (p && !p->open)
			RTC_ClosePeer (p);
		else if (p)
			p->peer = RTC_NOPEER;
		RTC_NewPeer (from, RTC_Relays (data, length), now);
		break;

	case ICEMSG_OFFER:		// the client's, which libdatachannel answers
		if (!p || p->pc < 0)
			break;
		RTC_JsonPair (text, "sdp", "type", "offer", value, sizeof(value), mid, sizeof(mid));
		RTC_Debug ("WebRTC: the client's %s (%i)\n", mid, from);
		rtcSetRemoteDescription (p->pc, value, mid);
		// the server's own channel, once answered (before, it would offer)
		if (p->own < 0)
			p->own = rtcCreateDataChannelEx (p->pc, "quake", &init);
		break;

	case ICEMSG_CANDIDATE:
		if (!p || p->pc < 0)
			break;
		RTC_JsonPair (text, "candidate", "sdpMid", "0", value, sizeof(value), mid, sizeof(mid));
		RTC_Debug ("WebRTC: the client's %s\n", value);
		rtcAddRemoteCandidate (p->pc, value, mid);
		break;

	case ICEMSG_PEERLOST:
		// the broker going; a client gone from it before its channel opened
		if (from == -1)
			RTC_HostFail (*text ? text : "the broker closed the room");
		else if (p && !p->open)
			RTC_ClosePeer (p);
		break;

	case ICEMSG_NAMEINUSE:
		// another server's code: another, taken at once
		if (rtc_host.code)
		{
			RTC_NewCode ();
			RTC_CloseBroker ();
			RTC_RoomAddress (rtc_host.broker, rtc_code, &rtc_host.adr);
			rtc_host.opened = now - HOST_REOPEN_TIME;
		}
		else
			RTC_HostFail ("another server has the room");
		break;
	}
}

// the broker's messages, and the clients' states
static void RTC_HostPoll (double now)
{
	static char	buffer[MAX_RTCMESSAGE];
	char		address[96];
	rtcpeer_t	*p;
	int			i, size, state, dc;

	if (rtc_host.adr.type != NA_RTC)
		return;

	if (rtc_host.ws < 0 && now - rtc_host.opened >= HOST_REOPEN_TIME)
		RTC_OpenHost (now);
	while (rtc_host.ws >= 0 && RTC_Receive (rtc_host.ws, buffer, sizeof(buffer) - 1, &size))
	{
		buffer[size > 0 ? size : 0] = 0;		// the relays' list ends
		if (size > 0)
			RTC_HostMessage (buffer, size, now);
	}
	if (rtc_host.ws >= 0 && rtcIsClosed (rtc_host.ws))
		RTC_HostFail (rtc_host.greeted ? "the broker's connection closed" : "the broker can't be reached");
	else if (rtc_host.ws >= 0 && !rtc_host.greeted && now - rtc_host.opened > CONNECT_TIME)
		RTC_HostFail ("the broker didn't take the room in time");
	else if (rtc_host.greeted && now - rtc_host.infosent >= INFO_TIME)
		RTC_SendInfo (now);

	for (i = 0, p = rtc_host.peers ; i < MAX_RTCPEERS ; i++, p++)
	{
		if (!p->used)
			continue;
		state = atomic_load (&p->state);
		dc = RTC_PeerChannel (p);
		if (!p->open && dc >= 0)
		{
			// by where its packets come from
			if (rtcGetRemoteAddress (p->pc, address, sizeof(address)) < 0 || !RTC_ClientAddress (address, &p->adr))
			{
				RTC_Debug ("WebRTC: a client without an address (%i)\n", p->peer);
				RTC_ClosePeer (p);
				continue;
			}
			p->open = true;
			p->heard = now;
			RTC_Debug ("WebRTC: client %s open (%i)\n", NET_AdrToString (p->adr), p->peer);
		}
		else if (state == RTC_FAILED || state == RTC_CLOSED || (p->open && state == RTC_DISCONNECTED)
			|| (p->open && dc < 0) || (p->open && now - p->heard > IDLE_TIME)
			|| (!p->open && now - p->started > CONNECT_TIME))
		{
			RTC_Debug ("WebRTC: client %s gone (%i)\n", p->open ? NET_AdrToString (p->adr) : "", p->peer);
			RTC_ClosePeer (p);
		}
	}
}

/*
=============
RTC_Host

The room the server hosts at the broker, rtc://broker[:port] or rtcs://: its
name, or "" for the invitation code; NULL for none. The same room again keeps
its connection and its clients.
=============
*/
bool RTC_Host (const char *broker, const char *room)
{
	netadr_t	adr = {.type = NA_INVALID};
	bool		code = false;
	int			i;

	if (!rtc_initialized)
		return false;
	if (broker && room)
	{
		code = !*room;
		if (code && !*rtc_code)
			RTC_NewCode ();
		RTC_RoomAddress (broker, code ? rtc_code : room, &adr);
	}
	if (NET_CompareAdr (adr, rtc_host.adr))
		return adr.type == NA_RTC;

	RTC_CloseBroker ();
	for (i = 0 ; i < MAX_RTCPEERS ; i++)
		if (rtc_host.peers[i].used)
			RTC_ClosePeer (&rtc_host.peers[i]);
	rtc_host.adr = adr;
	rtc_host.code = code;
	Q_strncpyz (rtc_host.broker, broker ? broker : "", sizeof(rtc_host.broker));
	rtc_host.failed = false;
	if (adr.type == NA_RTC)
		RTC_OpenHost (Sys_DoubleTime ());
	return adr.type == NA_RTC;
}

void RTC_HostInfo (const char *info)
{
	Q_strncpyz (rtc_host.info, info, sizeof(rtc_host.info));
}

/*
===============================================================================

PACKETS

===============================================================================
*/

bool RTC_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg)
{
	double		now;
	rtcconn_t	*c;
	rtcpeer_t	*p;
	int			i;

	if (!rtc_initialized)
		return false;
	now = Sys_DoubleTime ();
	RTC_PollEvents ();

	if (sock == NS_SERVER)
	{
		RTC_HostPoll (now);
		for (i = 0, p = rtc_host.peers ; i < MAX_RTCPEERS ; i++, p++)
			if (p->used && p->open && ((atomic_load (&p->channel) >= 0
				&& RTC_ChannelPacket (atomic_load (&p->channel), p->adr, from, msg))
				|| (p->own >= 0 && RTC_ChannelPacket (p->own, p->adr, from, msg))))
			{
				p->heard = now;
				return true;
			}
		return false;
	}

	RTC_Poll (now);
	for (i = 0, c = rtc_conns ; i < MAX_RTCCONNS ; i++, c++)
		if (c->used && c->open && (RTC_ChannelPacket (c->dc, c->adr, from, msg)
			|| (atomic_load (&c->extra) >= 0 && RTC_ChannelPacket (atomic_load (&c->extra), c->adr, from, msg))))
			return true;
	return false;
}

void RTC_SendPacket (netsrc_t sock, const void *data, int length, const netadr_t *to)
{
	double		now = Sys_DoubleTime ();
	rtcconn_t	*c, *unused = NULL;
	rtcpeer_t	*p;
	int			i;

	if (!rtc_initialized)
		return;
	// the server's clients
	if (sock == NS_SERVER)
	{
		for (i = 0, p = rtc_host.peers ; i < MAX_RTCPEERS ; i++, p++)
			if (p->used && p->open && to->type == NA_RTCCLIENT && NET_CompareAdr (p->adr, *to))
			{
				if (RTC_PeerChannel (p) >= 0)
					rtcSendMessage (RTC_PeerChannel (p), data, length);
				return;
			}
		return;
	}

	if (to->type != NA_RTC)
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
		atomic_store (&c->extra, -1);
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
	RTC_Host (NULL, NULL);
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
