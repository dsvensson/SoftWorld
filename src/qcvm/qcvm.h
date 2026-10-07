// qcvm.h -- a re-entrant QuakeC virtual machine for server, client and menu progs
//
// A C port of qcvm-rs. It runs every progs format fteqcc writes (version 6,
// FTE's version 7 with 16- or 32-bit records, KK7, uHexen2 and QTest) with
// every opcode FTE executes, and is built for untrusted, server-supplied progs:
// all input is validated, every resource QuakeC can grow has a hard limit, and
// no malformed progs can crash the program. Behaviour follows FTE; docs/qcvm
// lists where it deliberately differs.
//
// Programs (qc_progs_t) are loaded once, are immutable afterwards and may be
// shared by any number of VMs.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
==============================================================================

PROGRAMS

==============================================================================
*/

typedef struct qc_progs_s qc_progs_t;

// the on-disk format a program was loaded from
typedef enum
{
	QC_FORMAT_QTEST,	// version 3, from Quake's QTest release
	QC_FORMAT_V6,		// version 6, the standard format
	QC_FORMAT_FTE16,	// FTE's version 7, 16-bit statements and definitions
	QC_FORMAT_FTE32,	// FTE's version 7, 32-bit statements and definitions
	QC_FORMAT_KK7,		// KK QuakeWorld's version 7 (32-bit statements, 16-bit definitions),
						// which FTE also assumes for an unknown secondary version
	QC_FORMAT_UHEXEN2	// uHexen2's version 7 (32-bit records, opcode and type in the top half)
} qc_format_t;

// the type of a global or field definition: FTE's EV_* codes; others are
// compiler-internal
enum
{
	QC_EV_VOID,
	QC_EV_STRING,
	QC_EV_FLOAT,
	QC_EV_VECTOR,
	QC_EV_ENTITY,
	QC_EV_FIELD,
	QC_EV_FUNCTION,
	QC_EV_POINTER,
	QC_EV_INTEGER,
	QC_EV_UINT,
	QC_EV_INT64,
	QC_EV_UINT64,
	QC_EV_DOUBLE
};

// the words a value of a type takes, or -1 for an unknown type
int		QC_TypeWords (uint32_t type);

// what a function record describes
typedef enum
{
	QC_FUNC_NULL,			// function 0
	QC_FUNC_QUAKEC,			// QuakeC code from statement entry
	QC_FUNC_BUILTIN,		// builtin number (#N)
	QC_FUNC_NAMED_BUILTIN,	// a builtin bound by the function's own name (#0)
	QC_FUNC_INVALID			// a malformed record: calling it faults
} qc_funckind_t;

// why a function record is invalid
typedef enum
{
	QC_INVALID_NONE,
	QC_INVALID_ENTRY,		// the first statement is outside the statement table
	QC_INVALID_LOCALS		// the locals or a parameter copy are outside the globals
} qc_invalid_t;

typedef struct
{
	uint32_t		index;
	const char		*name;
	const char		*file;			// "" if the compiler kept none
	qc_funckind_t	kind;
	uint32_t		entry;			// QC_FUNC_QUAKEC: the first statement
	uint32_t		number;			// QC_FUNC_BUILTIN: the builtin number
	qc_invalid_t	invalid;		// QC_FUNC_INVALID: why
	uint32_t		parm_start;		// first global word of the parameters and locals
	uint32_t		locals;			// words of parameters and locals
	int32_t			num_parms;		// as declared (introspection only for builtins)
	uint8_t			parm_sizes[8];	// words of each parameter
} qc_funcinfo_t;

// a global or field definition
typedef struct
{
	const char		*name;
	uint32_t		type;			// QC_EV_*
	uint32_t		ofs;			// word offset into the globals or into an entity
	bool			save;			// 0x8000: saved in savegames
	bool			shared;			// 0x4000: shared between progs
} qc_definfo_t;

// something noteworthy the loader did or found; loading still succeeded
typedef enum
{
	QC_NOTE_ASSUMED_KK7,		// value: the unknown secondary version
	QC_NOTE_HEXEN2_CALLS,		// the Hexen 2 calling convention: CALLn became CALLnH
	QC_NOTE_POISONED_STATEMENT,	// index, value: the opcode; it faults if run
	QC_NOTE_JUMP_OUT_OF_RANGE,	// index: a jump that leaves the program, faults if taken
	QC_NOTE_INVALID_FUNCTION,	// index, value: the qc_invalid_t
	QC_NOTE_TYPES_IGNORED,		// the file has a types section
	QC_NOTE_LINE_NUMBERS_IGNORED,	// its line numbers lie outside the file
	QC_NOTE_BODYLESS_IGNORED	// its bodyless-function names lie outside the file
} qc_notekind_t;

typedef struct
{
	qc_notekind_t	kind;
	uint32_t		index;
	uint32_t		value;
} qc_loadnote_t;

// why a program (or its line numbers) could not be loaded
typedef enum
{
	QC_LOAD_OK,
	QC_LOAD_TRUNCATED,				// too short for a header
	QC_LOAD_UNSUPPORTED_VERSION,	// value: the version
	QC_LOAD_COMPRESSED,				// value: the compressed-sections mask; FTE refuses these too
	QC_LOAD_SECTION_OUT_OF_BOUNDS,	// what: which section lies outside the file
	QC_LOAD_LINE_NUMBERS,			// what: why a .lno file was rejected
	QC_LOAD_NAMES_TOO_LARGE,		// the definitions' names add up to more than 16 MiB
	QC_LOAD_NO_MEMORY
} qc_loaderr_t;

