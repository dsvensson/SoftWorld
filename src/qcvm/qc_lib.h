// qc_lib.h -- the standard builtins' internals: what the library modules share
//
// The standard builtins are everything FTE provides that needs no engine
// (qcvm-rs's src/stdlib, which docs/spec/builtins.md and strings.md describe).
// Each qc_lib_<area>.c registers its builtins from a table; QC_BuiltinsStandard
// registers them all.
#pragma once

#include "qc_local.h"

#include <stdarg.h>

/*
==============================================================================

REGISTRATION

==============================================================================
*/

typedef struct
{
	const char		*name;
	qc_builtin_t	func;			// NULL: an alias of target
	const char		*target;
	uint32_t		fte;			// FTE's number, for a numbering whose table omits the name
} qc_libentry_t;

bool	QC_LibRegister (qc_builtins_t *b, const qc_libentry_t *table, size_t count);

bool	QC_RegisterConvert (qc_builtins_t *b);
bool	QC_RegisterEntity (qc_builtins_t *b);
bool	QC_RegisterHostcalls (qc_builtins_t *b);
bool	QC_RegisterIntrospect (qc_builtins_t *b);
bool	QC_RegisterMath (qc_builtins_t *b);
bool	QC_RegisterReflect (qc_builtins_t *b);
bool	QC_RegisterTime (qc_builtins_t *b);
bool	QC_RegisterVector (qc_builtins_t *b);

/*
==============================================================================

ARGUMENTS AND RESULTS

==============================================================================
*/

// the most FTE concatenates of variadic string arguments
#define QC_CONCAT_MAX	65543

// FTE's concatenated varargs: the string arguments from the from-th on, joined
// (malloc'd, NUL-terminated; its length in *len if not NULL), or NULL after an
// out-of-memory error
char	*QC_LibConcat (qcvm_t *vm, int from, size_t *len);

int32_t	QC_LibArgInt (const qcvm_t *vm, int i);				// a float truncated
float	QC_LibOptFloat (const qcvm_t *vm, int i, float def);	// def if not passed

// an entity argument; past the entities, a warning and the world
uint32_t	QC_LibEntArg (qcvm_t *vm, int i);

// the first word of parameter slot i: an __out parameter the compiler copies back
void	QC_LibSetArgWord (qcvm_t *vm, int i, uint32_t word);

void	QC_LibReturnBool (qcvm_t *vm, bool b);

// Returns text as a temp string, or null for NULL; false past the limits
bool	QC_LibReturnOptString (qcvm_t *vm, const char *text);

// FTE's builtin error for builtins that carry on after it: in developer mode a
// warning and a zeroed result, and true for the builtin to go on with its
// fallback; else the error, and false to return
bool	QC_LibSoftError (qcvm_t *vm, const char *fmt, ...);

/*
==============================================================================

GLOBALS, FIELDS AND FUNCTIONS

==============================================================================
*/

// A global of the progs running now (in a builtin, the progs whose code called
// it) by name, if it has one of type want: its S byte offset
bool	QC_LibGlobal (const qcvm_t *vm, const char *name, uint32_t want, uint32_t *ofs);

int64_t		QC_LibField (const qcvm_t *vm, const char *name);		// a field's word, or -1
bool		QC_LibFieldOk (const qcvm_t *vm, uint32_t f, uint32_t words);
uint32_t	QC_LibWord (const qcvm_t *vm, uint32_t e, uint32_t f);	// 0 if either is invalid
float		QC_LibFloat (const qcvm_t *vm, uint32_t e, uint32_t f);
void		QC_LibVector (const qcvm_t *vm, uint32_t e, uint32_t f, float out[3]);
// writes whatever the entity's protection (the engine's writes)
void		QC_LibSetWord (qcvm_t *vm, uint32_t e, uint32_t f, uint32_t v);
void		QC_LibSetVector (qcvm_t *vm, uint32_t e, uint32_t f, const float v[3]);
float		QC_LibNamedFloat (const qcvm_t *vm, uint32_t e, const char *name);	// 0 without the field
void		QC_LibNamedVector (const qcvm_t *vm, uint32_t e, const char *name, float out[3]);

// A function by name as FTE's PR_FindFunction finds it, in the progs prnum
// names (0 the main progs, -1 the running one, -2 each in turn); an "N:" prefix
// names progs N. A function-typed global of the name gives its value. 0 if none.
qc_func_t	QC_LibFindFunction (const qcvm_t *vm, int32_t prnum, const char *name);

