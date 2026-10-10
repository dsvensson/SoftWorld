// qc_asm.c -- a small assembler that writes progs in every format the VM loads

#include "qc_asm.h"
#include "qc_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	int32_t		first_statement;
	uint32_t	parm_start;
	uint32_t	locals;
	uint32_t	name;
	uint32_t	file;
	int32_t		num_parms;
	uint8_t		parm_sizes[8];
} qa_function_t;

typedef struct
{
	uint32_t	type, ofs, name;
} qa_def_t;

typedef struct
{
	uint32_t	op, a, b, c;
} qa_statement_t;

struct qc_asm_s
{
	char			*strings;
	size_t			numstrings, stringsize;
	uint32_t		*interned;			// offsets of the strings QA_String made
	size_t			numinterned, internedsize;
	uint32_t		*globals;
	size_t			numglobals, globalssize;
	qa_def_t		*globaldefs;
	size_t			numglobaldefs, globaldefssize;
	qa_def_t		*fielddefs;
	size_t			numfielddefs, fielddefssize;
	qa_statement_t	*statements;
	size_t			numstatements, statementssize;
	qa_function_t	*functions;
	size_t			numfunctions, functionssize;
	uint32_t		entityfields;
	char			*bodyless;			// NUL-separated
	size_t			bodylesslen, bodylesssize;
	uint32_t		numbodyless;
	uint32_t		overrides[23];
	bool			overridden[23];
	uint32_t		kk7magic;
};

// room for one more element
static void *QA_Grow (void *p, size_t *size, size_t count, size_t elem, size_t more)
{
	if (count + more <= *size)
		return p;
	*size = (count + more) * 2;
	p = realloc (p, *size * elem);
	if (!p)
	{
		printf ("qc_asm: out of memory\n");
		exit (1);
	}
	return p;
}

