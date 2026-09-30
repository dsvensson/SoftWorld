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

/*
==============================================================================

MEMORY

One byte-addressed space of three regions, all below 2^31 so that pointers
are never taken for tagged string references:

0        region S: the main progs' strings, its globals (and a three-word zero
         tail), the local stack, then the strings and globals of added progs
e_base   region E: entity e's fields at e_base + (e << shift)
h_base   region H: the QuakeC heap

Each region is address space reserved up front and committed as it grows, so
nothing in it ever moves.

==============================================================================
*/

typedef struct
{
	uint8_t		*base;
	size_t		reserved;
	size_t		committed;
} qc_region_t;

bool	QC_RegionReserve (qc_region_t *r, size_t size);
bool	QC_RegionCommit (qc_region_t *r, size_t size);		// at least size bytes, zeroed when new
void	QC_RegionFree (qc_region_t *r);

typedef struct
{
	bool		in_use;
	bool		protected;
	double		freetime;		// host clock when freed
	uint32_t	serial;			// incremented by each spawn into the slot
} qc_entslot_t;

typedef struct
{
	uint32_t	ofs, size;		// size rounded to the granularity
	uint32_t	requested;
} qc_heapblock_t;

// the QuakeC heap: first fit over one region, its bookkeeping outside it
typedef struct
{
	qc_region_t		region;
	uint32_t		len;		// bytes handed out so far (the region's used end)
	uint32_t		max;
	qc_heapblock_t	*used;		// by offset
	uint32_t		numused, usedsize;
	qc_heapblock_t	*free;		// by offset, coalesced
	uint32_t		numfree, freesize;
} qc_heap_t;

bool		QC_HeapInit (qc_heap_t *h, uint32_t max);
void		QC_HeapFree (qc_heap_t *h);
bool		QC_HeapAlloc (qc_heap_t *h, uint32_t n, uint32_t *ofs);		// zeroed
bool		QC_HeapRelease (qc_heap_t *h, uint32_t ofs);	// false if no block starts there
bool		QC_HeapBlockSize (const qc_heap_t *h, uint32_t ofs, uint32_t *size);	// as requested
bool		QC_HeapRealloc (qc_heap_t *h, bool had, uint32_t old, uint32_t n, uint32_t *ofs);

// where an access lands
typedef enum
{
	QC_LOC_NONE,
	QC_LOC_S,
	QC_LOC_E,
	QC_LOC_H
} qc_loctype_t;

typedef struct
{
	qc_loctype_t	type;
	uint8_t			*p;			// the host address
	uint32_t		ent;		// QC_LOC_E: the entity
	size_t			avail;		// bytes readable from p within the region (or the entity's fields)
} qc_loc_t;

typedef struct
{
	qc_region_t		s;
	uint32_t		s_len;			// bytes of S in use
	qc_region_t		e;				// blocks of 1 << shift bytes, the header first
	uint8_t			*fields;		// e.base + header: entity 0's fields
	uint32_t		e_base;
	uint32_t		shift;
	uint32_t		header;
	uint32_t		field_bytes;	// bytes of each block that hold fields
	uint32_t		field_capacity;	// what they may grow to
	uint32_t		max_edicts;
	qc_entslot_t	*slots;
	uint32_t		num_edicts;		// slots allocated (the high-water mark)
	uint32_t		slotsize;
	qc_heap_t		heap;
	uint32_t		h_base;
	uint32_t		ls_base;		// the local stack: a byte offset in S
	uint32_t		ls_words;
} qc_mem_t;

// the slack committed past the end of S: operands of a progs with fewer than
// 28 globals may name its PARM and RETURN slots past it
#define QC_S_SLACK		256

qc_loc_t	QC_Locate (const qc_mem_t *m, uint32_t p, uint32_t n);

// a word of S by byte offset, as the VM's own reads it: 0 out of range, stray
// writes dropped (the interpreter's operands need no check; this is for
// offsets taken from definitions, which the loader doesn't check)
uint32_t	QC_GetS (const qc_mem_t *m, uint64_t ofs);
void		QC_SetS (qc_mem_t *m, uint64_t ofs, uint32_t v);

