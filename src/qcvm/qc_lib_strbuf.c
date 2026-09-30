// qc_lib_strbuf.c -- string buffers (docs/spec/builtins.md)
//
// A buffer is a vector of strings, some of them holes: its size is the highest
// index set + 1, and holes read as null. Handles are floats from 1. Invalid
// handles are ignored; where FTE then leaves the result as it was, this
// returns 0 or null. Entries are copies, so they outlive the temp strings they
// came from.

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

#define QC_MAX_SET_INDEX	(1u << 20)		// FTE refuses bufstr_set indices past this

typedef struct
{
	char	*s;			// NULL: a hole
	size_t	len;
} qc_bufslot_t;

typedef struct
{
	qc_bufslot_t	*slots;			// count of them: the buffer's size
	uint32_t		count, size;
	size_t			bytes;			// what the buffer is charged: its slots and strings
} qc_strbuf_t;

struct qc_strbufs_s
{
	qc_strbuf_t		**bufs;			// by handle - 1, NULL when free
	uint32_t		count;
};

static void QC_FreeSlots (qc_strbuf_t *b)
{
	uint32_t	i;

	for (i = 0 ; i < b->count ; i++)
		free (b->slots[i].s);
	free (b->slots);
	b->slots = NULL;
	b->count = b->size = 0;
	b->bytes = 0;
}

void QC_LibFreeBufs (qc_std_t *std)
{
	uint32_t	i;

	if (!std->bufs)
		return;
	for (i = 0 ; i < std->bufs->count ; i++)
		if (std->bufs->bufs[i])
		{
			QC_FreeSlots (std->bufs->bufs[i]);
			free (std->bufs->bufs[i]);
		}
	free (std->bufs->bufs);
	free (std->bufs);
	std->bufs = NULL;
}

static qc_strbufs_t *QC_Bufs (qcvm_t *vm)
{
	qc_std_t	*std = QC_LibState (vm);

	if (std && !std->bufs && !(std->bufs = calloc (1, sizeof(*std->bufs))))
		QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
	return std ? std->bufs : NULL;
}

// the buffer argument i names, if valid (FTE takes 1 from the float handle,
// then truncates)
static qc_strbuf_t *QC_BufArg (qcvm_t *vm, int i)
{
	qc_strbufs_t	*b = vm->std ? vm->std->bufs : NULL;
	int32_t			index = QC_FloatToInt (QC_ArgFloat (vm, i) - 1.0f);

	return b && index >= 0 && (uint32_t)index < b->count ? b->bufs[index] : NULL;
}

// an entry index argument (a negative one isn't valid): -1 if not
static int64_t QC_IndexArg (const qcvm_t *vm, int i)
{
	int32_t	index = QC_LibArgInt (vm, i);

	return index >= 0 ? index : -1;
}

// what a buffer would be charged with len bytes at index i
static size_t QC_BytesAfterSet (const qc_strbuf_t *b, uint32_t i, size_t len)
{
	size_t	grow = i >= b->count ? (size_t)i + 1 - b->count : 0;
	size_t	old = i < b->count && b->slots[i].s ? b->slots[i].len : 0;

	return b->bytes + grow * sizeof(qc_bufslot_t) + len - old;
}

// charges the change from old to new bytes; false with a warning past the limit
static bool QC_Recharge (qcvm_t *vm, size_t old, size_t new)
{
	if (new >= old)
	{
		if (!QC_LibCharge (vm, new - old))
		{
			QC_Warning (vm, "string buffers: out of memory for hash tables and string buffers");
			return false;
		}
		return true;
	}
	QC_LibRelease (vm, old - new);
	return true;
}