#define QA_PUSH(a, field, item) \
	do { \
		(a)->field = QA_Grow ((a)->field, &(a)->field##size, (a)->num##field, sizeof(*(a)->field), 1); \
		(a)->field[(a)->num##field++] = (item); \
	} while (0)

static void QA_PushBytes (qc_asm_t *a, const void *bytes, size_t n)
{
	a->strings = QA_Grow (a->strings, &a->stringsize, a->numstrings, 1, n);
	memcpy (a->strings + a->numstrings, bytes, n);
	a->numstrings += n;
}

qc_asm_t *QA_New (void)
{
	qc_asm_t	*a = calloc (1, sizeof(*a));
	uint32_t	i;

	if (!a)
		exit (1);
	a->kk7magic = 0x57514B4Bu;		// "KKQW"
	// as fteqcc: offset 0 is the null string, offset 1 a non-null empty one
	QA_PushBytes (a, "\0", 2);
	QA_PUSH (a, interned, 1);
	for (i = 0 ; i < 28 ; i++)
		QA_PUSH (a, globals, 0);
	// function 0 is the null function; statement 0 the DONE it points at
	QA_PUSH (a, functions, ((qa_function_t){0}));
	QA_PUSH (a, statements, ((qa_statement_t){QOP_DONE, 0, 0, 0}));
	return a;
}

void QA_Free (qc_asm_t *a)
{
	if (!a)
		return;
	free (a->strings);
	free (a->interned);
	free (a->globals);
	free (a->globaldefs);
	free (a->fielddefs);
	free (a->statements);
	free (a->functions);
	free (a->bodyless);
	free (a);
}

uint32_t QA_String (qc_asm_t *a, const char *s)
{
	uint32_t	ofs;
	size_t		i;

	for (i = 0 ; i < a->numinterned ; i++)
		if (!strcmp (a->strings + a->interned[i], s))
			return a->interned[i];
	ofs = (uint32_t)a->numstrings;
	QA_PushBytes (a, s, strlen (s) + 1);
	QA_PUSH (a, interned, ofs);
	return ofs;
}

uint32_t QA_Alloc (qc_asm_t *a, uint32_t words, const uint32_t *init, uint32_t count)
{
	uint32_t	ofs = (uint32_t)a->numglobals, i;

	for (i = 0 ; i < words ; i++)
		QA_PUSH (a, globals, i < count ? init[i] : 0);
	return ofs;
}

uint32_t QA_Global (qc_asm_t *a, const char *name, uint32_t type, const uint32_t *init, uint32_t count)
{
	uint32_t	words = type == QC_EV_VECTOR ? 3 : type == QC_EV_VOID ? 0 : 1;
	uint32_t	ofs;

	if (count > words)
		words = count;
	ofs = QA_Alloc (a, words, init, count);
	QA_DefGlobal (a, name, type, ofs);
	return ofs;
}

void QA_DefGlobal (qc_asm_t *a, const char *name, uint32_t type, uint32_t ofs)
{
	QA_DefGlobalAt (a, QA_String (a, name), type, ofs);
}

void QA_DefGlobalAt (qc_asm_t *a, uint32_t name, uint32_t type, uint32_t ofs)
{
	QA_PUSH (a, globaldefs, ((qa_def_t){type, ofs, name}));
}

uint32_t QA_Float (qc_asm_t *a, float v)
{
	uint32_t	u;

	memcpy (&u, &v, 4);
	return QA_Alloc (a, 1, &u, 1);
}

uint32_t QA_Int (qc_asm_t *a, int32_t v)
{
	uint32_t	u = (uint32_t)v;

	return QA_Alloc (a, 1, &u, 1);
}

uint32_t QA_Vector (qc_asm_t *a, float x, float y, float z)
{
	uint32_t	w[3];

	memcpy (&w[0], &x, 4);
	memcpy (&w[1], &y, 4);
	memcpy (&w[2], &z, 4);
	return QA_Alloc (a, 3, w, 3);
}

uint32_t QA_VectorRaw (qc_asm_t *a, uint32_t x, uint32_t y, uint32_t z)
{
	uint32_t	w[3] = {x, y, z};

	return QA_Alloc (a, 3, w, 3);
}

uint32_t QA_StrConst (qc_asm_t *a, const char *s)
{
	uint32_t	ofs = QA_String (a, s);

	return QA_Alloc (a, 1, &ofs, 1);
}

void QA_SetGlobal (qc_asm_t *a, uint32_t word, uint32_t value)
{
	a->globals[word] = value;
}

uint32_t QA_Temp (qc_asm_t *a, uint32_t words)
{
	return QA_Alloc (a, words, NULL, 0);
}

void QA_NullField (qc_asm_t *a)
{
	QA_PUSH (a, fielddefs, ((qa_def_t){QC_EV_VOID, a->entityfields, 0}));
}

uint32_t QA_Field (qc_asm_t *a, const char *name, uint32_t type, uint32_t *global)
{
	static const char	*suffixes[3] = {"_x", "_y", "_z"};
	uint32_t			words = type == QC_EV_VECTOR ? 3 : 1;
	uint32_t			ofs = a->entityfields, g, i;
	char				sub[256];

	a->entityfields += words;
	QA_PUSH (a, fielddefs, ((qa_def_t){type, ofs, QA_String (a, name)}));
	if (type == QC_EV_VECTOR)
	{
		for (i = 0 ; i < 3 ; i++)
		{
			snprintf (sub, sizeof(sub), "%s%s", name, suffixes[i]);
			QA_PUSH (a, fielddefs, ((qa_def_t){QC_EV_FLOAT, ofs + i, QA_String (a, sub)}));
		}
	}
	g = QA_Alloc (a, 1, &ofs, 1);
	QA_DefGlobal (a, name, QC_EV_FIELD, g);
	if (global)
		*global = g;
	return ofs;
}

uint32_t QA_Builtin (qc_asm_t *a, const char *name, uint32_t number, int32_t num_parms)
{
	uint32_t	index = (uint32_t)a->numfunctions;
	uint32_t	n = QA_String (a, name);
	uint32_t	g;

	QA_PUSH (a, functions, ((qa_function_t){.first_statement = -(int32_t)number, .name = n,
		.num_parms = num_parms}));
	g = QA_Alloc (a, 1, &index, 1);
	QA_DefGlobalAt (a, n, QC_EV_FUNCTION, g);
	return index;
}

qa_func_t QA_Function (qc_asm_t *a, const char *name, const uint8_t *parm_sizes, int num_parms,
	uint32_t extra_locals)
{
	uint32_t		index = (uint32_t)a->numfunctions;
	uint32_t		params = 0, locals, parm_start, n, file, g;
	qa_function_t	f = {0};
	int				i;

	for (i = 0 ; i < num_parms ; i++)
		params += parm_sizes[i];
	locals = params + extra_locals;
	parm_start = QA_Alloc (a, locals, NULL, 0);
	n = QA_String (a, name);
	file = QA_String (a, "test.qc");
	f.first_statement = (int32_t)a->numstatements;
	f.parm_start = parm_start;
	f.locals = locals;
	f.name = n;
	f.file = file;
	f.num_parms = num_parms;
	for (i = 0 ; i < num_parms && i < 8 ; i++)
		f.parm_sizes[i] = parm_sizes[i];
	QA_PUSH (a, functions, f);
	g = QA_Alloc (a, 1, &index, 1);
	QA_DefGlobalAt (a, n, QC_EV_FUNCTION, g);
	return (qa_func_t){index, parm_start};
}

void QA_PatchFunction (qc_asm_t *a, uint32_t index, int32_t first_statement, uint32_t parm_start, uint32_t locals)
{
	a->functions[index].first_statement = first_statement;
	a->functions[index].parm_start = parm_start;
	a->functions[index].locals = locals;
}

uint32_t QA_Emit (qc_asm_t *a, uint32_t op, uint32_t x, uint32_t y, uint32_t z)
{
	QA_PUSH (a, statements, ((qa_statement_t){op, x, y, z}));
	return (uint32_t)a->numstatements - 1;
}

uint32_t QA_Here (const qc_asm_t *a)
{
	return (uint32_t)a->numstatements;
}

void QA_Patch (qc_asm_t *a, uint32_t stmt, int which, uint32_t value)
{
	qa_statement_t	*s = &a->statements[stmt];

	if (which == 0)
		s->a = value;
	else if (which == 1)
		s->b = value;
	else
		s->c = value;
}

void QA_PatchJump (qc_asm_t *a, uint32_t stmt, int which, uint32_t target)
{
	QA_Patch (a, stmt, which, QA_Rel (stmt, target));
}

void QA_Bodyless (qc_asm_t *a, const char *name)
{
	size_t	n = strlen (name) + 1;

	a->bodyless = QA_Grow (a->bodyless, &a->bodylesssize, a->bodylesslen, 1, n);
	memcpy (a->bodyless + a->bodylesslen, name, n);
	a->bodylesslen += n;
	a->numbodyless++;
}

uint32_t QA_NumGlobals (const qc_asm_t *a)
{
	return (uint32_t)a->numglobals;
}

void QA_HeaderOverride (qc_asm_t *a, int i, uint32_t value)
{
	a->overrides[i] = value;
	a->overridden[i] = true;
}

void QA_SetKK7Magic (qc_asm_t *a, uint32_t magic)
{
	a->kk7magic = magic;
}

/*
==============================================================================

OUTPUT

==============================================================================
*/

typedef struct
{
	uint8_t	*data;
	size_t	len, size;
} qa_out_t;

static uint32_t QA_Put (qa_out_t *o, const void *bytes, size_t n)
{
	uint32_t	ofs = (uint32_t)o->len;

	o->data = QA_Grow (o->data, &o->size, o->len, 1, n);
	if (n)
		memcpy (o->data + o->len, bytes, n);
	o->len += n;
	return ofs;
}

static void QA_Put16 (qa_out_t *o, uint32_t v)
{
	uint8_t	b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};

	QA_Put (o, b, 2);
}

static void QA_Put32 (qa_out_t *o, uint32_t v)
{
	uint8_t	b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};

	QA_Put (o, b, 4);
}