// the host address of entity e's field words, if both are valid
uint8_t		*QC_FieldPtr (const qc_mem_t *m, uint32_t e, uint32_t word, uint32_t words);

// the text at a linear address, NUL-terminated within its region (a string in
// an entity ends with the entity's fields), or NULL if unreadable
const char	*QC_LinearString (const qc_mem_t *m, uint32_t p);

typedef enum
{
	QC_WRITE_OK,
	QC_WRITE_NULL,			// address 0
	QC_WRITE_INVALID,		// not writable memory
	QC_WRITE_PROTECTED		// a protected entity (skipped with a warning); ent says which
} qc_writeresult_t;

qc_writeresult_t	QC_CheckWrite (const qc_mem_t *m, uint32_t p, uint32_t n, qc_loc_t *loc);
qc_writeresult_t	QC_WriteBytes (qc_mem_t *m, uint32_t p, const void *bytes, uint32_t n, uint32_t *ent);
bool				QC_ReadBytes (const qc_mem_t *m, uint32_t p, void *out, uint32_t n);

static inline bool QC_InUse (const qc_mem_t *m, uint32_t e)
{
	return e < m->num_edicts && m->slots[e].in_use;
}

static inline bool QC_Protected (const qc_mem_t *m, uint32_t e)
{
	return e < m->num_edicts && m->slots[e].protected;
}

bool	QC_GrowEdicts (qc_mem_t *m, uint32_t e);	// slots up to and including e
void	QC_ClearEntity (qc_mem_t *m, uint32_t e);	// zeroes its fields and the reserve

/*
==============================================================================

STRINGS

==============================================================================
*/

#define QC_TEMP_TAG		0x80000000u
#define QC_STATIC_TAG	0xC0000000u
#define QC_INDEX_MASK	0x3FFFFFFFu
#define QC_TAG_MASK		0xC0000000u

// temp strings grow when QuakeC writes past their end, up to this
#define QC_MAX_TEMP_GROWTH	(1u << 20)

typedef struct
{
	uint8_t		*data;			// NULL: a free slot; size bytes, then a hidden NUL
	uint32_t	size;			// the text, a NUL and padding: a multiple of 4
} qc_tempslot_t;

typedef struct
{
	const char	*text;
	bool		owned;			// an interned copy; else the host's text
} qc_static_t;

typedef struct
{
	uint32_t	slot, count;
} qc_pin_t;

typedef struct
{
	qc_tempslot_t	*slots;
	uint32_t		numslots;		// allocated
	uint32_t		capacity;		// soft: a collection is due when half of it is live
	uint32_t		cursor;
	uint32_t		live;
	size_t			bytes;
	uint32_t		max_slots;
	size_t			max_bytes;

	qc_static_t		*statics;
	uint32_t		numstatics, staticsize;
	qc_map_t		static_text;	// interned text -> index
	qc_map_t		static_ptr;		// host pointer (its bytes as the key) -> index
	const char		**ptrkeys;		// the keys of static_ptr, one allocation each

	qc_pin_t		*pins;
	uint32_t		numpins, pinsize;
} qc_strings_t;

bool		QC_StringsInit (qc_strings_t *s, uint32_t max_slots, size_t max_bytes);
void		QC_StringsFree (qc_strings_t *s);
bool		QC_StringsFit (const qc_strings_t *s, size_t len);
// a temp string of len bytes; false when a limit is reached
bool		QC_TempAlloc (qc_strings_t *s, const void *text, size_t len, uint32_t *ref);
// the slot's bytes (padded), or NULL if the slot is free
uint8_t		*QC_TempData (const qc_strings_t *s, uint32_t slot, uint32_t *size);
// the slot's bytes grown with zeros to at least len (up to QC_MAX_TEMP_GROWTH)
uint8_t		*QC_TempGrow (qc_strings_t *s, uint32_t slot, size_t len);
bool		QC_StaticIntern (qc_strings_t *s, const char *text, size_t len, uint32_t *ref);
bool		QC_StaticBorrow (qc_strings_t *s, const char *text, uint32_t *ref);
const char	*QC_StaticText (const qc_strings_t *s, uint32_t index);
void		QC_StringsPin (qc_strings_t *s, uint32_t ref);
void		QC_StringsUnpin (qc_strings_t *s, uint32_t ref);
bool		QC_WantsCollection (const qc_strings_t *s);

