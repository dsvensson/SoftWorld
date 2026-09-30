// qc_progs.c -- loading compiled QuakeC programs
//
// Every format FTE accepts is converted into one canonical, validated form.
// All counts and offsets are range-checked, so any byte string either loads or
// yields an error: statements with an unknown opcode or an operand outside the
// globals are poisoned (they fault if run), jumps that leave the program land on
// a sentinel that faults, and malformed function records fault when called.

#include "qc_local.h"

#include <stdio.h>
#include <stdlib.h>

#define FTE16_MAGIC		0x021B1461u
#define FTE32_MAGIC		0x65167402u
#define UHEXEN2_MAGIC	0x37324855u		// "UH27"
#define KK7_MAGIC		0x57514B4Bu		// "KKQW"
#define LNO_MAGIC		0x464F4E4Cu		// "LNOF"

#define BREAKPOINT_BIT	0x8000u

// globals are followed by three zero words, which operands may name, as in FTE
#define GLOBALS_TAIL	3

// the most bytes the names of the functions and definitions may add up to:
// names are string table offsets and may overlap (fteqcc shares suffixes), so a
// small file could otherwise make the loader hash quadratically many bytes
#define NAME_BYTES_MAX	(16u << 20)

typedef struct
{
	uint32_t	version, crc;
	uint32_t	ofs_statements, num_statements;
	uint32_t	ofs_globaldefs, num_globaldefs;
	uint32_t	ofs_fielddefs, num_fielddefs;
	uint32_t	ofs_functions, num_functions;
	uint32_t	ofs_strings, len_strings;
	uint32_t	ofs_globals, num_globals;
	uint32_t	entity_fields;
} header_t;

// the version-7 fields the loader uses
typedef struct
{
	bool		present;
	uint32_t	ofs_linenums;
	uint32_t	ofs_bodylessfuncs, num_bodylessfuncs;
	uint32_t	num_types;
	uint32_t	blockscompressed;
} extheader_t;

typedef struct
{
	uint32_t	op, a, b, c;
	uint32_t	line;		// QTest keeps a source line with each statement
} rawstatement_t;

typedef struct
{
	int32_t		first_statement;
	uint32_t	parm_start;
	uint32_t	locals;
	uint32_t	name;
	uint32_t	file;
	int32_t		num_parms;
	uint8_t		parm_sizes[8];
} rawfunction_t;

// what a load builds up, freed on failure
typedef struct
{
	const uint8_t	*data;
	size_t			size;
	qc_progs_t		*progs;
	uint32_t		notesize;
	uint32_t		copysize;
	qc_loaderror_t	*error;
} loader_t;

static bool QC_LoadFail (loader_t *l, qc_loaderr_t kind, uint32_t value, const char *what)
{
	if (l->error)
	{
		l->error->kind = kind;
		l->error->value = value;
		l->error->what = what;
	}
	return false;
}

static bool QC_Note (loader_t *l, qc_notekind_t kind, uint32_t index, uint32_t value)
{
	qc_progs_t		*p = l->progs;
	qc_loadnote_t	*grown;

	if (p->numnotes == l->notesize)
	{
		l->notesize = l->notesize ? l->notesize * 2 : 16;
		grown = realloc (p->notes, l->notesize * sizeof(*grown));
		if (!grown)
			return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
		p->notes = grown;
	}
	p->notes[p->numnotes++] = (qc_loadnote_t){kind, index, value};
	return true;
}

/*
==============================================================================

HEADERS AND RECORDS

==============================================================================
*/

static bool QC_ReadHeader (loader_t *l, header_t *h, extheader_t *ext)
{
	const uint8_t	*d = l->data;
	uint32_t		w[23];
	int				i, words = 15;

	if (l->size < 60)
		return QC_LoadFail (l, QC_LOAD_TRUNCATED, 0, NULL);
	w[0] = QC_LE32 (d);
	if (w[0] == 7)
	{
		if (l->size < 92)
			return QC_LoadFail (l, QC_LOAD_TRUNCATED, 0, NULL);
		words = 23;
	}
	for (i = 0 ; i < words ; i++)
		w[i] = QC_LE32 (d + i * 4);

	*h = (header_t){w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
		w[12], w[13], w[14]};
	*ext = (extheader_t){0};

	switch (h->version)
	{
	case 3:
		l->progs->format = QC_FORMAT_QTEST;
		return true;
	case 6:
		l->progs->format = QC_FORMAT_V6;
		return true;
	case 7:
		switch (w[22])
		{
		case FTE16_MAGIC:	l->progs->format = QC_FORMAT_FTE16; break;
		case FTE32_MAGIC:	l->progs->format = QC_FORMAT_FTE32; break;
		case UHEXEN2_MAGIC:	l->progs->format = QC_FORMAT_UHEXEN2; break;
		case KK7_MAGIC:		l->progs->format = QC_FORMAT_KK7; break;
		default:
			l->progs->format = QC_FORMAT_KK7;
			if (!QC_Note (l, QC_NOTE_ASSUMED_KK7, 0, w[22]))
				return false;
			break;
		}
		// KK7 and uHexen2 are version 6 with wider records: FTE ignores their
		// extension fields
		if (l->progs->format != QC_FORMAT_FTE16 && l->progs->format != QC_FORMAT_FTE32)
			return true;
		if (w[21])
			return QC_LoadFail (l, QC_LOAD_COMPRESSED, w[21], NULL);
		*ext = (extheader_t){.present = true, .ofs_linenums = w[16], .ofs_bodylessfuncs = w[17],
			.num_bodylessfuncs = w[18], .num_types = w[20], .blockscompressed = w[21]};
		return true;
	default:
		return QC_LoadFail (l, QC_LOAD_UNSUPPORTED_VERSION, h->version, NULL);
	}
}

