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

// which end of a connection a socket or channel belongs to; each end has
// its own UDP socket, and a loopback to the other end in the same process
typedef enum { NS_CLIENT, NS_SERVER } netsrc_t;

typedef enum { NA_INVALID, NA_LOOPBACK, NA_IP } netadrtype_t;

typedef struct
{
	netadrtype_t	type;
	byte	ip[4];
	unsigned short	port;		// network byte order
} netadr_t;

void	NET_Init (void);
void	NET_Shutdown (void);

// opens the end's UDP socket, false if the port can't be bound; the loopback
// works without it
bool	NET_OpenSocket (netsrc_t sock, int port);
void	NET_CloseSocket (netsrc_t sock);
netadr_t	NET_SocketAddress (netsrc_t sock);	// type NA_INVALID without a socket

// the next packet for this end, from the loopback first, then the socket
bool	NET_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg);
void	NET_SendPacket (netsrc_t sock, int length, const void *data, netadr_t to);

bool	NET_CompareAdr (netadr_t a, netadr_t b);
bool	NET_CompareBaseAdr (netadr_t a, netadr_t b);
bool	NET_IsLocalAddress (netadr_t a);	// the loopback or this machine
char	*NET_AdrToString (netadr_t a);
char	*NET_BaseAdrToString (netadr_t a);
bool	NET_StringToAdr (const char *s, netadr_t *a);	// "local" is the loopback

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

bool Netchan_CanPacket (netchan_t *chan);
bool Netchan_CanReliable (netchan_t *chan);
