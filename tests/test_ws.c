// test_ws.c -- the WebSocket handshake and frames as the server takes them
// (net_wsproto.c): RFC 6455's examples, the requests browsers and FTE send and
// others refused, and frames whole, cut anywhere, fragmented, unmasked (FTE's)
// and broken

#include "net_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;

#define CHECK(cond) do { if (!(cond)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// RFC 6455 1.3's key and accept
static void TestAcceptKey (void)
{
	char	accept[32];
	const char	*key = "dGhlIHNhbXBsZSBub25jZQ==";

	WS_AcceptKey (key, strlen (key), accept);
	CHECK (!strcmp (accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

static int Parse (const char *text, wsrequest_t *request)
{
	return WS_ParseRequest (text, (int)strlen (text), request);
}

static void TestRequests (void)
{
	wsrequest_t	r;
	char		reply[512], big[WS_REQUESTMAX + 64];
	const char	*chrome = "GET /qw HTTP/1.1\r\nHost: localhost:27500\r\nConnection: Upgrade\r\nPragma: no-cache\r\n"
		"Upgrade: websocket\r\nOrigin: http://localhost:8000\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: fteqw\r\n\r\n";
	const char	*firefox = "GET / HTTP/1.1\r\nHost: localhost\r\nconnection: keep-alive, Upgrade\r\n"
		"upgrade: WebSocket\r\nsec-websocket-version: 13\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"sec-websocket-protocol: quake, fteqw\r\n\r\n";
	int			length;

	length = Parse (chrome, &r);
	CHECK (length == (int)strlen (chrome));
	CHECK (r.status == 101);
	CHECK (!strcmp (r.accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	CHECK (!strcmp (r.protocol, "fteqw"));
	WS_Reply (&r, reply, sizeof(reply));
	CHECK (strstr (reply, "101 Switching Protocols") && strstr (reply, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
		&& strstr (reply, "Sec-WebSocket-Protocol: fteqw"));

	CHECK (Parse (firefox, &r) > 0 && r.status == 101 && !strcmp (r.protocol, "fteqw"));

	// none offered, none answered; binary taken; quake alone refused
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n\r\n", &r) > 0 && r.status == 101 && !r.protocol[0]);
	WS_Reply (&r, reply, sizeof(reply));
	CHECK (!strstr (reply, "Sec-WebSocket-Protocol"));
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\nSec-WebSocket-Protocol: binary\r\n\r\n", &r) > 0
		&& r.status == 101 && !strcmp (r.protocol, "binary"));
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\nSec-WebSocket-Protocol: quake\r\n\r\n", &r) > 0
		&& r.status == 400);

	// a page asked for, another version, another method, no key
	CHECK (Parse ("GET / HTTP/1.1\r\nHost: localhost\r\nAccept: text/html\r\n\r\n", &r) > 0 && r.status == 426);
	WS_Reply (&r, reply, sizeof(reply));
	CHECK (strstr (reply, "426 Upgrade Required") && strstr (reply, "Sec-WebSocket-Version: 13"));
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 8\r\n"
		"Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n\r\n", &r) > 0 && r.status == 426);
	CHECK (Parse ("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n", &r) > 0 && r.status == 405);
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n",
		&r) > 0 && r.status == 400);

	// a proxy's client, the last address it was forwarded for
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\nX-Forwarded-For: 10.0.0.1, 192.0.2.7 \r\n\r\n", &r) > 0
		&& r.status == 101 && !strcmp (r.forwarded, "192.0.2.7"));

	// not all there yet; more than may be
	CHECK (Parse ("GET / HTTP/1.1\r\nUpgrade: websocket\r\n", &r) == 0);
	memset (big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	CHECK (Parse (big, &r) == -1);
}

// a client's frame, masked with mask
static int MakeFrame (byte *out, int opcode, bool fin, const byte *payload, int length, const byte mask[4])
{
	int		header = 2, i;

	out[0] = (byte)((fin ? 0x80 : 0) | opcode);
	if (length < 126)
		out[1] = (byte)(0x80 | length);
	else if (length < 65536)
	{
		out[1] = 0x80 | 126;
		out[2] = (byte)(length >> 8);
		out[3] = (byte)length;
		header = 4;
	}
	else
	{
		out[1] = 0x80 | 127;
		for (i = 0 ; i < 8 ; i++)
			out[2 + i] = (byte)(i < 4 ? 0 : (unsigned)length >> (8 * (7 - i)));
		header = 10;
	}
	memcpy (out + header, mask, 4);
	for (i = 0 ; i < length ; i++)
		out[header + 4 + i] = payload[i] ^ mask[i & 3];
	return header + 4 + length;
}