static uint32_t QA_PutDefs (qa_out_t *o, const qa_def_t *defs, size_t count, qc_format_t format)
{
	uint32_t	ofs = (uint32_t)o->len;
	size_t		i;

	for (i = 0 ; i < count ; i++)
	{
		const qa_def_t	*d = &defs[i];

		switch (format)
		{
		case QC_FORMAT_QTEST:
			QA_Put32 (o, d->type);
			QA_Put32 (o, d->name);
			QA_Put32 (o, d->ofs);
			break;
		case QC_FORMAT_V6:
		case QC_FORMAT_FTE16:
		case QC_FORMAT_KK7:
		case QC_FORMAT_QF:
			QA_Put16 (o, d->type);
			QA_Put16 (o, d->ofs);
			QA_Put32 (o, d->name);
			break;
		case QC_FORMAT_FTE32:
			QA_Put32 (o, d->type);
			QA_Put32 (o, d->ofs);
			QA_Put32 (o, d->name);
			break;
		case QC_FORMAT_UHEXEN2:
			QA_Put32 (o, d->type << 16);
			QA_Put32 (o, d->ofs);
			QA_Put32 (o, d->name);
			break;
		}
	}
	return ofs;
}

uint8_t *QA_Build (const qc_asm_t *a, qc_format_t format, size_t *size)
{
	bool		v7 = format == QC_FORMAT_FTE16 || format == QC_FORMAT_FTE32 || format == QC_FORMAT_KK7
		|| format == QC_FORMAT_UHEXEN2;
	qa_out_t	o = {0};
	uint32_t	header[23], ofs_strings, ofs_statements, ofs_globaldefs, ofs_fielddefs, ofs_functions;
	uint32_t	ofs_globals, ofs_bodyless = 0, magic;
	size_t		i, words = v7 ? 23 : 15;
	int			k;
	static const uint8_t	zero[92] = {0};

	QA_Put (&o, zero, words * 4);

	ofs_strings = QA_Put (&o, a->strings, a->numstrings);
	while (o.len % 4)
		QA_Put (&o, zero, 1);

	ofs_statements = (uint32_t)o.len;
	for (i = 0 ; i < a->numstatements ; i++)
	{
		const qa_statement_t	*s = &a->statements[i];

		switch (format)
		{
		case QC_FORMAT_QTEST:
			QA_Put32 (&o, (uint32_t)i + 1);
			[[fallthrough]];
		case QC_FORMAT_V6:
		case QC_FORMAT_FTE16:
		case QC_FORMAT_QF:
			QA_Put16 (&o, s->op);
			QA_Put16 (&o, s->a);
			QA_Put16 (&o, s->b);
			QA_Put16 (&o, s->c);
			break;
		case QC_FORMAT_FTE32:
		case QC_FORMAT_KK7:
			// 16-bit relative jumps were sign-extended into 32 bits by the caller
			QA_Put32 (&o, s->op);
			QA_Put32 (&o, s->a);
			QA_Put32 (&o, s->b);
			QA_Put32 (&o, s->c);
			break;
		case QC_FORMAT_UHEXEN2:
			QA_Put32 (&o, s->op << 16);
			QA_Put32 (&o, s->a);
			QA_Put32 (&o, s->b);
			QA_Put32 (&o, s->c);
			break;
		}
	}

	ofs_globaldefs = QA_PutDefs (&o, a->globaldefs, a->numglobaldefs, format);
	ofs_fielddefs = QA_PutDefs (&o, a->fielddefs, a->numfielddefs, format);

	ofs_functions = (uint32_t)o.len;
	for (i = 0 ; i < a->numfunctions ; i++)
	{
		const qa_function_t	*f = &a->functions[i];

		if (format == QC_FORMAT_QTEST)
		{
			QA_Put32 (&o, (uint32_t)f->first_statement);
			QA_Put32 (&o, 0);
			QA_Put32 (&o, f->locals);
			QA_Put32 (&o, 0);
			QA_Put32 (&o, f->name);
			QA_Put32 (&o, f->file);
			QA_Put32 (&o, (uint32_t)f->num_parms);
			QA_Put32 (&o, f->parm_start);
			for (k = 0 ; k < 8 ; k++)
				QA_Put32 (&o, f->parm_sizes[k]);
		}
		else
		{
			QA_Put32 (&o, (uint32_t)f->first_statement);
			QA_Put32 (&o, f->parm_start);
			QA_Put32 (&o, f->locals);
			QA_Put32 (&o, 0);
			QA_Put32 (&o, f->name);
			QA_Put32 (&o, f->file);
			QA_Put32 (&o, (uint32_t)f->num_parms);
			QA_Put (&o, f->parm_sizes, 8);
		}
	}

	ofs_globals = (uint32_t)o.len;
	for (i = 0 ; i < a->numglobals ; i++)
		QA_Put32 (&o, a->globals[i]);

	if (a->numbodyless)
		ofs_bodyless = QA_Put (&o, a->bodyless, a->bodylesslen);

	header[0] = format == QC_FORMAT_QTEST ? 3 : format == QC_FORMAT_V6 ? 6 : format == QC_FORMAT_QF ? 0x00fff002 : 7;
	header[1] = 0x1234;
	header[2] = ofs_statements;
	header[3] = (uint32_t)a->numstatements;
	header[4] = ofs_globaldefs;
	header[5] = (uint32_t)a->numglobaldefs;
	header[6] = ofs_fielddefs;
	header[7] = (uint32_t)a->numfielddefs;
	header[8] = ofs_functions;
	header[9] = (uint32_t)a->numfunctions;
	header[10] = ofs_strings;
	header[11] = (uint32_t)a->numstrings;
	header[12] = ofs_globals;
	header[13] = (uint32_t)a->numglobals;
	header[14] = a->entityfields;
	if (v7)
	{
		switch (format)
		{
		case QC_FORMAT_FTE16:	magic = 0x021B1461u; break;
		case QC_FORMAT_FTE32:	magic = 0x65167402u; break;
		case QC_FORMAT_UHEXEN2:	magic = 0x37324855u; break;		// "UH27"
		default:				magic = a->kk7magic; break;
		}
		header[15] = 0;
		header[16] = 0;
		header[17] = ofs_bodyless;
		header[18] = a->numbodyless;
		header[19] = 0;
		header[20] = 0;
		header[21] = 0;
		header[22] = magic;
	}
	for (i = 0 ; i < words ; i++)
	{
		uint32_t	v = a->overridden[i] ? a->overrides[i] : header[i];

		o.data[i * 4] = (uint8_t)v;
		o.data[i * 4 + 1] = (uint8_t)(v >> 8);
		o.data[i * 4 + 2] = (uint8_t)(v >> 16);
		o.data[i * 4 + 3] = (uint8_t)(v >> 24);
	}
	*size = o.len;
	return o.data;
}

