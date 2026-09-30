// qc_lib_memory.c -- the QuakeC heap, pointers, and base64 (docs/spec/builtins.md)
//
// memalloc's blocks are in the VM's heap (region H), so a pointer is h_base
// plus the block's offset, and every pointer opcode works with it. The copy and
// fill builtins take temp buffers too (createbuffer, temp strings), which grow
// when written past their end, as in FTE.

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QC_MAX_ALLOC	0x01000000		// memalloc's largest block (FTE's)

/*
==============================================================================

THE HEAP

==============================================================================
*/

// n zeroed bytes of the heap: the pointer, or 0
static uint32_t QC_HeapPointer (qcvm_t *vm, uint32_t n)
{
	uint32_t	ofs;

	if (!QC_HeapAlloc (&vm->mem.heap, n, &ofs) || ofs > UINT32_MAX - vm->mem.h_base)
		return 0;
	return vm->mem.h_base + ofs;
}

// memalloc's and memrealloc's size: 0 is 1; negative or past 16 MiB refused (-1)
static int64_t QC_AllocSize (int32_t size)
{
	if (!size)
		size = 1;
	return size < 0 || size > QC_MAX_ALLOC ? -1 : size;
}

/*
==============================================================================

POINTER RANGES

==============================================================================
*/

// ptr + ofs for a linear pointer, or -1
static int64_t QC_Linear (uint32_t p, int32_t ofs)
{
	int64_t	a = (int64_t)p + ofs;

	return a >= 0 && a <= UINT32_MAX ? a : -1;
}

// Reads n bytes at ptr + ofs (a linear pointer, or a temp buffer as large);
// a malloc'd copy, or NULL
static uint8_t *QC_ReadRange (qcvm_t *vm, uint32_t ptr, int32_t ofs, size_t n)
{
	uint8_t		*out, *data;
	uint32_t	size;
	int64_t		addr;

	if (n > UINT32_MAX || !(out = malloc (n ? n : 1)))
		return NULL;
	switch (ptr & QC_TAG_MASK)
	{
	case QC_TEMP_TAG:
		data = QC_TempData (&vm->strings, ptr & QC_INDEX_MASK, &size);
		if (data && ofs >= 0 && (uint64_t)ofs + n <= size)
		{
			memcpy (out, data + ofs, n);
			return out;
		}
		break;
	case QC_STATIC_TAG:
		break;
	default:
		addr = QC_Linear (ptr, ofs);
		if (addr >= 0 && !(addr == 0 && n > 1) && QC_ReadBytes (&vm->mem, (uint32_t)addr, out, (uint32_t)n))
			return out;
		break;
	}
	free (out);
	return NULL;
}

// a checked destination for a write
typedef struct
{
	bool		temp;
	uint32_t	addr;			// linear
	uint32_t	slot;			// temp
	size_t		start;
} qc_dest_t;

// Checks a write of n bytes at ptr + ofs: linear memory (not 0), or a temp
// buffer that may grow to 1 MiB
static bool QC_Dest (qcvm_t *vm, uint32_t ptr, int32_t ofs, size_t n, qc_dest_t *d)
{
	qc_loc_t			loc;
	qc_writeresult_t	r;
	int64_t				addr;

	switch (ptr & QC_TAG_MASK)
	{
	case QC_TEMP_TAG:
		if (!QC_TempData (&vm->strings, ptr & QC_INDEX_MASK, NULL) || ofs < 0 || (uint64_t)ofs + n > QC_MAX_TEMP_GROWTH)
			return false;
		*d = (qc_dest_t){.temp = true, .slot = ptr & QC_INDEX_MASK, .start = (size_t)ofs};
		return true;
	case QC_STATIC_TAG:
		return false;
	default:
		addr = QC_Linear (ptr, ofs);
		if (addr < 0 || n > UINT32_MAX)
			return false;
		r = QC_CheckWrite (&vm->mem, (uint32_t)addr, (uint32_t)n, &loc);
		if (r != QC_WRITE_OK && r != QC_WRITE_PROTECTED)
			return false;
		*d = (qc_dest_t){.addr = (uint32_t)addr};
		return true;
	}
}

