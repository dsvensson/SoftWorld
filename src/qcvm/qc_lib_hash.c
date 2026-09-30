// qc_lib_hash.c -- hash tables (docs/spec/builtins.md)
//
// Tables are chained: a new entry goes to the head of its bucket, and a lookup
// finds the newest entry for a key first. Handles are floats from 1; handle 0
// is the gamestate table, which always exists (FTE keeps it across maps; this
// keeps it for the VM's life). Keys are case-sensitive; string values are
// copied, so they outlive the temp strings they came from.

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

#define QC_EV_STRING_TYPE	1
#define QC_EV_VECTOR_TYPE	3		// the default value type of tables made without one
#define QC_HASH_REPLACE		256		// hash_add: replace the newest entry for the key
#define QC_HASH_ADD			512		// hash_add: another entry for the key
#define QC_GAMESTATE_BUCKETS	256
#define QC_MAX_BUCKETS		(1 << 16)	// the size is only a hint

typedef struct
{
	char		*key;
	size_t		keylen;
	int32_t		type;
	char		*text;			// an EV_STRING value, copied; else NULL and words
	size_t		textlen;
	uint32_t	words[3];
} qc_hashentry_t;

typedef struct
{
	qc_hashentry_t	*entries;		// the newest last
	uint32_t		count, size;
} qc_bucket_t;

typedef struct
{
	int32_t		default_type;
	qc_bucket_t	*buckets;
	uint32_t	numbuckets;
	size_t		bytes;			// what the table is charged: its buckets and entries
} qc_hashtable_t;

struct qc_hashtables_s
{
	qc_hashtable_t	**tables;		// by handle - 1, NULL when free
	uint32_t		count;
	qc_hashtable_t	*gamestate;		// handle 0, made when first used
};

// what an entry is charged: its key and text, and four entry slots of its
// bucket (a bucket's capacity never passes four times its entries: it at most
// doubles as it grows, and shrinks when it falls below half full)
static size_t QC_EntryBytes (size_t keylen, size_t textlen)
{
	return sizeof(qc_hashentry_t) * 4 + keylen + textlen;
}

static void QC_FreeEntry (qc_hashentry_t *e)
{
	free (e->key);
	free (e->text);
}

static void QC_FreeTable (qc_hashtable_t *t)
{
	uint32_t	b, i;

	if (!t)
		return;
	for (b = 0 ; b < t->numbuckets ; b++)
	{
		for (i = 0 ; i < t->buckets[b].count ; i++)
			QC_FreeEntry (&t->buckets[b].entries[i]);
		free (t->buckets[b].entries);
	}
	free (t->buckets);
	free (t);
}

void QC_LibFreeHash (qc_std_t *std)
{
	uint32_t	i;

	if (!std->hash)
		return;
	for (i = 0 ; i < std->hash->count ; i++)
		QC_FreeTable (std->hash->tables[i]);
	free (std->hash->tables);
	QC_FreeTable (std->hash->gamestate);
	free (std->hash);
	std->hash = NULL;
}

static qc_hashtable_t *QC_NewTable (uint32_t buckets, int32_t type)
{
	qc_hashtable_t	*t = calloc (1, sizeof(*t));

	if (!t)
		return NULL;
	if (!buckets)
		buckets = 1;
	t->buckets = calloc (buckets, sizeof(*t->buckets));
	if (!t->buckets)
	{
		free (t);
		return NULL;
	}
	t->numbuckets = buckets;
	t->default_type = type;
	t->bytes = (size_t)buckets * sizeof(qc_bucket_t);
	return t;
}

// FNV-1a: the same on every platform and run
static uint32_t QC_BucketOf (const qc_hashtable_t *t, const char *key, size_t len)
{
	uint32_t	h = 0x811C9DC5u;
	size_t		i;

	for (i = 0 ; i < len ; i++)
		h = (h ^ (uint8_t)key[i]) * 0x01000193u;
	return h % t->numbuckets;
}

static qc_hashtables_t *QC_Tables (qcvm_t *vm)
{
	qc_std_t	*std = QC_LibState (vm);

	if (std && !std->hash && !(std->hash = calloc (1, sizeof(*std->hash))))
		QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
	return std ? std->hash : NULL;
}

// the table of a handle, or NULL
static qc_hashtable_t *QC_TableOf (qcvm_t *vm, int32_t handle)
{
	qc_hashtables_t	*h = QC_Tables (vm);

	if (!h)
		return NULL;
	if (!handle)
	{
		if (!h->gamestate)
			h->gamestate = QC_NewTable (QC_GAMESTATE_BUCKETS, QC_EV_STRING_TYPE);
		return h->gamestate;
	}
	return handle > 0 && (uint32_t)handle <= h->count ? h->tables[handle - 1] : NULL;
}