typedef struct
{
	qc_loaderr_t	kind;
	uint32_t		value;
	const char		*what;
} qc_loaderror_t;

// Parses a compiled program (progs.dat, csprogs.dat, menu.dat, ...). Fails only
// if the data is not a program in a format FTE accepts or a section lies
// outside it; problems inside the code (bad opcodes, wild jumps, malformed
// functions) are load notes and fault only when executed. The program starts
// with one reference. error may be NULL.
qc_progs_t	*QC_LoadProgs (const void *data, size_t size, qc_loaderror_t *error);

// Attaches the source line numbers of a .lno file fteqcc wrote beside the
// program. Only before the program is shared: it changes the program.
bool	QC_AttachLineNumbers (qc_progs_t *progs, const void *lno, size_t size, qc_loaderror_t *error);

void	QC_RetainProgs (qc_progs_t *progs);
void	QC_ReleaseProgs (qc_progs_t *progs);		// frees it with the last reference

// the text of an error or a note, in buf
const char	*QC_LoadErrorText (const qc_loaderror_t *error, char *buf, size_t size);
const char	*QC_LoadNoteText (const qc_loadnote_t *note, char *buf, size_t size);

qc_format_t	QC_ProgsFormat (const qc_progs_t *progs);
uint32_t	QC_ProgsVersion (const qc_progs_t *progs);		// 3, 6 or 7
uint32_t	QC_ProgsCRC (const qc_progs_t *progs);			// of the system definitions
const qc_loadnote_t	*QC_ProgsNotes (const qc_progs_t *progs, uint32_t *count);

uint32_t	QC_ProgsNumStatements (const qc_progs_t *progs);
uint32_t	QC_ProgsNumGlobals (const qc_progs_t *progs);
uint32_t	QC_ProgsNumFunctions (const qc_progs_t *progs);	// with the null function 0
uint32_t	QC_ProgsEntityFields (const qc_progs_t *progs);	// words per entity

// the NUL-terminated string at an offset of the string table ("" out of range)
const char	*QC_ProgsString (const qc_progs_t *progs, uint32_t ofs);

// the initial value of a global word
bool	QC_ProgsInitialGlobal (const qc_progs_t *progs, uint32_t word, uint32_t *value);

// A function by name, as the program was loaded. As in FTE, a function-typed
// global of that name wins over the function table's names (its initial value).
bool	QC_ProgsFunctionIndex (const qc_progs_t *progs, const char *name, uint32_t *index);
bool	QC_ProgsFunction (const qc_progs_t *progs, uint32_t index, qc_funcinfo_t *info);

// definitions: by name (the first if a name repeats) or in file order
bool		QC_ProgsGlobalDef (const qc_progs_t *progs, const char *name, qc_definfo_t *info);
bool		QC_ProgsFieldDef (const qc_progs_t *progs, const char *name, qc_definfo_t *info);
uint32_t	QC_ProgsNumGlobalDefs (const qc_progs_t *progs);
uint32_t	QC_ProgsNumFieldDefs (const qc_progs_t *progs);
bool		QC_ProgsGlobalDefAt (const qc_progs_t *progs, uint32_t i, qc_definfo_t *info);
bool		QC_ProgsFieldDefAt (const qc_progs_t *progs, uint32_t i, qc_definfo_t *info);

// the name of the first global defined at a word, or NULL (word 0 has none)
const char	*QC_ProgsGlobalNameAt (const qc_progs_t *progs, uint32_t word);

// The builtins the code calls: the function operand of every CALL whose global
// starts out holding a builtin, as sorted, distinct function indices. Fills up
// to max of them and returns how many there are. Calls through a variable that
// only gets a builtin while the program runs are not seen.
uint32_t	QC_ProgsCalledBuiltins (const qc_progs_t *progs, uint32_t *out, uint32_t max);

// the functions this program expects another progs to provide
uint32_t	QC_ProgsNumBodyless (const qc_progs_t *progs);
const char	*QC_ProgsBodyless (const qc_progs_t *progs, uint32_t i);

// the source line of a statement, if known
bool	QC_ProgsSourceLine (const qc_progs_t *progs, uint32_t statement, uint32_t *line);

// one statement as text (opcode, operands, source line), in buf
const char	*QC_DisassembleStatement (const qc_progs_t *progs, uint32_t statement, char *buf, size_t size);

// a function's listing, a line at a time (without the newline)
void	QC_DisassembleFunction (const qc_progs_t *progs, uint32_t index,
			void (*line) (void *ctx, const char *text), void *ctx);

/*
==============================================================================

VALUES

==============================================================================
*/

typedef struct qcvm_s qcvm_t;
typedef struct qc_builtins_s qc_builtins_t;

typedef uint32_t	qc_ent_t;		// an entity: its number, 0 the world
typedef uint32_t	qc_str_t;		// a string: see QC_String
typedef uint32_t	qc_func_t;		// a function: its index | its progs << 24
typedef uint32_t	qc_ptr_t;		// a pointer: a byte address in VM memory