typedef struct
{
	size_t	statement, def, function;
} layout_t;

static layout_t QC_Layout (qc_format_t format)
{
	switch (format)
	{
	case QC_FORMAT_QTEST:
		return (layout_t){12, 12, 64};
	case QC_FORMAT_V6:
	case QC_FORMAT_FTE16:
		return (layout_t){8, 8, 36};
	case QC_FORMAT_FTE32:
	case QC_FORMAT_UHEXEN2:
		return (layout_t){16, 12, 36};
	case QC_FORMAT_KK7:
	default:
		return (layout_t){16, 8, 36};
	}
}

static bool QC_Has16BitStatements (qc_format_t format)
{
	return format == QC_FORMAT_QTEST || format == QC_FORMAT_V6 || format == QC_FORMAT_FTE16;
}

// the bytes of count records of size bytes at ofs, or NULL (with the error set)
// if they lie outside the file; an empty section is fine anywhere
static const uint8_t *QC_Section (loader_t *l, uint32_t ofs, uint32_t count, size_t size, const char *name)
{
	static const uint8_t	empty[1];
	uint64_t				len, end;

	if (!count)
		return empty;
	len = (uint64_t)count * size;
	end = (uint64_t)ofs + len;
	if (end > l->size)
	{
		QC_LoadFail (l, QC_LOAD_SECTION_OUT_OF_BOUNDS, 0, name);
		return NULL;
	}
	return l->data + ofs;
}

static rawstatement_t QC_DecodeStatement (qc_format_t format, const uint8_t *r)
{
	switch (format)
	{
	case QC_FORMAT_QTEST:
		return (rawstatement_t){QC_LE16 (r + 4), QC_LE16 (r + 6), QC_LE16 (r + 8), QC_LE16 (r + 10), QC_LE32 (r)};
	case QC_FORMAT_V6:
	case QC_FORMAT_FTE16:
		return (rawstatement_t){QC_LE16 (r), QC_LE16 (r + 2), QC_LE16 (r + 4), QC_LE16 (r + 6), 0};
	case QC_FORMAT_UHEXEN2:
		return (rawstatement_t){QC_LE32 (r) >> 16, QC_LE32 (r + 4), QC_LE32 (r + 8), QC_LE32 (r + 12), 0};
	case QC_FORMAT_FTE32:
	case QC_FORMAT_KK7:
	default:
		return (rawstatement_t){QC_LE32 (r), QC_LE32 (r + 4), QC_LE32 (r + 8), QC_LE32 (r + 12), 0};
	}
}

static qc_def_t QC_DecodeDef (qc_format_t format, const uint8_t *r)
{
	uint32_t	type, ofs, name;

	switch (format)
	{
	case QC_FORMAT_QTEST:
		type = QC_LE32 (r); name = QC_LE32 (r + 4); ofs = QC_LE32 (r + 8);
		break;
	case QC_FORMAT_V6:
	case QC_FORMAT_FTE16:
	case QC_FORMAT_KK7:
		type = QC_LE16 (r); ofs = QC_LE16 (r + 2); name = QC_LE32 (r + 4);
		break;
	case QC_FORMAT_UHEXEN2:
		type = QC_LE32 (r) >> 16; ofs = QC_LE32 (r + 4); name = QC_LE32 (r + 8);
		break;
	case QC_FORMAT_FTE32:
	default:
		type = QC_LE32 (r); ofs = QC_LE32 (r + 4); name = QC_LE32 (r + 8);
		break;
	}
	return (qc_def_t){.type = type & ~0xC000u, .ofs = ofs, .name = name,
		.save = (type & 0x8000) != 0, .shared = (type & 0x4000) != 0};
}

static rawfunction_t QC_DecodeFunction (qc_format_t format, const uint8_t *r)
{
	rawfunction_t	f;
	uint32_t		size;
	int				i;

	f.first_statement = (int32_t)QC_LE32 (r);
	f.locals = QC_LE32 (r + 8);
	f.name = QC_LE32 (r + 16);
	f.file = QC_LE32 (r + 20);
	f.num_parms = (int32_t)QC_LE32 (r + 24);
	if (format == QC_FORMAT_QTEST)
	{
		// first_statement, unused, locals, profile, name, file, numparms,
		// parm_start, then parm_size[8] as 32-bit ints
		f.parm_start = QC_LE32 (r + 28);
		for (i = 0 ; i < 8 ; i++)
		{
			size = QC_LE32 (r + 32 + i * 4);
			f.parm_sizes[i] = (uint8_t)(size > 255 ? 255 : size);
		}
	}
	else
	{
		f.parm_start = QC_LE32 (r + 4);
		memcpy (f.parm_sizes, r + 28, 8);
	}
	return f;
}

/*
==============================================================================

CANONICAL FORM

==============================================================================
*/

// FTE's rule: a program uses the Hexen 2 calling convention if any CALL1-CALL8
// passes something in operand b (uHexen2 progs always do)
static bool QC_UsesHexen2Calls (qc_format_t format, const rawstatement_t *raw, uint32_t count)
{
	uint32_t	i, op;

	if (format == QC_FORMAT_QTEST)
		return false;
	if (format == QC_FORMAT_UHEXEN2)
		return true;
	for (i = 0 ; i < count ; i++)
	{
		op = raw[i].op & ~BREAKPOINT_BIT;
		if (op >= QOP_CALL1 && op <= QOP_CALL8 && raw[i].b)
			return true;
	}
	return false;
}

