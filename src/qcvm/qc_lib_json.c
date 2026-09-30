// qc_lib_json.c -- JSON parsed into the VM's memory (docs/spec/builtins.md)
//
// json_parse builds the whole tree in one heap block (freed by json_free, which
// is memfree), for QuakeC to walk with pointers. A node is 16 bytes:
//
// +0  int     type     0 string, 1 number, 2 object, 3 array, 4 true, 5 false, 6 null
// +4  string  name     its key in its parent (array elements are "0", "1", ...), else null
// +8  union            object, array: int first child's pointer, int count
//                      number, true, false, null: double (1 for true, 0 for false and null)
//                      string: string (a temp string, which outlives the tree)
//
// The root is first; each object's or array's children are together, laid out
// in pre-order; the names follow the nodes. Objects keep their members in the
// document's order, duplicate keys too.
//
// Documents must be strict JSON (RFC 8259, nested 127 deep at most); a UTF-8
// byte-order mark first is skipped. FTE's parser is more lenient (comments,
// trailing commas, bare words, keys taken as they are). Keys and strings are
// unescaped, a NUL made the overlong C0 80 since it would end a QuakeC string.
// A document is measured before anything is made, so one whose tree can't fit
// the heap costs no memory.

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	QC_JSON_STRING,
	QC_JSON_NUMBER,
	QC_JSON_OBJECT,
	QC_JSON_ARRAY,
	QC_JSON_TRUE,
	QC_JSON_FALSE,
	QC_JSON_NULL
};

#define QC_JSON_NODE	16		// bytes a node
#define QC_JSON_DEPTH	128		// serde_json's recursion limit: 127 levels

typedef enum
{
	QC_JSON_OK,
	QC_JSON_INVALID,		// not JSON
	QC_JSON_TOO_LARGE,		// its layout past the limit
	QC_JSON_FAILED			// out of memory, or of temp strings (vm->error)
} qc_jsonresult_t;

typedef struct
{
	const char		*s;
	size_t			len, pos;
	int				depth;			// levels left
	bool			writing;		// pass 2

	// pass 1: the layout's size and the containers' child counts, pre-order
	size_t			nodes, names, limit;
	uint32_t		*counts;
	size_t			numcounts, countsize;

	// pass 2: the block being made, at base in the VM
	qcvm_t			*vm;
	uint8_t			*out;
	uint32_t		base;
	size_t			nextnode, nextname, nextcount;

	qc_jsonresult_t	result;
} qc_json_t;

static bool QC_JsonFail (qc_json_t *j, qc_jsonresult_t r)
{
	if (j->result == QC_JSON_OK)
		j->result = r;
	return false;
}

static void QC_JsonSpace (qc_json_t *j)
{
	char	c;

	while (j->pos < j->len && ((c = j->s[j->pos]) == ' ' || c == '\t' || c == '\n' || c == '\r'))
		j->pos++;
}

static int QC_JsonHex (char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
		return (c | 0x20) - 'a' + 10;
	return -1;
}

static bool QC_JsonHex4 (qc_json_t *j, uint32_t *v)
{
	int		k, d;

	if (j->len - j->pos < 4)
		return false;
	for (*v = 0, k = 0 ; k < 4 ; k++)
	{
		if ((d = QC_JsonHex (j->s[j->pos + k])) < 0)
			return false;
		*v = (*v << 4) | (uint32_t)d;
	}
	j->pos += 4;
	return true;
}

