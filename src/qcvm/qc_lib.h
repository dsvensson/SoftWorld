// qc_lib.h -- the standard builtins' internals: what the library modules share
//
// The standard builtins are everything FTE provides that needs no engine
// (qcvm-rs's src/stdlib, which docs/spec/builtins.md and strings.md describe).
// Each qc_lib_<area>.c registers its builtins from a table; QC_BuiltinsStandard
// registers them all.
#pragma once

#include "qc_local.h"

#include <stdarg.h>

typedef struct qc_sink_s qc_sink_t;		// text with a cap (qc_dtoa.c)

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
bool	QC_RegisterDigest (qc_builtins_t *b);
bool	QC_RegisterEntity (qc_builtins_t *b);
bool	QC_RegisterFormat (qc_builtins_t *b);
bool	QC_RegisterHash (qc_builtins_t *b);
bool	QC_RegisterHostcalls (qc_builtins_t *b);
bool	QC_RegisterIntrospect (qc_builtins_t *b);
bool	QC_RegisterJson (qc_builtins_t *b);
bool	QC_RegisterMath (qc_builtins_t *b);
bool	QC_RegisterMemory (qc_builtins_t *b);
bool	QC_RegisterProgs (qc_builtins_t *b);
bool	QC_RegisterReflect (qc_builtins_t *b);
bool	QC_RegisterString (qc_builtins_t *b);
bool	QC_RegisterStrbuf (qc_builtins_t *b);
bool	QC_RegisterStrftime (qc_builtins_t *b);
bool	QC_RegisterTime (qc_builtins_t *b);
bool	QC_RegisterThreads (qc_builtins_t *b);
bool	QC_RegisterTokenize (qc_builtins_t *b);
bool	QC_RegisterVector (qc_builtins_t *b);

/*
==============================================================================

THE LIBRARY'S STATE (vm->std)

==============================================================================
*/

// a token: its text and the bytes of the input it was read from
typedef struct
{
	char		*text;
	size_t		len;
	size_t		start, end;
} qc_token_t;

typedef struct qc_hashtables_s qc_hashtables_t;	// qc_lib_hash.c
typedef struct qc_strbufs_s qc_strbufs_t;		// qc_lib_strbuf.c

struct qc_std_s
{
	qc_token_t		*tokens;			// the token list (FTE keeps one a process, this one a VM)
	uint32_t		numtokens;
	size_t			token_bytes;		// what the list is charged
	qc_hashtables_t	*hash;
	qc_strbufs_t	*bufs;
	size_t			container_bytes;	// charged against limits.container_bytes
};

// the VM's library state, made when first needed; NULL after an out-of-memory error
qc_std_t	*QC_LibState (qcvm_t *vm);
void		QC_LibFreeTokens (qc_std_t *std);
void		QC_LibFreeHash (qc_std_t *std);
void		QC_LibFreeBufs (qc_std_t *std);

// Charges n bytes of containers (token lists, hash tables, string buffers)
// against limits.container_bytes; false (charging nothing) past it
bool	QC_LibCharge (qcvm_t *vm, size_t n);
void	QC_LibRelease (qcvm_t *vm, size_t n);

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

// the two-word types (__double, __int64, __uint64): an argument's bits and a result
uint64_t	QC_LibArg64 (const qcvm_t *vm, int i);
void		QC_LibReturn64 (qcvm_t *vm, uint64_t bits);
double		QC_LibArgDouble (const qcvm_t *vm, int i);
void		QC_LibReturnDouble (qcvm_t *vm, double d);

// an entity argument; past the entities, a warning and the world
uint32_t	QC_LibEntArg (qcvm_t *vm, int i);

void	QC_LibReturnBool (qcvm_t *vm, bool b);

// Returns text as a temp string, or null for NULL; false past the limits
bool	QC_LibReturnOptString (qcvm_t *vm, const char *text);

// Returns a sink's text as a temp string, and frees the sink; false past the
// limits or when the sink ran out of memory
bool	QC_LibReturnSink (qcvm_t *vm, qc_sink_t *s);

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
struct qc_sink_s
{
	char	*buf;			// NUL-terminated (NULL until something is added)
	size_t	len;
	size_t	size;			// allocated
	size_t	cap;
	bool	failed;			// out of memory: the text is short
};

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
int64_t	QC_Strtol (const char *s, uint32_t base, size_t *used);		// a 64-bit long, saturating
uint64_t	QC_Strtoul (const char *s, uint32_t base, size_t *used);	// ULONG_MAX on overflow