// CALLn becomes CALLnH, and RAND* results default to the return slot
static void QC_RewriteHexen2Calls (rawstatement_t *raw, uint32_t count)
{
	uint32_t	i, op;

	for (i = 0 ; i < count ; i++)
	{
		op = raw[i].op & ~BREAKPOINT_BIT;
		if (op >= QOP_CALL1 && op <= QOP_CALL8)
			raw[i].op = (raw[i].op & BREAKPOINT_BIT) | (op + QOP_CALL1H - QOP_CALL1);
		if (op >= QOP_RAND0 && op <= QOP_RANDV2 && !raw[i].c)
			raw[i].c = 1;
	}
}

// decodes, sanitizes and relocates every statement, then adds the sentinel
static bool QC_CanonicalStatements (loader_t *l, const rawstatement_t *raw, uint32_t count, uint64_t limit)
{
	qc_progs_t		*p = l->progs;
	bool			wide = !QC_Has16BitStatements (p->format);
	uint32_t		i, k, number, value, out[3], in[3];
	int64_t			offset, target;
	bool			ok, jumpout;
	qc_op_t			op;

	for (i = 0 ; i < count ; i++)
	{
		number = raw[i].op & ~BREAKPOINT_BIT;
		op = number < QOP_NUMREAL ? (qc_op_t)number : QOP_BAD;
		ok = op != QOP_BAD;
		jumpout = false;
		in[0] = raw[i].a;
		in[1] = raw[i].b;
		in[2] = raw[i].c;
		for (k = 0 ; k < 3 ; k++)
		{
			value = in[k];
			out[k] = 0;
			switch (qc_opinfo[op].operands[k])
			{
			case QC_OPND_G:
			case QC_OPND_U:
				if (value < limit && value <= UINT32_MAX / 4)
					out[k] = value * 4;
				else
					ok = false;
				break;
			case QC_OPND_A:
				if (value < limit)
					out[k] = value;
				else
					ok = false;
				break;
			case QC_OPND_J:
				offset = wide ? (int64_t)(int32_t)value : (int64_t)(int16_t)(uint16_t)value;
				target = (int64_t)i + offset;
				if (target >= 0 && target < (int64_t)count)
					out[k] = (uint32_t)target;
				else
				{
					out[k] = count;
					jumpout = true;
				}
				break;
			case QC_OPND_I:
			default:
				out[k] = value;
				break;
			}
		}

		p->statements[i].flags = (raw[i].op & BREAKPOINT_BIT) ? QC_STMT_BREAKPOINT : 0;
		if (ok)
		{
			if (jumpout && !QC_Note (l, QC_NOTE_JUMP_OUT_OF_RANGE, i, 0))
				return false;
			p->statements[i].op = (uint16_t)op;
			p->statements[i].a = out[0];
			p->statements[i].b = out[1];
			p->statements[i].c = out[2];
		}
		else
		{
			if (!QC_Note (l, QC_NOTE_POISONED_STATEMENT, i, number))
				return false;
			p->statements[i].op = QOP_BAD;
			p->statements[i].a = p->statements[i].b = p->statements[i].c = 0;
		}
	}
	p->statements[count] = (qc_stmt_t){.op = QOP_JUMP_OUT_OF_RANGE};
	return true;
}

static bool QC_AddCopy (loader_t *l, uint32_t src, uint32_t dst)
{
	qc_progs_t		*p = l->progs;
	qc_paramcopy_t	*grown;

	if (p->numcopies == l->copysize)
	{
		l->copysize = l->copysize ? l->copysize * 2 : 64;
		grown = realloc (p->copies, l->copysize * sizeof(*grown));
		if (!grown)
			return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
		p->copies = grown;
	}
	p->copies[p->numcopies++] = (qc_paramcopy_t){src, dst};
	return true;
}

// Parameter i is copied word by word from its PARM slot (global 4 + 3i) into
// consecutive locals from parm_start; at most eight are passed this way. Returns
// 0 if a copy lies outside the globals, -1 when out of memory.
static int QC_PlanParamCopies (loader_t *l, const rawfunction_t *raw, uint64_t limit)
{
	uint64_t	dst = raw->parm_start, src;
	int32_t		numparms = raw->num_parms < 0 ? 0 : raw->num_parms > 8 ? 8 : raw->num_parms;
	int32_t		slot;
	uint32_t	word;

	if ((uint64_t)raw->parm_start + raw->locals > limit)
		return 0;
	for (slot = 0 ; slot < numparms ; slot++)
	{
		for (word = 0 ; word < raw->parm_sizes[slot] ; word++)
		{
			src = 4 + 3 * (uint64_t)slot + word;
			if (src >= limit || dst >= limit || src > UINT32_MAX / 4 || dst > UINT32_MAX / 4)
				return 0;
			if (!QC_AddCopy (l, (uint32_t)src * 4, (uint32_t)dst * 4))
				return -1;
			dst++;
		}
	}
	return 1;
}

