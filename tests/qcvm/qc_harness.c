// qc_harness.c -- testing builtins directly

#include "qc_harness.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *QH_Dup (const char *s)
{
	size_t	len = strlen (s) + 1;
	char	*d = malloc (len);

	if (d)
		memcpy (d, s, len);
	return d;
}

/*
==============================================================================

THE HOST

==============================================================================
*/

static void QH_OnWarning (void *ctx, const qc_warning_t *w)
{
	qh_host_t		*host = ctx;
	qc_warning_t	kind = *w;
	char			text[1024];

	kind.backtrace.count = 0;		// the kind's text alone
	QC_WarningText (&kind, text, sizeof(text));
	host->warnings = realloc (host->warnings, sizeof(*host->warnings) * (size_t)(host->numwarnings + 1));
	host->warnings[host->numwarnings++] = (qh_warning_t){w->kind, QH_Dup (text)};
}

static void QH_OnPrint (void *ctx, const char *text)
{
	QT_TextAppend (&((qh_host_t *)ctx)->printed, text);
}

static void QH_OnDprint (void *ctx, const char *text)
{
	QT_TextAppend (&((qh_host_t *)ctx)->dprinted, text);
}

static void QH_OnCenterprint (void *ctx, const char *text)
{
	QT_TextAppend (&((qh_host_t *)ctx)->centerprinted, text);
}

static void QH_OnLocalcmd (void *ctx, const char *text)
{
	QT_TextAppend (&((qh_host_t *)ctx)->localcmds, text);
}

static void QH_OnDump (void *ctx, qc_dumpkind_t kind, const char *text)
{
	qh_host_t	*host = ctx;

	host->dumps = realloc (host->dumps, sizeof(*host->dumps) * (size_t)(host->numdumps + 1));
	host->dumps[host->numdumps++] = (qh_dump_t){kind, QH_Dup (text)};
}

static qh_cvar_t *QH_FindCvar (const qh_host_t *host, const char *name)
{
	int	i;

	for (i = 0 ; i < host->numcvars ; i++)
		if (!strcmp (host->cvars[i].name, name))
			return &host->cvars[i];
	return NULL;
}

static void QH_PutCvar (qh_host_t *host, const char *name, const char *value)
{
	qh_cvar_t	*c = QH_FindCvar (host, name);

	if (c)
	{
		free (c->value);
		c->value = QH_Dup (value);
		return;
	}
	host->cvars = realloc (host->cvars, sizeof(*host->cvars) * (size_t)(host->numcvars + 1));
	host->cvars[host->numcvars++] = (qh_cvar_t){QH_Dup (name), QH_Dup (value)};
}

// the value as a number if all of it (white space trimmed) is one, else 0
static float QH_OnCvarFloat (void *ctx, const char *name)
{
	const qh_cvar_t	*c = QH_FindCvar (ctx, name);
	char			*end;
	float			v;

	if (!c)
		return 0;
	v = strtof (c->value, &end);
	if (end == c->value)
		return 0;
	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
		end++;
	return *end ? 0 : v;
}

static const char *QH_OnCvarString (void *ctx, const char *name)
{
	const qh_cvar_t	*c = QH_FindCvar (ctx, name);

	return c ? c->value : NULL;
}

static void QH_OnCvarSet (void *ctx, const char *name, const char *value)
{
	QH_PutCvar (ctx, name, value);
}

static bool QH_OnCvarInfo (void *ctx, const char *name, qc_cvarinfo_t *info)
{
	if (!QH_FindCvar (ctx, name))
		return false;
	*info = (qc_cvarinfo_t){.flags = 1, .defaultvalue = "", .description = NULL};
	return true;
}

static bool QH_OnRegisterCvar (void *ctx, const char *name, const char *value, uint32_t flags)
{
	(void)flags;
	if (QH_FindCvar (ctx, name))
		return false;
	QH_PutCvar (ctx, name, value);
	return true;
}

static int QH_CompareNames (const void *a, const void *b)
{
	return strcmp (*(const char *const *)a, *(const char *const *)b);
}

// the cvars whose names start with pattern, sorted
static void QH_OnCvarList (void *ctx, const char *pattern, const char *antipattern,
	void (*add) (void *list, const char *name), void *list)
{
	const qh_host_t	*host = ctx;
	const char		**names = malloc (sizeof(*names) * (size_t)(host->numcvars + 1));
	int				i, n = 0;

	(void)antipattern;
	for (i = 0 ; i < host->numcvars ; i++)
		if (!strncmp (host->cvars[i].name, pattern, strlen (pattern)))
			names[n++] = host->cvars[i].name;
	qsort (names, (size_t)n, sizeof(*names), QH_CompareNames);
	for (i = 0 ; i < n ; i++)
		add (list, names[i]);
	free (names);
}