// a character as the VM stores text: UTF-8, a NUL as C0 80
static void QC_JsonPut (qc_sink_t *out, uint32_t ch)
{
	uint8_t	b[4];
	size_t	n;

	if (!ch)
	{
		QC_SinkAppend (out, "\xC0\x80", 2);
		return;
	}
	if (ch < 0x80)
	{
		b[0] = (uint8_t)ch;
		n = 1;
	}
	else if (ch < 0x800)
	{
		b[0] = (uint8_t)(0xC0 | (ch >> 6));
		b[1] = (uint8_t)(0x80 | (ch & 0x3F));
		n = 2;
	}
	else if (ch < 0x10000)
	{
		b[0] = (uint8_t)(0xE0 | (ch >> 12));
		b[1] = (uint8_t)(0x80 | ((ch >> 6) & 0x3F));
		b[2] = (uint8_t)(0x80 | (ch & 0x3F));
		n = 3;
	}
	else
	{
		b[0] = (uint8_t)(0xF0 | (ch >> 18));
		b[1] = (uint8_t)(0x80 | ((ch >> 12) & 0x3F));
		b[2] = (uint8_t)(0x80 | ((ch >> 6) & 0x3F));
		b[3] = (uint8_t)(0x80 | (ch & 0x3F));
		n = 4;
	}
	QC_SinkAppend (out, (const char *)b, n);
}

// the length of a well-formed UTF-8 sequence at s (no overlongs, surrogates or
// values past U+10FFFF), or 0
static size_t QC_JsonUtf8 (const uint8_t *s, size_t len)
{
	uint32_t	ch;
	size_t		n, i;

	if (s[0] < 0x80)
		return 1;
	if (s[0] >= 0xC2 && s[0] <= 0xDF)
		n = 2;
	else if (s[0] >= 0xE0 && s[0] <= 0xEF)
		n = 3;
	else if (s[0] >= 0xF0 && s[0] <= 0xF4)
		n = 4;
	else
		return 0;
	if (n > len)
		return 0;
	for (ch = s[0] & (0x7F >> n), i = 1 ; i < n ; i++)
	{
		if ((s[i] & 0xC0) != 0x80)
			return 0;
		ch = (ch << 6) | (s[i] & 0x3F);
	}
	if ((n == 3 && ch < 0x800) || (n == 4 && ch < 0x10000) || ch > 0x10FFFF || (ch >= 0xD800 && ch < 0xE000))
		return 0;
	return n;
}

// A string from its opening quote, unescaped into out (as the VM stores it)
static bool QC_JsonString (qc_json_t *j, qc_sink_t *out)
{
	uint32_t	ch, low;
	uint8_t		c;
	size_t		n;

	j->pos++;
	for ( ; ; )
	{
		if (j->pos >= j->len)
			return QC_JsonFail (j, QC_JSON_INVALID);
		c = (uint8_t)j->s[j->pos];
		if (c == '"')
		{
			j->pos++;
			return true;
		}
		if (c < 0x20)
			return QC_JsonFail (j, QC_JSON_INVALID);
		if (c != '\\')
		{
			if (!(n = QC_JsonUtf8 ((const uint8_t *)j->s + j->pos, j->len - j->pos)))
				return QC_JsonFail (j, QC_JSON_INVALID);
			QC_SinkAppend (out, j->s + j->pos, n);
			j->pos += n;
			continue;
		}
		if (++j->pos >= j->len)
			return QC_JsonFail (j, QC_JSON_INVALID);
		switch (j->s[j->pos++])
		{
		case '"':	QC_SinkPush (out, '"'); break;
		case '\\':	QC_SinkPush (out, '\\'); break;
		case '/':	QC_SinkPush (out, '/'); break;
		case 'b':	QC_SinkPush (out, '\b'); break;
		case 'f':	QC_SinkPush (out, '\f'); break;
		case 'n':	QC_SinkPush (out, '\n'); break;
		case 'r':	QC_SinkPush (out, '\r'); break;
		case 't':	QC_SinkPush (out, '\t'); break;
		case 'u':
			if (!QC_JsonHex4 (j, &ch))
				return QC_JsonFail (j, QC_JSON_INVALID);
			if (ch >= 0xDC00 && ch < 0xE000)
				return QC_JsonFail (j, QC_JSON_INVALID);
			if (ch >= 0xD800 && ch < 0xDC00)
			{
				// a high surrogate needs its low one
				if (j->len - j->pos < 2 || j->s[j->pos] != '\\' || j->s[j->pos + 1] != 'u')
					return QC_JsonFail (j, QC_JSON_INVALID);
				j->pos += 2;
				if (!QC_JsonHex4 (j, &low) || low < 0xDC00 || low >= 0xE000)
					return QC_JsonFail (j, QC_JSON_INVALID);
				ch = 0x10000 + ((ch - 0xD800) << 10) + (low - 0xDC00);
			}
			QC_JsonPut (out, ch);
			break;
		default:
			return QC_JsonFail (j, QC_JSON_INVALID);
		}
	}
}

