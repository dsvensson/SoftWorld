#pragma once
// sha2.h -- SHA-2 digests (FIPS 180-4), for QuakeC's digest builtins

#include "q_types.h"

#include <stddef.h>

#define SHA224_DIGEST_SIZE	28
#define SHA256_DIGEST_SIZE	32
#define SHA384_DIGEST_SIZE	48
#define SHA512_DIGEST_SIZE	64

void	SHA224_Block (const void *data, size_t length, byte digest[SHA224_DIGEST_SIZE]);
void	SHA256_Block (const void *data, size_t length, byte digest[SHA256_DIGEST_SIZE]);
void	SHA384_Block (const void *data, size_t length, byte digest[SHA384_DIGEST_SIZE]);
void	SHA512_Block (const void *data, size_t length, byte digest[SHA512_DIGEST_SIZE]);
