#pragma once
// net_qtv.h -- QTV at the server's TCP port: the request a viewer sends
// (net_qtvproto.c, without sockets), and the viewers (net_qtv.c), which
// net_ws.c hands the connections that began with one

#include "net.h"
#include "net_socket.h"
#include "q_types.h"

#define	QTV_REQUESTMAX	4096		// a request's lines, at most

// a request, as read
typedef struct
{
	int		version;			// the major version asked for, 0 none
	bool	raw;				// RAW: the stream without the reply's lines
	char	name[32];			// USERINFO's name, "" for none
} qtvrequest_t;

// a request's lines once they are all there, to the blank line (LF or CRLF):
// their length, 0 while they aren't, -1 if they aren't QTV's or are longer
// than QTV_REQUESTMAX
int		QTV_ParseRequest (const char *text, int length, qtvrequest_t *request);

// a connection whose first bytes, or first WebSocket message, began "QTV":
// what it has asked so far, and a WebSocket's frames after that message
void	QTV_Adopt (tcpsocket_t *socket, netadr_t from, bool websocket, const byte *request, int length,
			const byte *frames, int frameslength);
// every viewer closed, and the log let go
void	QTV_CloseAll (void);