static bool QC_JsonDigits (qc_json_t *j)
{
	size_t	start = j->pos;

	while (j->pos < j->len && j->s[j->pos] >= '0' && j->s[j->pos] <= '9')
		j->pos++;
	return j->pos > start;
}

// a number: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
static bool QC_JsonNumber (qc_json_t *j, double *value)
{
	size_t	start = j->pos, used;
	char	*text;

	if (j->s[j->pos] == '-')
		j->pos++;
	if (j->pos < j->len && j->s[j->pos] == '0')
		j->pos++;
	else if (!QC_JsonDigits (j))
		return QC_JsonFail (j, QC_JSON_INVALID);
	if (j->pos < j->len && j->s[j->pos] == '.')
	{
		j->pos++;
		if (!QC_JsonDigits (j))
			return QC_JsonFail (j, QC_JSON_INVALID);
	}
	if (j->pos < j->len && (j->s[j->pos] == 'e' || j->s[j->pos] == 'E'))
	{
		j->pos++;
		if (j->pos < j->len && (j->s[j->pos] == '+' || j->s[j->pos] == '-'))
			j->pos++;
		if (!QC_JsonDigits (j))
			return QC_JsonFail (j, QC_JSON_INVALID);
	}
	if (!j->writing)
		return true;
	if (!(text = malloc (j->pos - start + 1)))
		return QC_JsonFail (j, QC_JSON_FAILED);
	memcpy (text, j->s + start, j->pos - start);
	text[j->pos - start] = 0;
	*value = QC_Strtod (text, &used);
	free (text);
	return true;
}

static bool QC_JsonLiteral (qc_json_t *j, const char *word)
{
	size_t	n = strlen (word);

	if (j->len - j->pos < n || memcmp (j->s + j->pos, word, n))
		return QC_JsonFail (j, QC_JSON_INVALID);
	j->pos += n;
	return true;
}

/*
------------------------------------------------------------------------------
the layout (pass 2)
------------------------------------------------------------------------------
*/

static uint32_t QC_JsonAddr (const qc_json_t *j, size_t offset)
{
	return j->base + (uint32_t)offset;
}

// a name into the names area: its address (null for an empty one)
static uint32_t QC_JsonName (qc_json_t *j, const char *name, size_t len)
{
	size_t	at = j->nextname;

	if (!len)
		return 0;
	memcpy (j->out + at, name, len);
	j->out[at + len] = 0;
	j->nextname = at + len + 1;
	return QC_JsonAddr (j, at);
}

static void QC_JsonNode (qc_json_t *j, size_t slot, uint32_t type, uint32_t name, uint64_t value)
{
	uint8_t	*p = j->out + slot * QC_JSON_NODE;

	memcpy (p, &type, 4);
	memcpy (p + 4, &name, 4);
	memcpy (p + 8, &value, 8);
}

static uint64_t QC_JsonWords (uint32_t a, uint32_t b)
{
	return a | ((uint64_t)b << 32);
}

/*
------------------------------------------------------------------------------
the grammar, both passes
------------------------------------------------------------------------------
*/

static bool QC_JsonValue (qc_json_t *j, size_t slot, uint32_t name);

// a node with a name of namelen bytes: counted and checked against the limit
// (pass 1), or written into slot (pass 2)
static bool QC_JsonMember (qc_json_t *j, size_t namelen, size_t slot, uint32_t name)
{
	if (!j->writing)
	{
		j->nodes++;
		j->names += namelen ? namelen + 1 : 0;
		if (j->nodes > (SIZE_MAX - j->names) / QC_JSON_NODE || j->nodes * QC_JSON_NODE + j->names > j->limit)
			return QC_JsonFail (j, QC_JSON_TOO_LARGE);
	}
	return QC_JsonValue (j, slot, name);
}

