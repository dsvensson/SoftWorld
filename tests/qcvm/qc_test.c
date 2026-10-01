// qc_test.c -- what the QuakeC VM's tests share

#include "qc_test.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int	qt_failures;

bool QT_Check (bool ok, const char *what, const char *file, int line)
{
	if (!ok)
	{
		printf ("FAIL %s:%d: %s\n", file, line, what);
		qt_failures++;
	}
	return ok;
}

bool QT_CheckU (uint64_t got, uint64_t want, const char *what, const char *file, int line)
{
	if (got != want)
	{
		printf ("FAIL %s:%d: %s is %llu (0x%llx), want %llu (0x%llx)\n", file, line, what,
			(unsigned long long)got, (unsigned long long)got, (unsigned long long)want, (unsigned long long)want);
		qt_failures++;
	}
	return got == want;
}

bool QT_CheckI (int64_t got, int64_t want, const char *what, const char *file, int line)
{
	if (got != want)
	{
		printf ("FAIL %s:%d: %s is %lld, want %lld\n", file, line, what, (long long)got, (long long)want);
		qt_failures++;
	}
	return got == want;
}

bool QT_CheckF (double got, double want, const char *what, const char *file, int line)
{
	bool	ok = got == want || (got != got && want != want);

	if (!ok)
	{
		printf ("FAIL %s:%d: %s is %.17g, want %.17g\n", file, line, what, got, want);
		qt_failures++;
	}
	return ok;
}

bool QT_CheckS (const char *got, const char *want, const char *what, const char *file, int line)
{
	bool	ok = got && want ? !strcmp (got, want) : got == want;

	if (!ok)
	{
		printf ("FAIL %s:%d: %s is \"%s\", want \"%s\"\n", file, line, what, got ? got : "(null)",
			want ? want : "(null)");
		qt_failures++;
	}
	return ok;
}

int QT_Finish (const char *name, const char *passed)
{
	if (qt_failures)
	{
		printf ("%s: %d failures\n", name, qt_failures);
		return 1;
	}
	printf ("%s: %s\n", name, passed);
	return 0;
}

uint64_t QT_Rand (uint64_t *state)
{
	uint64_t	z = (*state += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

uint32_t QT_RandBelow (uint64_t *state, uint32_t n)
{
	return n ? (uint32_t)(QT_Rand (state) % n) : 0;
}

uint8_t *QT_LoadFile (const char *path, size_t *size)
{
	FILE	*f = fopen (path, "rb");
	uint8_t	*data;
	long	len;

	if (!f)
		return NULL;
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	data = len >= 0 ? malloc ((size_t)len + 1) : NULL;
	if (data && fread (data, 1, (size_t)len, f) != (size_t)len)
	{
		free (data);
		data = NULL;
	}
	fclose (f);
	if (data)
	{
		data[len] = 0;
		*size = (size_t)len;
	}
	return data;
}

uint64_t QT_EnvNumber (const char *name, uint64_t def)
{
	const char	*v = getenv (name);

	return v && *v ? strtoull (v, NULL, 0) : def;
}

void QT_TextAppend (qt_text_t *t, const char *s)
{
	size_t	n = strlen (s);

	if (t->len + n + 1 > t->size)
	{
		t->size = (t->len + n + 1) * 2;
		t->text = realloc (t->text, t->size);
		if (!t->text)
		{
			printf ("out of memory\n");
			exit (1);
		}
	}
	memcpy (t->text + t->len, s, n + 1);
	t->len += n;
}

void QT_TextFree (qt_text_t *t)
{
	free (t->text);
	*t = (qt_text_t){0};
}

bool QT_Contains (const char *haystack, const char *needle)
{
	return haystack && strstr (haystack, needle) != NULL;
}

bool QT_IsNan (double x)
{
	return isnan (x);
}

bool QT_SignBit (double x)
{
	return signbit (x);
}

// what the platform's memory functions need of the program
void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}
