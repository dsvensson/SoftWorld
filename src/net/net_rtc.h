#pragma once
// net_rtc.h -- WebRTC: QuakeWorld's packets over data channels (unordered, never
// sent again: as UDP), the peers found through a broker as FTE's are
// (net_rtc.c, over libdatachannel; net_rtc_none.c where the build has none: in
// a browser rtc:// is a URL, net_ws_web.c's)
//
// A server is reached by rtc://broker[:port]/room, or rtcs:// with the broker
// over TLS: the room a server took at the broker, or udp/ip:port for a server
// the broker knows to reach over DTLS. The broker's port is 27950 unless given.

#include "msg.h"
#include "net.h"
#include "q_types.h"

void	RTC_Init (void);
void	RTC_Shutdown (void);

// an rtc:// or rtcs:// URL as an address (NA_RTC); false for others, or where
// the build has no WebRTC
bool	RTC_ResolveURL (const char *s, netadr_t *a);
const char	*RTC_AdrToString (netadr_t a, bool port);

// the end's next packet over WebRTC, and a packet to a peer
bool	RTC_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg);
void	RTC_SendPacket (netsrc_t sock, const void *data, int length, const netadr_t *to);
