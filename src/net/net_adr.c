// net_adr.c -- addresses' ips, IPv4's and IPv6's (net.h): an IPv4 address is
// IPv6's ::ffff:a.b.c.d, and the text of both, read and written

#include "net.h"

#include <stdio.h>
#include <string.h>

static const byte	net_ipv4prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

bool NET_IsIPv4 (netadr_t a)
{
	return !memcmp (a.ip, net_ipv4prefix, sizeof(net_ipv4prefix));
}

void NET_SetIPv4 (netadr_t *a, const void *ip)
{
	memcpy (a->ip, net_ipv4prefix, sizeof(net_ipv4prefix));
	memcpy (a->ip + 12, ip, 4);
}

// a.b.c.d, the whole of s
static bool NET_ParseIPv4 (const char *s, byte ip[4])
{
	int		i, value, digits;

	for (i = 0 ; i < 4 ; i++)
	{
		for (value = 0, digits = 0 ; *s >= '0' && *s <= '9' && digits < 4 ; s++, digits++)
			value = value * 10 + *s - '0';
		if (!digits || value > 255 || (i < 3 && *s++ != '.'))
			return false;
		ip[i] = (byte)value;
	}
	return !*s;
}

static int NET_HexDigit (int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
		return (c | 0x20) - 'a' + 10;
	return -1;
}

bool NET_ParseIP (const char *s, byte ip[16])
{
	byte	v4[4];
	int		groups[8], count = 0, gap = -1, value, digits, i, j;

	if (NET_ParseIPv4 (s, v4))
	{
		memcpy (ip, net_ipv4prefix, sizeof(net_ipv4prefix));
		memcpy (ip + 12, v4, 4);
		return true;
	}

	// groups of up to four hex digits, the zeros of one run of them left out
	// (::), the last two an IPv4 address's maybe
	if (s[0] == ':' && s[1] == ':')
	{
		gap = 0;
		s += 2;
	}
	while (*s)
	{
		if (count <= 6 && NET_ParseIPv4 (s, v4))
		{
			groups[count++] = v4[0] << 8 | v4[1];
			groups[count++] = v4[2] << 8 | v4[3];
			break;
		}
		for (value = 0, digits = 0 ; NET_HexDigit (*s) >= 0 ; s++, digits++)
			value = value * 16 + NET_HexDigit (*s);
		if (!digits || digits > 4 || count == 8)
			return false;
		groups[count++] = value;
		if (!*s)
			break;
		if (*s++ != ':' || !*s)
			return false;
		if (*s == ':')
		{
			if (gap >= 0)
				return false;
			gap = count;
			s++;
		}
	}
	if (gap < 0 ? count != 8 : count > 7)
		return false;

	memset (ip, 0, 16);
	for (i = 0 ; i < count ; i++)
	{
		j = gap < 0 || i < gap ? i : 8 - count + i;
		ip[j * 2] = (byte)(groups[i] >> 8);
		ip[j * 2 + 1] = (byte)groups[i];
	}
	return true;
}

const char *NET_IPToBuf (const byte ip[16], char *s, size_t size)
{
	int			groups[8], best = -1, bestlength = 1, run, length = 0, i;

	if (!memcmp (ip, net_ipv4prefix, sizeof(net_ipv4prefix)))
	{
		snprintf (s, size, "%i.%i.%i.%i", ip[12], ip[13], ip[14], ip[15]);
		return s;
	}
	for (i = 0 ; i < 8 ; i++)
		groups[i] = ip[i * 2] << 8 | ip[i * 2 + 1];
	for (i = 0 ; i < 8 ; i += run ? run : 1)
	{
		for (run = 0 ; i + run < 8 && !groups[i + run] ; run++)
			;
		if (run > bestlength)
		{
			best = i;
			bestlength = run;
		}
	}
	for (i = 0 ; i < 8 ; )
		if (i == best)
		{
			length += snprintf (s + length, size - (size_t)length, "::");
			i += bestlength;
		}
		else
		{
			length += snprintf (s + length, size - (size_t)length, "%s%x",
				i && i != best + bestlength ? ":" : "", (unsigned)groups[i]);
			i++;
		}
	return s;
}

const char *NET_IPToString (const byte ip[16])
{
	static char	s[48];

	return NET_IPToBuf (ip, s, sizeof(s));
}