// an object or array's children: its count slot (pass 1), or its children's
// slots (pass 2)
static bool QC_JsonOpen (qc_json_t *j, size_t *countslot, size_t *first, uint32_t *n)
{
	uint32_t	*grown;
	size_t		size;

	if (!--j->depth)
		return QC_JsonFail (j, QC_JSON_INVALID);
	if (!j->writing)
	{
		if (j->numcounts == j->countsize)
		{
			size = j->countsize ? j->countsize * 2 : 64;
			if (!(grown = realloc (j->counts, size * sizeof(*grown))))
				return QC_JsonFail (j, QC_JSON_FAILED);
			j->counts = grown;
			j->countsize = size;
		}
		*countslot = j->numcounts;
		j->counts[j->numcounts++] = 0;
		return true;
	}
	*n = j->nextcount < j->numcounts ? j->counts[j->nextcount++] : 0;
	*first = j->nextnode;
	j->nextnode += *n;
	return true;
}

static bool QC_JsonArray (qc_json_t *j, size_t slot, uint32_t name)
{
	size_t		countslot = 0, first = 0;
	uint32_t	n = 0, k = 0;
	char		digits[16];
	int			len;

	if (!QC_JsonOpen (j, &countslot, &first, &n))
		return false;
	if (j->writing)
		QC_JsonNode (j, slot, QC_JSON_ARRAY, name, QC_JsonWords (QC_JsonAddr (j, first * QC_JSON_NODE), n));
	j->pos++;
	QC_JsonSpace (j);
	if (j->pos < j->len && j->s[j->pos] == ']')
		j->pos++;
	else
	{
		for ( ; ; k++)
		{
			len = snprintf (digits, sizeof(digits), "%u", k);
			if (j->writing && (k >= n || !QC_JsonMember (j, 0, first + k, QC_JsonName (j, digits, (size_t)len))))
				return QC_JsonFail (j, QC_JSON_INVALID);
			if (!j->writing && !QC_JsonMember (j, (size_t)len, 0, 0))
				return false;
			QC_JsonSpace (j);
			if (j->pos >= j->len)
				return QC_JsonFail (j, QC_JSON_INVALID);
			if (j->s[j->pos] == ']')
			{
				j->pos++;
				k++;
				break;
			}
			if (j->s[j->pos++] != ',')
				return QC_JsonFail (j, QC_JSON_INVALID);
		}
	}
	if (!j->writing)
		j->counts[countslot] = k;
	else if (k != n)
		return QC_JsonFail (j, QC_JSON_INVALID);
	j->depth++;
	return true;
}

static bool QC_JsonObject (qc_json_t *j, size_t slot, uint32_t name)
{
	size_t		countslot = 0, first = 0;
	uint32_t	n = 0, k = 0, keyaddr;
	qc_sink_t	key;
	bool		ok;

	if (!QC_JsonOpen (j, &countslot, &first, &n))
		return false;
	if (j->writing)
		QC_JsonNode (j, slot, QC_JSON_OBJECT, name, QC_JsonWords (QC_JsonAddr (j, first * QC_JSON_NODE), n));
	j->pos++;
	QC_JsonSpace (j);
	if (j->pos < j->len && j->s[j->pos] == '}')
		j->pos++;
	else
	{
		for ( ; ; k++)
		{
			QC_JsonSpace (j);
			if (j->pos >= j->len || j->s[j->pos] != '"')
				return QC_JsonFail (j, QC_JSON_INVALID);
			QC_SinkInit (&key, SIZE_MAX);
			ok = QC_JsonString (j, &key);
			if (ok && key.failed)
				ok = QC_JsonFail (j, QC_JSON_FAILED);
			QC_JsonSpace (j);
			if (ok && (j->pos >= j->len || j->s[j->pos++] != ':'))
				ok = QC_JsonFail (j, QC_JSON_INVALID);
			if (ok && j->writing)
			{
				keyaddr = QC_JsonName (j, QC_SinkText (&key), key.len);
				ok = k < n ? QC_JsonMember (j, 0, first + k, keyaddr) : QC_JsonFail (j, QC_JSON_INVALID);
			}
			else if (ok)
				ok = QC_JsonMember (j, key.len, 0, 0);
			QC_SinkFree (&key);
			if (!ok)
				return false;
			QC_JsonSpace (j);
			if (j->pos >= j->len)
				return QC_JsonFail (j, QC_JSON_INVALID);
			if (j->s[j->pos] == '}')
			{
				j->pos++;
				k++;
				break;
			}
			if (j->s[j->pos++] != ',')
				return QC_JsonFail (j, QC_JSON_INVALID);
		}
	}
	if (!j->writing)
		j->counts[countslot] = k;
	else if (k != n)
		return QC_JsonFail (j, QC_JSON_INVALID);
	j->depth++;
	return true;
}