// validates a function record and plans its parameter copies
static bool QC_CanonicalFunction (loader_t *l, uint32_t index, const rawfunction_t *raw, uint64_t limit)
{
	qc_progs_t		*p = l->progs;
	qc_function_t	*f = &p->functions[index];
	int				copies;

	*f = (qc_function_t){.parm_start = raw->parm_start, .locals = raw->locals,
		.num_parms = raw->num_parms, .copies_start = p->numcopies, .name = raw->name, .file = raw->file};
	memcpy (f->parm_sizes, raw->parm_sizes, 8);

	if (index == 0)
		f->kind = QC_FUNC_NULL;
	else if (raw->first_statement > 0)
	{
		f->entry = (uint32_t)raw->first_statement;
		if (f->entry >= p->numstatements)
		{
			f->kind = QC_FUNC_INVALID;
			f->invalid = QC_INVALID_ENTRY;
		}
		else if ((copies = QC_PlanParamCopies (l, raw, limit)) > 0)
			f->kind = QC_FUNC_QUAKEC;
		else if (copies < 0)
			return false;
		else
		{
			p->numcopies = f->copies_start;
			f->kind = QC_FUNC_INVALID;
			f->invalid = QC_INVALID_LOCALS;
		}
	}
	else if (raw->first_statement == 0)
		f->kind = QC_FUNC_NAMED_BUILTIN;
	else
	{
		f->kind = QC_FUNC_BUILTIN;
		f->number = 0u - (uint32_t)raw->first_statement;
	}
	f->copies_end = p->numcopies;

	if (f->kind == QC_FUNC_INVALID && !QC_Note (l, QC_NOTE_INVALID_FUNCTION, index, f->invalid))
		return false;
	return true;
}

// the length of the name at ofs, charging it against the name budget; -1 when
// the budget is spent (without scanning past it)
static int64_t QC_NameBytes (const qc_progs_t *p, uint32_t ofs, uint32_t *left)
{
	size_t		rest, window;
	const void	*nul;
	size_t		len;

	rest = ofs < p->numstrings ? p->numstrings - ofs : 0;
	window = rest < (size_t)*left + 1 ? rest : (size_t)*left + 1;
	nul = window ? memchr (p->strings + ofs, 0, window) : NULL;
	if (nul)
		len = (size_t)((const uint8_t *)nul - (p->strings + ofs));
	else if (window == rest)
		len = rest;
	else
		return -1;
	if (len > *left)
		return -1;
	*left -= (uint32_t)len;
	return (int64_t)len;
}

static bool QC_CheckNameBytes (loader_t *l)
{
	const qc_progs_t	*p = l->progs;
	uint32_t			left = NAME_BYTES_MAX, i;

	for (i = 0 ; i < p->numfunctions ; i++)
		if (QC_NameBytes (p, p->functions[i].name, &left) < 0)
			return QC_LoadFail (l, QC_LOAD_NAMES_TOO_LARGE, 0, NULL);
	for (i = 0 ; i < p->numglobaldefs ; i++)
		if (QC_NameBytes (p, p->globaldefs[i].name, &left) < 0)
			return QC_LoadFail (l, QC_LOAD_NAMES_TOO_LARGE, 0, NULL);
	for (i = 0 ; i < p->numfielddefs ; i++)
		if (QC_NameBytes (p, p->fielddefs[i].name, &left) < 0)
			return QC_LoadFail (l, QC_LOAD_NAMES_TOO_LARGE, 0, NULL);
	return true;
}

// maps a non-empty name to the index of its first occurrence
static bool QC_MapName (loader_t *l, qc_map_t *map, uint32_t name, uint32_t index)
{
	const char	*s = QC_Cstr (l->progs, name);

	if (!*s || QC_MapAdd (map, s, strlen (s), index))
		return true;
	return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
}

static int QC_CompareDefAtOfs (const void *a, const void *b)
{
	const qc_defatofs_t	*x = a, *y = b;

	if (x->ofs != y->ofs)
		return x->ofs < y->ofs ? -1 : 1;
	return x->def < y->def ? -1 : x->def > y->def;
}

// the first named global definition at each offset, sorted by offset
static bool QC_IndexNamesByOfs (loader_t *l)
{
	qc_progs_t	*p = l->progs;
	uint32_t	i, n = 0;

	if (!p->numglobaldefs)
		return true;
	p->names_by_ofs = malloc (p->numglobaldefs * sizeof(*p->names_by_ofs));
	if (!p->names_by_ofs)
		return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
	for (i = 0 ; i < p->numglobaldefs ; i++)
		if (*QC_Cstr (p, p->globaldefs[i].name))
			p->names_by_ofs[n++] = (qc_defatofs_t){p->globaldefs[i].ofs, i};
	qsort (p->names_by_ofs, n, sizeof(*p->names_by_ofs), QC_CompareDefAtOfs);

	// keep the first of each offset
	p->numnames_by_ofs = 0;
	for (i = 0 ; i < n ; i++)
		if (!p->numnames_by_ofs || p->names_by_ofs[p->numnames_by_ofs - 1].ofs != p->names_by_ofs[i].ofs)
			p->names_by_ofs[p->numnames_by_ofs++] = p->names_by_ofs[i];
	return true;
}