// Writes to a checked destination; a protected entity is skipped with a
// warning. False if a temp buffer couldn't grow.
static bool QC_WriteDest (qcvm_t *vm, const qc_dest_t *d, const uint8_t *bytes, size_t n)
{
	uint32_t			ent;
	uint8_t				*data;
	qc_writeresult_t	r;

	if (d->temp)
	{
		if (!(data = QC_TempGrow (&vm->strings, d->slot, d->start + n)))
			return false;
		memcpy (data + d->start, bytes, n);
		return true;
	}
	r = QC_WriteBytes (&vm->mem, d->addr, bytes, (uint32_t)n, &ent);
	if (r == QC_WRITE_PROTECTED)
		QC_Warn (vm, QC_WARN_READONLY_ENTITY, ent, NULL);
	return r == QC_WRITE_OK || r == QC_WRITE_PROTECTED;
}

static int32_t QC_OptInt (const qcvm_t *vm, int i)
{
	return QC_Argc (vm) > i ? QC_ArgInt (vm, i) : 0;
}

static void QC_ReturnZero (qcvm_t *vm)
{
	static const uint32_t	zero[3];

	QC_ReturnRaw (vm, zero);
}

/*
==============================================================================

BUILTINS

==============================================================================
*/

// __variant *memalloc(int size): a zeroed block of the heap (size 0 counts as 1);
// negative sizes, those past 16 MiB and a heap used up give null and a builtin error
static bool QC_Memalloc (qcvm_t *vm)
{
	int32_t		size = QC_ArgInt (vm, 0);
	int64_t		n = QC_AllocSize (size);
	uint32_t	p = n < 0 ? 0 : QC_HeapPointer (vm, (uint32_t)n);

	QC_ReturnWord (vm, p);
	if (!p)
		return QC_LibSoftError (vm, "memalloc: failure (size %d)", size);
	return true;
}

// void memfree(__variant *ptr): frees a block (null ignored; anything not a
// block's start only warns)
static bool QC_Memfree (qcvm_t *vm)
{
	uint32_t	p = QC_ArgWord (vm, 0);

	if (p && (p < vm->mem.h_base || !QC_HeapRelease (&vm->mem.heap, p - vm->mem.h_base)))
		QC_Warning (vm, "memfree: %#x is not an allocated block", p);
	return true;
}

// __variant *memrealloc(__variant *ptr, int size): a new block with the old
// one's contents (the rest zeroed), the old one freed; a null ptr allocates
static bool QC_Memrealloc (qcvm_t *vm)
{
	uint32_t	p = QC_ArgWord (vm, 0), ofs, r = 0;
	int32_t		size = QC_ArgInt (vm, 1);
	int64_t		n = QC_AllocSize (size);

	if (n >= 0 && (!p || p >= vm->mem.h_base)
		&& QC_HeapRealloc (&vm->mem.heap, p != 0, p ? p - vm->mem.h_base : 0, (uint32_t)n, &ofs)
		&& ofs <= UINT32_MAX - vm->mem.h_base)
		r = vm->mem.h_base + ofs;
	QC_ReturnWord (vm, r);
	if (!r)
		return QC_LibSoftError (vm, "memrealloc: failure (size %d)", size);
	return true;
}

// void memcpy(__variant *dst, __variant *src, int size, optional int srcofs,
// optional int dstofs): size bytes copied (overlapping safely); the offsets are
// bytes, the source's first, as FTE has it
static bool QC_Memcpy (qcvm_t *vm)
{
	uint32_t	dst = QC_ArgWord (vm, 0), src = QC_ArgWord (vm, 1);
	int32_t		size = QC_ArgInt (vm, 2), sofs = QC_OptInt (vm, 3), dofs = QC_OptInt (vm, 4);
	qc_dest_t	d;
	uint8_t		*bytes;
	bool		ok;

	if (size < 0)
		return QC_LibSoftError (vm, "memcpy: invalid size %#x", (uint32_t)size);
	if (!size)
		return true;
	if (!QC_Dest (vm, dst, dofs, (size_t)size, &d))
		return QC_LibSoftError (vm, "memcpy: invalid dest (%#x+%#x)", dst, (uint32_t)dofs);
	if (!(bytes = QC_ReadRange (vm, src, sofs, (size_t)size)))
		return QC_LibSoftError (vm, "memcpy: invalid source (%#x+%#x)", src, (uint32_t)sofs);
	ok = QC_WriteDest (vm, &d, bytes, (size_t)size);
	free (bytes);
	if (!ok)
		return QC_LibSoftError (vm, "memcpy: invalid dest (%#x+%#x)", dst, (uint32_t)dofs);
	return true;
}

