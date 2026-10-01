// qc_test.h -- what the QuakeC VM's tests share: checks, a seeded random
// number generator, file loading and skipping
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// a test that can't run returns this from main (SKIP_RETURN_CODE)
#define QT_SKIP		77

extern int	qt_failures;

// counts and reports a failed check; returns ok
bool	QT_Check (bool ok, const char *what, const char *file, int line);
#define QT_CHECK(cond)	QT_Check ((cond), #cond, __FILE__, __LINE__)

// the same, printing both values on failure
bool	QT_CheckU (uint64_t got, uint64_t want, const char *what, const char *file, int line);
bool	QT_CheckI (int64_t got, int64_t want, const char *what, const char *file, int line);
bool	QT_CheckF (double got, double want, const char *what, const char *file, int line);	// exactly
bool	QT_CheckS (const char *got, const char *want, const char *what, const char *file, int line);
#define QT_EQ_U(got, want)	QT_CheckU ((uint64_t)(got), (uint64_t)(want), #got, __FILE__, __LINE__)
#define QT_EQ_I(got, want)	QT_CheckI ((int64_t)(got), (int64_t)(want), #got, __FILE__, __LINE__)
#define QT_EQ_F(got, want)	QT_CheckF ((double)(got), (double)(want), #got, __FILE__, __LINE__)
#define QT_EQ_S(got, want)	QT_CheckS ((got), (want), #got, __FILE__, __LINE__)

// prints the summary and returns main's result
int		QT_Finish (const char *name, const char *passed);

// SplitMix64
uint64_t	QT_Rand (uint64_t *state);
uint32_t	QT_RandBelow (uint64_t *state, uint32_t n);		// 0 if n is 0

// a file's bytes (malloc'd), or NULL
uint8_t	*QT_LoadFile (const char *path, size_t *size);

// an unsigned number from the environment, or def
uint64_t	QT_EnvNumber (const char *name, uint64_t def);

// a growable text buffer
typedef struct
{
	char	*text;
	size_t	len, size;
} qt_text_t;

void	QT_TextAppend (qt_text_t *t, const char *s);
void	QT_TextFree (qt_text_t *t);
bool	QT_Contains (const char *haystack, const char *needle);

// isnan and signbit as functions: MSVC's macros put their argument in sizeof,
// where a call's compound literal (ARGS) is a temporary it warns is unused
bool	QT_IsNan (double x);
bool	QT_SignBit (double x);
