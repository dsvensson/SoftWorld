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

#include "cvar.h"
#include "host.h"
#include "msg.h"
#include "net.h"
#include "print.h"
#include "q_string.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define	PACKET_HEADER	8

/*

packet header
-------------
31	sequence
1	does this message contain a reliable payload
31	acknowledge sequence
1	acknowledge receipt of even/odd message
16  qport

The remote connection never knows if it missed a reliable message, the
local side detects that it has been dropped by seeing a sequence acknowledge
higher thatn the last reliable sequence, but without the correct evon/odd
bit for the reliable set.

If the sender notices that a reliable message has been dropped, it will be
retransmitted.  It will not be retransmitted again until a message after
the retransmit has been acknowledged and the reliable still failed to get there.

if the sequence number is -1, the packet should be handled without a netcon

The reliable message can be added to at any time by doing
MSG_Write* (&netchan->message, <data>).

If the message buffer is overflowed, either by a single message, or by
multiple frames worth piling up while the last reliable transmit goes
unacknowledged, the netchan signals a fatal error.

Reliable messages are allways placed first in a packet, then the unreliable
message is included if there is sufficient room.

To the receiver, there is no distinction between the reliable and unreliable
parts of the message, they are just processed out as a single larger message.

Illogical packet sequence numbers cause the packet to be dropped, but do
not kill the connection.  This, combined with the tight window of valid
reliable acknowledgement numbers provides protection against malicious
address spoofing.

The qport field is a workaround for bad address translating routers that
sometimes remap the client's source port on a packet during gameplay.

If the base part of the net address matches and the qport matches, then the
channel matches even if the IP port differs.  The IP port should be updated
to the new value before sending out any replies.


*/