static bool QC_JsonValue (qc_json_t *j, size_t slot, uint32_t name)
{
	qc_sink_t	text;
	double		v = 0, one = 1, zero = 0;
	uint64_t	bits;
	uint32_t	ref;
	char		c;

	QC_JsonSpace (j);
	if (j->pos >= j->len)
		return QC_JsonFail (j, QC_JSON_INVALID);
	c = j->s[j->pos];
	switch (c)
	{
	case '{':
		return QC_JsonObject (j, slot, name);
	case '[':
		return QC_JsonArray (j, slot, name);
	case '"':
		QC_SinkInit (&text, SIZE_MAX);
		if (!QC_JsonString (j, &text) || text.failed)
		{
			QC_SinkFree (&text);
			return QC_JsonFail (j, text.failed ? QC_JSON_FAILED : QC_JSON_INVALID);
		}
		if (j->writing)
		{
			if (!(ref = QC_NewTemp (j->vm, QC_SinkText (&text), text.len)))
			{
				QC_SinkFree (&text);
				return QC_JsonFail (j, QC_JSON_FAILED);
			}
			QC_JsonNode (j, slot, QC_JSON_STRING, name, ref);
		}
		QC_SinkFree (&text);
		return true;
	case 't':
		if (!QC_JsonLiteral (j, "true"))
			return false;
		memcpy (&bits, &one, 8);
		if (j->writing)
			QC_JsonNode (j, slot, QC_JSON_TRUE, name, bits);
		return true;
	case 'f':
		if (!QC_JsonLiteral (j, "false"))
			return false;
		memcpy (&bits, &zero, 8);
		if (j->writing)
			QC_JsonNode (j, slot, QC_JSON_FALSE, name, bits);
		return true;
	case 'n':
		if (!QC_JsonLiteral (j, "null"))
			return false;
		if (j->writing)
			QC_JsonNode (j, slot, QC_JSON_NULL, name, 0);
		return true;
	default:
		if (c != '-' && !(c >= '0' && c <= '9'))
			return QC_JsonFail (j, QC_JSON_INVALID);
		if (!QC_JsonNumber (j, &v))
			return false;
		memcpy (&bits, &v, 8);
		if (j->writing)
			QC_JsonNode (j, slot, QC_JSON_NUMBER, name, bits);
		return true;
	}
}

// the whole document: one value and white space around it
static qc_jsonresult_t QC_JsonDocument (qc_json_t *j)
{
	j->pos = 0;
	j->depth = QC_JSON_DEPTH;
	j->nextnode = 1;
	j->nextcount = 0;
	j->result = QC_JSON_OK;
	if (QC_JsonMember (j, 0, 0, 0))
	{
		QC_JsonSpace (j);
		if (j->pos != j->len)
			QC_JsonFail (j, QC_JSON_INVALID);
	}
	return j->result;
}

/*
==============================================================================

BUILTINS

==============================================================================
*/

