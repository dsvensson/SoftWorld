// qc_lib_tokenize.c -- tokenize, tokenize_console, tokenizebyseparator, argv
// and the rest (docs/spec/strings.md)
//
// Each tokenizer replaces the VM's token list (FTE keeps one a process, this
// one a VM). Tokens remember the bytes they came from, for argv_start_index and
// argv_end_index. The list is charged against limits.container_bytes: a string
// that would need more is tokenized as far as it goes, with a warning.

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

#define QC_TOKEN_BYTES	sizeof(qc_token_t)
#define QC_TOKEN_MAX	65535		// FTE's 64 KiB token buffer: a longer word goes on as the next token
#define QC_CSTRING_MAX	65534		// the longest \"..." token

// a token list being made, with the storage it may still take
typedef struct
{
	qc_token_t	*list;
	uint32_t	count, size;
	size_t		left;
	bool		exceeded;		// a token didn't fit: the list is short
	bool		failed;			// out of memory
} qc_tokens_t;

// adds a token (text taken over, or freed), charging it and the list's growth;
// false once the budget is spent
static bool QC_AddToken (qc_tokens_t *t, qc_sink_t *text, size_t start, size_t end)
{
	size_t		cost = QC_TOKEN_BYTES * 2 + text->len;
	qc_token_t	*grown;
	uint32_t	size;
	char		*copy;

	if (t->exceeded || cost > t->left)
	{
		t->exceeded = true;
		QC_SinkFree (text);
		return false;
	}
	if (t->count == t->size)
	{
		size = t->size ? t->size * 2 : 16;
		grown = realloc (t->list, size * sizeof(*grown));
		if (!grown)
		{
			t->failed = t->exceeded = true;
			QC_SinkFree (text);
			return false;
		}
		t->list = grown;
		t->size = size;
	}
	copy = text->buf ? text->buf : calloc (1, 1);
	if (!copy || text->failed)
	{
		free (copy);
		t->failed = t->exceeded = true;
		return false;
	}
	t->left -= cost;
	t->list[t->count++] = (qc_token_t){copy, text->len, start, end};
	*text = (qc_sink_t){0};
	return true;
}

// characters that are tokens by themselves in QuakeC mode (and end words)
static bool QC_IsSingle (char c)
{
	return c == '\n' || c == '{' || c == '}' || c == '(' || c == ')' || c == '[' || c == ']' || c == '\''
		|| c == ':' || c == ',' || c == ';';
}

static int QC_HexDigit (char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
		return (c | 0x20) - 'a' + 10;
	return -1;
}

// a \"..." token from the " at: C's escapes (\n \t \r \xHH \" \\ \' \$, a
// backslash and newline going on to the next line, anything else ?); a \x that
// makes NUL or " ends it. Returns where it ends.
static size_t QC_ParseCString (const char *s, size_t len, size_t at, qc_sink_t *out)
{
	size_t	p = at + 1;
	char	c, e;
	int		d, k;

	for ( ; ; )
	{
		if (out->len >= QC_CSTRING_MAX)
			return p;
		if (p >= len)
			return len;
		c = s[p++];
		if (c == '\\')
		{
			if (p >= len)
				return len;
			e = s[p++];
			switch (e)
			{
			case '\r':
			case '\n':
				if (e == '\r' && p < len && s[p] == '\n')
					p++;
				continue;
			case 'n':	c = '\n'; break;
			case 't':	c = '\t'; break;
			case 'r':	c = '\r'; break;
			case 'x':
				for (c = 0, k = 0 ; k < 2 && p < len && (d = QC_HexDigit (s[p])) >= 0 ; k++, p++)
					c = (char)(((uint8_t)c << 4) | d);
				break;
			case '$':
			case '\\':
			case '\'':	c = e; break;
			case '"':
				QC_SinkPush (out, '"');
				continue;
			default:	c = '?'; break;
			}
		}
		if (c == '"' || !c)
			return p;
		QC_SinkPush (out, c);
	}
}

