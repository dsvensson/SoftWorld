#pragma once
// md5.h -- MD5 digests (RFC 1321), for QuakeC's digest builtins

#include "q_types.h"

#include <stddef.h>

#define MD5_DIGEST_SIZE	16

void	MD5_Block (const void *data, size_t length, byte digest[MD5_DIGEST_SIZE]);