#define QC_FUNC(progs, index)	(((uint32_t)(progs) << 24) | ((uint32_t)(index) & 0x00FFFFFFu))
#define QC_FUNC_PROGS(f)		((uint32_t)(f) >> 24)
#define QC_FUNC_INDEX(f)		((uint32_t)(f) & 0x00FFFFFFu)

// a word of globals or fields
typedef union
{
	float		f;
	int32_t		i;
	uint32_t	u;
} qc_word_t;

// an argument or result: up to three words (a vector)
typedef struct
{
	uint32_t	w[3];
} qc_value_t;

qc_value_t	QC_ValFloat (float f);
qc_value_t	QC_ValInt (int32_t i);
qc_value_t	QC_ValVector (float x, float y, float z);
qc_value_t	QC_ValWord (uint32_t u);			// an entity, string, function or pointer

// (int)f as x86 makes it, on every platform: toward zero, and INT32_MIN for NaN
// and anything out of range (where C leaves the conversion undefined)
static inline int32_t QC_FloatToInt (float f)
{
	if (!(f >= -2147483648.0f && f < 2147483648.0f))
		return INT32_MIN;
	return (int32_t)f;
}

static inline int32_t QC_DoubleToInt (double d)
{
	if (!(d > -2147483649.0 && d < 2147483648.0))
		return INT32_MIN;
	return (int32_t)d;
}

/*
==============================================================================

CONFIGURATION

==============================================================================
*/

// Hard resource limits. Every one fails cleanly, with an error or a refused
// operation and a warning, so untrusted progs cannot take the host down.
typedef struct
{
	uint32_t	max_edicts;			// entities, the world included (FTE CSQC: 65536)
	uint32_t	local_stack_words;	// the locals' stack (FTE: 1,048,576)
	uint32_t	call_depth;			// QuakeC calls (FTE: 1024)
	uint32_t	runaway;			// jumps, calls and returns per host call, shared with the calls
									// builtins make back into QuakeC (FTE: 100,000,000)
	double		deadline;			// seconds a host call may take, checked every 65,536 of those;
									// 0 for none
	uint32_t	reentry;			// nested builtin -> QuakeC -> builtin calls
	uint32_t	heap_bytes;			// the QuakeC heap (memalloc)
	uint32_t	temp_strings;		// live temp strings
	size_t		temp_string_bytes;	// their total size
	uint32_t	progs_area_bytes;	// address space for progs added later (addprogs)
	uint32_t	progs;				// progs in one VM
	uint32_t	threads;			// sleeping QuakeC threads
	size_t		thread_bytes;		// what they hold (captured stacks and locals)
	uint32_t	string_buffers;
	uint32_t	string_buffer_entries;	// in one string buffer
	uint32_t	hash_tables;
	size_t		container_bytes;	// hash table, string buffer and token list contents
	uint32_t	warnings_per_call;	// reported per top-level call; the rest only counted
} qc_limits_t;

typedef enum
{
	QC_CSQC,	// client-side QuakeC (csprogs.dat)
	QC_SSQC,	// server-side QuakeC (progs.dat, qwprogs.dat)
	QC_MENU		// menu QuakeC (menu.dat)
} qc_kind_t;

// how bytes are decoded into characters (FTE's com_parseutf8)
typedef enum
{
	QC_CHARS_QUAKE,		// Quake's charset: a byte a character, the high bit red text
	QC_CHARS_UTF8,
	QC_CHARS_ISO8859_1	// Hexen 2
} qc_charscheme_t;

// a float field given a value whenever an entity is spawned: global's if the
// progs defines it, else value
typedef struct
{
	const char	*field;
	const char	*global;		// or NULL
	float		value;
} qc_spawndefault_t;

typedef struct
{
	qc_kind_t		kind;
	qc_limits_t		limits;
	bool			utf8;				// FTE's utf8_enable: string builtins count characters
	qc_charscheme_t	charscheme;
	bool			developer;			// FTE's developer: builtin errors are warnings
	uint64_t		seed;				// of the random numbers
	float			state_step;			// think interval of the STATE opcodes, in seconds

	// fields zeroed when an entity is removed (the rest stay readable until
	// the slot is reused), NULL-terminated
	const char		*const *remove_clears;
	const qc_spawndefault_t	*spawn_defaults;
	uint32_t		num_spawn_defaults;
	uint32_t		first_spawnable;	// slots below are never handed out by spawn
	uint32_t		field_reserve_bytes;	// per entity, for fields added later
	// Bytes of each entity block that belong to the host, before the fields:
	// QuakeC cannot address them, and the VM never writes them. A multiple of 4.
	uint32_t		entity_header_bytes;
	// globals copied between progs when execution passes from one to another,
	// besides those the compiler flagged as shared; NULL-terminated
	const char		*const *shared_globals;

	// FTE's behaviours the VM fixes, for differential tests only
	struct
	{
		bool		ne_s_raw_strcmp;	// NE_S stores strcmp's result
		bool		load_i64_zero3;		// LOAD_I64 on a bad entity zeroes three words
		bool		switch_reset_on_call;	// SWITCH state resets at calls and returns
	} compat;
} qc_config_t;

// the defaults for a kind of progs (the strings are static)
void	QC_DefaultConfig (qc_config_t *config, qc_kind_t kind);

/*
==============================================================================

ERRORS AND WARNINGS

==============================================================================
*/