// the NUL-separated names of the bodyless-function section
static bool QC_ReadBodyless (loader_t *l, uint32_t ofs, uint32_t count)
{
	qc_progs_t		*p = l->progs;
	const uint8_t	*rest, *end, *nul;
	uint32_t		i, size = 0;
	char			**grown;
	size_t			len;

	if (ofs > l->size)
		return QC_Note (l, QC_NOTE_BODYLESS_IGNORED, 0, 0);
	rest = l->data + ofs;
	end = l->data + l->size;
	for (i = 0 ; i < count ; i++)
	{
		nul = rest < end ? memchr (rest, 0, (size_t)(end - rest)) : NULL;
		if (!nul)
			return QC_Note (l, QC_NOTE_BODYLESS_IGNORED, 0, 0);
		if (p->numbodyless == size)
		{
			size = size ? size * 2 : 16;
			grown = realloc (p->bodyless, size * sizeof(*grown));
			if (!grown)
				return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
			p->bodyless = grown;
		}
		len = (size_t)(nul - rest);
		p->bodyless[p->numbodyless] = malloc (len + 1);
		if (!p->bodyless[p->numbodyless])
			return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
		memcpy (p->bodyless[p->numbodyless], rest, len + 1);
		p->numbodyless++;
		rest = nul + 1;
	}
	return true;
}

static int64_t QC_DefOfs (const qc_progs_t *p, const char *name)
{
	const qc_def_t	*d = QC_GlobalDefRaw (p, name);

	return d ? (int64_t)d->ofs : -1;
}

static bool QC_Parse (loader_t *l)
{
	qc_progs_t		*p = l->progs;
	header_t		h;
	extheader_t		ext;
	layout_t		layout;
	const uint8_t	*stmts, *gdefs, *fdefs, *funcs, *strings, *globals, *lines;
	rawstatement_t	*raw;
	rawfunction_t	rawf;
	uint64_t		limit;
	uint32_t		i, n;
	bool			ok;

	if (!QC_ReadHeader (l, &h, &ext))
		return false;
	layout = QC_Layout (p->format);
	p->version = h.version;
	p->crc = h.crc;

	if (!(stmts = QC_Section (l, h.ofs_statements, h.num_statements, layout.statement, "statement"))
		|| !(gdefs = QC_Section (l, h.ofs_globaldefs, h.num_globaldefs, layout.def, "global definition"))
		|| !(fdefs = QC_Section (l, h.ofs_fielddefs, h.num_fielddefs, layout.def, "field definition"))
		|| !(funcs = QC_Section (l, h.ofs_functions, h.num_functions, layout.function, "function"))
		|| !(strings = QC_Section (l, h.ofs_strings, h.len_strings, 1, "string"))
		|| !(globals = QC_Section (l, h.ofs_globals, h.num_globals, 4, "globals")))
		return false;

	// every count is now bounded by the file size
	p->numstrings = h.len_strings;
	p->strings = malloc ((size_t)h.len_strings + 1);
	p->numglobals = h.num_globals;
	p->globals = malloc (((size_t)h.num_globals + 1) * 4);
	p->numstatements = h.num_statements;
	p->statements = malloc (((size_t)h.num_statements + 1) * sizeof(qc_stmt_t));
	p->numglobaldefs = h.num_globaldefs;
	p->globaldefs = malloc (((size_t)h.num_globaldefs + 1) * sizeof(qc_def_t));
	p->numfielddefs = h.num_fielddefs;
	p->fielddefs = malloc (((size_t)h.num_fielddefs + 1) * sizeof(qc_def_t));
	p->numfunctions = h.num_functions;
	p->functions = malloc (((size_t)h.num_functions + 1) * sizeof(qc_function_t));
	p->entityfields = h.entity_fields;
	raw = malloc (((size_t)h.num_statements + 1) * sizeof(*raw));
	if (!p->strings || !p->globals || !p->statements || !p->globaldefs || !p->fielddefs
		|| !p->functions || !raw)
	{
		free (raw);
		return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
	}

	memcpy (p->strings, strings, h.len_strings);
	p->strings[h.len_strings] = 0;
	for (i = 0 ; i < h.num_globals ; i++)
		p->globals[i] = QC_LE32 (globals + i * 4);
	limit = (uint64_t)h.num_globals + GLOBALS_TAIL;

	for (i = 0 ; i < h.num_statements ; i++)
		raw[i] = QC_DecodeStatement (p->format, stmts + i * layout.statement);
	ok = true;
	if (QC_UsesHexen2Calls (p->format, raw, h.num_statements))
	{
		ok = QC_Note (l, QC_NOTE_HEXEN2_CALLS, 0, 0);
		QC_RewriteHexen2Calls (raw, h.num_statements);
	}
	if (ok)
		ok = QC_CanonicalStatements (l, raw, h.num_statements, limit);
	if (ok && p->format == QC_FORMAT_QTEST && h.num_statements)
	{
		p->lines = malloc (h.num_statements * sizeof(*p->lines));
		if (p->lines)
			for (i = 0 ; i < h.num_statements ; i++)
				p->lines[i] = raw[i].line;
		else
			ok = QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
	}
	free (raw);
	if (!ok)
		return false;

	for (i = 0 ; i < h.num_globaldefs ; i++)
		p->globaldefs[i] = QC_DecodeDef (p->format, gdefs + i * layout.def);
	for (i = 0 ; i < h.num_fielddefs ; i++)
		p->fielddefs[i] = QC_DecodeDef (p->format, fdefs + i * layout.def);

	for (i = 0 ; i < h.num_functions ; i++)
	{
		rawf = QC_DecodeFunction (p->format, funcs + i * layout.function);
		if (!QC_CanonicalFunction (l, i, &rawf, limit))
			return false;
	}

	// everything below scans, hashes and copies names: bound that work first
	if (!QC_CheckNameBytes (l))
		return false;
	for (i = 1 ; i < p->numfunctions ; i++)
		if (!QC_MapName (l, &p->functions_by_name, p->functions[i].name, i))
			return false;
	for (i = 0 ; i < p->numglobaldefs ; i++)
		if (!QC_MapName (l, &p->globals_by_name, p->globaldefs[i].name, i))
			return false;
	for (i = 0 ; i < p->numfielddefs ; i++)
		if (!QC_MapName (l, &p->fields_by_name, p->fielddefs[i].name, i))
			return false;
	if (!QC_IndexNamesByOfs (l))
		return false;

	if (ext.present)
	{
		if (ext.ofs_linenums)
		{
			lines = QC_Section (l, ext.ofs_linenums, h.num_statements, 4, "line number");
			if (!lines)
			{
				if (l->error)
					*l->error = (qc_loaderror_t){0};
				if (!QC_Note (l, QC_NOTE_LINE_NUMBERS_IGNORED, 0, 0))
					return false;
			}
			else if (h.num_statements)
			{
				free (p->lines);
				p->lines = malloc (h.num_statements * sizeof(*p->lines));
				if (!p->lines)
					return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
				for (i = 0 ; i < h.num_statements ; i++)
					p->lines[i] = QC_LE32 (lines + i * 4);
			}
		}
		if (ext.num_bodylessfuncs && !QC_ReadBodyless (l, ext.ofs_bodylessfuncs, ext.num_bodylessfuncs))
			return false;
		if (ext.num_types && !QC_Note (l, QC_NOTE_TYPES_IGNORED, 0, 0))
			return false;
	}

	// pointer-typed globals with bit 31 set are relocations
	for (i = n = 0 ; i < p->numglobaldefs ; i++)
		if (p->globaldefs[i].type == QC_EV_POINTER && p->globaldefs[i].ofs < p->numglobals
			&& (p->globals[p->globaldefs[i].ofs] & 0x80000000u))
			n++;
	if (n)
	{
		p->pointer_relocs = malloc (n * sizeof(*p->pointer_relocs));
		if (!p->pointer_relocs)
			return QC_LoadFail (l, QC_LOAD_NO_MEMORY, 0, NULL);
		for (i = 0 ; i < p->numglobaldefs ; i++)
			if (p->globaldefs[i].type == QC_EV_POINTER && p->globaldefs[i].ofs < p->numglobals
				&& (p->globals[p->globaldefs[i].ofs] & 0x80000000u))
				p->pointer_relocs[p->numpointer_relocs++] = p->globaldefs[i].ofs;
	}

	p->thisprogs = QC_DefOfs (p, "thisprogs");
	p->fasttrackarrays = QC_DefOfs (p, "__ext__fasttrackarrays");
	return true;
}