// One token at p (FTE's COM_StringParse): false when only white space and
// comments are left. qc picks tokenize's QuakeC mode (single-character tokens
// and single-quoted strings) over the console's (/* */ comments).
static bool QC_ParseToken (const char *s, size_t len, size_t p, bool qc, qc_sink_t *out, size_t *end)
{
	char	c, d, next;

	for ( ; ; )
	{
		while (p < len && (uint8_t)s[p] <= ' ' && s[p] != '\n')
			p++;
		if (p >= len)
			return false;
		c = s[p];
		if (c == '\n')
		{
			QC_SinkPush (out, '\n');
			*end = p + 1;
			return true;
		}
		next = p + 1 < len ? s[p + 1] : 0;
		if (c == '/' && next == '/')
		{
			while (p < len && s[p] != '\n')
				p++;
			continue;
		}
		if (!qc && c == '/' && next == '*')
		{
			for (p += 2 ; p + 1 < len && !(s[p] == '*' && s[p + 1] == '/') ; p++)
				;
			p = p + 1 < len ? p + 2 : len;
			continue;
		}
		break;
	}
	c = s[p];
	next = p + 1 < len ? s[p + 1] : 0;
	if (c == '\\' && next == '"')
	{
		*end = QC_ParseCString (s, len, p + 1, out);
		return true;
	}
	if (c == '"' || (qc && c == '\''))
	{
		// quoted: a doubled quote is a quote; the end of the string ends it
		for (p++ ; ; )
		{
			if (out->len >= QC_TOKEN_MAX)
				break;
			if (p >= len)
			{
				p = len;
				break;
			}
			d = s[p++];
			if (d == c)
			{
				if (p >= len || s[p] != c)
					break;
				p++;
			}
			QC_SinkPush (out, d);
		}
		*end = p;
		return true;
	}
	if (qc && QC_IsSingle (c))
	{
		QC_SinkPush (out, c);
		*end = p + 1;
		return true;
	}
	for ( ; ; )
	{
		if (out->len >= QC_TOKEN_MAX)
			break;
		QC_SinkPush (out, p < len ? s[p] : 0);
		p++;
		if (p >= len || (uint8_t)s[p] <= ' ' || (qc && QC_IsSingle (s[p])))
			break;
	}
	*end = p;
	return true;
}

// splits s as FTE's tokenize (qc) or tokenize_console does
static void QC_Split (const char *s, size_t len, bool qc, qc_tokens_t *t)
{
	size_t		p = 0, end;
	qc_sink_t	text;

	for ( ; ; )
	{
		while (p < len && (uint8_t)s[p] <= ' ')
			p++;
		if (p >= len)
			break;
		QC_SinkInit (&text, SIZE_MAX);
		if (!QC_ParseToken (s, len, p, qc, &text, &end))
		{
			QC_SinkFree (&text);
			break;
		}
		if (!QC_AddToken (t, &text, p, end))
			break;
		p = end;
	}
}

// Splits s at any of the separators (tried in order at each byte; empty ones
// ignored). An empty string has no tokens; otherwise its end ends the last.
static void QC_SplitBy (const char *s, size_t len, char *const *seps, const size_t *seplens, int numseps,
	qc_tokens_t *t)
{
	size_t		start = 0, p = 0;
	int			k;
	qc_sink_t	text;

	if (!len)
		return;
	while (p < len)
	{
		for (k = 0 ; k < numseps ; k++)
			if (seplens[k] && seplens[k] <= len - p && !memcmp (s + p, seps[k], seplens[k]))
				break;
		if (k == numseps)
		{
			p++;
			continue;
		}
		QC_SinkInit (&text, SIZE_MAX);
		QC_SinkAppend (&text, s + start, p - start);
		if (!QC_AddToken (t, &text, start, p))
			return;
		p += seplens[k];
		start = p;
	}
	QC_SinkInit (&text, SIZE_MAX);
	QC_SinkAppend (&text, s + start, len - start);
	QC_AddToken (t, &text, start, len);
}

// what a new list may take: the budget left once the current one is dropped
static bool QC_TokenBudget (qcvm_t *vm, qc_tokens_t *t)
{
	qc_std_t	*std = QC_LibState (vm);
	size_t		others;

	*t = (qc_tokens_t){0};
	if (!std)
		return false;
	others = std->container_bytes > std->token_bytes ? std->container_bytes - std->token_bytes : 0;
	t->left = vm->config.limits.container_bytes > others ? vm->config.limits.container_bytes - others : 0;
	return true;
}