typedef enum
{
	QC_ERR_NONE,
	QC_ERR_BAD_OPCODE,				// value: the opcode (also poisoned statements)
	QC_ERR_JUMP_OUT_OF_RANGE,
	QC_ERR_NULL_FUNCTION,
	QC_ERR_INVALID_FUNCTION,		// value: the function
	QC_ERR_BUILTIN_NOT_IMPLEMENTED,	// value: its number (0 for #0), message: its name
	QC_ERR_CALL_DEPTH,
	QC_ERR_LOCAL_STACK,
	QC_ERR_REENTRANCY,
	QC_ERR_TOO_MANY_ARGUMENTS,		// value: how many the host passed
	QC_ERR_RUNAWAY,
	QC_ERR_DEADLINE,
	QC_ERR_BAD_POINTER_READ,		// value: the address
	QC_ERR_BAD_POINTER_WRITE,		// value: the address
	QC_ERR_NULL_POINTER_WRITE,
	QC_ERR_ARRAY_INDEX,				// value: the index
	QC_ERR_BOUND_CHECK,				// value, low, high
	QC_ERR_PUSHED_TOO_MUCH,
	QC_ERR_GADDRESS,
	QC_ERR_STRING_CASE_RANGE,
	QC_ERR_NO_FREE_EDICTS,
	QC_ERR_OUT_OF_MEMORY,			// value: the qc_resource_t
	QC_ERR_QC,						// error() or objerror(); message: its text
	QC_ERR_BUILTIN,					// a builtin failed (a warning in developer mode); message
	QC_ERR_HOST						// the host failed; message
} qc_errkind_t;

typedef enum
{
	QC_RES_ENTITIES,
	QC_RES_TEMP_STRINGS,
	QC_RES_HEAP,
	QC_RES_FIELDS,
	QC_RES_PROGS_AREA,
	QC_RES_PROGS,
	QC_RES_THREADS,
	QC_RES_STRING_BUFFERS,
	QC_RES_HASH_TABLES
} qc_resource_t;

// a frame of a QuakeC backtrace
typedef struct
{
	qc_func_t	function;
	const char	*name;
	const char	*file;			// "" if unknown
	uint32_t	statement;
	int64_t		line;			// -1 if unknown
} qc_btframe_t;

// innermost frame first
typedef struct
{
	qc_btframe_t	*frames;
	uint32_t		count;
} qc_backtrace_t;

typedef struct
{
	qc_errkind_t	kind;
	int64_t			value;
	uint32_t		low, high;		// QC_ERR_BOUND_CHECK
	char			*message;		// or NULL
	qc_backtrace_t	backtrace;
} qc_error_t;

typedef enum
{
	QC_WARN_BAD_ENTITY,				// value: the entity number
	QC_WARN_BAD_FIELD,				// value: the field offset
	QC_WARN_READONLY_ENTITY,		// value: the entity; the write was skipped
	QC_WARN_BAD_STRING,				// value: the reference
	QC_WARN_BUILTIN,				// message
	QC_WARN_SUPPRESSED				// value: how many more this call
} qc_warnkind_t;

typedef struct
{
	qc_warnkind_t	kind;
	int64_t			value;
	char			*message;		// or NULL
	qc_backtrace_t	backtrace;		// where, innermost first
} qc_warning_t;

// the text of an error or warning (without the backtrace), in buf
const char	*QC_ErrorText (const qc_error_t *error, char *buf, size_t size);
const char	*QC_WarningText (const qc_warning_t *warning, char *buf, size_t size);

// the backtrace, a frame a line: "  name (file:line) @ statement n"
const char	*QC_BacktraceText (const qc_backtrace_t *backtrace, char *buf, size_t size);

/*
==============================================================================

THE HOST

What a VM asks of the program embedding it. Every callback may be NULL for its
default: printing goes nowhere, cvars read as unset, clocks come from the
system. ctx is the host's, as given to QC_Create. Callbacks must not longjmp
out of the VM unless the host calls QC_Abandon after.

==============================================================================
*/

// metadata of a cvar (cvar_type, cvar_defstring, cvar_description)
typedef struct
{
	uint32_t	flags;			// FTE's cvar_type bits: 1 exists, 2 archived, 4 private,
								// 8 engine-created, 16 has a description, 32 read-only
	const char	*defaultvalue;	// valid until the next host call
	const char	*description;	// or NULL
} qc_cvarinfo_t;

// a broken-down calendar time (strftime, calltimeofday)
typedef struct
{
	int32_t		year;			// e.g. 2026
	uint32_t	month;			// 0-11
	uint32_t	day;			// 1-31
	uint32_t	hour, minute, second;
	uint32_t	weekday;		// 0 Sunday
	uint32_t	yearday;		// 0-365
	int32_t		utc_offset;		// seconds
	const char	*zone;			// abbreviation, or NULL
} qc_calendar_t;

// where a dump requested by QuakeC goes
typedef enum
{
	QC_DUMP_COREDUMP,		// coredump(): globals and entities
	QC_DUMP_ENTITY,			// eprint(e)
	QC_DUMP_OBJERROR,		// the entity objerror prints
	QC_DUMP_TRACE			// a stack trace (stackdump)
} qc_dumpkind_t;

