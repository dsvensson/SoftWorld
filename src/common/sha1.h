#pragma once
// sha1.h -- SHA-1 digests (FIPS 180-4), of the files f_modified checks

#include "q_types.h"

#include <stddef.h>

#define SHA1_DIGEST_SIZE	20

void	SHA1_Block (const void *data, size_t length, byte digest[SHA1_DIGEST_SIZE]);