// Stores a copy of s at i, growing the buffer, charged; false (with a warning)
// past the memory limit
static bool QC_Store (qcvm_t *vm, qc_strbuf_t *b, uint32_t i, const char *s, size_t len)
{
	size_t			after = QC_BytesAfterSet (b, i, len);
	qc_bufslot_t	*grown;
	uint32_t		size;
	char			*copy;

	if (i >= b->size)
	{
		for (size = b->size ? b->size : 16 ; size <= i ; size = size > UINT32_MAX / 2 ? i + 1 : size * 2)
			;
		if (!(grown = realloc (b->slots, (size_t)size * sizeof(*grown))))
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
		memset (grown + b->size, 0, (size_t)(size - b->size) * sizeof(*grown));
		b->slots = grown;
		b->size = size;
	}
	if (!(copy = malloc (len + 1)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
	if (!QC_Recharge (vm, b->bytes, after))
	{
		free (copy);
		return false;
	}
	memcpy (copy, s, len);
	copy[len] = 0;
	free (b->slots[i].s);
	b->slots[i] = (qc_bufslot_t){copy, len};
	if (i >= b->count)
		b->count = i + 1;
	b->bytes = after;
	return true;
}

// the storage slots and strings take
static size_t QC_SlotsBytes (const qc_bufslot_t *slots, uint32_t count)
{
	size_t		n = (size_t)count * sizeof(qc_bufslot_t);
	uint32_t	i;

	for (i = 0 ; i < count ; i++)
		n += slots[i].s ? slots[i].len : 0;
	return n;
}

// Replaces a buffer's contents (taken over), charging the difference; false
// (with a warning, the new contents freed) past the memory limit
static bool QC_Replace (qcvm_t *vm, qc_strbuf_t *b, qc_bufslot_t *slots, uint32_t count)
{
	size_t		bytes = QC_SlotsBytes (slots, count);
	uint32_t	i;

	if (!QC_Recharge (vm, b->bytes, bytes))
	{
		for (i = 0 ; i < count ; i++)
			free (slots[i].s);
		free (slots);
		return false;
	}
	QC_FreeSlots (b);
	b->slots = slots;
	b->count = b->size = count;
	b->bytes = bytes;
	return true;
}

// the most entries a buffer may hold
static uint32_t QC_EntryLimit (const qcvm_t *vm)
{
	return vm->config.limits.string_buffer_entries;
}

// strbuf buf_create(optional string type = "string", optional float flags = 1):
// a new empty buffer, or -1 for another type or past the buffer limit
static bool QC_BufCreate (qcvm_t *vm)
{
	qc_strbufs_t	*bufs = QC_Bufs (vm);
	qc_strbuf_t		*b, **grown;
	uint32_t		i, live = 0;
	const char		*type;
	bool			isstring;

	if (!bufs)
		return false;
	type = QC_ArgString (vm, 0);
	isstring = QC_Argc (vm) == 0 || (strlen (type) == 6 && QC_LibEqualFold (type, "string", 6));
	for (i = 0 ; i < bufs->count ; i++)
		live += bufs->bufs[i] != NULL;
	QC_ReturnFloat (vm, -1);
	if (!isstring || live >= vm->config.limits.string_buffers)
		return true;
	if (!(b = calloc (1, sizeof(*b))))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
	for (i = 0 ; i < bufs->count && bufs->bufs[i] ; i++)
		;
	if (i == bufs->count)
	{
		if (!(grown = realloc (bufs->bufs, (bufs->count + 1) * sizeof(*grown))))
		{
			free (b);
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
		}
		bufs->bufs = grown;
		bufs->count++;
	}
	bufs->bufs[i] = b;
	QC_ReturnFloat (vm, (float)(i + 1));
	return true;
}

// void buf_del(strbuf buf)
static bool QC_BufDel (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	uint32_t	i;

	if (!b)
		return true;
	QC_LibRelease (vm, b->bytes);
	for (i = 0 ; vm->std->bufs->bufs[i] != b ; i++)
		;
	vm->std->bufs->bufs[i] = NULL;
	QC_FreeSlots (b);
	free (b);
	return true;
}

// float buf_getsize(strbuf buf): the highest index set + 1 (0 for an invalid handle)
static bool QC_BufGetsize (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);

	QC_ReturnFloat (vm, b ? (float)b->count : 0.0f);
	return true;
}

// copies of count slots (holes kept); NULL when out of memory
static qc_bufslot_t *QC_CopySlots (const qc_bufslot_t *slots, uint32_t count)
{
	qc_bufslot_t	*copy = calloc (count ? count : 1, sizeof(*copy));
	uint32_t		i;

	for (i = 0 ; copy && i < count ; i++)
	{
		if (!slots[i].s)
			continue;
		copy[i].len = slots[i].len;
		if (!(copy[i].s = malloc (slots[i].len + 1)))
		{
			while (i)
				free (copy[--i].s);
			free (copy);
			return NULL;
		}
		memcpy (copy[i].s, slots[i].s, slots[i].len + 1);
	}
	return copy;
}

// void buf_copy(strbuf from, strbuf to): to's contents made a copy of from's
// (holes too); both valid and different
static bool QC_BufCopy (qcvm_t *vm)
{
	qc_strbuf_t		*from = QC_BufArg (vm, 0), *to = QC_BufArg (vm, 1);
	qc_bufslot_t	*copy;

	if (!from || !to || from == to)
		return true;
	if (!(copy = QC_CopySlots (from->slots, from->count)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
	QC_Replace (vm, to, copy, from->count);
	return true;
}

static size_t	qc_sortprefix;
static bool		qc_sortbackward;

static int QC_CompareSlots (const void *a, const void *b)
{
	const qc_bufslot_t	*x = a, *y = b;
	size_t				xl = x->len < qc_sortprefix ? x->len : qc_sortprefix;
	size_t				yl = y->len < qc_sortprefix ? y->len : qc_sortprefix;
	int					c = memcmp (x->s, y->s, xl < yl ? xl : yl);

	if (!c)
		c = xl < yl ? -1 : xl > yl;
	return qc_sortbackward ? -c : c;
}

// void buf_sort(strbuf buf, float prefixlen, float backward): the holes dropped,
// then sorted by the first prefixlen bytes (all if 0 or less), descending if backward
static bool QC_BufSort (qcvm_t *vm)
{
	qc_strbuf_t		*b = QC_BufArg (vm, 0);
	int32_t			prefix = QC_LibArgInt (vm, 1);
	qc_bufslot_t	*kept;
	uint32_t		i, n = 0;

	if (!b)
		return true;
	if (!(kept = calloc (b->count ? b->count : 1, sizeof(*kept))))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
	for (i = 0 ; i < b->count ; i++)
		if (b->slots[i].s)
		{
			kept[n++] = b->slots[i];
			b->slots[i].s = NULL;
		}
	// a stable sort, as Rust's: equal prefixes keep their order
	qc_sortprefix = prefix <= 0 ? SIZE_MAX : (size_t)prefix;
	qc_sortbackward = QC_ArgFloat (vm, 2) != 0;
	for (i = 1 ; i < n ; i++)
	{
		qc_bufslot_t	v = kept[i];
		uint32_t		j = i;

		for ( ; j > 0 && QC_CompareSlots (&kept[j - 1], &v) > 0 ; j--)
			kept[j] = kept[j - 1];
		kept[j] = v;
	}
	// without the holes the buffer only shrinks, so this can't fail
	QC_Replace (vm, b, kept, n);
	return true;
}

// string buf_implode(strbuf buf, string glue): the entries set, with glue between
// them (once the output isn't empty); null for an invalid handle
static bool QC_BufImplode (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	const char	*glue;
	size_t		gluelen, total = 0;
	qc_sink_t	out;
	uint32_t	i;

	if (!b)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	glue = QC_ArgString (vm, 1);
	gluelen = strlen (glue);
	// sized first (the glue repeats), and refused before it's made if the temp
	// strings have no room for it
	for (i = 0 ; i < b->count ; i++)
		if (b->slots[i].s)
		{
			if (total)
				total += gluelen;
			total += b->slots[i].len;
			if (!QC_StringsFit (&vm->strings, total))
				return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
		}
	QC_SinkInit (&out, SIZE_MAX);
	for (i = 0 ; i < b->count ; i++)
		if (b->slots[i].s)
		{
			if (out.len)
				QC_SinkAppend (&out, glue, gluelen);
			QC_SinkAppend (&out, b->slots[i].s, b->slots[i].len);
		}
	return QC_LibReturnSink (vm, &out);
}

// string bufstr_get(strbuf buf, float index): a copy of the entry, or null for
// holes and invalid indices or handles
static bool QC_BufstrGet (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	int64_t		i = QC_IndexArg (vm, 1);

	if (!b || i < 0 || i >= b->count || !b->slots[i].s)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	return QC_ReturnString (vm, b->slots[i].s, b->slots[i].len);
}

// void bufstr_set(strbuf buf, float index, string s): a copy of s at index, the
// buffer grown; indices past 1048576 (or the entry limit) refused with a warning
static bool QC_BufstrSet (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	int64_t		i = QC_IndexArg (vm, 1);
	const char	*s;

	if (!b || i < 0)
		return true;
	if (i > QC_MAX_SET_INDEX || i >= QC_EntryLimit (vm))
	{
		QC_Warning (vm, "bufstr_set: index outside sanity range");
		return true;
	}
	s = QC_ArgString (vm, 2);
	return QC_Store (vm, b, (uint32_t)i, s, strlen (s)) || vm->error.kind == QC_ERR_NONE;
}

// float bufstr_add(strbuf buf, string s, float ordered): a copy of s at the end
// (if ordered) or in the first hole; its index. 0 for an invalid handle, -1
// (with a warning) when the buffer is full.
static bool QC_BufstrAdd (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	const char	*s;
	uint32_t	i = 0;

	if (!b)
	{
		QC_ReturnFloat (vm, 0);
		return true;
	}
	s = QC_ArgString (vm, 1);
	if (QC_LibArgInt (vm, 2))
		i = b->count;
	else
		while (i < b->count && b->slots[i].s)
			i++;
	if (i >= QC_EntryLimit (vm))
	{
		QC_Warning (vm, "bufstr_add: string buffer is full");
		QC_ReturnFloat (vm, -1);
		return true;
	}
	if (!QC_Store (vm, b, i, s, strlen (s)))
	{
		QC_ReturnFloat (vm, -1);
		return vm->error.kind == QC_ERR_NONE;
	}
	QC_ReturnFloat (vm, (float)i);
	return true;
}

// void bufstr_free(strbuf buf, float index): the entry made a hole (the size stays)
static bool QC_BufstrFree (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	int64_t		i = QC_IndexArg (vm, 1);

	if (!b || i < 0 || i >= b->count || !b->slots[i].s)
		return true;
	QC_LibRelease (vm, b->slots[i].len);
	b->bytes -= b->slots[i].len;
	free (b->slots[i].s);
	b->slots[i] = (qc_bufslot_t){0};
	return true;
}

bool QC_WildCompare (const char *pattern, const char *s, size_t slen)
{
	bool	*ok, *next, run, extend;
	size_t	j;
	char	p, c;

	// ok[j]: the pattern so far matches s[..j], row by row over the pattern
	ok = calloc (slen + 1, sizeof(*ok));
	next = calloc (slen + 1, sizeof(*next));
	if (!ok || !next)
	{
		free (ok);
		free (next);
		return false;
	}
	ok[0] = true;
	for ( ; (p = *pattern) ; pattern++)
	{
		if (p == '*')
		{
			for (run = false, j = 0 ; j <= slen ; j++)
			{
				// * takes in s[j - 1] only if it isn't a separator
				extend = j > 0 && run && s[j - 1] != '/' && s[j - 1] != '\\';
				run = ok[j] || extend;
				next[j] = run;
			}
		}
		else
		{
			next[0] = false;
			for (j = 1 ; j <= slen ; j++)
			{
				c = s[j - 1];
				next[j] = ok[j - 1] && (p == '?' || p == c
					|| ((p | 0x20) >= 'a' && (p | 0x20) <= 'z' && (p | 0x20) == (c | 0x20)));
			}
		}
		memcpy (ok, next, (slen + 1) * sizeof(*ok));
	}
	run = ok[slen];
	free (ok);
	free (next);
	return run;
}

// whether an entry matches a pattern by bufstr_find's rule
static bool QC_MatchesRule (const char *s, size_t len, const char *pattern, size_t plen, int32_t rule)
{
	switch (rule)
	{
	case 1:		return len == plen && !memcmp (s, pattern, len);
	case 2:		return len >= plen && !memcmp (s, pattern, plen);
	case 3:		return len >= plen && !memcmp (s + len - plen, pattern, plen);
	case 4:		return QC_Find (s, len, pattern, plen) >= 0;
	default:	return QC_WildCompare (pattern, s, len);
	}
}

// float bufstr_find(strbuf buf, string pattern, float rule, float start = 0,
// float step = 1): the first index start + k * step whose entry matches, or -1.
// Rules: 1 exact, 2 prefix, 3 suffix, 4 substring, any other (0, 5) a wildcard pattern.
static bool QC_BufstrFind (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 0);
	int32_t		rule = QC_LibArgInt (vm, 2);
	int32_t		start = QC_FloatToInt (QC_LibOptFloat (vm, 3, 0)), step = QC_FloatToInt (QC_LibOptFloat (vm, 4, 1));
	const char	*pattern;
	size_t		plen;
	uint64_t	i;

	QC_ReturnFloat (vm, -1);
	if (!b || start < 0 || step <= 0)
		return true;
	pattern = QC_ArgString (vm, 1);
	plen = strlen (pattern);
	for (i = (uint64_t)start ; i < b->count ; i += (uint64_t)step)
		if (b->slots[i].s && QC_MatchesRule (b->slots[i].s, b->slots[i].len, pattern, plen, rule))
		{
			QC_ReturnFloat (vm, (float)i);
			break;
		}
	return true;
}

typedef struct
{
	char		**names;
	uint32_t	count, size;
	bool		failed;
} qc_namelist_t;

static void QC_AddName (void *list, const char *name)
{
	qc_namelist_t	*l = list;
	char			**grown;
	uint32_t		size;

	if (l->count == l->size)
	{
		size = l->size ? l->size * 2 : 64;
		if (!(grown = realloc (l->names, (size_t)size * sizeof(*grown))))
		{
			l->failed = true;
			return;
		}
		l->names = grown;
		l->size = size;
	}
	if (!(l->names[l->count] = QC_LibDup (name)))
		l->failed = true;
	else
		l->count++;
}

static int QC_CompareNames (const void *a, const void *b)
{
	return strcmp (*(char *const *)a, *(char *const *)b);
}

// void buf_cvarlist(strbuf buf, string pattern, string antipattern): the buffer
// made the sorted names of the cvars the host lists
static bool QC_BufCvarlist (qcvm_t *vm)
{
	qc_strbuf_t		*b = QC_BufArg (vm, 0);
	qc_namelist_t	list = {0};
	qc_bufslot_t	*slots;
	char			*pattern;
	uint32_t		i, n;

	if (!b)
		return true;
	if (vm->host.cvar_list)
	{
		if (!(pattern = QC_LibDup (QC_ArgString (vm, 1))))
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
		vm->host.cvar_list (vm->ctx, pattern, QC_ArgString (vm, 2), QC_AddName, &list);
		free (pattern);
	}
	qsort (list.names, list.count, sizeof(*list.names), QC_CompareNames);
	n = list.count < QC_EntryLimit (vm) ? list.count : QC_EntryLimit (vm);
	slots = calloc (n ? n : 1, sizeof(*slots));
	for (i = 0 ; i < list.count ; i++)
	{
		if (slots && i < n)
			slots[i] = (qc_bufslot_t){list.names[i], strlen (list.names[i])};
		else
			free (list.names[i]);
	}
	free (list.names);
	if (!slots || list.failed)
	{
		for (i = 0 ; slots && i < n ; i++)
			free (slots[i].s);
		free (slots);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_STRING_BUFFERS, NULL);
	}
	QC_Replace (vm, b, slots, n);
	return true;
}

// float buf_loadfile(string path, strbuf buf): each line of the file the host
// reads added to the buffer; 1 if the file was read, else 0. Lines as FTE's
// VFS_GETS reads them: at \n, one \r dropped before it; a last line without one
// kept as it is; a line ends at a NUL.
static bool QC_BufLoadfile (qcvm_t *vm)
{
	qc_strbuf_t	*b = QC_BufArg (vm, 1);
	uint8_t		*data;
	size_t		size = 0, at = 0, end, len;
	const char	*line, *nl, *nul;

	QC_ReturnFloat (vm, 0);
	if (!b || !vm->host.read_file || !(data = vm->host.read_file (vm->ctx, QC_ArgString (vm, 0), &size)))
		return true;
	while (at < size)
	{
		line = (const char *)data + at;
		nl = memchr (line, '\n', size - at);
		len = nl ? (size_t)(nl - line) : size - at;
		end = nl ? at + len + 1 : size;
		if (nl && len && line[len - 1] == '\r')
			len--;
		if ((nul = memchr (line, 0, len)))
			len = (size_t)(nul - line);
		if (b->count >= QC_EntryLimit (vm) || !QC_Store (vm, b, b->count, line, len))
			break;
		at = end;
	}
	free (data);
	if (vm->error.kind != QC_ERR_NONE)
		return false;
	QC_ReturnFloat (vm, 1);
	return true;
}

static const qc_libentry_t	qc_strbuf[] = {
	{"buf_create", QC_BufCreate, NULL, 0},
	{"buf_del", QC_BufDel, NULL, 0},
	{"buf_getsize", QC_BufGetsize, NULL, 0},
	{"buf_copy", QC_BufCopy, NULL, 0},
	{"buf_sort", QC_BufSort, NULL, 0},
	{"buf_implode", QC_BufImplode, NULL, 0},
	{"bufstr_get", QC_BufstrGet, NULL, 0},
	{"bufstr_set", QC_BufstrSet, NULL, 0},
	{"bufstr_add", QC_BufstrAdd, NULL, 0},
	{"bufstr_free", QC_BufstrFree, NULL, 0},
	{"bufstr_find", QC_BufstrFind, NULL, 0},
	{"buf_cvarlist", QC_BufCvarlist, NULL, 0},
	{"buf_loadfile", QC_BufLoadfile, NULL, 0},
};

bool QC_RegisterStrbuf (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_strbuf, sizeof(qc_strbuf) / sizeof(qc_strbuf[0]));
}