// replaces the token list and returns its length; a warning if the budget cut it short
static bool QC_SetTokens (qcvm_t *vm, qc_tokens_t *t)
{
	qc_std_t	*std = vm->std;
	size_t		bytes = (size_t)t->count * QC_TOKEN_BYTES;
	uint32_t	i;

	if (t->failed)
	{
		for (i = 0 ; i < t->count ; i++)
			free (t->list[i].text);
		free (t->list);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	QC_LibRelease (vm, std->token_bytes);
	QC_LibFreeTokens (std);
	std->token_bytes = 0;
	for (i = 0 ; i < t->count ; i++)
		bytes += t->list[i].len + 1;
	// the budget left room for the list, so the charge succeeds
	if (t->count && !QC_LibCharge (vm, bytes))
	{
		for (i = 0 ; i < t->count ; i++)
			free (t->list[i].text);
		t->count = 0;
	}
	std->tokens = t->list;
	std->numtokens = t->count;
	std->token_bytes = t->count ? bytes : 0;
	if (t->exceeded)
		QC_Warning (vm, "tokenize: out of memory for the token list; the rest of the string is ignored");
	QC_ReturnFloat (vm, (float)t->count);
	return true;
}

// float tokenize(string): QuakeC's way: white space apart, "...", '...' and C's
// \"..." strings single tokens, { } ( ) [ ] : , ; tokens of their own, // a
// comment (the newline after it a token); the count
static bool QC_Tokenize (qcvm_t *vm)
{
	qc_tokens_t	t;
	const char	*s;

	if (!QC_TokenBudget (vm, &t))
		return false;
	s = QC_ArgString (vm, 0);
	QC_Split (s, strlen (s), true, &t);
	return QC_SetTokens (vm, &t);
}

// float tokenize_console(string): the console's way: white space apart, "..."
// and \"..." strings single tokens, // and /* */ comments; the count
static bool QC_TokenizeConsole (qcvm_t *vm)
{
	qc_tokens_t	t;
	const char	*s;

	if (!QC_TokenBudget (vm, &t))
		return false;
	s = QC_ArgString (vm, 0);
	QC_Split (s, strlen (s), false, &t);
	return QC_SetTokens (vm, &t);
}

// float tokenizebyseparator(string s, string separator...): apart at up to seven
// separators (nothing trimmed; empty tokens kept); the count
static bool QC_Tokenizebyseparator (qcvm_t *vm)
{
	int			argc = QC_Argc (vm) < 8 ? QC_Argc (vm) : 8, n = 0, i;
	char		*seps[7];
	size_t		seplens[7], len;
	qc_tokens_t	t;
	const char	*s;
	bool		ok = true;

	for (i = 1 ; i < argc && ok ; i++)
	{
		s = QC_ArgString (vm, i);
		seplens[n] = strlen (s);
		seps[n] = malloc (seplens[n] + 1);
		if (seps[n])
		{
			memcpy (seps[n], s, seplens[n] + 1);
			n++;
		}
		else
			ok = false;
	}
	if (!ok)
	{
		while (n)
			free (seps[--n]);
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	}
	if (QC_TokenBudget (vm, &t))
	{
		s = QC_ArgString (vm, 0);
		len = strlen (s);
		QC_SplitBy (s, len, seps, seplens, n, &t);
		ok = QC_SetTokens (vm, &t);
	}
	else
		ok = false;
	while (n)
		free (seps[--n]);
	return ok;
}

// the token at a QuakeC index: negative ones count from the end
static const qc_token_t *QC_TokenAt (qcvm_t *vm, int32_t index)
{
	qc_std_t	*std = vm->std;
	int64_t		i;

	if (!std)
		return NULL;
	i = index < 0 ? (int64_t)index + std->numtokens : index;
	return i >= 0 && i < std->numtokens ? &std->tokens[i] : NULL;
}

// string argv(float index): a token's copy (negative counts from the end), or
// null out of range
static bool QC_Argv (qcvm_t *vm)
{
	const qc_token_t	*t = QC_TokenAt (vm, QC_LibArgInt (vm, 0));

	if (!t)
	{
		QC_ReturnWord (vm, 0);
		return true;
	}
	return QC_ReturnString (vm, t->text, t->len);
}

// float argc(): how many tokens
static bool QC_ArgcBuiltin (qcvm_t *vm)
{
	QC_ReturnFloat (vm, vm->std ? (float)vm->std->numtokens : 0.0f);
	return true;
}

// float argv_start_index(float index): where a token started, or -1
static bool QC_ArgvStartIndex (qcvm_t *vm)
{
	const qc_token_t	*t = QC_TokenAt (vm, QC_LibArgInt (vm, 0));

	QC_ReturnFloat (vm, t ? (float)t->start : -1.0f);
	return true;
}

// float argv_end_index(float index): where a token ended, or -1
static bool QC_ArgvEndIndex (qcvm_t *vm)
{
	const qc_token_t	*t = QC_TokenAt (vm, QC_LibArgInt (vm, 0));

	QC_ReturnFloat (vm, t ? (float)t->end : -1.0f);
	return true;
}

static const qc_libentry_t	qc_tokenize[] = {
	{"tokenize", QC_Tokenize, NULL, 0},
	{"tokenize_console", QC_TokenizeConsole, NULL, 0},
	{"tokenizebyseparator", QC_Tokenizebyseparator, NULL, 0},
	{"argv", QC_Argv, NULL, 0},
	{"argc", QC_ArgcBuiltin, NULL, 0},
	{"argv_start_index", QC_ArgvStartIndex, NULL, 0},
	{"argv_end_index", QC_ArgvEndIndex, NULL, 0},
};

bool QC_RegisterTokenize (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_tokenize, sizeof(qc_tokenize) / sizeof(qc_tokenize[0]));
}