// The table argument i names: false, after a builtin error (with which FTE
// carries on without a table) when there is none; *t NULL if it carried on
static bool QC_TableArg (qcvm_t *vm, int i, qc_hashtable_t **t, int32_t *handle)
{
	*handle = QC_LibArgInt (vm, i);
	*t = QC_TableOf (vm, *handle);
	if (*t)
		return true;
	return QC_LibSoftError (vm, "hash: invalid hash table");
}

// the newest entry of key from the skip-th on (of type type unless 0), or NULL
static qc_hashentry_t *QC_Match (qc_hashtable_t *t, const char *key, size_t len, int32_t type, uint32_t skip,
	uint32_t *bucket, uint32_t *index)
{
	uint32_t		b = QC_BucketOf (t, key, len), i;
	qc_hashentry_t	*e;

	for (i = t->buckets[b].count ; i-- > 0 ; )
	{
		e = &t->buckets[b].entries[i];
		if (e->keylen != len || memcmp (e->key, key, len) || (type && e->type != type))
			continue;
		if (skip--)
			continue;
		*bucket = b;
		*index = i;
		return e;
	}
	return NULL;
}

static void QC_RemoveEntry (qc_hashtable_t *t, uint32_t b, uint32_t i)
{
	qc_bucket_t		*bucket = &t->buckets[b];
	qc_hashentry_t	*shrunk;

	t->bytes -= QC_EntryBytes (bucket->entries[i].keylen, bucket->entries[i].textlen);
	QC_FreeEntry (&bucket->entries[i]);
	memmove (bucket->entries + i, bucket->entries + i + 1, (bucket->count - i - 1) * sizeof(*bucket->entries));
	bucket->count--;
	// capacity given back once the bucket is under half full, so an entry's
	// charge stays an upper bound
	if (bucket->size > bucket->count * 2)
	{
		if (!bucket->count)
		{
			free (bucket->entries);
			bucket->entries = NULL;
			bucket->size = 0;
		}
		else if ((shrunk = realloc (bucket->entries, bucket->count * sizeof(*shrunk))))
		{
			bucket->entries = shrunk;
			bucket->size = bucket->count;
		}
	}
}

// returns an entry's value: a string as a new temp string, else its words
static bool QC_ReturnEntry (qcvm_t *vm, const qc_hashentry_t *e)
{
	if (e->text)
		return QC_ReturnString (vm, e->text, e->textlen);
	QC_ReturnRaw (vm, e->words);
	return true;
}

// hashtable hash_createtab(float size, optional float type = EV_VECTOR): a new
// table (a size under 4 is 64 buckets; type 0 is EV_VECTOR); 0 past the table or
// memory limits
static bool QC_HashCreatetab (qcvm_t *vm)
{
	int32_t			size = QC_LibArgInt (vm, 0), type = QC_Argc (vm) > 1 ? QC_LibArgInt (vm, 1) : QC_EV_VECTOR_TYPE;
	uint32_t		buckets = size < 4 ? 64 : size > QC_MAX_BUCKETS ? QC_MAX_BUCKETS : (uint32_t)size, i, live = 0;
	qc_hashtables_t	*h = QC_Tables (vm);
	qc_hashtable_t	*t, **grown;

	if (!h)
		return false;
	if (!type)
		type = QC_EV_VECTOR_TYPE;
	for (i = 0 ; i < h->count ; i++)
		live += h->tables[i] != NULL;
	QC_ReturnFloat (vm, 0);
	if (live >= vm->config.limits.hash_tables)
		return true;
	if (!(t = QC_NewTable (buckets, type)))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
	if (!QC_LibCharge (vm, t->bytes))
	{
		QC_FreeTable (t);
		QC_Warning (vm, "hash_createtab: out of memory for hash tables and string buffers");
		return true;
	}
	for (i = 0 ; i < h->count && h->tables[i] ; i++)
		;
	if (i == h->count)
	{
		if (!(grown = realloc (h->tables, (h->count + 1) * sizeof(*grown))))
		{
			QC_LibRelease (vm, t->bytes);
			QC_FreeTable (t);
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
		}
		h->tables = grown;
		h->count++;
	}
	h->tables[i] = t;
	QC_ReturnFloat (vm, (float)(i + 1));
	return true;
}

// void hash_destroytab(hashtable table): the gamestate table can't be destroyed
static bool QC_HashDestroytab (qcvm_t *vm)
{
	qc_hashtable_t	*t;
	int32_t			handle;

	if (!QC_TableArg (vm, 0, &t, &handle))
		return false;
	if (t && handle > 0)
	{
		QC_LibRelease (vm, t->bytes);
		QC_FreeTable (t);
		vm->std->hash->tables[handle - 1] = NULL;
	}
	return true;
}

