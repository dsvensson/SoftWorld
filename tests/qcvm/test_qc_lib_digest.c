// test_qc_lib_digest.c -- crc16, digest_hex and digest_ptr (qcvm-rs's
// tests/all/builtins_strings/digest.rs and the unit tests of
// src/stdlib/digest.rs, docs/spec/strings.md)

#include "qc_harness.h"
#include "qc_lib.h"

#include <stdio.h>
#include <string.h>

// builtins FTE binds by name that its CSQC declarations don't list
static const char	*extra[] = {"argc", "instr", "ftou", "utof", "strcmp", NULL};

// CSQC with QuakeWorld's charset defaults (utf8_enable 0, the Quake scheme)
static qh_t *Harness (void)
{
	qc_config_t	config;

	QC_DefaultConfig (&config, QC_CSQC);
	config.utf8 = false;
	config.charscheme = QC_CHARS_QUAKE;
	return QH_New (QC_NUMBERING_CSQC, &config, QH_Named, extra);
}

// digest_hex's text of one argument, NULL for null
static const char *Digest (qh_t *h, const char *alg, const char *data)
{
	return QH_OptString (h, "digest_hex", ARGS (QH_S (h, alg), QH_S (h, data)));
}

// a string result that must not be null
static void Eq (const char *got, const char *want, const char *what)
{
	if (!QT_CHECK (got != NULL))
		printf ("  %s is null\n", what);
	else if (!QT_EQ_S (got, want))
		printf ("  %s\n", what);
}

// CRC-16/CCITT-FALSE, as the reference the builtins must agree with
static uint16_t Crc16 (const uint8_t *data, size_t len)
{
	uint16_t	crc = 0xFFFF;
	size_t		i;
	int			bit;

	for (i = 0 ; i < len ; i++)
	{
		crc ^= (uint16_t)(data[i] << 8);
		for (bit = 0 ; bit < 8 ; bit++)
			crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

// crc16 on the spec's examples, case-insensitive, over the strings joined
static void TestCrc16Examples (void)
{
	qh_t	*h = Harness ();
	float	lower;

	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, ""))), 65535);
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "A"))), 47381);
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "123456789"))), 10673);
	lower = QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "abc")));
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (1), QH_S (h, "ABC"))), lower);
	QT_CHECK (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "ABC"))) != lower);
	// the strings are joined
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "1234"), QH_S (h, "56789"))), 10673);
	QH_Free (h);
}

// digest_hex's CRC16 is little-endian; unknown names give null
static void TestDigestHexCrc16AndUnknownNames (void)
{
	qh_t	*h = Harness ();

	// FTE writes the CRC16's bytes little-endian
	Eq (Digest (h, "CRC16", "123456789"), "b129", "CRC16 123456789");
	Eq (Digest (h, "CRC16", "A"), "15b9", "CRC16 A");
	Eq (Digest (h, "CRC16", ""), "ffff", "CRC16 empty");
	QT_CHECK (Digest (h, "crc16", "abc") == NULL);
	QT_CHECK (Digest (h, "", "abc") == NULL);
	QH_Free (h);
}

