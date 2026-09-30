// qc_lib_reflect.c -- entity field reflection, and field values as text for
// eprint, objerror and coredump (docs/spec/builtins.md)
//
// The field table is the VM's: every field of the progs (a vector four times:
// v, v_x, v_y, v_z) and those the host added. The text is FTE's savegame
// writer's ("ugly value strings").

#include "qc_lib.h"

#include <string.h>

// float numentityfields(): the named fields (a vector counts four times)
static bool QC_Numentityfields (qcvm_t *vm)
{
	QC_ReturnFloat (vm, (float)vm->fields.count);
	return true;
}

static const qc_fieldentry_t *QC_FieldArg (const qcvm_t *vm)
{
	uint32_t	i = QC_F2U (QC_ArgFloat (vm, 0));

	return i < vm->fields.count ? &vm->fields.entries[i] : NULL;
}

// string entityfieldname(float index): null out of range
static bool QC_Entityfieldname (qcvm_t *vm)
{
	const qc_fieldentry_t	*f = QC_FieldArg (vm);

	return QC_LibReturnOptString (vm, f ? f->name : NULL);
}

// float entityfieldtype(float index): its EV_ type, 0 out of range
static bool QC_Entityfieldtype (qcvm_t *vm)
{
	const qc_fieldentry_t	*f = QC_FieldArg (vm);

	QC_ReturnFloat (vm, f ? (float)f->type : 0.0f);
	return true;
}

// float findentityfield(string name): the first field of that name's index, or 0
static bool QC_Findentityfield (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0);
	uint32_t	i;

	for (i = 0 ; i < vm->fields.count ; i++)
		if (!strcmp (vm->fields.entries[i].name, name))
			break;
	QC_ReturnFloat (vm, i < vm->fields.count ? (float)i : 0.0f);
	return true;
}

// field_t entityfieldref(float index): its reference (offset), or 0
static bool QC_Entityfieldref (qcvm_t *vm)
{
	const qc_fieldentry_t	*f = QC_FieldArg (vm);

	QC_ReturnWord (vm, f ? f->ofs : 0);
	return true;
}

/*
==============================================================================

VALUES AS TEXT

==============================================================================
*/

// C's %#x
static void QC_AltHex (qc_sink_t *s, uint32_t v)
{
	if (v)
		QC_SinkPrintf (s, "%#x", v);
	else
		QC_SinkPush (s, '0');
}

// a function as progs:name, as FTE writes them
static void QC_FunctionName (const qcvm_t *vm, qc_sink_t *s, uint32_t f)
{
	uint32_t		pr = f >> 24;
	qc_funcinfo_t	info;

	if (pr >= vm->numprogs)
	{
		QC_SinkAppend (s, "BAD FUNCTION INDEX: ", 20);
		QC_AltHex (s, f);
	}
	else if (QC_ProgsFunction (vm->progs[pr].progs, f & 0x00FFFFFF, &info))
		QC_SinkPrintf (s, "%u:%s", pr, info.name);
	else
		QC_SinkPrintf (s, "%u:CORRUPT FUNCTION POINTER", pr);
}