// void hash_add(hashtable table, string key, __variant value, optional float
// typeandflags): stores a value (type flags & 255, 0 the table's). Unless
// HASH_ADD (512) is set without HASH_REPLACE (256), the newest entry of the key
// is replaced. Empty keys are ignored.
static bool QC_HashAdd (qcvm_t *vm)
{
	qc_hashtable_t	*t;
	int32_t			handle, flags, type;
	const char		*arg;
	qc_hashentry_t	e = {0}, *old = NULL, *grown;
	uint32_t		ob = 0, oi = 0, b, size;
	size_t			oldbytes;

	if (!QC_TableArg (vm, 0, &t, &handle))
		return false;
	arg = QC_ArgString (vm, 1);
	if (!t || !*arg)
		return true;
	flags = QC_FloatToInt (QC_LibOptFloat (vm, 3, 0));
	type = flags & 0xFF ? flags & 0xFF : t->default_type;
	e.keylen = strlen (arg);
	e.key = malloc (e.keylen + 1);
	if (!e.key)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
	memcpy (e.key, arg, e.keylen + 1);
	e.type = type;
	QC_ArgRaw (vm, 2, e.words);
	if (type == QC_EV_STRING_TYPE)
	{
		arg = QC_ArgString (vm, 2);
		e.textlen = strlen (arg);
		if (!(e.text = malloc (e.textlen + 1)))
		{
			QC_FreeEntry (&e);
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
		}
		memcpy (e.text, arg, e.textlen + 1);
		e.words[0] = e.words[1] = e.words[2] = 0;
	}
	if (!(flags & QC_HASH_ADD) || (flags & QC_HASH_REPLACE))
		old = QC_Match (t, e.key, e.keylen, 0, 0, &ob, &oi);
	oldbytes = old ? QC_EntryBytes (old->keylen, old->textlen) : 0;
	// the difference charged
	if (QC_EntryBytes (e.keylen, e.textlen) > oldbytes
		? !QC_LibCharge (vm, QC_EntryBytes (e.keylen, e.textlen) - oldbytes) : false)
	{
		QC_FreeEntry (&e);
		QC_Warning (vm, "hash_add: out of memory for hash tables and string buffers");
		return true;
	}
	if (QC_EntryBytes (e.keylen, e.textlen) < oldbytes)
		QC_LibRelease (vm, oldbytes - QC_EntryBytes (e.keylen, e.textlen));
	if (old)
		QC_RemoveEntry (t, ob, oi);
	b = QC_BucketOf (t, e.key, e.keylen);
	if (t->buckets[b].count == t->buckets[b].size)
	{
		size = t->buckets[b].size ? t->buckets[b].size * 2 : 1;
		if (!(grown = realloc (t->buckets[b].entries, size * sizeof(*grown))))
		{
			QC_LibRelease (vm, QC_EntryBytes (e.keylen, e.textlen));
			QC_FreeEntry (&e);
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
		}
		t->buckets[b].entries = grown;
		t->buckets[b].size = size;
	}
	t->buckets[b].entries[t->buckets[b].count++] = e;
	t->bytes += QC_EntryBytes (e.keylen, e.textlen);
	return true;
}

// __variant hash_get(hashtable table, string key, optional __variant default = 0,
// optional float requiretype = 0, optional float index = 0): the newest value of
// the key (entries of other types skipped when requiretype is set, and the first
// index matches), else default. Strings come back as temp strings.
static bool QC_HashGet (qcvm_t *vm)
{
	qc_hashtable_t	*t;
	int32_t			handle, type = QC_FloatToInt (QC_LibOptFloat (vm, 3, 0)), index = QC_FloatToInt (QC_LibOptFloat (vm, 4, 0));
	uint32_t		def[3] = {0, 0, 0}, b, i;
	const char		*key;
	qc_hashentry_t	*e;

	if (!QC_TableArg (vm, 0, &t, &handle))
		return false;
	if (QC_Argc (vm) > 2)
		QC_ArgRaw (vm, 2, def);
	key = QC_ArgString (vm, 1);
	e = t ? QC_Match (t, key, strlen (key), type, index > 0 ? (uint32_t)index : 0, &b, &i) : NULL;
	if (e)
		return QC_ReturnEntry (vm, e);
	QC_ReturnRaw (vm, def);
	return true;
}