// an animation opcode for the host to perform, or for the VM's default
typedef enum
{
	QC_STATE_STATE,			// self.frame = frame; self.think = func; self.nextthink = time + step
	QC_STATE_CSTATE,		// Hexen 2: cycle self.frame from first to last (func thinks)
	QC_STATE_CWSTATE,		// the same on self.weaponframe
	QC_STATE_THINKTIME		// ent.nextthink = time + delay
} qc_statekind_t;

typedef struct
{
	qc_statekind_t	kind;
	float			frame;			// STATE
	float			first, last;	// CSTATE, CWSTATE
	qc_func_t		func;			// STATE's think, the cycling function of CSTATE/CWSTATE
	qc_ent_t		ent;			// THINKTIME
	float			delay;			// THINKTIME
} qc_stateop_t;

typedef struct
{
	void		(*warning) (void *ctx, const qc_warning_t *warning);
	void		(*print) (void *ctx, const char *text);
	void		(*dprint) (void *ctx, const char *text);			// developer prints
	void		(*centerprint) (void *ctx, const char *text);
	void		(*localcmd) (void *ctx, const char *text);			// to the command buffer
	void		(*dump) (void *ctx, qc_dumpkind_t kind, const char *text);

	float		(*cvar_float) (void *ctx, const char *name);
	// NULL if the cvar doesn't exist; valid until the next host call
	const char	*(*cvar_string) (void *ctx, const char *name);
	void		(*cvar_set) (void *ctx, const char *name, const char *value);
	bool		(*cvar_info) (void *ctx, const char *name, qc_cvarinfo_t *info);
	// creates a cvar unless it exists; whether it did
	bool		(*register_cvar) (void *ctx, const char *name, const char *value, uint32_t flags);
	// the cvars matching pattern (wildcards if it has * or ?, else a prefix) and
	// not antipattern, one call of add each
	void		(*cvar_list) (void *ctx, const char *pattern, const char *antipattern,
					void (*add) (void *list, const char *name), void *list);
	bool		(*cvars_have_unsaved) (void *ctx);

	// NULL: the extensions the VM's standard builtins implement completely
	bool		(*check_extension) (void *ctx, const char *name);
	uint32_t	(*check_command) (void *ctx, const char *name);		// 1 command, 2 alias, 3 cvar
	void		(*register_command) (void *ctx, const char *name);
	float		(*is_demo) (void *ctx);				// 0 no, 1 a demo, 2 an MVD
	bool		(*is_server) (void *ctx);
	bool		(*sim_time) (void *ctx, double *time);	// gettime(5)
	bool		(*calendar_time) (void *ctx, bool local, qc_calendar_t *out);

	// a file for QuakeC (buf_loadfile), malloc'd; the VM frees it
	uint8_t		*(*read_file) (void *ctx, const char *path, size_t *size);
	// another progs for addprogs, with a reference for the VM
	qc_progs_t	*(*load_progs) (void *ctx, const char *name);

	// Performs an animation opcode; false with *handled unset lets the VM apply
	// FTE's default. Return false after QC_Fail to abort the call.
	bool		(*state_op) (void *ctx, qcvm_t *vm, const qc_stateop_t *op, bool *handled);

	// a line per statement while tracing (traceon)
	void		(*trace) (void *ctx, const char *line);

	// after an entity is spawned, with its fields zeroed and defaulted
	void		(*on_spawn) (void *ctx, qcvm_t *vm, qc_ent_t e);
	// before an entity is removed, its fields intact; false refuses (with a
	// warning of the host's own, if any)
	bool		(*on_remove) (void *ctx, qcvm_t *vm, qc_ent_t e);

	// a monotonic clock in seconds: entity reuse and deadlines
	double		(*clock) (void *ctx);
} qc_host_t;

/*
==============================================================================

VIRTUAL MACHINES

==============================================================================
*/

// A VM running progs (it takes a reference), binding its builtins by number and
// name against builtins (the registry must outlive the VM; NULL for none).
// config and host are copied. On failure, error (if not NULL) gets why.
qcvm_t	*QC_Create (qc_progs_t *progs, const qc_builtins_t *builtins, const qc_config_t *config,
			const qc_host_t *host, void *ctx, qc_error_t *error);
void	QC_Destroy (qcvm_t *vm);

// back to the state it was created in: globals initialized, entities and
// strings dropped; false (with QC_LastError) if memory can't be rebuilt
bool	QC_Reset (qcvm_t *vm);

// the error of the last call that failed
const qc_error_t	*QC_LastError (const qcvm_t *vm);
void	QC_FreeError (qc_error_t *error);	// what an error from QC_Create holds

qc_progs_t	*QC_MainProgs (const qcvm_t *vm);
void		*QC_HostContext (const qcvm_t *vm);
const qc_config_t	*QC_Config (const qcvm_t *vm);
void	QC_SetDeveloper (qcvm_t *vm, bool on);

// the host clock for entity reuse, in seconds (the VM's own until set)
void	QC_SetTime (qcvm_t *vm, double seconds);
double	QC_Time (const qcvm_t *vm);

/*
------------------------------------------------------------------------------
globals and fields
------------------------------------------------------------------------------
*/

// A global of the main progs by name: its word (an index into QC_Globals) and
// type. Globals stay where they are for the VM's life.
bool		QC_FindGlobal (const qcvm_t *vm, const char *name, uint32_t *word, uint32_t *type);
qc_word_t	*QC_Globals (qcvm_t *vm);	// the main progs' globals
uint32_t	QC_NumGlobals (const qcvm_t *vm);

