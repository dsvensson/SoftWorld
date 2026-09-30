// qc_local.h -- the QuakeC VM's internals, shared by its files
#pragma once

#include "qcvm.h"
#include "qc_map.h"
#include "qc_opcode.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) && !defined(_M_X64) && !defined(_M_ARM64)
#error "qcvm: unknown byte order"
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "qcvm: VM memory is little-endian, and so must the host be"
#endif

/*
==============================================================================

BYTES AND NUMBERS

==============================================================================
*/

static inline uint16_t QC_LE16 (const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t QC_LE32 (const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t QC_FloatBits (float f)
{
	uint32_t	u;

	memcpy (&u, &f, 4);
	return u;
}

static inline float QC_BitsFloat (uint32_t u)
{
	float	f;

	memcpy (&f, &u, 4);
	return f;
}

static inline uint64_t QC_DoubleBits (double d)
{
	uint64_t	u;

	memcpy (&u, &d, 8);
	return u;
}

static inline double QC_BitsDouble (uint64_t u)
{
	double	d;

	memcpy (&d, &u, 8);
	return d;
}

// Conversions as FTE on x86 makes them, on every platform: C leaves an
// out-of-range float to integer conversion undefined, and x86 gives the
// "integer indefinite" value INT_MIN.

// (int)f: toward zero; NaN and out of range give INT32_MIN
static inline int32_t QC_F2I (float f)
{
	if (!(f >= -2147483648.0f && f < 2147483648.0f))
		return INT32_MIN;
	return (int32_t)f;
}

// (long long)d: toward zero; NaN and out of range give INT64_MIN
static inline int64_t QC_D2I64 (double d)
{
	if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
		return INT64_MIN;
	return (int64_t)d;
}

// (unsigned)f as x86-64 compilers emit it: a 64-bit signed conversion, then
// the low 32 bits
static inline uint32_t QC_F2U (float f)
{
	return (uint32_t)(uint64_t)QC_D2I64 ((double)f);
}

// (unsigned long long)d as x86-64 compilers emit it
static inline uint64_t QC_D2U64 (double d)
{
	if (isnan (d))
		return (uint64_t)1 << 63;
	if (d < 9223372036854775808.0)
		return (uint64_t)QC_D2I64 (d);
	return (uint64_t)QC_D2I64 (d - 9223372036854775808.0) ^ ((uint64_t)1 << 63);
}

// FTE's float truth (NOT_F, AND_F, OR_F, IF_F): the value without its sign bit
// is not zero, so -0 is false and denormals are true
static inline bool QC_FloatTrue (uint32_t bits)
{
	return (bits & 0x7FFFFFFFu) != 0;
}

// the float a comparison stores
static inline uint32_t QC_FBool (bool b)
{
	return b ? 0x3F800000u : 0;
}

/*
==============================================================================

PROGRAMS

==============================================================================
*/

// statement flag: a debugger breakpoint is set on it
#define QC_STMT_BREAKPOINT	1

// A statement in canonical form. Global operands are byte offsets from the start
// of the program's globals; jumps are absolute statement indices; global-index
// and immediate operands are raw.
typedef struct
{
	uint16_t	op;
	uint16_t	flags;
	uint32_t	a, b, c;
} qc_stmt_t;

typedef struct
{
	uint32_t	type;
	uint32_t	ofs;		// word offset into the globals or an entity
	uint32_t	name;		// string table offset
	bool		save;
	bool		shared;
} qc_def_t;

// one parameter word copied from a PARM slot into a function's locals, as byte
// offsets from the start of the globals
typedef struct
{
	uint32_t	src, dst;
} qc_paramcopy_t;

typedef struct
{
	qc_funckind_t	kind;
	qc_invalid_t	invalid;
	uint32_t		entry;			// QC_FUNC_QUAKEC
	uint32_t		number;			// QC_FUNC_BUILTIN
	uint32_t		parm_start;
	uint32_t		locals;
	int32_t			num_parms;
	uint8_t			parm_sizes[8];
	uint32_t		copies_start;	// range into progs->copies
	uint32_t		copies_end;
	uint32_t		name;
	uint32_t		file;
} qc_function_t;

typedef struct
{
	uint32_t	ofs;		// global word
	uint32_t	def;		// index of the first definition there
} qc_defatofs_t;

struct qc_progs_s
{
	int				refcount;

	qc_format_t		format;
	uint32_t		version;
	uint32_t		crc;

	uint8_t			*strings;		// as stored, plus a NUL so every string ends
	uint32_t		numstrings;		// bytes as stored

	uint32_t		*globals;		// initial values
	uint32_t		numglobals;

	qc_stmt_t		*statements;	// numstatements, then a QOP_JUMP_OUT_OF_RANGE sentinel
	uint32_t		numstatements;

	qc_function_t	*functions;
	uint32_t		numfunctions;
	qc_paramcopy_t	*copies;
	uint32_t		numcopies;

	qc_def_t		*globaldefs;
	uint32_t		numglobaldefs;
	qc_def_t		*fielddefs;
	uint32_t		numfielddefs;
	uint32_t		entityfields;	// words per entity, from the header

	qc_map_t		functions_by_name;
	qc_map_t		globals_by_name;
	qc_map_t		fields_by_name;
	qc_defatofs_t	*names_by_ofs;	// sorted by ofs: the first named global at each
	uint32_t		numnames_by_ofs;

	char			**bodyless;		// the extern functions it expects from other progs
	uint32_t		numbodyless;

	uint32_t		*pointer_relocs;	// global words of pointer relocations
	uint32_t		numpointer_relocs;

	int64_t			thisprogs;		// global word of "thisprogs", or -1
	int64_t			fasttrackarrays;	// of "__ext__fasttrackarrays", or -1

	uint32_t		*lines;			// source line of each statement, or NULL

	qc_loadnote_t	*notes;
	uint32_t		numnotes;
};

// the string at a string table offset ("" out of range)
static inline const char *QC_Cstr (const qc_progs_t *progs, uint32_t ofs)
{
	return ofs < progs->numstrings ? (const char *)progs->strings + ofs : "";
}

const qc_def_t	*QC_GlobalDefRaw (const qc_progs_t *progs, const char *name);
const qc_def_t	*QC_FieldDefRaw (const qc_progs_t *progs, const char *name);