// __variant hash_delete(hashtable table, string key): removes the key's newest
// entry (of any type) and returns its value, or zero
static bool QC_HashDelete (qcvm_t *vm)
{
	static const uint32_t	zero[3];
	qc_hashtable_t			*t;
	int32_t					handle;
	const char				*key;
	qc_hashentry_t			*e, copy;
	uint32_t				b, i;
	bool					ok;

	QC_ReturnRaw (vm, zero);
	if (!QC_TableArg (vm, 0, &t, &handle))
		return false;
	key = QC_ArgString (vm, 1);
	if (!t || !(e = QC_Match (t, key, strlen (key), 0, 0, &b, &i)))
		return true;
	// the value returned before the entry goes
	copy = *e;
	ok = QC_ReturnEntry (vm, &copy);
	QC_LibRelease (vm, QC_EntryBytes (e->keylen, e->textlen));
	QC_RemoveEntry (t, b, i);
	return ok;
}

// every entry in enumeration order: bucket by bucket, the newest first in each
static qc_hashentry_t *QC_EntryAt (qc_hashtable_t *t, uint32_t n)
{
	uint32_t	b;

	for (b = 0 ; b < t->numbuckets ; b++)
	{
		if (n < t->buckets[b].count)
			return &t->buckets[b].entries[t->buckets[b].count - 1 - n];
		n -= t->buckets[b].count;
	}
	return NULL;
}

// string hash_getkey(hashtable table, float index): the key of the index-th
// entry in enumeration order (unspecified, and changed by adds and deletes), or null
static bool QC_HashGetkey (qcvm_t *vm)
{
	static const uint32_t	zero[3];
	qc_hashtable_t			*t;
	int32_t					handle, index;
	qc_hashentry_t			*e;

	QC_ReturnRaw (vm, zero);
	if (!QC_TableArg (vm, 0, &t, &handle))
		return false;
	index = QC_LibArgInt (vm, 1);
	if (!t || index < 0 || !(e = QC_EntryAt (t, (uint32_t)index)))
		return true;
	return QC_ReturnString (vm, e->key, e->keylen);
}

// void hash_getcb(hashtable table, void(string key, __variant value) callback,
// optional string key): callback once each entry (or each entry of key), in
// enumeration order; the entries are copied first, so the callback may change
// the table. FTE ships this doing nothing; this does what it documents.
static bool QC_HashGetcb (qcvm_t *vm)
{
	qc_hashtable_t	*t;
	int32_t			handle;
	qc_func_t		callback = QC_ArgWord (vm, 1);
	char			*key = NULL;
	qc_hashentry_t	*snap = NULL, *e;
	uint32_t		n = 0, count = 0, i;
	qc_value_t		args[2];
	bool			ok = true;

	if (!QC_TableArg (vm, 0, &t, &handle))
		return false;
	if (!t)
		return true;
	if (QC_Argc (vm) > 2 && !(key = QC_LibDup (QC_ArgString (vm, 2))))
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
	for (i = 0 ; (e = QC_EntryAt (t, i)) ; i++)
		count++;
	if (count && !(snap = calloc (count, sizeof(*snap))))
		ok = QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
	for (i = 0 ; ok && i < count ; i++)
	{
		e = QC_EntryAt (t, i);
		if (key && strcmp (key, e->key))
			continue;
		snap[n] = *e;
		snap[n].key = QC_LibDup (e->key);
		snap[n].text = e->text ? QC_LibDup (e->text) : NULL;
		if (!snap[n].key || (e->text && !snap[n].text))
			ok = QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_HASH_TABLES, NULL);
		n++;
	}
	for (i = 0 ; ok && i < n ; i++)
	{
		args[0] = QC_ValWord (QC_TempString (vm, snap[i].key, snap[i].keylen));
		args[1] = snap[i].text ? QC_ValWord (QC_TempString (vm, snap[i].text, snap[i].textlen))
			: (qc_value_t){{snap[i].words[0], snap[i].words[1], snap[i].words[2]}};
		if (!args[0].w[0] || (snap[i].text && !args[1].w[0]))
			ok = false;
		else
			ok = QC_Call (vm, callback, 2, args, NULL);
	}
	for (i = 0 ; i < n ; i++)
		QC_FreeEntry (&snap[i]);
	free (snap);
	free (key);
	return ok;
}

static const qc_libentry_t	qc_hash[] = {
	{"hash_createtab", QC_HashCreatetab, NULL, 0},
	{"hash_destroytab", QC_HashDestroytab, NULL, 0},
	{"hash_add", QC_HashAdd, NULL, 0},
	{"hash_get", QC_HashGet, NULL, 0},
	{"hash_delete", QC_HashDelete, NULL, 0},
	{"hash_getkey", QC_HashGetkey, NULL, 0},
	{"hash_getcb", QC_HashGetcb, NULL, 293},
};

bool QC_RegisterHash (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_hash, sizeof(qc_hash) / sizeof(qc_hash[0]));
}
