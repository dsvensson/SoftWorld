// qc_harness.h -- testing builtins directly (qcvm-rs's tests/all/support/harness.rs)
//
// A VM with the standard builtins, running a progs that declares every builtin
// FTE knows for a numbering (so tests call them by name), and a host that
// records what they printed, dumped, warned and asked for.
#pragma once

#include "qc_asm.h"
#include "qc_local.h"		// QC_FloatBits and the like, and the VM's insides
#include "qc_test.h"

typedef struct
{
	qc_dumpkind_t	kind;
	char			*text;
} qh_dump_t;

typedef struct
{
	qc_warnkind_t	kind;
	char			*text;			// the kind's text, without where
} qh_warning_t;

typedef struct
{
	char	*name, *value;
} qh_cvar_t;

typedef struct
{
	char	*path;
	uint8_t	*data;
	size_t	size;
	char	*pack;			// the pack it is in (whichpack), NULL for none
} qh_file_t;

// the host: what the builtins did and asked
typedef struct
{
	qt_text_t		printed, dprinted, centerprinted, localcmds;
	qh_dump_t		*dumps;
	int				numdumps;
	qh_warning_t	*warnings;
	int				numwarnings;
	qh_cvar_t		*cvars;			// an in-memory cvar table
	int				numcvars;
	char			**commands;		// registercommand's
	int				numcommands;
	qh_file_t		*files;			// what read_file and file_open find, and file_write makes
	int				numfiles;
	int				openfiles;		// file_open's not yet closed
	bool			has_now;		// calendar_time's answer, else the VM's UTC
	qc_calendar_t	now;
	bool			has_sim_time;	// gettime(5)'s
	double			sim_time;
} qh_host_t;

typedef struct
{
	qcvm_t			*vm;
	qc_builtins_t	*builtins;
	qh_host_t		host;
	char			*result;		// QH_String's last text
} qh_t;

// adds globals, fields, functions and builtins to the progs before the known builtins
typedef void (*qh_setup_t) (qc_asm_t *a, void *ctx);

// A harness for a numbering: config NULL for the defaults of its kind, setup
// NULL for nothing added. QH_NewWith runs its own builtins (it takes them).
qh_t	*QH_New (qc_numbering_t numbering, const qc_config_t *config, qh_setup_t setup, void *ctx);
qh_t	*QH_NewWith (qc_numbering_t numbering, const qc_config_t *config, qc_builtins_t *builtins,
			qh_setup_t setup, void *ctx);
qh_t	*QH_Csqc (void);
void	QH_Free (qh_t *h);

// a setup declaring builtins FTE binds by name (ctx: a NULL-terminated list of names)
void	QH_Named (qc_asm_t *a, void *ctx);

// a setup adding peek(pointer p, int i) = p[i], an int load through a pointer
void	QH_AddPeek (qc_asm_t *a);

qc_func_t	QH_Func (qh_t *h, const char *name);

// arguments
#define F(x)		QC_ValFloat (x)
#define I(x)		QC_ValInt (x)
#define V(x, y, z)	QC_ValVector (x, y, z)
#define W(x)		QC_ValWord (x)
qc_value_t	QH_S (qh_t *h, const char *text);		// a new temp string

// an argument list: a count and the values. Counted by the preprocessor: MSVC
// makes a compound literal in sizeof a temporary it then warns is unused.
#define QH_COUNT_(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, n, ...)	n
#define QH_COUNT(...)	QH_COUNT_ (__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define ARGS(...)	QH_COUNT (__VA_ARGS__), (qc_value_t[]){__VA_ARGS__}
#define NOARGS		0, NULL

// Calls a builtin (or function) by name: false with the VM's error
bool	QH_Call (qh_t *h, const char *name, int argc, const qc_value_t *args, qc_value_t *ret);
// the kind of error the call fails with, QC_ERR_NONE if it doesn't
qc_errkind_t	QH_Fails (qh_t *h, const char *name, int argc, const qc_value_t *args);
// the last error's message ("" without one)
const char	*QH_ErrorMessage (qh_t *h);

// calls that must succeed (a failed check if not), and their results
qc_value_t	QH_Raw (qh_t *h, const char *name, int argc, const qc_value_t *args);
float		QH_Float (qh_t *h, const char *name, int argc, const qc_value_t *args);
int32_t		QH_Int (qh_t *h, const char *name, int argc, const qc_value_t *args);
uint32_t	QH_Word (qh_t *h, const char *name, int argc, const qc_value_t *args);
void		QH_Vector (qh_t *h, float out[3], const char *name, int argc, const qc_value_t *args);
// the result's text ("" for null), good until the next QH_String or QH_OptString
const char	*QH_String (qh_t *h, const char *name, int argc, const qc_value_t *args);
// the same, NULL for a null reference
const char	*QH_OptString (qh_t *h, const char *name, int argc, const qc_value_t *args);

// a string reference's text ("" for null or invalid)
const char	*QH_Text (qh_t *h, uint32_t ref);

// the first word of parameter slot i after a call (__out parameters)
uint32_t	QH_ParmWord (qh_t *h, int i);
// p[i] as QuakeC reads it (needs QH_AddPeek)
int32_t		QH_Peek (qh_t *h, uint32_t p, int32_t i);

// the warnings' texts: how many there are, and the i-th
int			QH_NumWarnings (const qh_t *h);
const char	*QH_WarningText (const qh_t *h, int i);
void		QH_ClearWarnings (qh_t *h);

// cvars and files for the host to answer with
void		QH_SetCvar (qh_t *h, const char *name, const char *value);
const char	*QH_Cvar (const qh_t *h, const char *name);		// NULL if none
void		QH_AddFile (qh_t *h, const char *path, const void *data, size_t size);
void		QH_AddPackedFile (qh_t *h, const char *pack, const char *path, const void *data, size_t size);
// a file's bytes and size, NULL if there is none
const uint8_t	*QH_File (const qh_t *h, const char *path, size_t *size);
