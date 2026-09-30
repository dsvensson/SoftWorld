/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#pragma once
// md4.h -- MD4 digests (RFC 1320), and the block checksums of maps and models

#include "q_types.h"

#include <stddef.h>

#define MD4_DIGEST_SIZE	16

void		MD4_Block (const void *data, size_t length, byte digest[MD4_DIGEST_SIZE]);

// the digest's four words xored
unsigned	Com_BlockChecksum (const void *buffer, int length);
