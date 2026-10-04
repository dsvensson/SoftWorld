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

#pragma once

#include "msg.h"
#include "protocol.h"
#include "q_types.h"
#include "cvar.h"
// net.h -- quake's interface to the networking layer

#define	PORT_ANY	-1
#define	MAX_UDP_PACKET	(MAX_MSGLEN*2)	// one more than msg + header
#define	MAX_FRAGMENTED	65536			// a packet put together of fragments, at most (FTE's)

// which end of a connection a socket or channel belongs to; each end has
// its own UDP socket, and a loopback to the other end in the same process
typedef enum { NS_CLIENT, NS_SERVER } netsrc_t;

// NA_WS: a client on the server's WebSocket port (net_ws.c), by the address
// and port it connected from; it prints and compares as NA_IP, but packets to
// it go through its connection. NA_URL: a server a browser reaches by URL
// (ws://, wss://, and rtc:// and rtcs:// over the browser's WebRTC: the web's
// net_ws_web.c), ip the URL's number there.
// NA_RTC: a peer over WebRTC (net_rtc.c), ip its URL's number, port the
// broker's. NA_RTCCLIENT: a client of the server's over WebRTC, by the
// address and port its packets come from; it prints and compares as NA_IP,
// but packets to it go through its data channel.
typedef enum { NA_INVALID, NA_LOOPBACK, NA_IP, NA_WS, NA_URL, NA_RTC, NA_RTCCLIENT } netadrtype_t;

typedef struct
{
	netadrtype_t	type;
	byte	ip[4];
	unsigned short	port;		// network byte order
} netadr_t;

// the passwords of a server, and the ones its clients give; a listen
// server's own client uses the same ones
extern	cvar_t	password;		// for entering the game (userinfo "password")
extern	cvar_t	rcon_password;	// for remote console commands

void	NET_Init (void);
void	NET_Shutdown (void);

// opens the end's UDP socket, false if the port can't be bound; the loopback
// works without it
bool	NET_OpenSocket (netsrc_t sock, int port);
void	NET_CloseSocket (netsrc_t sock);
netadr_t	NET_SocketAddress (netsrc_t sock);	// type NA_INVALID without a socket, or a browser's

// the server's WebSocket port (net_ws.c): TCP at the port number of its UDP,
// for browsers' clients; true if it listens there. Closing the server's socket
// closes it too.
bool	NET_ListenWebSocket (int port);
void	NET_CloseWebSocket (void);
// the room the server takes clients in over WebRTC at net_webrtc_broker: its
// name, "" for this run's invitation code, NULL for none (the same room again
// keeps it), and what the server tells the broker it is, an info string
// (net_rtc.c); false where the build has no WebRTC
bool	NET_HostRTC (const char *room);
void	NET_RTCInfo (const char *info);
// a broker's room as a URL, rtc://broker[:port] or rtcs:// and /room; false
// if the broker isn't one, or the URL doesn't fit
bool	NET_RoomURL (const char *broker, const char *room, char *url, int size);

// the next packet for this end, from the loopback first, then the socket
bool	NET_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg);
void	NET_SendPacket (netsrc_t sock, int length, const void *data, netadr_t to);

bool	NET_CompareAdr (netadr_t a, netadr_t b);
bool	NET_CompareBaseAdr (netadr_t a, netadr_t b);
bool	NET_IsLocalAddress (netadr_t a);	// the loopback or this machine
char	*NET_AdrToString (netadr_t a);
char	*NET_BaseAdrToString (netadr_t a);
bool	NET_StringToAdr (const char *s, netadr_t *a);	// "local" is the loopback, ws:// a URL

// the mtu to ask a server for fragments of (FTE's PROTOCOL_VERSION_FRAGMENT) on
// the path to it, 0 for none: WebRTC's, over which FTE's servers send a packet
// in one piece, and too big a one is lost
int		NET_FragmentMTU (netadr_t a);

//============================================================================

#define	OLD_AVG		0.99		// total = oldtotal*OLD_AVG + new*(1-OLD_AVG)

#define	MAX_LATENT	32

typedef struct
{
	bool	fatal_error;

	float		last_received;		// for timeouts

// the statistics are cleared at each client begin, because
// the server connecting process gives a bogus picture of the data
	float		frame_latency;		// rolling average
	float		frame_rate;

	int			drop_count;			// dropped packets, cleared each level
	int			good_count;			// cleared each level
	int			dropped;			// packets dropped before the last one processed

	netadr_t	remote_address;
	netsrc_t	sock;			// NS_CLIENT channels send the qport, NS_SERVER read it
	int			qport;
	int			fragmtu;		// FTE's fragmentation agreed, at this mtu; 0 for none

// bandwidth estimator
	double		cleartime;			// if realtime > nc->cleartime, free to go
	double		rate;				// seconds / byte

// sequencing variables
	int			incoming_sequence;
	int			incoming_acknowledged;
	int			incoming_reliable_acknowledged;	// single bit

	int			incoming_reliable_sequence;		// single bit, maintained local

	int			outgoing_sequence;
	int			reliable_sequence;			// single bit
	int			last_reliable_sequence;		// sequence number of last send

// reliable staging and holding areas
	sizebuf_t	message;		// writing buffer to send to server
	byte		message_buf[MAX_MSGLEN];

	int			reliable_length;
	byte		reliable_buf[MAX_MSGLEN];	// unacked reliable message

// time and size data to calculate bandwidth
	int			outgoing_size[MAX_LATENT];
	double		outgoing_time[MAX_LATENT];
} netchan_t;

void Netchan_Init (void);
void Netchan_Transmit (netchan_t *chan, int length, byte *data);
void Netchan_OutOfBand (netsrc_t sock, netadr_t adr, int length, byte *data);
void Netchan_OutOfBandPrint (netsrc_t sock, netadr_t adr, char *format, ...);
// true if msg, which came from 'from', is the channel's next packet; reading
// continues after its header
bool Netchan_Process (netchan_t *chan, netadr_t from, sizebuf_t *msg);
void Netchan_Setup (netchan_t *chan, netadr_t adr, int qport, netsrc_t sock);
// FTE's fragmentation, as both ends agreed in the connection (an mtu, 0 for
// none): every packet has an offset after its header, and the client's end
// puts the pieces together (one channel at a time: a client's)
void Netchan_SetFragments (netchan_t *chan, int mtu);

bool Netchan_CanPacket (netchan_t *chan);
bool Netchan_CanReliable (netchan_t *chan);