// marking and sweeping: marks the temp strings referenced by the aligned words
// of a root, then frees the rest; returns how many were freed
bool		QC_GCBegin (qc_strings_t *s, uint8_t **marks);
void		QC_GCMark (const qc_strings_t *s, uint8_t *marks, const uint8_t *root, size_t len);
uint32_t	QC_GCSweep (qc_strings_t *s, uint8_t *marks);

/*
==============================================================================

THE VIRTUAL MACHINE

==============================================================================
*/

// the function of the engine: no QuakeC is running
#define QC_NO_FUNCTION		UINT32_MAX

// byte offsets in a progs' globals
#define QC_OFS_RETURN		4
#define QC_OFS_PARM0		16
#define QC_OFS_PARM1		28

// what a SWITCH compares
typedef enum
{
	QC_SWITCH_FLOAT,
	QC_SWITCH_VECTOR,
	QC_SWITCH_STRING,
	QC_SWITCH_INT
} qc_switchkind_t;

// the interpreter's registers
typedef struct
{
	uint32_t		pc;				// statement to run next
	uint32_t		func;			// function index in its progs, or QC_NO_FUNCTION
	uint32_t		prnum;
	uint32_t		pushed;			// words PUSHed by the current function
	uint32_t		ls_top;			// local stack words in use
	uint32_t		switch_ref;		// byte offset in S of what a SWITCH compares
	qc_switchkind_t	switch_kind;
	uint32_t		locals_addr;	// the current function's locals: byte offset in S,
	uint32_t		locals_words;	// saved on the local stack while it runs
} qc_exec_t;

// a caller's saved context
typedef struct
{
	uint32_t		resume_pc;
	uint32_t		func;
	uint32_t		prnum;
	uint32_t		pushed;
	uint32_t		switch_ref;
	qc_switchkind_t	switch_kind;
	uint32_t		locals_at;		// local stack word where the callee's saved locals start
	uint32_t		locals_addr;
	uint32_t		locals_words;
} qc_frame_t;

// what entering a QuakeC function needs, in absolute addresses
typedef struct
{
	uint32_t	entry;			// UINT32_MAX: not a QuakeC function
	uint32_t	locals_addr;
	uint32_t	locals_words;
	uint32_t	copies_start;	// its parameter copies in the progs state's copies
	uint32_t	copies_end;
} qc_qcfunc_t;

typedef enum
{
	QC_CALLEE_NULL,
	QC_CALLEE_QC,
	QC_CALLEE_BUILTIN,		// slot: index into the registry
	QC_CALLEE_MISSING,		// a builtin the registry lacks
	QC_CALLEE_INVALID
} qc_calleekind_t;

typedef struct
{
	uint32_t	kind;
	uint32_t	slot;
} qc_callee_t;

// what the animation opcodes use: S byte offsets and field words, -1 without
typedef struct
{
	int64_t		self_g, time_g, cycle_wrapped_g;
	int64_t		frame_f, think_f, nextthink_f, weaponframe_f;
} qc_statehandles_t;

// a progs' copy of a global kept in sync between progs
typedef struct
{
	bool		present;
	uint32_t	offset;			// absolute byte offset in S
	uint32_t	words;
} qc_sharedglobal_t;

// a progs loaded into a VM
typedef struct
{
	qc_progs_t			*progs;
	uint32_t			sbase;			// byte address of its strings
	uint32_t			gbase;			// of its globals
	qc_stmt_t			*code;			// its statements, global operands relocated to S
	qc_callee_t			*callees;		// a function a callee
	qc_qcfunc_t			*funcs;
	qc_paramcopy_t		*copies;		// absolute
	uint32_t			numcopies;
	qc_statehandles_t	state;
	qc_sharedglobal_t	*shared;		// one per slot of the VM's shared table
	uint32_t			numshared;
	uint64_t			*profile;		// statements each function ran, or NULL
} qc_progstate_t;

