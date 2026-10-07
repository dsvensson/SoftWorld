// test_netadr.c -- addresses' ips as net_adr.c reads and writes them: IPv4's
// (as ::ffff:a.b.c.d), IPv6's in each form RFC 4291 allows and RFC 5952's
// text, and what isn't an address

#include "net.h"

#include <stdio.h>
#include <string.h>

static int	failures;

#define CHECK(cond) do { if (!(cond)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// s read as the 16 bytes, and written back as text
static void Expect (const char *s, const byte ip[16], const char *text)
{
	byte	got[16], again[16];

	if (!NET_ParseIP (s, got))
	{
		printf ("FAIL %s isn't read\n", s);
		failures++;
		return;
	}
	if (memcmp (got, ip, 16))
	{
		printf ("FAIL %s read wrong\n", s);
		failures++;
	}
	if (strcmp (NET_IPToString (got), text))
	{
		printf ("FAIL %s written as %s, not %s\n", s, NET_IPToString (got), text);
		failures++;
	}
	// the text reads back as the same
	CHECK (NET_ParseIP (NET_IPToString (got), again) && !memcmp (again, got, 16));
}

static void TestRead (void)
{
	Expect ("192.246.40.70", (const byte[16]){[10] = 0xff, 0xff, 192, 246, 40, 70}, "192.246.40.70");
	Expect ("0.0.0.0", (const byte[16]){[10] = 0xff, 0xff}, "0.0.0.0");
	Expect ("::ffff:192.0.2.1", (const byte[16]){[10] = 0xff, 0xff, 192, 0, 2, 1}, "192.0.2.1");
	Expect ("::1", (const byte[16]){[15] = 1}, "::1");
	Expect ("::", (const byte[16]){0}, "::");
	Expect ("1::", (const byte[16]){[1] = 1}, "1::");
	Expect ("2001:db8::1", (const byte[16]){0x20, 0x01, 0x0d, 0xb8, [15] = 1}, "2001:db8::1");
	Expect ("2001:DB8:0:0:8:800:200C:417A",
		(const byte[16]){0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 8, 8, 0, 0x20, 0x0c, 0x41, 0x7a},
		"2001:db8::8:800:200c:417a");
	Expect ("1:2:3:4:5:6:7:8", (const byte[16]){0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}, "1:2:3:4:5:6:7:8");
	Expect ("0001:0002:0003:0004:0005:0006:0007:0008",
		(const byte[16]){0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}, "1:2:3:4:5:6:7:8");
	// :: for one group, though RFC 5952 writes the 0
	Expect ("1:2:3:4:5:6:7::", (const byte[16]){0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 0}, "1:2:3:4:5:6:7:0");
	Expect ("1:2:3:4:5:6:1.2.3.4", (const byte[16]){0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 1, 2, 3, 4},
		"1:2:3:4:5:6:102:304");
	Expect ("64:ff9b::192.0.2.33", (const byte[16]){0, 0x64, 0xff, 0x9b, [12] = 192, 0, 2, 33}, "64:ff9b::c000:221");
	Expect ("fe80::", (const byte[16]){0xfe, 0x80}, "fe80::");
}

// RFC 5952 4.2: the longest run of zero groups, the first of runs as long,
// never one group alone
static void TestWrite (void)
{
	Expect ("2001:db8:0:0:1:0:0:1", (const byte[16]){0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1},
		"2001:db8::1:0:0:1");
	Expect ("2001:0:0:1:0:0:0:1", (const byte[16]){0x20, 0x01, 0, 0, 0, 0, 0, 1, [15] = 1}, "2001:0:0:1::1");
	Expect ("2001:db8:0:1:1:1:1:1", (const byte[16]){0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
		"2001:db8:0:1:1:1:1:1");
	Expect ("0:0:0:0:0:0:0:1", (const byte[16]){[15] = 1}, "::1");
}

static void TestNotAddresses (void)
{
	const char	*bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.4x", " 1.2.3.4", "1..2.3", "1.2.3.4:27500",
		":", ":::", "1:2", "1:2:3:4:5:6:7:8:9", "::1:2:3:4:5:6:7:8", "1::2::3", "12345::", "g::1", "1:", ":1",
		"1:2:3:4:5:6:7:1.2.3.4", "::1%eth0", "[::1]", "localhost", "1.2.3.4/8"};
	byte		ip[16];
	int			i;

	for (i = 0 ; i < (int)(sizeof(bad) / sizeof(bad[0])) ; i++)
		if (NET_ParseIP (bad[i], ip))
		{
			printf ("FAIL \"%s\" read as an address\n", bad[i]);
			failures++;
		}
}

// IPv4's are ::ffff:a.b.c.d
static void TestIPv4 (void)
{
	netadr_t	a = {.type = NA_IP};

	NET_SetIPv4 (&a, (const byte[4]){127, 0, 0, 1});
	CHECK (NET_IsIPv4 (a));
	CHECK (!strcmp (NET_IPToString (a.ip), "127.0.0.1"));
	CHECK (NET_ParseIP ("::1", a.ip) && !NET_IsIPv4 (a));
}

// the caller's buffer: what NET_IPToString's holds, and none of it shared
static void TestIPToBuf (void)
{
	byte	one[16], two[16];
	char	a[48], b[48];

	CHECK (NET_ParseIP ("2001:db8::1:0:0:1", one) && NET_ParseIP ("10.0.0.7", two));
	CHECK (NET_IPToBuf (one, a, sizeof(a)) == a && NET_IPToBuf (two, b, sizeof(b)) == b);
	CHECK (!strcmp (a, "2001:db8::1:0:0:1") && !strcmp (b, "10.0.0.7"));
	CHECK (!strcmp (a, NET_IPToString (one)));
}

int main (void)
{
	TestRead ();
	TestWrite ();
	TestNotAddresses ();
	TestIPv4 ();
	TestIPToBuf ();
	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("Addresses: IPv4's and IPv6's, read and written\n");
	return 0;
}
