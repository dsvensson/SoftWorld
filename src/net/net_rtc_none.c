// net_rtc_none.c -- WebRTC where the build has none (net_rtc.h): rtc:// isn't
// an address, and nothing comes or goes

#include "net_rtc.h"
#include "print.h"

void RTC_Init (void)
{
}

void RTC_Shutdown (void)
{
}

bool RTC_ResolveURL (const char *s, netadr_t *a)
{
	(void)s;
	(void)a;
	return false;
}

const char *RTC_AdrToString (netadr_t a, bool port)
{
	(void)a;
	(void)port;
	return "";
}

bool RTC_GetPacket (netsrc_t sock, netadr_t *from, sizebuf_t *msg)
{
	(void)sock;
	(void)from;
	(void)msg;
	return false;
}

void RTC_SendPacket (netsrc_t sock, const void *data, int length, const netadr_t *to)
{
	(void)sock;
	(void)data;
	(void)length;
	(void)to;
}

bool RTC_Host (const char *url)
{
	if (url && *url)
		Con_Printf ("WebRTC: this build has none, for %s\n", url);
	return false;
}

void RTC_HostInfo (const char *info)
{
	(void)info;
}
