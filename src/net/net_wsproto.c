// net_wsproto.c -- WebSocket's handshake and frames (RFC 6455), as a server
// takes them: what net_ws.c reads and writes, without sockets
//
// A request is an HTTP GET asking to upgrade (Upgrade: websocket, Connection
// with the token upgrade, Sec-WebSocket-Version 13, a Sec-WebSocket-Key), at
// any path, so a proxy may put the server under one. Of the subprotocols it
// offers "fteqw" is taken (FTE's QuakeWorld packets), else "binary"; none
// offered is none answered; others alone are refused. A plain request (a
// browser opening the port as a page) is told it is a WebSocket port.
//
// A client's frames are masked, and the payloads are unmasked in place; FTE's
// client sends them unmasked, which are taken as they are. No extension is
// agreed, so a frame with a reserved bit set is broken, as is a control frame
// fragmented or longer than 125 bytes.

#include "net_ws.h"
#include "sha1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	WS_GUID		"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static void WS_Base64 (const byte *data, size_t length, char *out)
{
	static const char	digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint32_t	v;
	size_t		i;

	for (i = 0 ; i + 2 < length ; i += 3)
	{
		v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
		*out++ = digits[v >> 18];
		*out++ = digits[(v >> 12) & 63];
		*out++ = digits[(v >> 6) & 63];
		*out++ = digits[v & 63];
	}
	if (i < length)
	{
		v = (uint32_t)data[i] << 16 | (i + 1 < length ? (uint32_t)data[i + 1] << 8 : 0);
		*out++ = digits[v >> 18];
		*out++ = digits[(v >> 12) & 63];
		*out++ = i + 1 < length ? digits[(v >> 6) & 63] : '=';
		*out++ = '=';
	}
	*out = 0;
}

void WS_AcceptKey (const char *key, size_t keylength, char accept[32])
{
	char	text[128];
	byte	digest[SHA1_DIGEST_SIZE];
	size_t	length;

	if (keylength > sizeof(text) - sizeof(WS_GUID))
		keylength = sizeof(text) - sizeof(WS_GUID);
	memcpy (text, key, keylength);
	memcpy (text + keylength, WS_GUID, sizeof(WS_GUID) - 1);
	length = keylength + sizeof(WS_GUID) - 1;
	SHA1_Block (text, length, digest);
	WS_Base64 (digest, sizeof(digest), accept);
}

/*
===============================================================================

THE HANDSHAKE

===============================================================================
*/