static void QH_OnRegisterCommand (void *ctx, const char *name)
{
	qh_host_t	*host = ctx;

	host->commands = realloc (host->commands, sizeof(*host->commands) * (size_t)(host->numcommands + 1));
	host->commands[host->numcommands++] = QH_Dup (name);
}

static bool QH_OnSimTime (void *ctx, double *time)
{
	const qh_host_t	*host = ctx;

	*time = host->sim_time;
	return host->has_sim_time;
}

static bool QH_OnCalendarTime (void *ctx, bool local, qc_calendar_t *out)
{
	const qh_host_t	*host = ctx;

	(void)local;
	*out = host->now;
	return host->has_now;
}

static uint8_t *QH_OnReadFile (void *ctx, const char *path, size_t *size)
{
	const qh_host_t	*host = ctx;
	uint8_t			*data;
	int				i;

	for (i = 0 ; i < host->numfiles ; i++)
		if (!strcmp (host->files[i].path, path))
		{
			data = malloc (host->files[i].size + 1);
			memcpy (data, host->files[i].data, host->files[i].size);
			*size = host->files[i].size;
			return data;
		}
	return NULL;
}

static const qc_host_t	qh_host = {
	.warning = QH_OnWarning,
	.print = QH_OnPrint,
	.dprint = QH_OnDprint,
	.centerprint = QH_OnCenterprint,
	.localcmd = QH_OnLocalcmd,
	.dump = QH_OnDump,
	.cvar_float = QH_OnCvarFloat,
	.cvar_string = QH_OnCvarString,
	.cvar_set = QH_OnCvarSet,
	.cvar_info = QH_OnCvarInfo,
	.register_cvar = QH_OnRegisterCvar,
	.cvar_list = QH_OnCvarList,
	.register_command = QH_OnRegisterCommand,
	.sim_time = QH_OnSimTime,
	.calendar_time = QH_OnCalendarTime,
	.read_file = QH_OnReadFile,
};

/*
==============================================================================

HARNESSES

==============================================================================
*/

qh_t *QH_NewWith (qc_numbering_t numbering, const qc_config_t *config, qc_builtins_t *builtins,
	qh_setup_t setup, void *ctx)
{
	static const char	*views[3] = {"v_forward", "v_right", "v_up"};
	qc_asm_t			*a = QA_New ();
	qh_t				*h = calloc (1, sizeof(*h));
	qc_config_t			defaults;
	const char			*name;
	uint32_t			i, number;
	bool				numbered;

	// the globals many builtins touch
	QA_Global (a, "self", QC_EV_ENTITY, NULL, 0);
	QA_Global (a, "other", QC_EV_ENTITY, NULL, 0);
	QA_Global (a, "time", QC_EV_FLOAT, NULL, 0);
	for (i = 0 ; i < 3 ; i++)
		QA_Global (a, views[i], QC_EV_VECTOR, NULL, 0);
	if (setup)
		setup (a, ctx);
	for (i = 0 ; i < QC_NumKnownBuiltins (numbering) ; i++)
		if (QC_KnownBuiltin (numbering, i, &name, &number, &numbered))
			QA_Builtin (a, name, numbered ? number : 0, 8);
	if (!config)
	{
		QC_DefaultConfig (&defaults, numbering == QC_NUMBERING_SSQC ? QC_SSQC
			: numbering == QC_NUMBERING_MENU ? QC_MENU : QC_CSQC);
		config = &defaults;
	}
	h->builtins = builtins;
	h->vm = QA_CreateVM (a, config, builtins, &qh_host, &h->host);
	QA_Free (a);
	return h;
}

qh_t *QH_New (qc_numbering_t numbering, const qc_config_t *config, qh_setup_t setup, void *ctx)
{
	return QH_NewWith (numbering, config, QC_BuiltinsStandard (numbering), setup, ctx);
}

qh_t *QH_Csqc (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, NULL, NULL);
}

void QH_Free (qh_t *h)
{
	qh_host_t	*host = &h->host;
	int			i;

	QC_Destroy (h->vm);
	QC_BuiltinsFree (h->builtins);
	QT_TextFree (&host->printed);
	QT_TextFree (&host->dprinted);
	QT_TextFree (&host->centerprinted);
	QT_TextFree (&host->localcmds);
	for (i = 0 ; i < host->numdumps ; i++)
		free (host->dumps[i].text);
	free (host->dumps);
	QH_ClearWarnings (h);
	for (i = 0 ; i < host->numcvars ; i++)
	{
		free (host->cvars[i].name);
		free (host->cvars[i].value);
	}
	free (host->cvars);
	for (i = 0 ; i < host->numcommands ; i++)
		free (host->commands[i]);
	free (host->commands);
	for (i = 0 ; i < host->numfiles ; i++)
	{
		free (host->files[i].path);
		free (host->files[i].data);
	}
	free (host->files);
	free (h->result);
	free (h);
}