static void TestFrames (void)
{
	static const byte	hello[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};	// RFC 6455 5.7
	static const byte	mask[4] = {0x12, 0x34, 0x56, 0x78};
	static byte	payload[70000], frame[70100], copy[70100];
	wsframe_t	f;
	byte		header[10];
	int			length, code, i, cut;

	memcpy (frame, hello, sizeof(hello));
	CHECK (WS_ReadFrame (frame, sizeof(hello), 2900, &f, &code) == (int)sizeof(hello));
	CHECK (f.opcode == WS_TEXT && f.fin && f.length == 5 && !memcmp (f.payload, "Hello", 5));

	for (i = 0 ; i < (int)sizeof(payload) ; i++)
		payload[i] = (byte)(i * 7 + 3);

	// a 16-bit length, whole, and cut at every byte: nothing until it is all there
	length = MakeFrame (frame, WS_BINARY, true, payload, 256, mask);
	for (cut = 0 ; cut < length ; cut++)
	{
		memcpy (copy, frame, (size_t)length);
		CHECK (WS_ReadFrame (copy, cut, 2900, &f, &code) == 0);
	}
	CHECK (WS_ReadFrame (frame, length, 2900, &f, &code) == length && f.opcode == WS_BINARY && f.length == 256
		&& !memcmp (f.payload, payload, 256));

	// two frames after each other, a message fragmented in them
	length = MakeFrame (frame, WS_BINARY, false, payload, 10, mask);
	i = MakeFrame (frame + length, WS_CONTINUATION, true, payload + 10, 20, mask);
	CHECK (WS_ReadFrame (frame, length + i, 2900, &f, &code) == length && !f.fin && f.length == 10);
	CHECK (WS_ReadFrame (frame + length, i, 2900, &f, &code) == i && f.fin && f.opcode == WS_CONTINUATION
		&& !memcmp (f.payload, payload + 10, 20));

	// a ping, and a 64-bit length too big for a packet
	length = MakeFrame (frame, WS_PING, true, payload, 4, mask);
	CHECK (WS_ReadFrame (frame, length, 2900, &f, &code) == length && f.opcode == WS_PING && f.length == 4);
	length = MakeFrame (frame, WS_BINARY, true, payload, 65536, mask);
	CHECK (WS_ReadFrame (frame, 10, 2900, &f, &code) == -1 && code == 1009);

	// unmasked, as FTE's client sends them: taken as they are
	CHECK (WS_ReadFrame ((byte []){0x82, 0x02, 0xff, 0x01}, 3, 2900, &f, &code) == 0);
	CHECK (WS_ReadFrame ((byte []){0x82, 0x02, 0xff, 0x01}, 4, 2900, &f, &code) == 4 && f.length == 2
		&& f.payload[0] == 0xff && f.payload[1] == 0x01);

	// broken: a reserved bit, a reserved opcode, a control frame fragmented or
	// too long
	CHECK (WS_ReadFrame ((byte []){0xc2, 0x80, 0, 0, 0, 0}, 6, 2900, &f, &code) == -1 && code == 1002);
	CHECK (WS_ReadFrame ((byte []){0x83, 0x80, 0, 0, 0, 0}, 6, 2900, &f, &code) == -1 && code == 1002);
	CHECK (WS_ReadFrame ((byte []){0x09, 0x80, 0, 0, 0, 0}, 6, 2900, &f, &code) == -1 && code == 1002);
	length = MakeFrame (frame, WS_PING, true, payload, 126, mask);
	CHECK (WS_ReadFrame (frame, length, 2900, &f, &code) == -1 && code == 1002);

	// the server's headers
	CHECK (WS_FrameHeader (header, WS_BINARY, 0) == 2 && header[0] == 0x82 && header[1] == 0);
	CHECK (WS_FrameHeader (header, WS_BINARY, 125) == 2 && header[1] == 125);
	CHECK (WS_FrameHeader (header, WS_BINARY, 126) == 4 && header[1] == 126 && header[2] == 0 && header[3] == 126);
	CHECK (WS_FrameHeader (header, WS_CLOSE, 65535) == 4 && header[0] == 0x88 && header[2] == 255 && header[3] == 255);
	CHECK (WS_FrameHeader (header, WS_BINARY, 65536) == 10 && header[1] == 127 && header[7] == 1 && header[9] == 0);
}

int main (void)
{
	TestAcceptKey ();
	TestRequests ();
	TestFrames ();
	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("WebSocket: RFC 6455's handshake and frames\n");
	return 0;
}