static int WS_Lower (int c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

static bool WS_SameText (const char *a, size_t alength, const char *b)
{
	size_t	i;

	if (strlen (b) != alength)
		return false;
	for (i = 0 ; i < alength ; i++)
		if (WS_Lower ((byte)a[i]) != WS_Lower ((byte)b[i]))
			return false;
	return true;
}

static bool WS_IsSpace (int c)
{
	return c == ' ' || c == '\t';
}

// a comma-separated header value has the token, whatever its case
static bool WS_HasToken (const char *value, size_t length, const char *token)
{
	size_t	start, end, i = 0;

	while (i < length)
	{
		while (i < length && (WS_IsSpace (value[i]) || value[i] == ','))
			i++;
		start = i;
		while (i < length && value[i] != ',')
			i++;
		for (end = i ; end > start && WS_IsSpace (value[end - 1]) ; end--)
			;
		if (end > start && WS_SameText (value + start, end - start, token))
			return true;
	}
	return false;
}

int WS_ParseRequest (const char *text, int length, wsrequest_t *request)
{
	const char	*end = NULL, *line, *next, *colon, *value;
	size_t		namelength, valuelength;
	int			i;
	bool		upgrade = false, connection = false, version = false, key = false, offered = false;

	for (i = 0 ; i + 3 < length ; i++)
		if (!memcmp (text + i, "\r\n\r\n", 4))
		{
			end = text + i + 2;		// after the last header's line
			break;
		}
	if (!end)
		return length >= WS_REQUESTMAX ? -1 : 0;
	if (end + 2 - text > WS_REQUESTMAX)
		return -1;

	memset (request, 0, sizeof(*request));
	request->status = 101;
	if (length < 4 || memcmp (text, "GET ", 4))
		request->status = 405;

	// the headers, a line each after the request's
	line = memchr (text, '\n', (size_t)(end - text));
	for (line = line ? line + 1 : end ; line < end ; line = next + 1)
	{
		next = memchr (line, '\n', (size_t)(end - line));
		if (!next)
			break;
		colon = memchr (line, ':', (size_t)(next - line));
		if (!colon)
			continue;
		namelength = (size_t)(colon - line);
		for (value = colon + 1 ; value < next && WS_IsSpace (*value) ; value++)
			;
		for (valuelength = (size_t)(next - value) ; valuelength && (WS_IsSpace (value[valuelength - 1])
			|| value[valuelength - 1] == '\r') ; valuelength--)
			;

		if (WS_SameText (line, namelength, "Upgrade"))
			upgrade |= WS_HasToken (value, valuelength, "websocket");
		else if (WS_SameText (line, namelength, "Connection"))
			connection |= WS_HasToken (value, valuelength, "upgrade");
		else if (WS_SameText (line, namelength, "Sec-WebSocket-Version"))
			version |= WS_HasToken (value, valuelength, "13");
		else if (WS_SameText (line, namelength, "Sec-WebSocket-Key") && valuelength && valuelength < 64)
		{
			WS_AcceptKey (value, valuelength, request->accept);
			key = true;
		}
		else if (WS_SameText (line, namelength, "Sec-WebSocket-Protocol"))
		{
			offered = true;
			if (WS_HasToken (value, valuelength, "fteqw"))
				strcpy (request->protocol, "fteqw");
			else if (WS_HasToken (value, valuelength, "binary") && !request->protocol[0])
				strcpy (request->protocol, "binary");
		}
		else if (WS_SameText (line, namelength, "X-Forwarded-For"))
		{
			// the last address, the one the proxy saw
			const char	*last = value + valuelength;

			while (last > value && last[-1] != ',')
				last--;
			while (last < value + valuelength && WS_IsSpace (*last))
				last++;
			snprintf (request->forwarded, sizeof(request->forwarded), "%.*s",
				(int)(value + valuelength - last), last);
		}
	}

	if (request->status == 101)
	{
		if (!upgrade)
			request->status = 426;		// not a WebSocket request
		else if (!version)
			request->status = 426;		// another version of it
		else if (!connection || !key || (offered && !request->protocol[0]))
			request->status = 400;
	}
	return (int)(end + 2 - text);
}

int WS_Reply (const wsrequest_t *request, char *out, int size)
{
	static const char	notice[] = "A QuakeWorld server's WebSocket port: browsers' clients connect to it.\n";
	int		length;

	switch (request->status)
	{
	case 101:
		if (request->protocol[0])
			length = snprintf (out, (size_t)size, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				"Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: %s\r\n\r\n",
				request->accept, request->protocol);
		else
			length = snprintf (out, (size_t)size, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				"Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", request->accept);
		break;
	case 426:
		length = snprintf (out, (size_t)size, "HTTP/1.1 426 Upgrade Required\r\nUpgrade: websocket\r\n"
			"Sec-WebSocket-Version: 13\r\nConnection: close\r\nContent-Type: text/plain\r\n"
			"Content-Length: %zu\r\n\r\n%s", sizeof(notice) - 1, notice);
		break;
	case 405:
		length = snprintf (out, (size_t)size, "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\n"
			"Connection: close\r\nContent-Length: 0\r\n\r\n");
		break;
	default:
		length = snprintf (out, (size_t)size, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n"
			"Content-Length: 0\r\n\r\n");
		break;
	}
	return length < size ? length : size - 1;
}

/*
===============================================================================

FRAMES

===============================================================================
*/

int WS_ReadFrame (byte *data, int length, int maxpayload, wsframe_t *frame, int *closecode)
{
	uint64_t	payload;
	int			header = 2, masklength, i;
	byte		*mask;

	if (length < 2)
		return 0;
	frame->fin = (data[0] & 0x80) != 0;
	frame->opcode = data[0] & 15;
	// a client's frames are masked, but FTE's own client's aren't, and FTE's
	// servers take them so
	masklength = data[1] & 0x80 ? 4 : 0;
	*closecode = 1002;
	if (data[0] & 0x70)
		return -1;		// a reserved bit: an extension never agreed
	if ((frame->opcode > WS_BINARY && frame->opcode < WS_CLOSE) || frame->opcode > WS_PONG)
		return -1;		// a reserved opcode

	payload = data[1] & 127;
	if (payload == 126)
	{
		if (length < 4)
			return 0;
		payload = (uint64_t)data[2] << 8 | data[3];
		header = 4;
	}
	else if (payload == 127)
	{
		if (length < 10)
			return 0;
		for (payload = 0, i = 0 ; i < 8 ; i++)
			payload = payload << 8 | data[2 + i];
		if (payload >> 63)
			return -1;
		header = 10;
	}

	if (frame->opcode >= WS_CLOSE && (!frame->fin || payload > 125))
		return -1;		// a control frame whole and short
	if (payload > (uint64_t)maxpayload)
	{
		*closecode = 1009;
		return -1;
	}
	if ((uint64_t)length < (uint64_t)header + masklength + payload)
		return 0;

	mask = data + header;
	frame->payload = data + header + masklength;
	frame->length = (int)payload;
	if (masklength)
		for (i = 0 ; i < frame->length ; i++)
			frame->payload[i] ^= mask[i & 3];
	*closecode = 0;
	return header + masklength + frame->length;
}

int WS_FrameHeader (byte out[10], int opcode, int length)
{
	int		i;

	out[0] = (byte)(0x80 | opcode);
	if (length < 126)
	{
		out[1] = (byte)length;
		return 2;
	}
	if (length < 65536)
	{
		out[1] = 126;
		out[2] = (byte)(length >> 8);
		out[3] = (byte)length;
		return 4;
	}
	out[1] = 127;
	for (i = 0 ; i < 8 ; i++)
		out[2 + i] = (byte)(i < 4 ? 0 : (unsigned)length >> (8 * (7 - i)));
	return 10;
}