// the same of progs pr (NULL and false past those loaded)
bool		QC_FindGlobalIn (const qcvm_t *vm, uint32_t pr, const char *name, uint32_t *word, uint32_t *type);
qc_word_t	*QC_GlobalsIn (qcvm_t *vm, uint32_t pr);

// a field by name: its word offset in an entity, and type
bool		QC_FindField (const qcvm_t *vm, const char *name, uint32_t *ofs, uint32_t *type);

// the field, added if the progs lacks it (from the reserve each entity has);
// false if it exists with another size or the reserve is spent
bool		QC_EnsureField (qcvm_t *vm, const char *name, uint32_t type, uint32_t *ofs);

// the field table: every field the VM knows, the main progs' first
uint32_t	QC_NumFields (const qcvm_t *vm);
bool		QC_FieldAt (const qcvm_t *vm, uint32_t i, const char **name, uint32_t *type, uint32_t *ofs);
uint32_t	QC_FieldWords (const qcvm_t *vm);		// words of fields each entity has

// Reads or writes words of an entity's fields (ignoring protection, which is
// QuakeC's only); false if the entity or field doesn't exist.
bool		QC_GetField (const qcvm_t *vm, qc_ent_t e, uint32_t ofs, uint32_t words, uint32_t *out);
bool		QC_SetField (qcvm_t *vm, qc_ent_t e, uint32_t ofs, uint32_t words, const uint32_t *in);

// bytes of VM memory; false if not all readable or writable
bool		QC_ReadMemory (const qcvm_t *vm, qc_ptr_t p, void *out, size_t len);
bool		QC_WriteMemory (qcvm_t *vm, qc_ptr_t p, const void *in, size_t len);

// n zeroed bytes of the QuakeC heap, as memalloc gives them (up to 16 MiB),
// for the host to hand QuakeC: the pointer, or 0; and freeing a block, as
// memfree does
qc_ptr_t	QC_Alloc (qcvm_t *vm, size_t n);
bool		QC_Free (qcvm_t *vm, qc_ptr_t p);

/*
------------------------------------------------------------------------------
entities

Each entity is a block of the stride's size, which never moves: the host's
header (entity_header_bytes), then the fields. QC_Edicts is block 0; block e
is at QC_Edicts + (e << QC_EdictShift).
------------------------------------------------------------------------------
*/

uint8_t		*QC_Edicts (qcvm_t *vm);
uint32_t	QC_EdictShift (const qcvm_t *vm);
uint32_t	QC_NumEdicts (const qcvm_t *vm);	// slots allocated so far, the world included
uint32_t	QC_MaxEdicts (const qcvm_t *vm);

// Commits the memory of the first count blocks now (else it is committed as
// slots are first spawned into), for a host that reaches any block below
// count directly; they read as zero until spawned into.
bool		QC_CommitEdicts (qcvm_t *vm, uint32_t count);

// allocates an entity as FTE does (reusing a slot freed over half a second
// ago, or in the first two seconds, else a new one, else any free one), its
// fields zeroed and defaulted; false with QC_ERR_NO_FREE_EDICTS when all are used
bool		QC_Spawn (qcvm_t *vm, qc_ent_t *e);

// marks a slot in use, allocating the slots up to it (the host's own, below
// first_spawnable); its fields are zeroed if it was free
bool		QC_ClaimEdict (qcvm_t *vm, qc_ent_t e);

// frees an entity (instant: its slot may be reused at once), zeroing the
// remove_clears fields; false (with a warning) for the world, a free, protected
// or unknown entity, or when on_remove refuses
bool		QC_Remove (qcvm_t *vm, qc_ent_t e, bool instant);

bool		QC_IsFree (const qcvm_t *vm, qc_ent_t e);	// or beyond the slots
bool		QC_SetProtected (qcvm_t *vm, qc_ent_t e, bool on);	// the old setting
bool		QC_IsProtected (const qcvm_t *vm, qc_ent_t e);
// how often a slot was spawned into: (e, serial) names an entity for its life
uint32_t	QC_Serial (const qcvm_t *vm, qc_ent_t e);

/*
------------------------------------------------------------------------------
strings

A string reference is a byte address in VM memory (the progs' strings come
first, so their offsets are addresses), a temp string (0x80000000 | slot) or a
static string (0xC0000000 | index); 0 is null. Temp strings are collected when
nothing in VM memory refers to them (or pins them), and only when no QuakeC
runs. Static strings live as long as the VM.
------------------------------------------------------------------------------
*/

// The text, always NUL-terminated; "" for null or unresolvable references. It
// stays valid until QuakeC runs again or the builtin reading it returns.
const char	*QC_String (qcvm_t *vm, qc_str_t s);
bool		QC_IsValidString (qcvm_t *vm, qc_str_t s);
bool		QC_IsTempString (qc_str_t s);

// a new temp string of len bytes; 0 (with QC_LastError) past the limits
qc_str_t	QC_TempString (qcvm_t *vm, const char *text, size_t len);

// a permanent copy (identical text gives the same reference)
qc_str_t	QC_Intern (qcvm_t *vm, const char *text, size_t len);

// a permanent reference to the host's own NUL-terminated text, read where it is
// whenever QuakeC reads it (so it follows the text as it changes); the same
// pointer gives the same reference. The text must outlive the VM.
qc_str_t	QC_HostString (qcvm_t *vm, const char *text);

