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
// net_wins.c -- UDP networking over Winsock 2

#include <winsock2.h>
#include <ws2tcpip.h>

#include "args.h"
#include "cvar.h"
#include "net.h"
#include "print.h"
#include "q_string.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>

netadr_t	net_local_adr;

netadr_t	net_from;
sizebuf_t	net_message;
static SOCKET	net_socket = INVALID_SOCKET;

#define	MAX_UDP_PACKET	(MAX_MSGLEN*2)	// one more than msg + header
static byte	net_message_buffer[MAX_UDP_PACKET];

//=============================================================================

static void NetadrToSockadr (const netadr_t *a, struct sockaddr_in *s)
{
	memset (s, 0, sizeof(*s));
	s->sin_family = AF_INET;
	memcpy (&s->sin_addr, a->ip, 4);
	s->sin_port = a->port;
}

static void SockadrToNetadr (const struct sockaddr_in *s, netadr_t *a)
{
	memcpy (a->ip, &s->sin_addr, 4);
	a->port = s->sin_port;
}

bool	NET_CompareBaseAdr (netadr_t a, netadr_t b)
{
	return memcmp (a.ip, b.ip, 4) == 0;
}

bool	NET_CompareAdr (netadr_t a, netadr_t b)
{
	return memcmp (a.ip, b.ip, 4) == 0 && a.port == b.port;
}

char	*NET_AdrToString (netadr_t a)
{
	static	char	s[64];

	snprintf (s, sizeof(s), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], ntohs(a.port));

	return s;
}

