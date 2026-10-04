#pragma once
// net_win.h -- what net_udp_win.c and net_tcp_win.c share: the sockets'
// addresses, IPv4's and IPv6's
//
// A socket is IPv6's, taking IPv4's too (as ::ffff:a.b.c.d), where the system
// has IPv6 and -ip doesn't name an IPv4 address; IPv4's otherwise.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "net.h"

// a socket of the type (SOCK_DGRAM, SOCK_STREAM), non-blocking, and the address
// to bind it to at the port (PORT_ANY for any): -ip's, or every interface's;
// INVALID_SOCKET if there can be none
SOCKET	Win_Socket (int type, int port, struct sockaddr_storage *address, int *length);

// an address as a socket of the family takes it, its length; 0 if it can't
// (IPv6's to an IPv4 socket)
int		Win_ToSockaddr (const netadr_t *a, int family, struct sockaddr_storage *s);
void	Win_FromSockaddr (const struct sockaddr_storage *s, netadr_t *a);