// keeps a temp string alive until unpinned; pins nest
void		QC_Pin (qcvm_t *vm, qc_str_t s);
void		QC_Unpin (qcvm_t *vm, qc_str_t s);
uint32_t	QC_NumTempStrings (const qcvm_t *vm);

// collects temp strings now (only when no QuakeC runs); how many were freed
uint32_t	QC_CollectGarbage (qcvm_t *vm);

/*
==============================================================================

BUILTINS

A progs refers to builtins by number (= #N) or by name (= #0, bound by the
function's own name). A registry maps both to functions; each VM binds its
progs' builtins against it when the progs is loaded. Builtins the registry
lacks fail only when QuakeC calls them.

==============================================================================
*/

typedef enum
{
	QC_NUMBERING_CSQC,		// FTE's client-side numbers
	QC_NUMBERING_SSQC,		// FTE's server-side numbers (id's and QuakeWorld's, extended)
	QC_NUMBERING_MENU,		// FTE's menu numbers
	QC_NUMBERING_NONE		// by name only
} qc_numbering_t;

// A builtin reads its arguments (QC_Arg*), writes its result (QC_Return*) and
// may call back into QuakeC. It returns false to fail the call, after QC_Error,
// QC_HostError or QC_Abort; the host's context is QC_HostContext (vm).
typedef bool (*qc_builtin_t) (qcvm_t *vm);

qc_builtins_t	*QC_BuiltinsCreate (qc_numbering_t numbering);		// empty; NULL out of memory
void			QC_BuiltinsFree (qc_builtins_t *b);
qc_numbering_t	QC_BuiltinsNumbering (const qc_builtins_t *b);

// Registers (or replaces) a builtin: at its number in FTE's table for the
// registry's numbering (by name only if it has none there), or at number.
// false when out of memory.
bool	QC_BuiltinsSet (qc_builtins_t *b, const char *name, qc_builtin_t func);
bool	QC_BuiltinsSetNumbered (qc_builtins_t *b, uint32_t number, const char *name, qc_builtin_t func);

// alias resolves (by name) to target's function
bool	QC_BuiltinsAlias (qc_builtins_t *b, const char *alias, const char *target);
void	QC_BuiltinsRemove (qc_builtins_t *b, const char *name);
bool	QC_BuiltinsContains (const qc_builtins_t *b, const char *name);
qc_builtin_t	QC_BuiltinsFind (const qc_builtins_t *b, const char *name);

// FTE's number of a builtin, if it has one
bool	QC_BuiltinNumber (qc_numbering_t numbering, const char *name, uint32_t *number);

// the registered builtins, in the order they were registered: how many, and the
// i-th one's name and the number it is bound to (*numbered false for those
// bound by name only)
uint32_t	QC_BuiltinsCount (const qc_builtins_t *b);
bool		QC_BuiltinsAt (const qc_builtins_t *b, uint32_t i, const char **name, uint32_t *number,
				bool *numbered);

// The standard builtins, numbered for numbering: every builtin FTE provides
// that needs no engine (NULL when out of memory). A host adds its own and
// replaces or removes any of them.
qc_builtins_t	*QC_BuiltinsStandard (qc_numbering_t numbering);

// whether the standard builtins implement an extension completely: what
// checkextension answers for a host without check_extension
bool	QC_StandardExtension (const char *name);

// every builtin FTE declares for a numbering: numbered ones, then those bound
// by name (*numbered false)
uint32_t	QC_NumKnownBuiltins (qc_numbering_t numbering);
bool		QC_KnownBuiltin (qc_numbering_t numbering, uint32_t i, const char **name, uint32_t *number,
				bool *numbered);

/*
==============================================================================

CALLING QUAKEC

==============================================================================
*/

// Calls a QuakeC function or a builtin with up to 8 arguments, its result in
// *ret (if not NULL). Builtins may call it too (re-entrantly); they read their
// arguments first and set their result after, as the parameter and return
// slots are shared. False on an error (QC_LastError), after which the VM is
// fine to use.
bool		QC_Call (qcvm_t *vm, qc_func_t f, int argc, const qc_value_t *args, qc_value_t *ret);

// the same with the main progs' self set to an entity, and restored after
bool		QC_CallAs (qcvm_t *vm, qc_ent_t self, qc_func_t f, int argc, const qc_value_t *args, qc_value_t *ret);

// A function of the main progs, or of progs pr, by name, as QuakeC sees it
// now: a function-typed global of that name gives its current value (0 if
// QuakeC cleared it), else the function of that name; 0 if none.
qc_func_t	QC_FindFunction (const qcvm_t *vm, const char *name);
qc_func_t	QC_FindFunctionIn (const qcvm_t *vm, uint32_t pr, const char *name);

// statement tracing (traceon): each statement goes to the host's trace first
void		QC_SetTrace (qcvm_t *vm, bool on);
bool		QC_IsTracing (const qcvm_t *vm);

// Counts the statements each function runs (the interpreter is slower while
// it does). QC_Profile gives progs pr's counts, one per function (NULL if
// never counted); QC_ClearProfile starts them over.
void			QC_SetProfiling (qcvm_t *vm, bool on);
const uint64_t	*QC_Profile (const qcvm_t *vm, uint32_t pr, uint32_t *count);
void			QC_ClearProfile (qcvm_t *vm);