qc_progs_t *QA_Load (const qc_asm_t *a, qc_format_t format)
{
	qc_loaderror_t	error;
	qc_progs_t		*progs;
	uint8_t			*data;
	size_t			size;
	char			text[256];

	data = QA_Build (a, format, &size);
	progs = QC_LoadProgs (data, size, &error);
	free (data);
	if (!progs)
	{
		printf ("QA_Load: %s\n", QC_LoadErrorText (&error, text, sizeof(text)));
		exit (1);
	}
	return progs;
}

qcvm_t *QA_CreateVM (const qc_asm_t *a, const qc_config_t *config, const qc_builtins_t *builtins,
	const qc_host_t *host, void *ctx)
{
	return QA_CreateVMAs (a, QC_FORMAT_FTE16, config, builtins, host, ctx);
}

qcvm_t *QA_CreateVMAs (const qc_asm_t *a, qc_format_t format, const qc_config_t *config,
	const qc_builtins_t *builtins, const qc_host_t *host, void *ctx)
{
	qc_progs_t	*progs = QA_Load (a, format);
	qc_error_t	error;
	qcvm_t		*vm;
	char		text[256];

	vm = QC_Create (progs, builtins, config, host, ctx, &error);
	QC_ReleaseProgs (progs);
	if (!vm)
	{
		printf ("QA_CreateVM: %s\n", QC_ErrorText (&error, text, sizeof(text)));
		QC_FreeError (&error);
		exit (1);
	}
	return vm;
}