// digest_hex on the standard test vectors of every algorithm
static void TestDigestHexVectors (void)
{
	static const struct
	{
		const char	*alg, *data, *want;
	} cases[] = {
		{"MD5", "", "d41d8cd98f00b204e9800998ecf8427e"},
		{"MD5", "abc", "900150983cd24fb0d6963f7d28e17f72"},
		{"MD4", "", "31d6cfe0d16ae931b73c59d7e0c089c0"},
		{"MD4", "abc", "a448017aaf21d8525fc10ae87aa6729d"},
		{"SHA1", "", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
		{"SHA1", "abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
		{"SHA256", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
		{"SHA2-256", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
		{"SHA224", "abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
		{"SHA2-224", "abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
		{"SHA384", "abc", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
			"8086072ba1e7cc2358baeca134c825a7"},
		{"SHA2-384", "abc", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
			"8086072ba1e7cc2358baeca134c825a7"},
		{"SHA512", "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
			"2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
		{"SHA2-512", "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
			"2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
		// FTE writes the CRC16's bytes little-endian
		{"CRC16", "123456789", "b129"},
		{"CRC16", "A", "15b9"},
		{"CRC16", "", "ffff"},
	};
	qh_t	*h = Harness ();
	char	what[64];
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
	{
		snprintf (what, sizeof(what), "%s of \"%s\"", cases[i].alg, cases[i].data);
		Eq (Digest (h, cases[i].alg, cases[i].data), cases[i].want, what);
	}
	// unknown (the names are case-sensitive): null
	QT_CHECK (Digest (h, "md5", "abc") == NULL);
	QT_CHECK (Digest (h, "SHA3", "abc") == NULL);
	QT_CHECK (Digest (h, "", "abc") == NULL);
	// the data arguments are joined
	Eq (QH_OptString (h, "digest_hex", ARGS (QH_S (h, "MD5"), QH_S (h, "a"), QH_S (h, "bc"))),
		"900150983cd24fb0d6963f7d28e17f72", "MD5 of \"a\" \"bc\"");
	QH_Free (h);
}

static void BufSetup (qc_asm_t *a, void *ctx)
{
	static const uint32_t	zeros[8] = {0};

	(void)ctx;
	QA_Global (a, "buf", QC_EV_FLOAT, zeros, 8);
}

// digest_ptr hashes the VM's memory, NULs included, and temp strings by reference
static void TestDigestPtrHashesVmMemory (void)
{
	qh_t		*h = QH_New (QC_NUMBERING_CSQC, NULL, BufSetup, NULL);
	uint32_t	word = 0, type = 0, ptr;
	qc_value_t	p, t;
	char		want[8], crc[8];
	uint16_t	c;

	if (!QT_CHECK (QC_FindGlobal (h->vm, "buf", &word, &type)))
	{
		QH_Free (h);
		return;
	}
	ptr = h->vm->progs[0].gbase + word * 4;
	QT_CHECK (QC_WriteMemory (h->vm, ptr, "a\0bc", 4));
	p = I ((int32_t)ptr);
	// NULs are hashed too
	c = Crc16 ((const uint8_t *)"a\0bc", 4);
	snprintf (want, sizeof(want), "%02x%02x", c & 0xFF, c >> 8);
	Eq (QH_OptString (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), p, I (4))), want, "CRC16 of a\\0bc");
	// with an offset
	snprintf (crc, sizeof(crc), "%s", Digest (h, "CRC16", "bc"));
	Eq (QH_OptString (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), p, I (2), I (2))), crc, "CRC16 at offset 2");
	// no bytes
	Eq (QH_OptString (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), p, I (0))), "ffff", "CRC16 of nothing");
	// an unknown algorithm: null
	QT_CHECK (QH_OptString (h, "digest_ptr", ARGS (QH_S (h, "nope"), p, I (4))) == NULL);
	// a temp string through its reference
	t = QH_S (h, "abc");
	snprintf (crc, sizeof(crc), "%s", Digest (h, "CRC16", "abc"));
	Eq (QH_OptString (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), I ((int32_t)t.w[0]), I (3))), crc, "CRC16 of a temp");
	// invalid ranges are builtin errors
	QT_EQ_I (QH_Fails (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), I (0x7FFF0000), I (4))), QC_ERR_BUILTIN);
	QT_EQ_I (QH_Fails (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), p, I (-1))), QC_ERR_BUILTIN);
	QT_EQ_I (QH_Fails (h, "digest_ptr", ARGS (QH_S (h, "CRC16"), I ((int32_t)t.w[0]), I (5))), QC_ERR_BUILTIN);
	QH_Free (h);
}

// digest_ptr with MD5, through a temp string's reference
static void TestDigestPtrWithMd5 (void)
{
	qh_t		*h = Harness ();
	qc_value_t	t = QH_S (h, "abc");

	Eq (QH_OptString (h, "digest_ptr", ARGS (QH_S (h, "MD5"), I ((int32_t)t.w[0]), I (3))),
		"900150983cd24fb0d6963f7d28e17f72", "MD5 of a temp");
	QH_Free (h);
}

// the CRC's check values (the unit test of src/stdlib/digest.rs)
static void TestCrc16Vectors (void)
{
	qh_t	*h = Harness ();

	QT_EQ_U (Crc16 ((const uint8_t *)"", 0), 0xFFFF);
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, ""))), 0xFFFF);
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "A"))), 0xB915);
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "123456789"))), 0x29B1);
	QT_EQ_F (QH_Float (h, "crc16", ARGS (F (1), QH_S (h, "ABC"))), QH_Float (h, "crc16", ARGS (F (0), QH_S (h, "abc"))));
	QH_Free (h);
}

// CRC16 as a digest is little-endian; the name is case-sensitive (the unit test)
static void TestCrc16DigestIsLittleEndian (void)
{
	uint8_t	out[64];

	QT_EQ_U (QC_Digest ("CRC16", "123456789", 9, out), 2);
	QT_CHECK (out[0] == 0xB1 && out[1] == 0x29);
	QT_EQ_U (QC_Digest ("crc16", "", 0, out), 0);
}

int main (void)
{
	TestCrc16Examples ();
	TestDigestHexCrc16AndUnknownNames ();
	TestDigestHexVectors ();
	TestDigestPtrHashesVmMemory ();
	TestDigestPtrWithMd5 ();
	TestCrc16Vectors ();
	TestCrc16DigestIsLittleEndian ();
	return QT_Finish ("lib_digest", "crc16 and the digests are FTE's, of strings and of memory");
}