static void QC_FreeProgs (qc_progs_t *p)
{
	uint32_t	i;

	if (!p)
		return;
	free (p->strings);
	free (p->globals);
	free (p->statements);
	free (p->functions);
	free (p->copies);
	free (p->globaldefs);
	free (p->fielddefs);
	QC_MapClear (&p->functions_by_name);
	QC_MapClear (&p->globals_by_name);
	QC_MapClear (&p->fields_by_name);
	free (p->names_by_ofs);
	for (i = 0 ; i < p->numbodyless ; i++)
		free (p->bodyless[i]);
	free (p->bodyless);
	free (p->pointer_relocs);
	free (p->lines);
	free (p->notes);
	free (p);
}

qc_progs_t *QC_LoadProgs (const void *data, size_t size, qc_loaderror_t *error)
{
	loader_t	l = {.data = data, .size = size, .error = error};

	if (error)
		*error = (qc_loaderror_t){0};
	l.progs = calloc (1, sizeof(*l.progs));
	if (!l.progs)
	{
		QC_LoadFail (&l, QC_LOAD_NO_MEMORY, 0, NULL);
		return NULL;
	}
	l.progs->refcount = 1;
	l.progs->thisprogs = l.progs->fasttrackarrays = -1;
	if (!QC_Parse (&l))
	{
		QC_FreeProgs (l.progs);
		return NULL;
	}
	return l.progs;
}

void QC_RetainProgs (qc_progs_t *progs)
{
	progs->refcount++;
}

void QC_ReleaseProgs (qc_progs_t *progs)
{
	if (progs && --progs->refcount <= 0)
		QC_FreeProgs (progs);
}

/*
==============================================================================

LINE NUMBERS

fteqcc's .lno files: "LNOF", version 1, the program's global definition,
global, field definition and statement counts, then a line per statement, all
little-endian 32-bit words.

==============================================================================
*/

static bool QC_LnoFail (qc_loaderror_t *error, const char *why)
{
	if (error)
		*error = (qc_loaderror_t){QC_LOAD_LINE_NUMBERS, 0, why};
	return false;
}

bool QC_AttachLineNumbers (qc_progs_t *progs, const void *lno, size_t size, qc_loaderror_t *error)
{
	const uint8_t	*d = lno;
	uint32_t		expect[4], *lines, i;

	if (error)
		*error = (qc_loaderror_t){0};
	if (size < 8)
		return QC_LnoFail (error, "truncated");
	if (QC_LE32 (d) != LNO_MAGIC)
		return QC_LnoFail (error, "not a .lno file");
	if (QC_LE32 (d + 4) != 1)
		return QC_LnoFail (error, "unsupported .lno version");
	expect[0] = progs->numglobaldefs;
	expect[1] = progs->numglobals;
	expect[2] = progs->numfielddefs;
	expect[3] = progs->numstatements;
	for (i = 0 ; i < 4 ; i++)
	{
		if (size < 12 + i * 4)
			return QC_LnoFail (error, "truncated");
		if (QC_LE32 (d + 8 + i * 4) != expect[i])
			return QC_LnoFail (error, "counts do not match the program");
	}
	if ((size - 24) / 4 < progs->numstatements)
		return QC_LnoFail (error, "truncated");
	lines = malloc (((size_t)progs->numstatements + 1) * sizeof(*lines));
	if (!lines)
	{
		if (error)
			*error = (qc_loaderror_t){QC_LOAD_NO_MEMORY, 0, NULL};
		return false;
	}
	for (i = 0 ; i < progs->numstatements ; i++)
		lines[i] = QC_LE32 (d + 24 + i * 4);
	free (progs->lines);
	progs->lines = lines;
	return true;
}

