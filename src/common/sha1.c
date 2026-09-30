// sha1.c -- SHA-1 digests (FIPS 180-4)

#include "sha1.h"

#include <string.h>

static uint32_t SHA1_Rotate (uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

// one 64 byte block into the state
static void SHA1_Transform (uint32_t state[5], const byte block[64])
{
	uint32_t	w[80], a, b, c, d, e, f, k, t;
	int			i;

	for (i = 0 ; i < 16 ; i++)
		w[i] = (uint32_t)block[i*4] << 24 | (uint32_t)block[i*4+1] << 16 | (uint32_t)block[i*4+2] << 8 | block[i*4+3];
	for ( ; i < 80 ; i++)
		w[i] = SHA1_Rotate (w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	for (i = 0 ; i < 80 ; i++)
	{
		if (i < 20)
		{
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		}
		else if (i < 40)
		{
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		}
		else if (i < 60)
		{
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		}
		else
		{
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		t = SHA1_Rotate (a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = SHA1_Rotate (b, 30);
		b = a;
		a = t;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

void SHA1_Block (const void *data, size_t length, byte digest[SHA1_DIGEST_SIZE])
{
	uint32_t	state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	const byte	*p = data;
	byte		last[128];
	uint64_t	bits = (uint64_t)length * 8;
	size_t		rest, padded;
	int			i;

	for ( ; length >= 64 ; length -= 64, p += 64)
		SHA1_Transform (state, p);

	// what is left, a 1 bit, zeros, and the length in bits, in one block or two
	rest = length;
	memset (last, 0, sizeof(last));
	memcpy (last, p, rest);
	last[rest] = 0x80;
	padded = rest + 1 + 8 <= 64 ? 64 : 128;
	for (i = 0 ; i < 8 ; i++)
		last[padded - 1 - i] = (byte)(bits >> (8 * i));
	SHA1_Transform (state, last);
	if (padded == 128)
		SHA1_Transform (state, last + 64);

	for (i = 0 ; i < SHA1_DIGEST_SIZE ; i++)
		digest[i] = (byte)(state[i / 4] >> (24 - 8 * (i % 4)));
}