// jsonnode json_parse(string json): the document in a new heap block, its root
// node; null if the text isn't valid JSON
static bool QC_JsonParse (qcvm_t *vm)
{
	char			*text = QC_LibDup (QC_ArgString (vm, 0));
	const char		*s = text;
	qc_json_t		j = {0};
	qc_jsonresult_t	r;
	size_t			size;
	uint32_t		base = 0;

	QC_ReturnWord (vm, 0);
	if (!text)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HEAP, NULL);
	if (!strncmp (s, "\xEF\xBB\xBF", 3))
		s += 3;
	j.s = s;
	j.len = strlen (s);
	j.vm = vm;
	// measured first, so a document whose tree can't fit the heap costs nothing
	j.limit = vm->config.limits.heap_bytes;
	r = QC_JsonDocument (&j);
	if (r == QC_JSON_OK)
	{
		size = j.nodes * QC_JSON_NODE + j.names;
		if (size > UINT32_MAX || !(base = QC_LibHeapAlloc (vm, (uint32_t)size)) || !(j.out = calloc (size, 1)))
			r = QC_JSON_TOO_LARGE;
		else
		{
			j.writing = true;
			j.base = base;
			j.nextname = j.nodes * QC_JSON_NODE;
			r = QC_JsonDocument (&j);
			if (r == QC_JSON_OK)
				QC_WriteBytes (&vm->mem, base, j.out, (uint32_t)size, NULL);
		}
	}
	free (j.out);
	free (j.counts);
	free (text);
	if (r != QC_JSON_OK && base)
		QC_LibHeapFree (vm, base);		// not leaked
	switch (r)
	{
	case QC_JSON_OK:
		QC_ReturnWord (vm, base);
		return true;
	case QC_JSON_INVALID:
		return true;
	case QC_JSON_TOO_LARGE:
		return QC_LibSoftError (vm, "json_parse: out of memory");
	default:
		if (vm->error.kind == QC_ERR_NONE)
			QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HEAP, NULL);
		return false;
	}
}

// a node as it is in the VM's memory
typedef struct
{
	uint32_t	type, name, a, b;
} qc_jsonnode_t;

static bool QC_ReadNode (qcvm_t *vm, uint32_t p, qc_jsonnode_t *n)
{
	uint32_t	w[4];

	if (!QC_ReadBytes (&vm->mem, p, w, 16))
		return false;
	*n = (qc_jsonnode_t){w[0], w[1], w[2], w[3]};
	return true;
}

// The node argument i points at: the null node for a null pointer; false
// after a builtin error (the null node if it carries on, as FTE) for a pointer
// outside the VM's memory
static bool QC_NodeArg (qcvm_t *vm, int i, qc_jsonnode_t *n)
{
	uint32_t	p = QC_ArgWord (vm, i);

	*n = (qc_jsonnode_t){QC_JSON_NULL, 0, 0, 0};
	if (!p || QC_ReadNode (vm, p, n))
		return true;
	*n = (qc_jsonnode_t){QC_JSON_NULL, 0, 0, 0};
	return QC_LibSoftError (vm, "json: bad node pointer %#x", p);
}

static double QC_NodeNumber (const qc_jsonnode_t *n)
{
	return QC_BitsDouble (n->a | ((uint64_t)n->b << 32));
}

static bool QC_IsContainer (const qc_jsonnode_t *n)
{
	return n->type == QC_JSON_OBJECT || n->type == QC_JSON_ARRAY;
}

// json_type_e json_get_value_type(jsonnode node): 0 string, 1 number, 2 object,
// 3 array, 4 true, 5 false, 6 null (and for a null node)
static bool QC_JsonGetValueType (qcvm_t *vm)
{
	qc_jsonnode_t	n;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	QC_ReturnWord (vm, n.type);
	return true;
}

// string json_get_name(jsonnode node): its key in its parent (an array element's
// index), or null for the root
static bool QC_JsonGetName (qcvm_t *vm)
{
	qc_jsonnode_t	n;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	QC_ReturnWord (vm, n.name);
	return true;
}

