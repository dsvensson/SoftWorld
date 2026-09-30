// qc_disasm.c -- human-readable listings of statements and functions

#include "qc_local.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct
{
	char	*buf;
	size_t	size;
	size_t	len;
} text_t;

// appends, truncating at the end of the buffer
static void QC_Append (text_t *t, const char *fmt, ...)
{
	va_list	args;
	int		n;

	if (t->len + 1 >= t->size)
		return;
	va_start (args, fmt);
	n = vsnprintf (t->buf + t->len, t->size - t->len, fmt, args);
	va_end (args);
	if (n > 0)
		t->len += (size_t)n < t->size - t->len ? (size_t)n : t->size - t->len - 1;
}

// a global operand: its name, or g<word> and its initial value
static void QC_AppendGlobal (text_t *t, const qc_progs_t *progs, uint32_t byteofs)
{
	uint32_t	word = byteofs / 4, value = 0;
	const char	*name = QC_ProgsGlobalNameAt (progs, word);

	if (name && *name && strcmp (name, "IMMEDIATE"))
	{
		QC_Append (t, "%s", name);
		return;
	}
	QC_ProgsInitialGlobal (progs, word, &value);
	QC_Append (t, "g%u", word);
	if (value)
		QC_Append (t, "(=0x%x)", value);
}

const char *QC_DisassembleStatement (const qc_progs_t *progs, uint32_t statement, char *buf, size_t size)
{
	text_t				t = {buf, size, 0};
	const qc_stmt_t		*s;
	const qc_opinfo_t	*info;
	uint32_t			k, value, line;
	bool				first = true;

	if (!size)
		return buf;
	buf[0] = 0;
	if (statement > progs->numstatements)		// the sentinel is there to see
	{
		QC_Append (&t, "%6u: <no statement>", statement);
		return buf;
	}
	s = &progs->statements[statement];
	info = &qc_opinfo[s->op];
	QC_Append (&t, "%6u: %-14s", statement, info->name);
	for (k = 0 ; k < 3 ; k++)
	{
		if (info->operands[k] == QC_OPND_U)
			continue;
		QC_Append (&t, first ? " " : ", ");
		first = false;
		value = k == 0 ? s->a : k == 1 ? s->b : s->c;
		switch (info->operands[k])
		{
		case QC_OPND_G:
			QC_AppendGlobal (&t, progs, value);
			break;
		case QC_OPND_A:
			QC_Append (&t, "&g%u", value);
			break;
		case QC_OPND_J:
			QC_Append (&t, "-> %u", value);
			break;
		default:
			QC_Append (&t, "%u", value);
			break;
		}
	}
	if (QC_ProgsSourceLine (progs, statement, &line))
		QC_Append (&t, "    ; line %u", line);
	return buf;
}

void QC_DisassembleFunction (const qc_progs_t *progs, uint32_t index,
	void (*line) (void *ctx, const char *text), void *ctx)
{
	char				buf[1024];
	text_t				t = {buf, sizeof(buf), 0};
	const qc_function_t	*f;
	uint32_t			end, i;
	int					parms;

	if (index >= progs->numfunctions)
	{
		snprintf (buf, sizeof(buf), "; no function %u", index);
		line (ctx, buf);
		return;
	}
	f = &progs->functions[index];
	QC_Append (&t, "; function %u %s", index, QC_Cstr (progs, f->name));
	if (*QC_Cstr (progs, f->file))
		QC_Append (&t, " (%s)", QC_Cstr (progs, f->file));
	line (ctx, buf);

	t.len = 0;
	switch (f->kind)
	{
	case QC_FUNC_NULL:
		QC_Append (&t, ";   Null");
		break;
	case QC_FUNC_BUILTIN:
		QC_Append (&t, ";   Builtin { number: %u }", f->number);
		break;
	case QC_FUNC_NAMED_BUILTIN:
		QC_Append (&t, ";   NamedBuiltin");
		break;
	case QC_FUNC_INVALID:
		QC_Append (&t, ";   Invalid(%s)", f->invalid == QC_INVALID_ENTRY ? "EntryOutOfRange" : "LocalsOutOfRange");
		break;
	case QC_FUNC_QUAKEC:
		parms = f->num_parms < 0 ? 0 : f->num_parms > 8 ? 8 : f->num_parms;
		QC_Append (&t, ";   parm_start %u locals %u parms %d sizes [", f->parm_start, f->locals, f->num_parms);
		for (i = 0 ; i < (uint32_t)parms ; i++)
			QC_Append (&t, i ? ", %u" : "%u", f->parm_sizes[i]);
		QC_Append (&t, "]");
		break;
	}
	line (ctx, buf);
	if (f->kind != QC_FUNC_QUAKEC)
		return;

	// its statements run up to the next function's
	end = progs->numstatements;
	for (i = 0 ; i < progs->numfunctions ; i++)
		if (progs->functions[i].kind == QC_FUNC_QUAKEC && progs->functions[i].entry > f->entry
			&& progs->functions[i].entry < end)
			end = progs->functions[i].entry;
	for (i = f->entry ; i < end ; i++)
		line (ctx, QC_DisassembleStatement (progs, i, buf, sizeof(buf)));
}