static cvar_t	showpackets = {.name = "showpackets", .string = "0",
	.description = "Prints each packet sent (-->) and received (<--): its sequence numbers, reliable flags and size.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "Every packet printed"}, {0}}};
static cvar_t	showdrop = {.name = "showdrop", .string = "0",
	.description = "Prints packets that arrive out of order, and how many were lost before one.",
	.values = (const cvar_value_t[]){{"0", "Off"}, {"1", "Lost and out of order packets printed"}, {0}}};
static cvar_t	qport = {.name = "qport", .string = "0",
	.description = "The client's number, picked at start, that lets a server tell clients at one address apart "
		"and follow a router's remapped port."};

/*
===============
Netchan_Init

===============
*/
void Netchan_Init (void)
{
	int		port;

	// a value of its own for each client process: the server tells clients at
	// one address apart by it (Sys_DoubleTime counts from the first call, and
	// was near 0 here for every client)
	port = Sys_Seed () & 0xffff;

	Cvar_RegisterVariable (&showpackets);
	Cvar_RegisterVariable (&showdrop);
	Cvar_RegisterVariable (&qport);
	Cvar_SetValue("qport", (float)port);
}

/*
===============
Netchan_OutOfBand

Sends an out-of-band datagram
================
*/
void Netchan_OutOfBand (netsrc_t sock, netadr_t adr, int length, byte *data)
{
	sizebuf_t	send;
	byte		send_buf[MAX_MSGLEN + PACKET_HEADER];

// write the packet header
	send.data = send_buf;
	send.maxsize = sizeof(send_buf);
	send.cursize = 0;
	
	MSG_WriteLong (&send, -1);	// -1 sequence means out of band
	SZ_Write (&send, data, length);

// send the datagram
	NET_SendPacket (sock, send.cursize, send.data, adr);
}

/*
===============
Netchan_OutOfBandPrint

Sends a text message in an out-of-band datagram
================
*/
void Netchan_OutOfBandPrint (netsrc_t sock, netadr_t adr, char *format, ...)
{
	va_list		argptr;
	static char		string[8192];		// ??? why static?
	
	va_start (argptr, format);
	vsnprintf (string, sizeof(string), format,argptr);
	va_end (argptr);


	Netchan_OutOfBand (sock, adr, (int)strlen(string), (byte *)string);
}


/*
==============
Netchan_Setup

called to open a channel to a remote system
==============
*/
void Netchan_Setup (netchan_t *chan, netadr_t adr, int remoteqport, netsrc_t sock)
{
	memset (chan, 0, sizeof(*chan));

	chan->remote_address = adr;
	chan->last_received = (float)host.realtime;

	chan->message.data = chan->message_buf;
	chan->message.allowoverflow = true;
	chan->message.maxsize = sizeof(chan->message_buf);

	chan->qport = remoteqport;
	chan->sock = sock;
	
	chan->rate = 1.0/2500;
}


/*
==============
Netchan_SetFragments

FTE's fragmentation (PROTOCOL_VERSION_FRAGMENT), as the connection agreed it
==============
*/
void Netchan_SetFragments (netchan_t *chan, int mtu)
{
	chan->fragmtu = mtu > 0 ? mtu : 0;
}

// the pieces of the packet coming together: only a client's channel gets them
static byte		frag_buf[MAX_FRAGMENTED];
static int		frag_length;
static unsigned	frag_sequence;

/*
===============
Netchan_CanPacket

Returns true if the bandwidth choke isn't active
================
*/
#define	MAX_BACKUP	200
bool Netchan_CanPacket (netchan_t *chan)
{
	if (chan->remote_address.type == NA_LOOPBACK)
		return true;			// no bandwidth to share
	if (chan->cleartime < host.realtime + MAX_BACKUP*chan->rate)
		return true;
	return false;
}


/*
===============
Netchan_CanReliable

Returns true if the bandwidth choke isn't 
================
*/
bool Netchan_CanReliable (netchan_t *chan)
{
	if (chan->reliable_length)
		return false;			// waiting for ack
	return Netchan_CanPacket (chan);
}

/*
===============
Netchan_SendFragments

A packet bigger than FTE's fragmentation lets go in pieces, as FTE's do: each
with the packet's header, and after it the piece's offset in the payload
over 4 (a multiple of 8) with 1 for more to come. False if it fits whole.
===============
*/
static bool Netchan_SendFragments (netchan_t *chan, const sizebuf_t *send)
{
	byte	piece[MAX_MSGLEN + PACKET_HEADER + 4];
	int		header, payload, offset, next, chunk;
	bool	more;

	if (!chan->fragmtu)
		return false;
	header = PACKET_HEADER + (chan->sock == NS_CLIENT ? 2 : 0) + 2;
	payload = send->cursize - header;
	// what a piece's payload may be: the mtu less the headers, and WebRTC's
	// SCTP and DTLS, as FTE leaves
	chunk = (chan->fragmtu - header - 60) & ~7;
	if (payload <= chunk || chunk < 64)
		return false;

	for (offset = 0 ; offset < payload ; offset = next)
	{
		next = offset + chunk;
		more = next < payload;
		if (!more)
			next = payload;
		memcpy (piece, send->data, (size_t)header - 2);
		piece[header - 2] = (byte)(((offset >> 2) | more) & 0xff);
		piece[header - 1] = (byte)(((offset >> 2) | more) >> 8);
		memcpy (piece + header, send->data + header + offset, (size_t)(next - offset));
		NET_SendPacket (chan->sock, header + next - offset, piece, chan->remote_address);
	}
	return true;
}

/*
===============
Netchan_Transmit

tries to send an unreliable message to a connection, and handles the
transmition / retransmition of the reliable messages.

A 0 length will still generate a packet and deal with the reliable messages.
================
*/
void Netchan_Transmit (netchan_t *chan, int length, byte *data)
{
	sizebuf_t	send;
	byte		send_buf[MAX_MSGLEN + PACKET_HEADER + 4];	// and the qport, and the fragment offset
	bool	send_reliable;
	unsigned	w1, w2;
	int			i;

// check for message overflow
	if (chan->message.overflowed)
	{
		chan->fatal_error = true;
		Con_Printf ("%s:Outgoing message overflow\n"
			, NET_AdrToString (chan->remote_address));
		return;
	}

// if the remote side dropped the last reliable message, resend it
	send_reliable = false;

	if (chan->incoming_acknowledged > chan->last_reliable_sequence
	&& chan->incoming_reliable_acknowledged != chan->reliable_sequence)
		send_reliable = true;

// if the reliable transmit buffer is empty, copy the current message out
	if (!chan->reliable_length && chan->message.cursize)
	{
		memcpy (chan->reliable_buf, chan->message_buf, chan->message.cursize);
		chan->reliable_length = chan->message.cursize;
		chan->message.cursize = 0;
		chan->reliable_sequence ^= 1;
		send_reliable = true;
	}

// write the packet header
	send.data = send_buf;
	send.maxsize = sizeof(send_buf);
	send.cursize = 0;

	// the reliable bits are the top bits: shifted unsigned, which an int can't
	w1 = (unsigned)chan->outgoing_sequence | ((unsigned)send_reliable << 31);
	w2 = (unsigned)chan->incoming_sequence | ((unsigned)chan->incoming_reliable_sequence << 31);

	chan->outgoing_sequence++;

	MSG_WriteLong (&send, w1);
	MSG_WriteLong (&send, w2);

	// send the qport if we are a client
	if (chan->sock == NS_CLIENT)
		MSG_WriteShort (&send, chan->qport);
	// with FTE's fragmentation, a whole packet's offset: none
	if (chan->fragmtu)
		MSG_WriteShort (&send, 0);

// copy the reliable message to the packet first
	if (send_reliable)
	{
		SZ_Write (&send, chan->reliable_buf, chan->reliable_length);
		chan->last_reliable_sequence = chan->outgoing_sequence;
	}
	
// add the unreliable part if space is available
	if (send.maxsize - send.cursize >= length)
		SZ_Write (&send, data, length);

// send the datagram
	i = chan->outgoing_sequence & (MAX_LATENT-1);
	chan->outgoing_size[i] = send.cursize;
	chan->outgoing_time[i] = host.realtime;

	if (!Netchan_SendFragments (chan, &send))
		NET_SendPacket (chan->sock, send.cursize, send.data, chan->remote_address);

	if (chan->cleartime < host.realtime)
		chan->cleartime = host.realtime + send.cursize*chan->rate;
	else
		chan->cleartime += send.cursize*chan->rate;

	if (showpackets.value)
		Con_Printf ("--> s=%i(%i) a=%i(%i) %i\n"
			, chan->outgoing_sequence
			, send_reliable
			, chan->incoming_sequence
			, chan->incoming_reliable_sequence
			, send.cursize);

}

/*
=================
Netchan_Process

called for each packet from remote_address; reading continues after
the header
=================
*/
bool Netchan_Process (netchan_t *chan, netadr_t from, sizebuf_t *msg)
{
	unsigned		sequence, sequence_ack;
	unsigned		reliable_ack, reliable_message;
	int				header, offset, length;
	bool			more;

	if (!NET_CompareAdr (from, chan->remote_address))
		return false;
	
// get sequence numbers		
	MSG_BeginReading (msg);
	sequence = MSG_ReadLong ();
	sequence_ack = MSG_ReadLong ();

	// read the qport if we are a server
	if (chan->sock == NS_SERVER)
		MSG_ReadShort ();
	header = MSG_GetReadCount ();
	// with FTE's fragmentation, the piece's offset (over 4, 1 for more to come)
	offset = chan->fragmtu ? (unsigned short)MSG_ReadShort () : 0;

	reliable_message = sequence >> 31;
	reliable_ack = sequence_ack >> 31;

	sequence &= ~(1u << 31);
	sequence_ack &= ~(1u << 31);

	if (showpackets.value)
		Con_Printf ("<-- s=%i(%i) a=%i(%i) %i\n"
			, sequence
			, reliable_message
			, sequence_ack
			, reliable_ack
			, msg->cursize);

// get a rate estimation

//
// discard stale or duplicated packets
//
	if (sequence <= (unsigned)chan->incoming_sequence)
	{
		if (showdrop.value)
			Con_Printf ("%s:Out of order packet %i at %i\n"
				, NET_AdrToString (chan->remote_address)
				,  sequence
				, chan->incoming_sequence);
		return false;
	}

//
// with FTE's fragmentation the offset goes, and a packet's pieces are kept
// until the last (a piece lost loses the packet): the message is then the
// plain packet, as a demo records it
//
	if (chan->fragmtu)
	{
		length = msg->cursize - MSG_GetReadCount ();
		more = offset & 1;
		offset = (offset & ~1) << 2;
		if (!more && !offset)
		{
			memmove (msg->data + header, msg->data + header + 2, (size_t)length);
			frag_length = 0;
		}
		else
		{
			if (sequence != frag_sequence)
			{
				frag_sequence = sequence;
				frag_length = 0;
			}
			if (offset != frag_length || offset + length > MAX_FRAGMENTED)
			{
				if (showdrop.value)
					Con_Printf ("%s:Fragment lost before %i of %u\n", NET_AdrToString (chan->remote_address), offset, sequence);
				frag_length = 0;
				return false;
			}
			memcpy (frag_buf + offset, msg->data + MSG_GetReadCount (), (size_t)length);
			frag_length += length;
			if (more)
				return false;
			length = frag_length;
			frag_length = 0;
			if (header + length > msg->maxsize)
				return false;
			memcpy (msg->data + header, frag_buf, (size_t)length);
		}
		msg->cursize = header + length;
		MSG_BeginReading (msg);
		msg_readcount = header;
	}

//
// dropped packets don't keep the message from being used
//
	chan->dropped = sequence - (chan->incoming_sequence+1);
	if (chan->dropped > 0)
	{
		chan->drop_count += 1;

		if (showdrop.value)
			Con_Printf ("%s:Dropped %i packets at %i\n"
			, NET_AdrToString (chan->remote_address)
			, sequence-(chan->incoming_sequence+1)
			, sequence);
	}

//
// if the current outgoing reliable message has been acknowledged
// clear the buffer to make way for the next
//
	if (reliable_ack == (unsigned)chan->reliable_sequence)
		chan->reliable_length = 0;	// it has been received
	
//
// if this message contains a reliable message, bump incoming_reliable_sequence 
//
	chan->incoming_sequence = sequence;
	chan->incoming_acknowledged = sequence_ack;
	chan->incoming_reliable_acknowledged = reliable_ack;
	if (reliable_message)
		chan->incoming_reliable_sequence ^= 1;

//
// the message can now be read from the current message pointer
// update statistics counters
//
	chan->frame_latency = (float)(chan->frame_latency*OLD_AVG
		+ (chan->outgoing_sequence-sequence_ack)*(1.0f-OLD_AVG));
	chan->frame_rate = (float)(chan->frame_rate*OLD_AVG
		+ (host.realtime-chan->last_received)*(1.0f-OLD_AVG));
	chan->good_count += 1;

	chan->last_received = (float)host.realtime;

	return true;
}