// A builtin of the loaded progs that no registered builtin satisfies: calling
// it fails. With reachable, only those the code calls.
typedef struct
{
	qc_func_t	function;
	uint32_t	number;			// 0 for those bound by name
	const char	*name;
} qc_unbound_t;

// fills up to max of them and returns how many there are
uint32_t	QC_UnboundBuiltins (const qcvm_t *vm, bool reachable, qc_unbound_t *out, uint32_t max);
bool		QC_IsBuiltinBound (const qcvm_t *vm, qc_func_t f);

/*
------------------------------------------------------------------------------
multiprogs

Further progs in one VM (FTE_MULTIPROGS: addprogs, CSQC add-ons). Each keeps
its own globals, PARM and RETURN slots included; function values carry the
progs number in their top byte. Entity fields are unified by name, and new
ones take the room each entity reserves (field_reserve_bytes). Globals flagged
as shared, and the configuration's shared_globals, are copied between progs
whenever execution passes from one to another.
------------------------------------------------------------------------------
*/

// Loads another progs (taking a reference) and gives its number: its
// functions become callable, its fields are unified with those known, its
// extern (bodyless) functions are linked to those other progs define, and its
// init(float prevprogs) runs. False (QC_LastError) when the progs limit, the
// address space for progs or the fields' room runs out (nothing is added), or
// with init's error (the progs stays loaded).
bool		QC_AddProgs (qcvm_t *vm, qc_progs_t *progs, uint32_t *prnum);
uint32_t	QC_NumProgs (const qcvm_t *vm);		// 1 and those added
qc_progs_t	*QC_LoadedProgs (const qcvm_t *vm, uint32_t pr);	// NULL past them

/*
------------------------------------------------------------------------------
threads and autocvars
------------------------------------------------------------------------------
*/

// Resumes the sleeping QuakeC threads (sleep, fork) whose wake time has come
// by the main progs' time global, in wake order; *ran (if not NULL) gets how
// many ran. Call it once a frame, when no QuakeC runs. False with the first
// error a thread raised (those not yet resumed stay asleep).
bool		QC_RunThreads (qcvm_t *vm, uint32_t *ran);
uint32_t	QC_SleepingThreads (const qcvm_t *vm);

// Copies the host's cvars (cvar_string) into every progs' autocvars, the
// autocvar_<name> globals, parsed by type: floats as atof, integers as atoi,
// vectors as stov, strings as temp strings. Those of cvars the host lacks keep
// their value (the progs' default until set). FTE does it when a progs loads
// and whenever a cvar changes; QC_SyncAutocvar does it for one cvar. False
// when out of memory.
bool		QC_SyncAutocvars (qcvm_t *vm);
bool		QC_SyncAutocvar (qcvm_t *vm, const char *cvar);

// After the host longjmps out of a call (from a builtin or a callback), puts
// the VM back as if no QuakeC ran: frames unwound, locals restored.
void		QC_Abandon (qcvm_t *vm);

/*
------------------------------------------------------------------------------
inside builtins

Arguments and results are the PARM and RETURN slots of the progs whose code
called the builtin.
------------------------------------------------------------------------------
*/

int			QC_Argc (const qcvm_t *vm);
qc_value_t	QC_ArgValue (const qcvm_t *vm, int i);		// zero past the eighth
float		QC_ArgFloat (const qcvm_t *vm, int i);
int32_t		QC_ArgInt (const qcvm_t *vm, int i);
uint32_t	QC_ArgWord (const qcvm_t *vm, int i);		// any raw word: entity, string, pointer...
void		QC_ArgVector (const qcvm_t *vm, int i, float out[3]);
const char	*QC_ArgString (qcvm_t *vm, int i);			// "" for null or invalid; see QC_String

// the first word of argument slot i: an __out parameter, which the compiler
// copies back after the call
void		QC_SetArgWord (qcvm_t *vm, int i, uint32_t word);

void		QC_ReturnValue (qcvm_t *vm, qc_value_t v);
void		QC_ReturnFloat (qcvm_t *vm, float f);
void		QC_ReturnInt (qcvm_t *vm, int32_t i);
void		QC_ReturnWord (qcvm_t *vm, uint32_t u);
void		QC_ReturnVector (qcvm_t *vm, const float v[3]);
// new text as a temp string; false (for the builtin to return) past the limits
bool		QC_ReturnString (qcvm_t *vm, const char *text, size_t len);

// the builtin running: what QuakeC called, its number (0 by name) and name
qc_func_t	QC_BuiltinFunction (const qcvm_t *vm, uint32_t *number, const char **name);

// the name of the QuakeC function running, or that called the builtin running
// ("" if none)
const char	*QC_CallerName (const qcvm_t *vm);

// Fails the builtin, for it to return: QC_Error is FTE's builtin error (only a
// warning, and a zero result, in developer mode); QC_HostError always fails.
bool		QC_Error (qcvm_t *vm, const char *fmt, ...);
bool		QC_HostError (qcvm_t *vm, const char *fmt, ...);

// Unwinds QuakeC to the host's nearest call, which then returns ret as if the
// function it called had: FTE's abort
bool		QC_Abort (qcvm_t *vm, qc_value_t ret);

// a warning with the current backtrace, for the host's warning callback
void		QC_Warning (qcvm_t *vm, const char *fmt, ...);