// int json_get_integer(jsonnode node): a number truncated (1 and 0 for true and
// false), a string by atoi, else 0
static bool QC_JsonGetInteger (qcvm_t *vm)
{
	qc_jsonnode_t	n;
	int32_t			v = 0;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	if (n.type == QC_JSON_NUMBER || n.type == QC_JSON_TRUE || n.type == QC_JSON_FALSE)
		v = QC_DoubleToInt (QC_NodeNumber (&n));
	else if (n.type == QC_JSON_STRING)
		v = (int32_t)QC_Strtol (QC_String (vm, n.a), 10);
	QC_ReturnInt (vm, v);
	return true;
}

// float json_get_float(jsonnode node): a number (1 and 0 for true and false), a
// string by atof, else 0
static bool QC_JsonGetFloat (qcvm_t *vm)
{
	qc_jsonnode_t	n;
	float			v = 0;
	size_t			used;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	if (n.type == QC_JSON_NUMBER || n.type == QC_JSON_TRUE || n.type == QC_JSON_FALSE)
		v = (float)QC_NodeNumber (&n);
	else if (n.type == QC_JSON_STRING)
		v = (float)QC_Strtod (QC_String (vm, n.a), &used);
	QC_ReturnFloat (vm, v);
	return true;
}

// string json_get_string(jsonnode node): a string node's text, else null
static bool QC_JsonGetString (qcvm_t *vm)
{
	qc_jsonnode_t	n;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	QC_ReturnWord (vm, n.type == QC_JSON_STRING ? n.a : 0);
	return true;
}

// jsonnode json_find_object_child(jsonnode node, string key): an object's (or
// array's, named by index) first child of exactly that name, or null
static bool QC_JsonFindObjectChild (qcvm_t *vm)
{
	qc_jsonnode_t	n, child;
	uint32_t		k, p, found = 0;
	char			*key;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	if (!(key = QC_LibDup (QC_ArgString (vm, 1))))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	for (k = 0 ; QC_IsContainer (&n) && k < n.b ; k++)
	{
		if ((uint64_t)n.a + (uint64_t)k * 16 > UINT32_MAX)
			break;
		p = n.a + k * 16;
		if (!QC_ReadNode (vm, p, &child))
		{
			if (!QC_LibSoftError (vm, "json: bad node pointer %#x", p))
			{
				free (key);
				return false;
			}
			break;
		}
		if (!strcmp (QC_String (vm, child.name), key))
		{
			found = p;
			break;
		}
	}
	free (key);
	QC_ReturnWord (vm, found);
	return true;
}

// int json_get_length(jsonnode node): an object's or array's children, else 0
static bool QC_JsonGetLength (qcvm_t *vm)
{
	qc_jsonnode_t	n;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	QC_ReturnWord (vm, QC_IsContainer (&n) ? n.b : 0);
	return true;
}

// jsonnode json_get_child_at_index(jsonnode node, int index): an object's or
// array's index-th child, or null out of range
static bool QC_JsonGetChildAtIndex (qcvm_t *vm)
{
	qc_jsonnode_t	n;
	uint32_t		index;

	if (!QC_NodeArg (vm, 0, &n))
		return false;
	index = QC_ArgWord (vm, 1);
	QC_ReturnWord (vm, QC_IsContainer (&n) && index < n.b ? n.a + index * 16 : 0);
	return true;
}

static const qc_libentry_t	qc_json[] = {
	{"json_parse", QC_JsonParse, NULL, 0},
	{"json_free", QC_LibMemfree, NULL, 0},
	{"json_get_value_type", QC_JsonGetValueType, NULL, 0},
	{"json_get_name", QC_JsonGetName, NULL, 0},
	{"json_get_integer", QC_JsonGetInteger, NULL, 0},
	{"json_get_float", QC_JsonGetFloat, NULL, 0},
	{"json_get_string", QC_JsonGetString, NULL, 0},
	{"json_find_object_child", QC_JsonFindObjectChild, NULL, 0},
	{"json_get_length", QC_JsonGetLength, NULL, 0},
	{"json_get_child_at_index", QC_JsonGetChildAtIndex, NULL, 0},
};

bool QC_RegisterJson (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_json, sizeof(qc_json) / sizeof(qc_json[0]));
}