void QC_FormatValue (qcvm_t *vm, qc_sink_t *s, uint32_t type, const uint32_t words[3])
{
	const char	*text, *p;
	float		v;
	double		d;
	int64_t		i;
	uint32_t	k;
	bool		ints;

	switch (type)
	{
	case QC_EV_STRING:
		text = QC_Str (vm, words[0]);
		for (p = text ? text : "" ; *p ; p++)
		{
			if (*p == '\n')
				QC_SinkAppend (s, "\\n", 2);
			else if (*p == '"')
				QC_SinkAppend (s, "\\\"", 2);
			else if (*p == '\\')
				QC_SinkAppend (s, "\\\\", 2);
			else
				QC_SinkPush (s, *p);
		}
		break;
	case QC_EV_ENTITY:
	case QC_EV_INTEGER:
		QC_SinkPrintf (s, "%d", (int32_t)words[0]);
		break;
	case QC_EV_FUNCTION:
		QC_FunctionName (vm, s, words[0]);
		break;
	case QC_EV_FIELD:
		for (k = 0 ; k < vm->fields.count ; k++)
			if (vm->fields.entries[k].ofs == words[0])
				break;
		if (k < vm->fields.count)
			QC_SinkAppend (s, vm->fields.entries[k].name, strlen (vm->fields.entries[k].name));
		else
			QC_SinkPrintf (s, "bad field %u", words[0]);
		break;
	case QC_EV_VOID:
		QC_SinkAppend (s, "void", 4);
		break;
	case QC_EV_FLOAT:
		v = QC_BitsFloat (words[0]);
		if (v == (float)QC_FloatToInt (v))
			QC_SinkPrintf (s, "%d", QC_FloatToInt (v));
		else
			QC_FormatF (s, (double)v, 6);
		break;
	case QC_EV_DOUBLE:
		d = QC_BitsDouble (words[0] | ((uint64_t)words[1] << 32));
		i = QC_D2I64 (d);
		if (d == (double)i)
			QC_SinkPrintf (s, "%lld", (long long)i);
		else
			QC_FormatF (s, d, 6);
		break;
	case QC_EV_UINT:
		QC_SinkPrintf (s, "%u", words[0]);
		break;
	case QC_EV_INT64:
		QC_SinkPrintf (s, "%lld", (long long)(int64_t)(words[0] | ((uint64_t)words[1] << 32)));
		break;
	case QC_EV_UINT64:
		QC_SinkPrintf (s, "%llu", (unsigned long long)(words[0] | ((uint64_t)words[1] << 32)));
		break;
	case QC_EV_VECTOR:
		for (ints = true, k = 0 ; k < 3 ; k++)
		{
			v = QC_BitsFloat (words[k]);
			ints &= v == (float)QC_FloatToInt (v);
		}
		for (k = 0 ; k < 3 ; k++)
		{
			if (k)
				QC_SinkPush (s, ' ');
			v = QC_BitsFloat (words[k]);
			if (ints)
				QC_SinkPrintf (s, "%d", QC_FloatToInt (v));
			else
				QC_FormatG (s, (double)v, 6);
		}
		break;
	case QC_EV_POINTER:
		QC_AltHex (s, words[0]);
		break;
	default:
		QC_SinkPrintf (s, "bad type %u", type);
		break;
	}
}

// v_x, v_y, v_z: a vector's components
static bool QC_IsComponent (const char *name)
{
	size_t	len = strlen (name);

	return len > 2 && name[len - 2] == '_' && (name[len - 1] == 'x' || name[len - 1] == 'y' || name[len - 1] == 'z');
}

static void QC_QuotedPair (qc_sink_t *s, const char *name, const qc_sink_t *value)
{
	QC_SinkPush (s, '"');
	QC_SinkAppend (s, name, strlen (name));
	QC_SinkAppend (s, "\" \"", 3);
	QC_SinkAppend (s, QC_SinkText (value), value->len);
	QC_SinkAppend (s, "\"\n", 2);
}

// the "field" "value" lines of an entity's fields that aren't all zero
static void QC_EntityFields (qcvm_t *vm, qc_sink_t *s, uint32_t e)
{
	const qc_fieldentry_t	*f;
	const uint8_t			*p;
	uint32_t				i, k, n, words[3];
	int						tw;
	qc_sink_t				value;

	for (i = 0 ; i < vm->fields.count ; i++)
	{
		f = &vm->fields.entries[i];
		if (!*f->name || QC_IsComponent (f->name) || strstr (f->name, "::"))
			continue;
		tw = QC_TypeWords (f->type);
		n = tw < 1 ? 1 : tw > 3 ? 3 : (uint32_t)tw;
		if (!(p = QC_FieldPtr (&vm->mem, e, f->ofs, n)))
			continue;
		words[0] = words[1] = words[2] = 0;
		memcpy (words, p, (size_t)n * 4);
		for (k = 0 ; k < 3 && !words[k] ; k++)
			;
		if (k == 3)
			continue;
		QC_SinkInit (&value, SIZE_MAX);
		QC_FormatValue (vm, &value, f->type, words);
		QC_QuotedPair (s, f->name, &value);
		s->failed |= value.failed;
		QC_SinkFree (&value);
	}
}

