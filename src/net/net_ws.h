#pragma once
// net_ws.h -- WebSocket (RFC 6455) at the server's port, for browsers'
// clients: the handshake and the frames (net_wsproto.c), and the connections
// (net_ws.c). A packet is a binary message, as FTE's servers and clients
// send them (the subprotocol "fteqw").

#include "msg.h"
#include "net.h"
#include "q_types.h"

#include <stddef.h>

#define	WS_REQUESTMAX	4096		// an upgrade request's headers, at most

// an upgrade request, as read
typedef struct
{
	int		status;				// 101 to upgrade; 400, 405 or 426 to refuse
	char	accept[32];			// Sec-WebSocket-Accept, 28 characters
	char	protocol[16];		// the subprotocol taken, "" for none
	char	forwarded[48];		// X-Forwarded-For's last address, "" for none
} wsrequest_t;

// the Sec-WebSocket-Accept of a request's Sec-WebSocket-Key
void	WS_AcceptKey (const char *key, size_t keylength, char accept[32]);

// a request's headers once they are all there: their length, 0 while they
// aren't, -1 if they are more than WS_REQUESTMAX
int		WS_ParseRequest (const char *text, int length, wsrequest_t *request);

// the response to a request, and its length
int		WS_Reply (const wsrequest_t *request, char *out, int size);

enum { WS_CONTINUATION = 0, WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

typedef struct
{
	int		opcode;
	bool	fin;			// the message's last frame
	byte	*payload;		// unmasked, where it was read
	int		length;
} wsframe_t;

// a client's frame at the start of data, masked or (FTE's client's) not: its
// length, 0 while it isn't all there, -1 for one the connection is closed
// over, with the code to close it with (1002 broken, 1009 too big)
int		WS_ReadFrame (byte *data, int length, int maxpayload, wsframe_t *frame, int *closecode);

// a server's frame's header (unmasked) for a payload of length: its size, 2,
// 4 or 10 bytes
int		WS_FrameHeader (byte out[10], int opcode, int length);

// net_ws.c: the server's packets from and to its WebSocket connections
bool	WS_GetPacket (netadr_t *from, sizebuf_t *msg);
void	WS_SendPacket (const void *data, int length, const netadr_t *to);
