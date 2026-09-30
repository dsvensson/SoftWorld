// test_sha2.c -- SHA224_Block, SHA256_Block, SHA384_Block and SHA512_Block on
// FIPS 180's test messages, and on messages that end on each side of a block's end

#include "sha2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;

typedef void (*digest_t) (const void *data, size_t length, byte *digest);

static void Check (const char *alg, digest_t f, int size, const void *data, size_t length, const char *expected)
{
	byte	digest[SHA512_DIGEST_SIZE];
	char	hex[SHA512_DIGEST_SIZE * 2 + 1];
	int		i;

	f (data, length, digest);
	for (i = 0 ; i < size ; i++)
		snprintf (hex + i * 2, 3, "%02x", digest[i]);
	if (!strcmp (hex, expected))
		return;
	printf ("%s of %zu bytes: %s, not %s\n", alg, length, hex, expected);
	failures++;
}

#define SHA224(d, l, e)	Check ("SHA-224", (digest_t)SHA224_Block, SHA224_DIGEST_SIZE, d, l, e)
#define SHA256(d, l, e)	Check ("SHA-256", (digest_t)SHA256_Block, SHA256_DIGEST_SIZE, d, l, e)
#define SHA384(d, l, e)	Check ("SHA-384", (digest_t)SHA384_Block, SHA384_DIGEST_SIZE, d, l, e)
#define SHA512(d, l, e)	Check ("SHA-512", (digest_t)SHA512_Block, SHA512_DIGEST_SIZE, d, l, e)

int main (void)
{
	static char	million[1000000];
	const char	*m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	const char	*m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
		"mnopqrstnopqrstu";

	SHA224 ("", 0, "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f");
	SHA224 ("abc", 3, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
	SHA224 (m448, 56, "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525");
	SHA256 ("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	SHA256 ("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	SHA256 (m448, 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	SHA384 ("", 0, "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
	SHA384 ("abc", 3, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
	SHA384 (m896, 112, "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039");
	SHA512 ("", 0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f"
		"63b931bd47417a81a538327af927da3e");
	SHA512 ("abc", 3, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
		"454d4423643ce80e2a9ac94fa54ca49f");
	SHA512 (m896, 112, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433a"
		"c7d329eeb6dd26545e96e55b874be909");
	memset (million, 'a', sizeof(million));
	SHA256 (million, sizeof(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	SHA512 (million, sizeof(million), "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a"
		"4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
	// the padding's edges: the length fitting in the last block or not
	SHA256 (million, 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	SHA256 (million, 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	SHA512 (million, 111, "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811b953828274461"
		"673c68d04e297b0eb7b2b4d60fc6b566a2");
	SHA512 (million, 112, "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568b88fd2d970929b"
		"719ecbb152f58f130a407c8830604b70ca");
	if (failures)
		return 1;
	printf ("sha2: the digests are FIPS 180's\n");
	return 0;
}