// FTE's stov: an optional leading ', then up to three numbers apart by spaces or
// tabs; a ' or something not a number ends it
void	QC_ParseVector (const char *s, float out[3]);

/*
==============================================================================

CHARACTERS (qc_lib_charset.c)

==============================================================================
*/

#define QC_REPLACEMENT	0xFFFD

// why a UTF-8 sequence didn't decode cleanly
typedef enum
{
	QC_UTF8_OK,
	QC_UTF8_MALFORMED,		// a stray continuation byte, FE or FF, or a lead byte cut short
	QC_UTF8_ILLEGAL,		// an overlong form, U+FFFE, U+FFFF, or past U+10FFFF
	QC_UTF8_LONE_HIGH,		// a high surrogate without a low one after it
	QC_UTF8_LOW				// a low surrogate
} qc_utf8err_t;

// One character at s (len bytes left) and the bytes it takes (at least one):
// FTE's lenient UTF-8 decoder, or a scheme's
uint32_t	QC_DecodeUtf8 (const uint8_t *s, size_t len, size_t *used, qc_utf8err_t *err);
uint32_t	QC_DecodeChar (const uint8_t *s, size_t len, qc_charscheme_t scheme, size_t *used);

size_t	QC_CharCount (const uint8_t *s, size_t len, qc_charscheme_t scheme);
// the byte offset of character index, or len if there are fewer
size_t	QC_ByteOffset (const uint8_t *s, size_t len, size_t index, qc_charscheme_t scheme);
// the characters that end at or before byte offset ofs
size_t	QC_CharOffset (const uint8_t *s, size_t len, size_t ofs, qc_charscheme_t scheme);

// FTE's UTF-8 encoder: NUL as C0 80, up to 0x7FFFFFFF in the old 5 and 6-byte forms
void	QC_EncodeUtf8 (qc_sink_t *out, uint32_t ch);
// a character in a scheme; what it can't write is ?, or FTE's ^U and ^{} markup
void	QC_EncodeChar (qc_sink_t *out, uint32_t ch, qc_charscheme_t scheme, bool markup);

/*
==============================================================================

STRINGS (qc_lib_string.c, qc_lib_format.c)

==============================================================================
*/

// a malloc'd copy, or NULL
char	*QC_LibDup (const char *s);

// n bytes equal but for ASCII case
bool	QC_LibEqualFold (const char *a, const char *b, size_t n);

// FTE's wildcmp: ? any byte, * any run of bytes but / and \, letters in either case
bool	QC_WildCompare (const char *pattern, const char *s, size_t len);

// base64 (+, / and = padding); decoding as FTE's: - and _ taken for + and /,
// control characters skipped, at most cap bytes (QC_Base64Capacity's estimate)
void	QC_Base64Encode (qc_sink_t *out, const uint8_t *data, size_t len);
void	QC_Base64Decode (qc_sink_t *out, const char *s, size_t len, size_t cap);
size_t	QC_Base64Capacity (size_t len);

// the first needle in haystack (an empty one at 0), or -1; linear time for long needles
int64_t	QC_Find (const char *haystack, size_t hlen, const char *needle, size_t nlen);

// FTE's COM_QuotedString: the text quoted for the console's tokenizer to read
// back as one argument, as if into a buffer of bufsize bytes
void	QC_QuoteString (qc_sink_t *out, const char *s, size_t len, size_t bufsize);

/*
==============================================================================

MODULE HELPERS

==============================================================================
*/

// ftos's text of v (FTE's digits)
void	QC_FtosText (qc_sink_t *s, float v);

// strftime's text of a time
void	QC_StrftimeText (qc_sink_t *out, const char *fmt, const qc_calendar_t *t);

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

// n zeroed bytes of the QuakeC heap: the pointer, or 0; and freeing a block
uint32_t	QC_LibHeapAlloc (qcvm_t *vm, uint32_t n);
bool		QC_LibHeapFree (qcvm_t *vm, uint32_t p);
bool		QC_LibMemfree (qcvm_t *vm);			// the memfree builtin (json_free too)

// the digest of data by FTE's name for it (up to 64 bytes): its size, or 0
size_t	QC_Digest (const char *alg, const void *data, size_t len, uint8_t out[64]);

// the UTC calendar time of seconds since 1970
void	QC_CalendarFromUnix (int64_t secs, qc_calendar_t *out);
// the host's calendar time, else UTC from the system clock; false if neither
bool	QC_LibCalendar (qcvm_t *vm, bool local, qc_calendar_t *out);