/*
==============================================================================

TEXT

==============================================================================
*/

const char *QC_LoadErrorText (const qc_loaderror_t *error, char *buf, size_t size)
{
	switch (error->kind)
	{
	case QC_LOAD_OK:
		snprintf (buf, size, "no error");
		break;
	case QC_LOAD_TRUNCATED:
		snprintf (buf, size, "progs header truncated");
		break;
	case QC_LOAD_UNSUPPORTED_VERSION:
		snprintf (buf, size, "unsupported progs version %u", error->value);
		break;
	case QC_LOAD_COMPRESSED:
		snprintf (buf, size, "progs uses compressed sections (0x%x)", error->value);
		break;
	case QC_LOAD_SECTION_OUT_OF_BOUNDS:
		snprintf (buf, size, "progs %s section lies outside the file", error->what);
		break;
	case QC_LOAD_LINE_NUMBERS:
		snprintf (buf, size, "line numbers rejected: %s", error->what);
		break;
	case QC_LOAD_NAMES_TOO_LARGE:
		snprintf (buf, size, "progs definition names are too large");
		break;
	case QC_LOAD_NO_MEMORY:
	default:
		snprintf (buf, size, "out of memory");
		break;
	}
	return buf;
}

static const char *QC_InvalidText (uint32_t reason)
{
	return reason == QC_INVALID_ENTRY ? "its entry is outside the program" : "its locals are outside the globals";
}

const char *QC_LoadNoteText (const qc_loadnote_t *note, char *buf, size_t size)
{
	switch (note->kind)
	{
	case QC_NOTE_ASSUMED_KK7:
		snprintf (buf, size, "unknown secondary version 0x%08x; assuming the KK7 layout", note->value);
		break;
	case QC_NOTE_HEXEN2_CALLS:
		snprintf (buf, size, "Hexen 2 calling convention detected");
		break;
	case QC_NOTE_POISONED_STATEMENT:
		snprintf (buf, size, "statement %u (opcode %u) is invalid and will fault if run", note->index, note->value);
		break;
	case QC_NOTE_JUMP_OUT_OF_RANGE:
		snprintf (buf, size, "statement %u jumps outside the program", note->index);
		break;
	case QC_NOTE_INVALID_FUNCTION:
		snprintf (buf, size, "function %u is invalid (%s)", note->index, QC_InvalidText (note->value));
		break;
	case QC_NOTE_TYPES_IGNORED:
		snprintf (buf, size, "types section ignored");
		break;
	case QC_NOTE_LINE_NUMBERS_IGNORED:
		snprintf (buf, size, "line-number section out of bounds; ignored");
		break;
	case QC_NOTE_BODYLESS_IGNORED:
	default:
		snprintf (buf, size, "bodyless-function section out of bounds; ignored");
		break;
	}
	return buf;
}

/*
==============================================================================

QUERIES

==============================================================================
*/

int QC_TypeWords (uint32_t type)
{
	switch (type)
	{
	case QC_EV_VOID:
		return 0;
	case QC_EV_VECTOR:
		return 3;
	case QC_EV_INT64:
	case QC_EV_UINT64:
	case QC_EV_DOUBLE:
		return 2;
	case QC_EV_STRING:
	case QC_EV_FLOAT:
	case QC_EV_ENTITY:
	case QC_EV_FIELD:
	case QC_EV_FUNCTION:
	case QC_EV_POINTER:
	case QC_EV_INTEGER:
	case QC_EV_UINT:
		return 1;
	default:
		return -1;
	}
}

qc_format_t QC_ProgsFormat (const qc_progs_t *progs)		{ return progs->format; }
uint32_t QC_ProgsVersion (const qc_progs_t *progs)			{ return progs->version; }
uint32_t QC_ProgsCRC (const qc_progs_t *progs)				{ return progs->crc; }
uint32_t QC_ProgsNumStatements (const qc_progs_t *progs)	{ return progs->numstatements; }
uint32_t QC_ProgsNumGlobals (const qc_progs_t *progs)		{ return progs->numglobals; }
uint32_t QC_ProgsNumFunctions (const qc_progs_t *progs)		{ return progs->numfunctions; }
uint32_t QC_ProgsEntityFields (const qc_progs_t *progs)		{ return progs->entityfields; }
uint32_t QC_ProgsNumGlobalDefs (const qc_progs_t *progs)	{ return progs->numglobaldefs; }
uint32_t QC_ProgsNumFieldDefs (const qc_progs_t *progs)		{ return progs->numfielddefs; }
uint32_t QC_ProgsNumBodyless (const qc_progs_t *progs)		{ return progs->numbodyless; }

const qc_loadnote_t *QC_ProgsNotes (const qc_progs_t *progs, uint32_t *count)
{
	*count = progs->numnotes;
	return progs->notes;
}

const char *QC_ProgsString (const qc_progs_t *progs, uint32_t ofs)
{
	return QC_Cstr (progs, ofs);
}