void QC_EntityBlock (qcvm_t *vm, qc_sink_t *s, uint32_t e)
{
	QC_SinkAppend (s, "{\n", 2);
	QC_EntityFields (vm, s, e);
	QC_SinkAppend (s, "}\n", 2);
}

void QC_CoredumpText (qcvm_t *vm, qc_sink_t *s)
{
	const qc_progs_t	*p;
	const qc_def_t		*d;
	const char			*name;
	uint32_t			pr, i, k, words[3], e;
	uint64_t			at;
	qc_funcinfo_t		own;
	qc_sink_t			value;

	QC_SinkPrintf (s, "general {\n\"maxprogs\" \"%u\"\n\"numentities\" \"%u\"\n}\n", vm->config.limits.progs,
		vm->mem.num_edicts);
	for (pr = 0 ; pr < vm->numprogs ; pr++)
		QC_SinkPrintf (s, "progs %u {\n\"crc\" \"%u\"\n}\n", pr, vm->progs[pr].progs->crc);
	QC_SinkAppend (s, "stacktrace {\n", 13);
	QC_BacktraceSink (vm, s);
	QC_SinkAppend (s, "}\n", 2);

	for (pr = 0 ; pr < vm->numprogs ; pr++)
	{
		p = vm->progs[pr].progs;
		QC_SinkPrintf (s, "globals %u {\n", pr);
		for (i = 0 ; i < p->numglobaldefs ; i++)
		{
			d = &p->globaldefs[i];
			name = QC_Cstr (p, d->name);
			if (!*name || QC_IsComponent (name) || !d->save)
				continue;
			at = (uint64_t)vm->progs[pr].gbase + (uint64_t)d->ofs * 4;
			for (k = 0 ; k < 3 ; k++)
				words[k] = QC_GetS (&vm->mem, at + (uint64_t)k * 4);
			switch (d->type)
			{
			case QC_EV_FUNCTION:
				// a function still holding its own name's function isn't interesting
				if (words[0] >> 24 == pr && QC_ProgsFunction (p, words[0] & 0x00FFFFFF, &own)
					&& !strcmp (own.name, name))
					continue;
				break;
			case QC_EV_STRING:
			case QC_EV_FLOAT:
			case QC_EV_DOUBLE:
			case QC_EV_INTEGER:
			case QC_EV_UINT:
			case QC_EV_INT64:
			case QC_EV_UINT64:
			case QC_EV_ENTITY:
			case QC_EV_VECTOR:
				break;
			default:
				continue;
			}
			QC_SinkInit (&value, SIZE_MAX);
			QC_FormatValue (vm, &value, d->type, words);
			QC_QuotedPair (s, name, &value);
			s->failed |= value.failed;
			QC_SinkFree (&value);
		}
		QC_SinkAppend (s, "}\n", 2);
	}

	for (e = 0 ; e < vm->mem.num_edicts ; e++)
	{
		if (!vm->mem.slots[e].in_use)
			continue;
		QC_SinkPrintf (s, "entity %u{\n", e);
		QC_EntityFields (vm, s, e);
		QC_SinkAppend (s, "}\n", 2);
	}
}

static const qc_libentry_t	qc_reflect[] = {
	{"numentityfields", QC_Numentityfields, NULL, 0},
	{"entityfieldname", QC_Entityfieldname, NULL, 0},
	{"entityfieldtype", QC_Entityfieldtype, NULL, 0},
	{"findentityfield", QC_Findentityfield, NULL, 0},
	{"entityfieldref", QC_Entityfieldref, NULL, 0},
};

bool QC_RegisterReflect (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_reflect, sizeof(qc_reflect) / sizeof(qc_reflect[0]));
}