// void memfill8(__variant *dst, int value, int size, optional int dstofs): size
// bytes of value's low byte
static bool QC_Memfill8 (qcvm_t *vm)
{
	uint32_t	dst = QC_ArgWord (vm, 0), value = QC_ArgWord (vm, 1);
	int32_t		size = QC_ArgInt (vm, 2), dofs = QC_OptInt (vm, 3);
	qc_dest_t	d;
	uint8_t		*fill;
	bool		ok;

	if (size < 0 || !QC_Dest (vm, dst, dofs, (size_t)size, &d))
		return QC_LibSoftError (vm, "memfill8: invalid dest");
	if (!(fill = malloc (size ? (size_t)size : 1)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HEAP, NULL);
	memset (fill, (int)(value & 0xFF), (size_t)size);
	ok = QC_WriteDest (vm, &d, fill, (size_t)size);
	free (fill);
	if (!ok)
		return QC_LibSoftError (vm, "memfill8: invalid dest");
	return true;
}

// the address memgetval and memsetval use, ptr + ofs * 4 (the offset counts
// words): a fractional offset and a misaligned address only warn; false if
// it's no address
static bool QC_WordAddress (qcvm_t *vm, const char *name, uint32_t *addr)
{
	int32_t	p = QC_ArgInt (vm, 0);
	float	ofs = QC_ArgFloat (vm, 1);
	int64_t	a;

	if (ofs != (float)QC_FloatToInt (ofs))
		QC_Warning (vm, "%s: non-integer offset", name);
	// exactly (FTE rounds the sum through a float, losing precision past 2^24)
	a = QC_D2I64 ((double)p + (double)ofs * 4.0);
	if (a < 0 || a > UINT32_MAX)
		return false;
	*addr = (uint32_t)a;
	if (*addr & 3)
		QC_Warning (vm, "%s: misaligned pointer (%#x)", name, *addr);
	return true;
}

// __variant memgetval(__variant *ptr, float ofs): the word at ptr + ofs * 4;
// reading outside the VM's memory is an error
static bool QC_Memgetval (qcvm_t *vm)
{
	uint32_t	addr, v;

	if (!QC_WordAddress (vm, "memgetval", &addr))
		return QC_Fail (vm, QC_ERR_BAD_POINTER_READ, QC_ArgWord (vm, 0), NULL);
	if (!QC_ReadBytes (&vm->mem, addr, &v, 4))
		return QC_Fail (vm, QC_ERR_BAD_POINTER_READ, addr, NULL);
	QC_ReturnWord (vm, v);
	return true;
}

// void memsetval(__variant *ptr, float ofs, __variant value): writes the word at
// ptr + ofs * 4; outside the VM's memory an error, a protected entity skipped
// with a warning
static bool QC_Memsetval (qcvm_t *vm)
{
	uint32_t			value = QC_ArgWord (vm, 2), addr, ent;
	qc_writeresult_t	r;

	if (!QC_WordAddress (vm, "memsetval", &addr))
		return QC_Fail (vm, QC_ERR_BAD_POINTER_WRITE, QC_ArgWord (vm, 0), NULL);
	r = QC_WriteBytes (&vm->mem, addr, &value, 4, &ent);
	switch (r)
	{
	case QC_WRITE_OK:
		return true;
	case QC_WRITE_PROTECTED:
		QC_Warn (vm, QC_WARN_READONLY_ENTITY, ent, NULL);
		return true;
	case QC_WRITE_NULL:
		return QC_Fail (vm, QC_ERR_NULL_POINTER_WRITE, 0, NULL);
	default:
		return QC_Fail (vm, QC_ERR_BAD_POINTER_WRITE, addr, NULL);
	}
}

// __variant *memptradd(__variant *ptr, float ofs): ptr + ofs (bytes); fractional,
// misaligned and negative offsets are builtin errors
static bool QC_Memptradd (qcvm_t *vm)
{
	uint32_t	p = QC_ArgWord (vm, 0);
	float		f = QC_ArgFloat (vm, 1);
	int32_t		ofs = QC_FloatToInt (f);

	if ((float)ofs != f && !QC_LibSoftError (vm, "memptradd: non-integer offset"))
		return false;
	if ((ofs & 3) && !QC_LibSoftError (vm, "memptradd: offset is not 32-bit aligned"))
		return false;
	if (ofs < 0 && !QC_LibSoftError (vm, "memptradd: negative offset"))
		return false;
	QC_ReturnWord (vm, p + (uint32_t)ofs);
	return true;
}

// int memcmp(__variant *a, __variant *b, int size, optional int aofs, optional
// int bofs): the difference of the first bytes that differ, or 0
static bool QC_Memcmp (qcvm_t *vm)
{
	uint32_t	a = QC_ArgWord (vm, 0), b = QC_ArgWord (vm, 1);
	int32_t		size = QC_ArgInt (vm, 2), aofs = QC_OptInt (vm, 3), bofs = QC_OptInt (vm, 4), diff = 0;
	uint8_t		*x, *y;
	size_t		i;

	QC_ReturnZero (vm);
	if (size < 0)
		return QC_LibSoftError (vm, "memcmp: invalid size");
	if (!size)
		return true;
	if (!(x = QC_ReadRange (vm, a, aofs, (size_t)size)))
		return QC_LibSoftError (vm, "memcmp: invalid first pointer");
	if (!(y = QC_ReadRange (vm, b, bofs, (size_t)size)))
	{
		free (x);
		return QC_LibSoftError (vm, "memcmp: invalid second pointer");
	}
	for (i = 0 ; i < (size_t)size && !diff ; i++)
		diff = (int32_t)x[i] - (int32_t)y[i];
	free (x);
	free (y);
	QC_ReturnInt (vm, diff);
	return true;
}

// void *createbuffer(int size): a zeroed temp buffer of size + 1 bytes (a temp
// string's handle that works as a pointer; collected as temp strings are, not
// freed); null if size is 0 or less
static bool QC_Createbuffer (qcvm_t *vm)
{
	int32_t		size = QC_ArgInt (vm, 0);
	char		*zeros;
	uint32_t	r;

	QC_ReturnZero (vm);
	if (size <= 0)
		return true;
	if ((size_t)size > vm->config.limits.temp_string_bytes)
		return QC_LibSoftError (vm, "createbuffer: %d bytes is too large", size);
	if (!(zeros = calloc ((size_t)size, 1)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	r = QC_NewTemp (vm, zeros, (size_t)size);
	free (zeros);
	if (!r)
		return false;
	QC_ReturnWord (vm, r);
	return true;
}

static const char	qc_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void QC_Base64Encode (qc_sink_t *out, const uint8_t *data, size_t len)
{
	size_t		i;
	uint32_t	v;

	for (i = 0 ; i < len ; i += 3)
	{
		v = (uint32_t)data[i] << 16;
		if (i + 1 < len)
			v |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len)
			v |= data[i + 2];
		QC_SinkPush (out, qc_base64[(v >> 18) & 63]);
		QC_SinkPush (out, qc_base64[(v >> 12) & 63]);
		QC_SinkPush (out, i + 1 < len ? qc_base64[(v >> 6) & 63] : '=');
		QC_SinkPush (out, i + 2 < len ? qc_base64[v & 63] : '=');
	}
}

// a base64 symbol's value as FTE takes it: - and _ for 62 and 63, anything else
// (padding too) 0
static uint32_t QC_Base64Value (uint8_t c)
{
	if (c >= 'A' && c <= 'Z')
		return (uint32_t)(c - 'A');
	if (c >= 'a' && c <= 'z')
		return (uint32_t)(c - 'a' + 26);
	if (c >= '0' && c <= '9')
		return (uint32_t)(c - '0' + 52);
	if (c == '+' || c == '-')
		return 62;
	if (c == '/' || c == '_')
		return 63;
	return 0;
}

size_t QC_Base64Capacity (size_t len)
{
	return (len + 3) / 4 * 3 + 1;
}

void QC_Base64Decode (qc_sink_t *out, const char *s, size_t len, size_t cap)
{
	size_t		i = 0, room = cap;
	uint32_t	v;

#define AT(i)	((i) < len ? (uint8_t)s[i] : 0)
	// control characters before a group's first two symbols are skipped; = or
	// the end after two or three ends the data; invalid symbols are zero bits
	while (room > 1)
	{
		while (AT (i) >= 1 && AT (i) < ' ')
			i++;
		if (i >= len)
			break;
		v = QC_Base64Value (AT (i)) << 18;
		for (i++ ; AT (i) >= 1 && AT (i) < ' ' ; i++)
			;
		if (i >= len)
			break;
		v |= QC_Base64Value (AT (i)) << 12;
		i++;
		QC_SinkPush (out, (char)((v >> 16) & 0xFF));
		if (i >= len || AT (i) == '=' || room < 2)
			break;
		v |= QC_Base64Value (AT (i)) << 6;
		i++;
		QC_SinkPush (out, (char)((v >> 8) & 0xFF));
		if (i >= len || AT (i) == '=' || room < 3)
			break;
		v |= QC_Base64Value (AT (i));
		i++;
		QC_SinkPush (out, (char)(v & 0xFF));
		room -= 3;
	}
#undef AT
}

// string base64encode(__variant *ptr, int size): size bytes of memory as base64
// (+, / and = padding); null and a builtin error if they can't be read
static bool QC_Base64encodeBuiltin (qcvm_t *vm)
{
	uint32_t	p = QC_ArgWord (vm, 0);
	int32_t		size = QC_ArgInt (vm, 1);
	uint8_t		*data = NULL;
	qc_sink_t	out;

	if (size < 0 || (size > 0 && (!p || !(data = QC_ReadRange (vm, p, 0, (size_t)size)))))
	{
		QC_ReturnZero (vm);
		return QC_LibSoftError (vm, "base64encode: invalid pointer");
	}
	// four characters a three bytes; refused before what couldn't be stored is made
	if (!QC_StringsFit (&vm->strings, ((size_t)size + 2) / 3 * 4))
	{
		free (data);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	QC_SinkInit (&out, SIZE_MAX);
	QC_Base64Encode (&out, data, (size_t)size);
	free (data);
	return QC_LibReturnSink (vm, &out);
}

// __variant *base64decode(string s, __out int size): decoded into a new heap
// block (memfree it; a NUL follows the data), its length in size
static bool QC_Base64decodeBuiltin (qcvm_t *vm)
{
	const char	*s = QC_ArgString (vm, 0);
	size_t		len = strlen (s), cap = QC_Base64Capacity (len);
	qc_sink_t	data;
	uint32_t	p;

	QC_SinkInit (&data, SIZE_MAX);
	QC_Base64Decode (&data, s, len, cap);
	p = cap <= UINT32_MAX ? QC_HeapPointer (vm, (uint32_t)cap) : 0;
	if (!p || data.failed)
	{
		QC_SinkFree (&data);
		QC_LibSetArgWord (vm, 1, 0);
		QC_ReturnZero (vm);
		return QC_LibSoftError (vm, "base64decode: out of memory");
	}
	if (data.len)
		QC_WriteBytes (&vm->mem, p, data.buf, (uint32_t)data.len, NULL);
	QC_LibSetArgWord (vm, 1, (uint32_t)data.len);
	QC_SinkFree (&data);
	QC_ReturnWord (vm, p);
	return true;
}

static const qc_libentry_t	qc_memory[] = {
	{"memalloc", QC_Memalloc, NULL, 0},
	{"memfree", QC_Memfree, NULL, 0},
	{"memrealloc", QC_Memrealloc, NULL, 0},
	{"memcpy", QC_Memcpy, NULL, 0},
	{"memfill8", QC_Memfill8, NULL, 0},
	{"memgetval", QC_Memgetval, NULL, 0},
	{"memsetval", QC_Memsetval, NULL, 0},
	{"memptradd", QC_Memptradd, NULL, 0},
	{"memcmp", QC_Memcmp, NULL, 0},
	{"createbuffer", QC_Createbuffer, NULL, 0},
	{"base64encode", QC_Base64encodeBuiltin, NULL, 0},
	{"base64decode", QC_Base64decodeBuiltin, NULL, 0},
};

bool QC_RegisterMemory (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_memory, sizeof(qc_memory) / sizeof(qc_memory[0]));
}
