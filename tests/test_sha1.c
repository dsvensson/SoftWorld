// test_sha1.c -- SHA1_Block on FIPS 180's test messages, and on messages that
// end on each side of a block's end

#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;

static void Check (const char *what, const void *data, size_t length, const char *expected)
{
	byte	digest[SHA1_DIGEST_SIZE];
	char	hex[SHA1_DIGEST_SIZE * 2 + 1];
	int		i;

	SHA1_Block (data, length, digest);
	for (i = 0 ; i < SHA1_DIGEST_SIZE ; i++)
		snprintf (hex + i * 2, 3, "%02x", digest[i]);
	if (!strcmp (hex, expected))
		return;
	printf ("%s: %s, not %s\n", what, hex, expected);
	failures++;
}

int main (void)
{
	static char	million[1000000];
	char		text[128];

	Check ("empty", "", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	Check ("abc", "abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d");
	Check ("448 bits", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
		"84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	Check ("896 bits", "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
		"mnopqrstnopqrstu", 112, "a49b2446a02c645bf419f995b67091253a04a259");
	memset (million, 'a', sizeof(million));
	Check ("a million a's", million, sizeof(million), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

	// 55 bytes fit the length in the last block, 56 don't; 64 fill one
	memset (text, 'a', sizeof(text));
	Check ("55 a's", text, 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a");
	Check ("56 a's", text, 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699");
	Check ("64 a's", text, 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d");

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("SHA-1: FIPS 180's digests\n");
	return 0;
}