typedef struct
{
	char		*name;
	uint32_t	type;
	uint32_t	ofs;
} qc_fieldentry_t;

// the VM-wide entity field layout: the main progs' fields, then those added by
// the host or by later progs
typedef struct
{
	qc_fieldentry_t	*entries;
	uint32_t		count, size;
	qc_map_t		by_name;
	uint32_t		words;			// in use per entity
} qc_fieldtable_t;

// a field zeroed by remove, or given a value by spawn
typedef struct
{
	uint32_t	ofs;
	uint32_t	words;
	int64_t		global;			// spawn: an S byte offset to take the value from, or -1
	uint32_t	value;
} qc_fieldfill_t;

typedef struct
{
	char		**names;
	uint32_t	count, size;
	qc_map_t	by_name;
} qc_sharedtable_t;

typedef struct qc_std_s qc_std_t;			// the standard builtins' state (qc_lib.h)
typedef struct qc_thread_s qc_thread_t;		// a sleeping QuakeC thread (qc_threads.c)

#define QC_STRING_COPIES	16

struct qcvm_s
{
	qc_progs_t			*main;
	const qc_builtins_t	*builtins;
	qc_host_t			host;
	void				*ctx;
	double				started;		// the VM's own clock at creation
	bool				time_set;
	double				time;			// the host's clock, when set
	uint32_t			budget;			// instructions left for the current host call
	bool				has_deadline;
	double				deadline;

	qc_mem_t			mem;
	qc_strings_t		strings;
	qc_progstate_t		*progs;
	uint32_t			numprogs;
	qc_fieldtable_t		fields;
	qc_frame_t			*frames;		// call_depth of them
	uint32_t			numframes;
	qc_exec_t			x;
	uint32_t			argc;
	qc_func_t			builtin;		// the builtin running
	uint32_t			nesting;		// host calls in progress
	uint64_t			rng;

	qc_config_t			config;			// its lists are the VM's own copies:
	char				**shared_names;	// shared_globals,
	char				**clear_names;	// remove_clears
	qc_spawndefault_t	*spawn_list;	// and spawn_defaults

	qc_warning_t		*warnings;
	uint32_t			numwarnings, warningsize;
	uint32_t			warnings_this_call;
	uint32_t			suppressed;

	bool				trace;
	bool				traced;			// the statement at x.pc was reported and runs next
	bool				profiling;		// counting statements (progs' profile)

	qc_fieldfill_t		*remove_clears;
	uint32_t			numremove_clears;
	qc_fieldfill_t		*spawn_defaults;
	uint32_t			numspawn_defaults;

	qc_std_t			*std;
	bool				aborting;		// the builtin returning false called QC_Abort
	bool				has_abort_ret;
	uint32_t			abort_ret[3];
	qc_sharedtable_t	shared;
	uint32_t			entry_depth;	// frame depth the innermost execution returns at
	qc_thread_t			*threads;
	uint32_t			numthreads;

	qc_error_t			error;

	// copies of strings QuakeC left unterminated at the end of a region
	char				*copies[QC_STRING_COPIES];
	uint32_t			nextcopy;
};

// the VM's clock: the host's when set, else the time since creation
double	QC_Now (const qcvm_t *vm);

// Sets vm->error and returns false. fmt (may be NULL) gives the message.
bool	QC_Fail (qcvm_t *vm, qc_errkind_t kind, int64_t value, const char *fmt, ...);
void	QC_ClearError (qc_error_t *e);

// records a warning with the current backtrace, rate limited per top-level call
void	QC_Warn (qcvm_t *vm, qc_warnkind_t kind, int64_t value, const char *fmt, ...);
void	QC_FlushWarnings (qcvm_t *vm);

