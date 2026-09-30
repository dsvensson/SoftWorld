// sha2.c -- SHA-2 digests (FIPS 180-4): SHA-224 and SHA-256 on 32-bit words,
// SHA-384 and SHA-512 on 64-bit ones

#include "sha2.h"

#include <string.h>

static const uint32_t	sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static const uint64_t	sha512_k[80] = {
	0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
	0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
	0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
	0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
	0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
	0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
	0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
	0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
	0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
	0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
	0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
	0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
	0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
	0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
	0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
	0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

static uint32_t SHA256_Rotate (uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static uint64_t SHA512_Rotate (uint64_t x, int n)
{
	return (x >> n) | (x << (64 - n));
}

// one 64 byte block into the state
static void SHA256_Transform (uint32_t state[8], const byte block[64])
{
	uint32_t	w[64], v[8], s0, s1, t1, t2;
	int			i;

	for (i = 0 ; i < 16 ; i++)
		w[i] = (uint32_t)block[i*4] << 24 | (uint32_t)block[i*4+1] << 16 | (uint32_t)block[i*4+2] << 8 | block[i*4+3];
	for ( ; i < 64 ; i++)
	{
		s0 = SHA256_Rotate (w[i-15], 7) ^ SHA256_Rotate (w[i-15], 18) ^ (w[i-15] >> 3);
		s1 = SHA256_Rotate (w[i-2], 17) ^ SHA256_Rotate (w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	memcpy (v, state, sizeof(v));
	for (i = 0 ; i < 64 ; i++)
	{
		t1 = v[7] + (SHA256_Rotate (v[4], 6) ^ SHA256_Rotate (v[4], 11) ^ SHA256_Rotate (v[4], 25))
			+ ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha256_k[i] + w[i];
		t2 = (SHA256_Rotate (v[0], 2) ^ SHA256_Rotate (v[0], 13) ^ SHA256_Rotate (v[0], 22))
			+ ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		memmove (v + 1, v, sizeof(v[0]) * 7);
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (i = 0 ; i < 8 ; i++)
		state[i] += v[i];
}

// one 128 byte block into the state
static void SHA512_Transform (uint64_t state[8], const byte block[128])
{
	uint64_t	w[80], v[8], s0, s1, t1, t2;
	int			i, j;

	for (i = 0 ; i < 16 ; i++)
		for (w[i] = 0, j = 0 ; j < 8 ; j++)
			w[i] = w[i] << 8 | block[i*8+j];
	for ( ; i < 80 ; i++)
	{
		s0 = SHA512_Rotate (w[i-15], 1) ^ SHA512_Rotate (w[i-15], 8) ^ (w[i-15] >> 7);
		s1 = SHA512_Rotate (w[i-2], 19) ^ SHA512_Rotate (w[i-2], 61) ^ (w[i-2] >> 6);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	memcpy (v, state, sizeof(v));
	for (i = 0 ; i < 80 ; i++)
	{
		t1 = v[7] + (SHA512_Rotate (v[4], 14) ^ SHA512_Rotate (v[4], 18) ^ SHA512_Rotate (v[4], 41))
			+ ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha512_k[i] + w[i];
		t2 = (SHA512_Rotate (v[0], 28) ^ SHA512_Rotate (v[0], 34) ^ SHA512_Rotate (v[0], 39))
			+ ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		memmove (v + 1, v, sizeof(v[0]) * 7);
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (i = 0 ; i < 8 ; i++)
		state[i] += v[i];
}

// SHA-224 or SHA-256 from its initial state, the first size bytes of the state out
static void SHA256_Digest (uint32_t state[8], const void *data, size_t length, byte *digest, int size)
{
	const byte	*p = data;
	byte		last[128];
	uint64_t	bits = (uint64_t)length * 8;
	size_t		rest, padded;
	int			i;

	for ( ; length >= 64 ; length -= 64, p += 64)
		SHA256_Transform (state, p);

	// what is left, a 1 bit, zeros, and the length in bits, in one block or two
	rest = length;
	memset (last, 0, sizeof(last));
	memcpy (last, p, rest);
	last[rest] = 0x80;
	padded = rest + 1 + 8 <= 64 ? 64 : 128;
	for (i = 0 ; i < 8 ; i++)
		last[padded - 1 - i] = (byte)(bits >> (8 * i));
	SHA256_Transform (state, last);
	if (padded == 128)
		SHA256_Transform (state, last + 64);

	for (i = 0 ; i < size ; i++)
		digest[i] = (byte)(state[i / 4] >> (24 - 8 * (i % 4)));
}

// SHA-384 or SHA-512 from its initial state, the first size bytes of the state out
static void SHA512_Digest (uint64_t state[8], const void *data, size_t length, byte *digest, int size)
{
	const byte	*p = data;
	byte		last[256];
	uint64_t	bits = (uint64_t)length * 8;
	size_t		rest, padded;
	int			i;

	for ( ; length >= 128 ; length -= 128, p += 128)
		SHA512_Transform (state, p);

	// the length is 128 bits: its high 64 are zero for lengths a size_t holds
	// (bar the top three bits of the byte count)
	rest = length;
	memset (last, 0, sizeof(last));
	memcpy (last, p, rest);
	last[rest] = 0x80;
	padded = rest + 1 + 16 <= 128 ? 128 : 256;
	for (i = 0 ; i < 8 ; i++)
		last[padded - 1 - i] = (byte)(bits >> (8 * i));
	last[padded - 9] = (byte)((uint64_t)length >> 61);
	SHA512_Transform (state, last);
	if (padded == 256)
		SHA512_Transform (state, last + 128);

	for (i = 0 ; i < size ; i++)
		digest[i] = (byte)(state[i / 8] >> (56 - 8 * (i % 8)));
}

void SHA224_Block (const void *data, size_t length, byte digest[SHA224_DIGEST_SIZE])
{
	uint32_t	state[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7,
		0xbefa4fa4};

	SHA256_Digest (state, data, length, digest, SHA224_DIGEST_SIZE);
}

void SHA256_Block (const void *data, size_t length, byte digest[SHA256_DIGEST_SIZE])
{
	uint32_t	state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
		0x5be0cd19};

	SHA256_Digest (state, data, length, digest, SHA256_DIGEST_SIZE);
}

void SHA384_Block (const void *data, size_t length, byte digest[SHA384_DIGEST_SIZE])
{
	uint64_t	state[8] = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
		0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};

	SHA512_Digest (state, data, length, digest, SHA384_DIGEST_SIZE);
}

void SHA512_Block (const void *data, size_t length, byte digest[SHA512_DIGEST_SIZE])
{
	uint64_t	state[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
		0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};

	SHA512_Digest (state, data, length, digest, SHA512_DIGEST_SIZE);
}