void QH_Named (qc_asm_t *a, void *ctx)
{
	const char	**names = ctx;

	for ( ; *names ; names++)
		QA_Builtin (a, *names, 0, 8);
}

void QH_AddPeek (qc_asm_t *a)
{
	static const uint8_t	sizes[2] = {1, 1};
	qa_func_t				f = QA_Function (a, "peek", sizes, 2, 1);

	QA_Emit (a, QOP_LOADP_I, QA_Local (f, 0), QA_Local (f, 1), QA_Local (f, 2));
	QA_Emit (a, QOP_RETURN, QA_Local (f, 2), 0, 0);
}

qc_func_t QH_Func (qh_t *h, const char *name)
{
	qc_func_t	f = QC_FindFunction (h->vm, name);

	if (!f)
		printf ("  no builtin stub %s\n", name);
	return f;
}

qc_value_t QH_S (qh_t *h, const char *text)
{
	return QC_ValWord (QC_TempString (h->vm, text, strlen (text)));
}

/*
==============================================================================

CALLS

==============================================================================
*/

bool QH_Call (qh_t *h, const char *name, int argc, const qc_value_t *args, qc_value_t *ret)
{
	qc_value_t	unused;

	return QC_Call (h->vm, QH_Func (h, name), argc, args, ret ? ret : &unused);
}

qc_errkind_t QH_Fails (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	return QH_Call (h, name, argc, args, NULL) ? QC_ERR_NONE : QC_LastError (h->vm)->kind;
}

const char *QH_ErrorMessage (qh_t *h)
{
	const qc_error_t	*e = QC_LastError (h->vm);

	return e && e->message ? e->message : "";
}

qc_value_t QH_Raw (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	qc_value_t	ret = {{0, 0, 0}};
	char		text[1024];

	if (!QT_CHECK (QH_Call (h, name, argc, args, &ret)))
		printf ("  %s: %s\n", name, QC_ErrorText (QC_LastError (h->vm), text, sizeof(text)));
	return ret;
}

float QH_Float (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	return QC_BitsFloat (QH_Raw (h, name, argc, args).w[0]);
}

int32_t QH_Int (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	return (int32_t)QH_Raw (h, name, argc, args).w[0];
}

uint32_t QH_Word (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	return QH_Raw (h, name, argc, args).w[0];
}

void QH_Vector (qh_t *h, float out[3], const char *name, int argc, const qc_value_t *args)
{
	qc_value_t	r = QH_Raw (h, name, argc, args);
	int			k;

	for (k = 0 ; k < 3 ; k++)
		out[k] = QC_BitsFloat (r.w[k]);
}

const char *QH_Text (qh_t *h, uint32_t ref)
{
	return QC_String (h->vm, ref);
}

const char *QH_OptString (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	uint32_t	ref = QH_Word (h, name, argc, args);

	free (h->result);
	h->result = ref ? QH_Dup (QC_String (h->vm, ref)) : NULL;
	return h->result;
}

const char *QH_String (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	const char	*s = QH_OptString (h, name, argc, args);

	return s ? s : "";
}

uint32_t QH_ParmWord (qh_t *h, int i)
{
	return QC_Globals (h->vm)[4 + 3 * i].u;
}

int32_t QH_Peek (qh_t *h, uint32_t p, int32_t i)
{
	return QH_Int (h, "peek", ARGS (W (p), I (i)));
}

/*
==============================================================================

RECORDS

==============================================================================
*/

int QH_NumWarnings (const qh_t *h)
{
	return h->host.numwarnings;
}

const char *QH_WarningText (const qh_t *h, int i)
{
	return i >= 0 && i < h->host.numwarnings ? h->host.warnings[i].text : "";
}

void QH_ClearWarnings (qh_t *h)
{
	int	i;

	for (i = 0 ; i < h->host.numwarnings ; i++)
		free (h->host.warnings[i].text);
	free (h->host.warnings);
	h->host.warnings = NULL;
	h->host.numwarnings = 0;
}

void QH_SetCvar (qh_t *h, const char *name, const char *value)
{
	QH_PutCvar (&h->host, name, value);
}

const char *QH_Cvar (const qh_t *h, const char *name)
{
	const qh_cvar_t	*c = QH_FindCvar (&h->host, name);

	return c ? c->value : NULL;
}

void QH_AddFile (qh_t *h, const char *path, const void *data, size_t size)
{
	qh_host_t	*host = &h->host;
	uint8_t		*copy = malloc (size ? size : 1);

	memcpy (copy, data, size);
	host->files = realloc (host->files, sizeof(*host->files) * (size_t)(host->numfiles + 1));
	host->files[host->numfiles++] = (qh_file_t){QH_Dup (path), copy, size};
}
