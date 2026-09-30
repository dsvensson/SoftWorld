// qc_lib_digest.c -- crc16, digest_hex and digest_ptr (docs/spec/strings.md)
//
// The digests are common's: MD4, MD5, SHA-1 and SHA-2, and CRC-16/CCITT-FALSE.

#include "qc_lib.h"

#include "crc.h"
#include "md4.h"
#include "md5.h"
#include "sha1.h"
#include "sha2.h"

#include <stdlib.h>
#include <string.h>

// CRC-16/CCITT-FALSE (0x1021, from 0xFFFF, not reflected), of the bytes lowered
// to ASCII lower case if asked
static uint16_t QC_Crc16 (const char *data, size_t len, bool lowercase)
{
	uint16_t	crc = 0xFFFF;
	size_t		i;
	int			bit;
	uint8_t		b;

	for (i = 0 ; i < len ; i++)
	{
		b = (uint8_t)data[i];
		if (lowercase && b >= 'A' && b <= 'Z')
			b = (uint8_t)(b + 32);
		crc ^= (uint16_t)(b << 8);
		for (bit = 0 ; bit < 8 ; bit++)
			crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

// the digest of data by FTE's algorithm name: its size, or 0 for a name it doesn't know
size_t QC_Digest (const char *alg, const void *data, size_t len, uint8_t out[64])
{
	uint16_t	crc;

	if (!strcmp (alg, "MD4"))
	{
		MD4_Block (data, len, out);
		return MD4_DIGEST_SIZE;
	}
	if (!strcmp (alg, "MD5"))
	{
		MD5_Block (data, len, out);
		return MD5_DIGEST_SIZE;
	}
	if (!strcmp (alg, "SHA1"))
	{
		SHA1_Block (data, len, out);
		return SHA1_DIGEST_SIZE;
	}
	if (!strcmp (alg, "SHA2-224") || !strcmp (alg, "SHA224"))
	{
		SHA224_Block (data, len, out);
		return SHA224_DIGEST_SIZE;
	}
	if (!strcmp (alg, "SHA2-256") || !strcmp (alg, "SHA256"))
	{
		SHA256_Block (data, len, out);
		return SHA256_DIGEST_SIZE;
	}
	if (!strcmp (alg, "SHA2-384") || !strcmp (alg, "SHA384"))
	{
		SHA384_Block (data, len, out);
		return SHA384_DIGEST_SIZE;
	}
	if (!strcmp (alg, "SHA2-512") || !strcmp (alg, "SHA512"))
	{
		SHA512_Block (data, len, out);
		return SHA512_DIGEST_SIZE;
	}
	if (!strcmp (alg, "CRC16"))
	{
		// FTE writes the CRC's bytes little-endian
		crc = QC_Crc16 (data, len, false);
		out[0] = (uint8_t)crc;
		out[1] = (uint8_t)(crc >> 8);
		return 2;
	}
	return 0;
}

// a digest in lower-case hex, or null for an algorithm it doesn't know
static bool QC_ReturnDigest (qcvm_t *vm, const char *alg, const void *data, size_t len)
{
	static const char	digits[] = "0123456789abcdef";
	uint8_t				digest[64];
	char				hex[129];
	size_t				n = QC_Digest (alg, data, len, digest), i;

	if (!n)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	for (i = 0 ; i < n ; i++)
	{
		hex[i * 2] = digits[digest[i] >> 4];
		hex[i * 2 + 1] = digits[digest[i] & 15];
	}
	return QC_ReturnString (vm, hex, n * 2);
}

// float crc16(float caseinsensitive, string...): CRC-16/CCITT-FALSE of the strings joined
static bool QC_Crc16Builtin (qcvm_t *vm)
{
	bool	lowercase = QC_LibArgInt (vm, 0) != 0;
	size_t	len;
	char	*text = QC_LibConcat (vm, 1, &len);

	if (!text)
		return false;
	QC_ReturnFloat (vm, (float)QC_Crc16 (text, len, lowercase));
	free (text);
	return true;
}

// string digest_hex(string alg, string data...): the lower-case hex digest of the
// strings joined, by MD4, MD5, SHA1, SHA2-224 (SHA224), SHA2-256 (SHA256),
// SHA2-384 (SHA384), SHA2-512 (SHA512) or CRC16; null for any other name
static bool QC_DigestHex (qcvm_t *vm)
{
	size_t	len;
	char	*data = QC_LibConcat (vm, 1, &len), *alg;
	bool	ok;

	if (!data)
		return false;
	if (!(alg = QC_LibDup (QC_ArgString (vm, 0))))
	{
		free (data);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	ok = QC_ReturnDigest (vm, alg, data, len);
	free (alg);
	free (data);
	return ok;
}

// len bytes of the VM's memory at base + offset, through a temp or static
// string's handle too; a malloc'd copy, or NULL
static uint8_t *QC_ReadBlock (qcvm_t *vm, uint32_t base, uint32_t offset, size_t len)
{
	uint8_t		*out = malloc (len ? len : 1), *data;
	const char	*text;
	uint32_t	size;

	if (!out)
		return NULL;
	if (len <= UINT32_MAX && QC_ReadBytes (&vm->mem, base + offset, out, (uint32_t)len))
		return out;
	switch (base & QC_TAG_MASK)
	{
	case QC_TEMP_TAG:
		data = QC_TempData (&vm->strings, base & QC_INDEX_MASK, &size);
		if (data && (uint64_t)offset + len <= size)
		{
			memcpy (out, data + offset, len);
			return out;
		}
		break;
	case QC_STATIC_TAG:
		text = QC_StaticText (&vm->strings, base & QC_INDEX_MASK);
		if (text && (uint64_t)offset + len <= strlen (text))
		{
			memcpy (out, text + offset, len);
			return out;
		}
		break;
	default:
		break;
	}
	free (out);
	return NULL;
}

// string digest_ptr(string alg, void *data, int length, optional int offset):
// digest_hex over length bytes of the VM's memory (NULs and all)
static bool QC_DigestPtr (qcvm_t *vm)
{
	uint32_t	base = QC_ArgWord (vm, 1), offset = QC_Argc (vm) > 3 ? QC_ArgWord (vm, 3) : 0;
	int32_t		len = QC_ArgInt (vm, 2);
	uint8_t		*data = len >= 0 ? QC_ReadBlock (vm, base, offset, (size_t)len) : NULL;
	char		*alg;
	bool		ok;

	if (!data)
		return QC_Error (vm, "digest_ptr: invalid pointer");
	if (!(alg = QC_LibDup (QC_ArgString (vm, 0))))
	{
		free (data);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	ok = QC_ReturnDigest (vm, alg, data, (size_t)len);
	free (alg);
	free (data);
	return ok;
}

static const qc_libentry_t	qc_digest[] = {
	{"crc16", QC_Crc16Builtin, NULL, 0},
	{"digest_hex", QC_DigestHex, NULL, 0},
	{"digest_ptr", QC_DigestPtr, NULL, 0},
};

bool QC_RegisterDigest (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_digest, sizeof(qc_digest) / sizeof(qc_digest[0]));
}
