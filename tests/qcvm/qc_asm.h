// qc_asm.h -- a small assembler that writes progs in every format the VM loads
//
// Tests build programs instruction by instruction, so opcodes can be checked
// without a QuakeC compiler. Globals are laid out as fteqcc lays them out: 28
// reserved words (null, return, eight parameter slots), then everything else.
#pragma once

#include "qcvm.h"
#include "qc_opcode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// reserved globals
#define QA_OFS_RETURN	1
#define QA_OFS_PARM0	4
#define QA_PARM(i)		(QA_OFS_PARM0 + 3 * (i))

typedef struct qc_asm_s qc_asm_t;

// a function under construction
typedef struct
{
	uint32_t	index;			// the value a function global holds
	uint32_t	parm_start;		// first global word of its parameters and locals
} qa_func_t;

static inline uint32_t QA_Local (qa_func_t f, uint32_t i)
{
	return f.parm_start + i;
}

qc_asm_t	*QA_New (void);
void		QA_Free (qc_asm_t *a);

// a string's offset in the string table (interned)
uint32_t	QA_String (qc_asm_t *a, const char *s);

// words global words starting with init's (the rest zero), without a definition
uint32_t	QA_Alloc (qc_asm_t *a, uint32_t words, const uint32_t *init, uint32_t count);

// a named global, as big as its type or its initial words; returns its word
uint32_t	QA_Global (qc_asm_t *a, const char *name, uint32_t type, const uint32_t *init, uint32_t count);
static inline uint32_t QA_Global1 (qc_asm_t *a, const char *name, uint32_t type, uint32_t init)
{
	return QA_Global (a, name, type, &init, 1);
}

// a definition for an existing word; QA_DefGlobalAt names it by string offset
void		QA_DefGlobal (qc_asm_t *a, const char *name, uint32_t type, uint32_t ofs);
void		QA_DefGlobalAt (qc_asm_t *a, uint32_t name, uint32_t type, uint32_t ofs);

// unnamed constants
uint32_t	QA_Float (qc_asm_t *a, float v);
uint32_t	QA_Int (qc_asm_t *a, int32_t v);
uint32_t	QA_Vector (qc_asm_t *a, float x, float y, float z);
uint32_t	QA_VectorRaw (qc_asm_t *a, uint32_t x, uint32_t y, uint32_t z);
uint32_t	QA_StrConst (qc_asm_t *a, const char *s);

void		QA_SetGlobal (qc_asm_t *a, uint32_t word, uint32_t value);
uint32_t	QA_Temp (qc_asm_t *a, uint32_t words);		// unnamed scratch globals

// An entity field and a field-typed global holding its offset (vectors also
// get _x, _y and _z fields). Returns the field's word offset; *global (if not
// NULL) gets the global's.
uint32_t	QA_Field (qc_asm_t *a, const char *name, uint32_t type, uint32_t *global);
// the null field definition qcc and fteqcc write first: void, unnamed, at the
// offset the next field takes (0 before any)
void		QA_NullField (qc_asm_t *a);

// a builtin (number 0: bound by name) and a function global for it; returns
// its function index
uint32_t	QA_Builtin (qc_asm_t *a, const char *name, uint32_t number, int32_t num_parms);

// A QuakeC function starting at the next statement: parameters of the given
// sizes, then extra_locals words, and a function global for it.
qa_func_t	QA_Function (qc_asm_t *a, const char *name, const uint8_t *parm_sizes, int num_parms,
				uint32_t extra_locals);

// overrides fields of a declared function record
void		QA_PatchFunction (qc_asm_t *a, uint32_t index, int32_t first_statement, uint32_t parm_start,
				uint32_t locals);

// statements: returns the index
uint32_t	QA_Emit (qc_asm_t *a, uint32_t op, uint32_t x, uint32_t y, uint32_t z);
uint32_t	QA_Here (const qc_asm_t *a);

// the relative jump from statement from to to, as a jump operand stores it
static inline uint32_t QA_Rel (uint32_t from, uint32_t to)
{
	return (uint32_t)((int32_t)to - (int32_t)from);
}

// rewrites operand which (0 = a, 1 = b, 2 = c) of a statement
void		QA_Patch (qc_asm_t *a, uint32_t stmt, int which, uint32_t value);
void		QA_PatchJump (qc_asm_t *a, uint32_t stmt, int which, uint32_t target);

void		QA_Bodyless (qc_asm_t *a, const char *name);		// FTE formats only
uint32_t	QA_NumGlobals (const qc_asm_t *a);

// replaces header word i (of 15, or 23 in version 7) when building
void		QA_HeaderOverride (qc_asm_t *a, int i, uint32_t value);

// the secondary version QA_Build writes for QC_FORMAT_KK7 ("KKQW" by default)
void		QA_SetKK7Magic (qc_asm_t *a, uint32_t magic);

// the program in a format (malloc'd)
uint8_t		*QA_Build (const qc_asm_t *a, qc_format_t format, size_t *size);

// built and loaded; fails the test run on a load error
qc_progs_t	*QA_Load (const qc_asm_t *a, qc_format_t format);

// a VM running the program (as Fte16); config, builtins and host may be NULL
// (the CSQC defaults, no builtins, no host); fails the test run if it can't
qcvm_t		*QA_CreateVM (const qc_asm_t *a, const qc_config_t *config, const qc_builtins_t *builtins,
				const qc_host_t *host, void *ctx);
qcvm_t		*QA_CreateVMAs (const qc_asm_t *a, qc_format_t format, const qc_config_t *config,
				const qc_builtins_t *builtins, const qc_host_t *host, void *ctx);
