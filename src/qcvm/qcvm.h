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
