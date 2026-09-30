// test_md5.c -- MD5_Block on RFC 1321's test messages

#include "md5.h"

#include <stdio.h>
#include <string.h>

static const struct
{
	const char	*text;
	const char	*digest;
} tests[] = {
	{"", "d41d8cd98f00b204e9800998ecf8427e"},
	{"a", "0cc175b9c0f1b6a831c399e269772661"},
	{"abc", "900150983cd24fb0d6963f7d28e17f72"},
	{"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
	{"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
	{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
	{"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
		"57edf4a22be3c955ac49da2e2107b67a"},
};

int main (void)
{
	byte	digest[MD5_DIGEST_SIZE];
	char	hex[MD5_DIGEST_SIZE * 2 + 1];
	int		failures = 0, i;

	for (size_t t = 0 ; t < sizeof(tests) / sizeof(tests[0]) ; t++)
	{
		MD5_Block (tests[t].text, strlen (tests[t].text), digest);
		for (i = 0 ; i < MD5_DIGEST_SIZE ; i++)
			snprintf (hex + i * 2, 3, "%02x", digest[i]);
		if (strcmp (hex, tests[t].digest))
		{
			printf ("\"%s\": %s, not %s\n", tests[t].text, hex, tests[t].digest);
			failures++;
		}
	}
	if (failures)
		return 1;
	printf ("md5: the digests are RFC 1321's\n");
	return 0;
}