// the progs prnum names, as QC_LibFindFunction searches them: [*first, *end)
void	QC_LibProgsRange (const qcvm_t *vm, int32_t prnum, uint32_t *first, uint32_t *end);

/*
==============================================================================

NUMBERS AS TEXT (qc_dtoa.c)

C's printf conversions made exactly on every platform (glibc's output): the
digits of a double's exact binary value, rounded half to even, and C's
conventions on top. And C's strtod, strtol and strtoul in the C locale.

==============================================================================
*/

// text that takes at most cap bytes, dropping the rest
typedef struct
{
	char	*buf;			// NUL-terminated (NULL until something is added)
	size_t	len;
	size_t	size;			// allocated
	size_t	cap;
	bool	failed;			// out of memory: the text is short
} qc_sink_t;

void	QC_SinkInit (qc_sink_t *s, size_t cap);
void	QC_SinkFree (qc_sink_t *s);
size_t	QC_SinkRoom (const qc_sink_t *s);
void	QC_SinkPush (qc_sink_t *s, char c);
void	QC_SinkAppend (qc_sink_t *s, const char *text, size_t n);
void	QC_SinkFill (qc_sink_t *s, char c, size_t n);
void	QC_SinkPrintf (qc_sink_t *s, const char *fmt, ...);		// integers and text only
const char	*QC_SinkText (const qc_sink_t *s);						// "" if empty

// a printf conversion's flags, width and precision
typedef struct
{
	bool	left;			// -
	bool	zero;			// 0
	bool	plus;			// +
	bool	space;			// ' '
	bool	alt;			// #
	size_t	width;
	bool	has_prec;
	size_t	prec;
} qc_spec_t;

// C's %e %f %g (or %E %F %G) of v
void	QC_FormatFloat (qc_sink_t *s, double v, char conv, const qc_spec_t *spec);
// C's %d %u %o %x %X of a sign and magnitude; is_signed enables + and ' '
void	QC_FormatInt (qc_sink_t *s, bool negative, uint64_t magnitude, uint32_t radix, bool upper,
			bool is_signed, const qc_spec_t *spec);
// C's %s of len bytes
void	QC_FormatStr (qc_sink_t *s, const char *text, size_t len, const qc_spec_t *spec);

// C's %.<prec>f, %.<prec>e and %.<prec>g of v appended to a sink
void	QC_FormatF (qc_sink_t *s, double v, size_t prec);
void	QC_FormatE (qc_sink_t *s, double v, size_t prec);
void	QC_FormatG (qc_sink_t *s, double v, size_t prec);

bool	QC_IsCSpace (int c);				// isspace in the C locale

// C's strtod in the C locale: the value, and the bytes read in *used (0: no
// number). Decimal numbers are rounded correctly; hex floats, inf, infinity,
// nan and nan(chars) are read too.
double	QC_Strtod (const char *s, size_t *used);
int64_t	QC_Strtol (const char *s, uint32_t base);		// a 64-bit long, saturating
uint64_t	QC_Strtoul (const char *s, uint32_t base);	// ULONG_MAX on overflow

/*
==============================================================================

MODULE HELPERS

==============================================================================
*/

// ftos's text of v (FTE's digits)
void	QC_FtosText (qc_sink_t *s, float v);

// Quake's AngleVectors: forward, right, up of (pitch, yaw, roll)
void	QC_AngleVectors (const float angles[3], float forward[3], float right[3], float up[3]);

// a value as FTE's savegame writer spells it (PR_UglyValueString)
void	QC_FormatValue (qcvm_t *vm, qc_sink_t *s, uint32_t type, const uint32_t words[3]);
// an entity as a savegame block: {, a "field" "value" line each non-zero field, }
void	QC_EntityBlock (qcvm_t *vm, qc_sink_t *s, uint32_t e);
// FTE's coredump: the progs, the call stack, the saved globals, every entity
void	QC_CoredumpText (qcvm_t *vm, qc_sink_t *s);
// the call stack, a line per frame
void	QC_BacktraceSink (const qcvm_t *vm, qc_sink_t *s);

// the UTC calendar time of seconds since 1970
void	QC_CalendarFromUnix (int64_t secs, qc_calendar_t *out);
// the host's calendar time, else UTC from the system clock; false if neither
bool	QC_LibCalendar (qcvm_t *vm, bool local, qc_calendar_t *out);
