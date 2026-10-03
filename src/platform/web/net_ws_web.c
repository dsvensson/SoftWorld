// net_ws_web.c -- the network in a browser page: none yet; a page has no UDP,
// and plays the games of its own server (the loopback) and demos (WebSocket
// comes next)

#include "net_socket.h"

#include <string.h>

void UDP_Init (void)
{
}

void UDP_Shutdown (void)
{
}

udpsocket_t *UDP_Open (int port)
{
	(void)port;
	return NULL;
}

void UDP_Close (udpsocket_t *s)
{
	(void)s;
}

netadr_t UDP_Address (udpsocket_t *s)
{
	netadr_t	none = {.type = NA_INVALID};

	(void)s;
	return none;
}

int UDP_Recv (udpsocket_t *s, byte *buf, int maxlen, netadr_t *from)
{
	(void)s;
	(void)buf;
	(void)maxlen;
	(void)from;
	return 0;
}

void UDP_Send (udpsocket_t *s, const void *data, int length, const netadr_t *to)
{
	(void)s;
	(void)data;
	(void)length;
	(void)to;
}

bool UDP_Resolve (const char *host, netadr_t *a)
{
	(void)host;
	memset (a, 0, sizeof(*a));
	return false;
}

tcpsocket_t *TCP_Connect (const netadr_t *to)
{
	(void)to;
	return NULL;
}

void TCP_Close (tcpsocket_t *s)
{
	(void)s;
}

int TCP_State (tcpsocket_t *s)
{
	(void)s;
	return TCP_FAILED;
}

int TCP_Recv (tcpsocket_t *s, byte *buf, int maxlen)
{
	(void)s;
	(void)buf;
	(void)maxlen;
	return -1;
}

bool TCP_Send (tcpsocket_t *s, const void *data, int length)
{
	(void)s;
	(void)data;
	(void)length;
	return false;
}