// the current QuakeC call stack, innermost first; false when out of memory
bool	QC_Backtrace (const qcvm_t *vm, qc_backtrace_t *bt);
void	QC_FreeBacktrace (qc_backtrace_t *bt);

// The text of a string reference, or NULL if it resolves to nothing. With warn,
// NULL comes back as "" with a warning.
const char	*QC_Str (qcvm_t *vm, uint32_t ref);
const char	*QC_StrOrWarn (qcvm_t *vm, uint32_t ref);

// the temp string (0 and an error past the limits)
uint32_t	QC_NewTemp (qcvm_t *vm, const char *text, size_t len);

// the absolute S byte offset of a global word of a progs, or -1
int64_t		QC_GlobalOffset (const qcvm_t *vm, uint32_t prnum, uint32_t word);
static inline uint32_t QC_GBase (const qcvm_t *vm)
{
	return vm->progs[vm->x.prnum].gbase;
}

const qc_fieldentry_t	*QC_FieldEntry (const qcvm_t *vm, const char *name);
bool	QC_AddFieldEntry (qc_fieldtable_t *t, const char *name, uint32_t type, uint32_t ofs);

void	QC_ApplySpawnDefaults (qcvm_t *vm, uint32_t e);

// collects temp strings now, whatever is running; how many were freed
uint32_t	QC_CollectNow (qcvm_t *vm);
// marks the temp strings the sleeping threads' snapshots refer to
void		QC_MarkThreads (qcvm_t *vm, uint8_t *marks);

bool	QC_InitProgState (qcvm_t *vm, qc_progstate_t *ps, qc_progs_t *p, uint32_t sbase, uint32_t gbase);
void	QC_FixupGlobals (qcvm_t *vm, const qc_progs_t *p, uint32_t gbase, uint32_t prnum);
bool	QC_RegisterShared (qcvm_t *vm, uint32_t pr);

/*
==============================================================================

THE INTERPRETER

==============================================================================
*/

// why the interpreter loop stopped
typedef enum
{
	QC_EXIT_RETURNED,		// the function entered at the exit depth returned
	QC_EXIT_BUILTIN,		// a builtin to call; execution resumes after the call
	QC_EXIT_STATEOP,		// an animation opcode; execution resumes after it
	QC_EXIT_TRACE,			// tracing: the statement at x.pc runs next
	QC_EXIT_BUDGET,			// the budget is spent: the statement at x.pc had no effect
	QC_EXIT_FAULT			// an error (vm->error)
} qc_exitkind_t;

typedef struct
{
	qc_exitkind_t	kind;
	uint32_t		slot;		// QC_EXIT_BUILTIN
	qc_func_t		func;
	qc_stateop_t	op;			// QC_EXIT_STATEOP
} qc_exit_t;

// runs until the frames are back at exit_depth, a builtin must be called, or a fault
qc_exit_t	QC_Run (qcvm_t *vm, uint32_t exit_depth, uint32_t *budget);

bool		QC_Enter (qcvm_t *vm, uint32_t prnum, uint32_t index, uint32_t resume_pc);
void		QC_Leave (qcvm_t *vm);
void		QC_Unwind (qcvm_t *vm, uint32_t depth);
void		QC_SwitchProgs (qcvm_t *vm, uint32_t from, uint32_t to, bool in);
uint32_t	QC_Rand15 (qcvm_t *vm);
bool		QC_StringsEqual (qcvm_t *vm, uint32_t a, uint32_t b);
bool		QC_PtrRead (qcvm_t *vm, uint32_t base, uint32_t offset, void *out, uint32_t n);
bool		QC_PtrWrite (qcvm_t *vm, uint32_t base, uint32_t offset, const void *bytes, uint32_t n);
bool		QC_MissingBuiltin (qcvm_t *vm, qc_func_t f);

// the words of the current progs' PARM slot i (zero past the eighth), and RETURN
void		QC_ArgRaw (const qcvm_t *vm, int i, uint32_t out[3]);
void		QC_ReturnRaw (qcvm_t *vm, const uint32_t w[3]);