bool QC_ProgsInitialGlobal (const qc_progs_t *progs, uint32_t word, uint32_t *value)
{
	if (word >= progs->numglobals)
		return false;
	*value = progs->globals[word];
	return true;
}

const qc_def_t *QC_GlobalDefRaw (const qc_progs_t *progs, const char *name)
{
	uint32_t	i;

	if (!QC_MapGet (&progs->globals_by_name, name, strlen (name), &i))
		return NULL;
	return &progs->globaldefs[i];
}

const qc_def_t *QC_FieldDefRaw (const qc_progs_t *progs, const char *name)
{
	uint32_t	i;

	if (!QC_MapGet (&progs->fields_by_name, name, strlen (name), &i))
		return NULL;
	return &progs->fielddefs[i];
}

bool QC_ProgsFunctionIndex (const qc_progs_t *progs, const char *name, uint32_t *index)
{
	const qc_def_t	*def = QC_GlobalDefRaw (progs, name);
	uint32_t		value;

	if (def && def->type == QC_EV_FUNCTION && QC_ProgsInitialGlobal (progs, def->ofs, &value)
		&& value && value < progs->numfunctions)
	{
		*index = value;
		return true;
	}
	return QC_MapGet (&progs->functions_by_name, name, strlen (name), index);
}

bool QC_ProgsFunction (const qc_progs_t *progs, uint32_t index, qc_funcinfo_t *info)
{
	const qc_function_t	*f;

	if (index >= progs->numfunctions)
		return false;
	f = &progs->functions[index];
	*info = (qc_funcinfo_t){.index = index, .name = QC_Cstr (progs, f->name), .file = QC_Cstr (progs, f->file),
		.kind = f->kind, .entry = f->entry, .number = f->number, .invalid = f->invalid,
		.parm_start = f->parm_start, .locals = f->locals, .num_parms = f->num_parms};
	memcpy (info->parm_sizes, f->parm_sizes, 8);
	return true;
}

static void QC_DefInfo (const qc_progs_t *progs, const qc_def_t *d, qc_definfo_t *info)
{
	*info = (qc_definfo_t){QC_Cstr (progs, d->name), d->type, d->ofs, d->save, d->shared};
}

bool QC_ProgsGlobalDef (const qc_progs_t *progs, const char *name, qc_definfo_t *info)
{
	const qc_def_t	*d = QC_GlobalDefRaw (progs, name);

	if (d)
		QC_DefInfo (progs, d, info);
	return d != NULL;
}

bool QC_ProgsFieldDef (const qc_progs_t *progs, const char *name, qc_definfo_t *info)
{
	const qc_def_t	*d = QC_FieldDefRaw (progs, name);

	if (d)
		QC_DefInfo (progs, d, info);
	return d != NULL;
}

bool QC_ProgsGlobalDefAt (const qc_progs_t *progs, uint32_t i, qc_definfo_t *info)
{
	if (i >= progs->numglobaldefs)
		return false;
	QC_DefInfo (progs, &progs->globaldefs[i], info);
	return true;
}

bool QC_ProgsFieldDefAt (const qc_progs_t *progs, uint32_t i, qc_definfo_t *info)
{
	if (i >= progs->numfielddefs)
		return false;
	QC_DefInfo (progs, &progs->fielddefs[i], info);
	return true;
}

const char *QC_ProgsGlobalNameAt (const qc_progs_t *progs, uint32_t word)
{
	uint32_t	lo = 0, hi = progs->numnames_by_ofs, mid;

	if (!word)
		return NULL;
	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		if (progs->names_by_ofs[mid].ofs < word)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == progs->numnames_by_ofs || progs->names_by_ofs[lo].ofs != word)
		return NULL;
	return QC_Cstr (progs, progs->globaldefs[progs->names_by_ofs[lo].def].name);
}

static int QC_CompareU32 (const void *a, const void *b)
{
	uint32_t	x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

uint32_t QC_ProgsCalledBuiltins (const qc_progs_t *progs, uint32_t *out, uint32_t max)
{
	uint32_t		*found, count = 0, distinct = 0, i, value;
	qc_funckind_t	kind;

	found = malloc (((size_t)progs->numstatements + 1) * sizeof(*found));
	if (!found)
		return 0;
	for (i = 0 ; i < progs->numstatements ; i++)
	{
		if (QC_CallArgc (progs->statements[i].op) < 0)
			continue;
		if (!QC_ProgsInitialGlobal (progs, progs->statements[i].a / 4, &value))
			continue;
		value &= 0x00FFFFFF;
		if (value >= progs->numfunctions)
			continue;
		kind = progs->functions[value].kind;
		if (kind == QC_FUNC_BUILTIN || kind == QC_FUNC_NAMED_BUILTIN)
			found[count++] = value;
	}
	qsort (found, count, sizeof(*found), QC_CompareU32);
	for (i = 0 ; i < count ; i++)
	{
		if (i && found[i] == found[i - 1])
			continue;
		if (distinct < max)
			out[distinct] = found[i];
		distinct++;
	}
	free (found);
	return distinct;
}

const char *QC_ProgsBodyless (const qc_progs_t *progs, uint32_t i)
{
	return i < progs->numbodyless ? progs->bodyless[i] : NULL;
}

bool QC_ProgsSourceLine (const qc_progs_t *progs, uint32_t statement, uint32_t *line)
{
	if (!progs->lines || statement >= progs->numstatements)
		return false;
	*line = progs->lines[statement];
	return true;
}