char	*NET_BaseAdrToString (netadr_t a)
{
	static	char	s[64];

	snprintf (s, sizeof(s), "%i.%i.%i.%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);

	return s;
}

/*
=============
NET_StringToAdr

idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
=============
*/
bool	NET_StringToAdr (char *s, netadr_t *a)
{
	struct addrinfo	hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
	struct addrinfo	*result;
	char			copy[128];
	char			*colon;
	unsigned short	port = 0;

	Q_strncpyz (copy, s, sizeof(copy));

	// strip off a trailing :port if present
	colon = strrchr (copy, ':');
	if (colon)
	{
		*colon = 0;
		port = htons ((unsigned short)atoi (colon + 1));
	}

	if (getaddrinfo (copy, NULL, &hints, &result) != 0 || !result)
		return false;

	SockadrToNetadr ((const struct sockaddr_in *)result->ai_addr, a);
	a->port = port;
	freeaddrinfo (result);

	return true;
}

//=============================================================================

bool NET_GetPacket (void)
{
	int 	ret;
	struct sockaddr_in	from;
	int		fromlen;

	fromlen = sizeof(from);
	ret = recvfrom (net_socket, (char *)net_message_buffer, sizeof(net_message_buffer), 0, (struct sockaddr *)&from, &fromlen);

	if (ret == SOCKET_ERROR)
	{
		int err = WSAGetLastError ();

		if (err == WSAEWOULDBLOCK || err == WSAECONNRESET)
			return false;
		if (err == WSAEMSGSIZE)
		{
			SockadrToNetadr (&from, &net_from);
			Con_Printf ("Warning:  Oversize packet from %s\n", NET_AdrToString (net_from));
			return false;
		}

		Sys_Error ("NET_GetPacket: Winsock error %i", err);
	}

	SockadrToNetadr (&from, &net_from);

	net_message.cursize = ret;
	if (ret == sizeof(net_message_buffer) )
	{
		Con_Printf ("Oversize packet from %s\n", NET_AdrToString (net_from));
		return false;
	}

	return ret > 0;
}

//=============================================================================

void NET_SendPacket (int length, void *data, netadr_t to)
{
	int ret;
	struct sockaddr_in	addr;

	NetadrToSockadr (&to, &addr);

	ret = sendto (net_socket, data, length, 0, (struct sockaddr *)&addr, sizeof(addr) );
	if (ret == SOCKET_ERROR)
	{
		int err = WSAGetLastError();

// wouldblock is silent
		if (err == WSAEWOULDBLOCK)
			return;

		if (err == WSAEADDRNOTAVAIL)
			Con_DPrintf("NET_SendPacket Warning: %i\n", err);
		else
			Con_Printf ("NET_SendPacket ERROR: %i\n", err);
	}
}

/*
====================
NET_Sleep

Waits for a packet to arrive, at most msec milliseconds.
====================
*/
void NET_Sleep (int msec)
{
	fd_set			fdset;
	struct timeval	timeout;

	FD_ZERO (&fdset);
	FD_SET (net_socket, &fdset);
	timeout.tv_sec = msec / 1000;
	timeout.tv_usec = (msec % 1000) * 1000;
	select (0, &fdset, NULL, NULL, &timeout);
}

//=============================================================================

static SOCKET UDP_OpenSocket (int port)
{
	SOCKET	newsocket;
	struct sockaddr_in address = {.sin_family = AF_INET};
	u_long	nonblocking = 1;
	int		i;

	newsocket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (newsocket == INVALID_SOCKET)
		Sys_Error ("UDP_OpenSocket: socket: Winsock error %i", WSAGetLastError ());

	if (ioctlsocket (newsocket, FIONBIO, &nonblocking) == SOCKET_ERROR)
		Sys_Error ("UDP_OpenSocket: ioctl FIONBIO: Winsock error %i", WSAGetLastError ());

//ZOID -- check for interface binding option
	if ((i = COM_CheckParm("-ip")) != 0 && i + 1 < com_argc)
	{
		if (inet_pton (AF_INET, com_argv[i+1], &address.sin_addr) != 1)
			Sys_Error ("UDP_OpenSocket: bad -ip address %s", com_argv[i+1]);
		Con_Printf("Binding to IP Interface Address of %s\n", com_argv[i+1]);
	}
	else
		address.sin_addr.s_addr = INADDR_ANY;

	if (port == PORT_ANY)
		address.sin_port = 0;
	else
		address.sin_port = htons((unsigned short)port);
	if (bind (newsocket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR)
		Sys_Error ("UDP_OpenSocket: bind: Winsock error %i", WSAGetLastError ());

	return newsocket;
}

static void NET_GetLocalAddress (void)
{
	char	buff[512];
	struct sockaddr_in	address;
	int		namelen;

	if (gethostname(buff, sizeof(buff)) != 0)
		Q_strncpyz (buff, "localhost", sizeof(buff));
	buff[sizeof(buff)-1] = 0;

	if (!NET_StringToAdr (buff, &net_local_adr))
		NET_StringToAdr ("127.0.0.1", &net_local_adr);

	namelen = sizeof(address);
	if (getsockname (net_socket, (struct sockaddr *)&address, &namelen) == SOCKET_ERROR)
		Sys_Error ("NET_Init: getsockname: Winsock error %i", WSAGetLastError ());
	net_local_adr.port = address.sin_port;

	Con_Printf("IP address %s\n", NET_AdrToString (net_local_adr) );
}

/*
====================
NET_Init
====================
*/
void NET_Init (int port)
{
	WSADATA	winsockdata;

	if (WSAStartup (MAKEWORD(2, 2), &winsockdata))
		Sys_Error ("Winsock initialization failed.");

	//
	// open the single socket to be used for all communications
	//
	net_socket = UDP_OpenSocket (port);

	//
	// init the message buffer
	//
	net_message.maxsize = sizeof(net_message_buffer);
	net_message.data = net_message_buffer;

	//
	// determine my name & address
	//
	NET_GetLocalAddress ();

	Con_Printf("UDP Initialized\n");
}

/*
====================
NET_Shutdown
====================
*/
void	NET_Shutdown (void)
{
	if (net_socket != INVALID_SOCKET)
		closesocket (net_socket);
	net_socket = INVALID_SOCKET;
	WSACleanup ();
}
