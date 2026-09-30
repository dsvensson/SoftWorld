// qc_lib_hostcalls.c -- builtins the host answers: print, dprint, error,
// objerror, the cvars, localcmd... (docs/spec/builtins.md)
//
// The variadic text builtins concatenate their string arguments first.

#include "qc_lib.h"

#include <stdlib.h>
#include <string.h>

// void print(string, ...)
static bool QC_Print (qcvm_t *vm)
{
	char	*text = QC_LibConcat (vm, 0, NULL);

	if (!text)
		return false;
	if (vm->host.print)
		vm->host.print (vm->ctx, text);
	free (text);
	return true;
}

// void dprint(string, ...): in developer mode only
static bool QC_Dprint (qcvm_t *vm)
{
	char	*text;

	if (!vm->config.developer)
		return true;
	if (!(text = QC_LibConcat (vm, 0, NULL)))
		return false;
	if (vm->host.dprint)
		vm->host.dprint (vm->ctx, text);
	free (text);
	return true;
}

// void cprint(string, ...): a centre print
static bool QC_Cprint (qcvm_t *vm)
{
	char	*text = QC_LibConcat (vm, 0, NULL);

	if (!text)
		return false;
	if (vm->host.centerprint)
		vm->host.centerprint (vm->ctx, text);
	free (text);
	return true;
}

// fails the call with QC_ERR_QC and the text
static bool QC_QuakeCError (qcvm_t *vm, char *text)
{
	QC_Fail (vm, QC_ERR_QC, 0, "%s", text);
	free (text);
	return false;
}

// void error(string, ...): the call fails with the message
static bool QC_ErrorBuiltin (qcvm_t *vm)
{
	char	*text = QC_LibConcat (vm, 0, NULL);

	return text ? QC_QuakeCError (vm, text) : false;
}

// void objerror(string, ...): self's fields to the host's dump, self removed,
// and the call fails with the message (fatal, as in FTE's CSQC)
static bool QC_Objerror (qcvm_t *vm)
{
	char		*text = QC_LibConcat (vm, 0, NULL);
	uint32_t	g, e;
	qc_sink_t	s;

	if (!text)
		return false;
	if (QC_LibGlobal (vm, "self", QC_EV_ENTITY, &g))
	{
		e = QC_GetS (&vm->mem, g);
		if (e >= vm->mem.num_edicts)
			e = 0;
		QC_SinkInit (&s, SIZE_MAX);
		QC_SinkPrintf (&s, "Entity %u:\n", e);
		QC_EntityBlock (vm, &s, e);
		if (vm->host.dump)
			vm->host.dump (vm->ctx, QC_DUMP_OBJERROR, QC_SinkText (&s));
		QC_SinkFree (&s);
		if (e)
			QC_Remove (vm, e, false);
	}
	return QC_QuakeCError (vm, text);
}

// void localcmd(string, ...): to the console's command buffer
static bool QC_Localcmd (qcvm_t *vm)
{
	char	*text = QC_LibConcat (vm, 0, NULL);

	if (!text)
		return false;
	if (vm->host.localcmd)
		vm->host.localcmd (vm->ctx, text);
	free (text);
	return true;
}

// float cvar(string name)
static bool QC_Cvar (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0);

	QC_ReturnFloat (vm, vm->host.cvar_float ? vm->host.cvar_float (vm->ctx, name) : 0.0f);
	return true;
}

// string cvar_string(string name): null if there's no such cvar
static bool QC_CvarString (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0);

	return QC_LibReturnOptString (vm, vm->host.cvar_string ? vm->host.cvar_string (vm->ctx, name) : NULL);
}

// void cvar_set(string name, string value)
static bool QC_CvarSet (qcvm_t *vm)
{
	char		*name;
	const char	*value;

	if (!vm->host.cvar_set)
		return true;
	// the second argument may take the first's string copy
	name = malloc (strlen (QC_ArgString (vm, 0)) + 1);
	if (!name)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	strcpy (name, QC_ArgString (vm, 0));
	value = QC_ArgString (vm, 1);
	vm->host.cvar_set (vm->ctx, name, value);
	free (name);
	return true;
}

static bool QC_CvarInfo (qcvm_t *vm, qc_cvarinfo_t *info)
{
	*info = (qc_cvarinfo_t){0};
	return vm->host.cvar_info && vm->host.cvar_info (vm->ctx, QC_ArgString (vm, 0), info);
}

// float cvar_type(string name): 0 no such cvar, else bits 1 exists, 2 archived,
// 4 private, 8 made by the engine, 16 has a description, 32 read-only
static bool QC_CvarType (qcvm_t *vm)
{
	qc_cvarinfo_t	info;

	QC_ReturnFloat (vm, QC_CvarInfo (vm, &info) ? (float)info.flags : 0.0f);
	return true;
}

// string cvar_defstring(string name): its default, or null
static bool QC_CvarDefstring (qcvm_t *vm)
{
	qc_cvarinfo_t	info;

	return QC_LibReturnOptString (vm, QC_CvarInfo (vm, &info) ? (info.defaultvalue ? info.defaultvalue : "") : NULL);
}

// string cvar_description(string name): its description, or null
static bool QC_CvarDescription (qcvm_t *vm)
{
	qc_cvarinfo_t	info;

	return QC_LibReturnOptString (vm, QC_CvarInfo (vm, &info) ? info.description : NULL);
}

// float registercvar(string name, string value, optional float flags): the cvar
// unless it exists; whether it was made. FTE ignores value without flags; this
// always takes it.
static bool QC_Registercvar (qcvm_t *vm)
{
	char		*name;
	const char	*value;
	uint32_t	flags = QC_Argc (vm) > 2 ? (uint32_t)QC_LibArgInt (vm, 2) : 0;
	bool		made = false;

	if (vm->host.register_cvar)
	{
		name = malloc (strlen (QC_ArgString (vm, 0)) + 1);
		if (!name)
			return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
		strcpy (name, QC_ArgString (vm, 0));
		value = QC_Argc (vm) > 1 ? QC_ArgString (vm, 1) : "";
		made = vm->host.register_cvar (vm->ctx, name, value, flags);
		free (name);
	}
	QC_LibReturnBool (vm, made);
	return true;
}

// float checkextension(string name): the host's answer (case-sensitive)
static bool QC_Checkextension (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0);

	QC_LibReturnBool (vm, vm->host.check_extension ? vm->host.check_extension (vm->ctx, name)
		: QC_StandardExtension (name));
	return true;
}

// float checkcommand(string name): 1 a command, 2 an alias, 3 a cvar, 0 none
static bool QC_Checkcommand (qcvm_t *vm)
{
	const char	*name = QC_ArgString (vm, 0);

	QC_ReturnFloat (vm, vm->host.check_command ? (float)vm->host.check_command (vm->ctx, name) : 0.0f);
	return true;
}

// void registercommand(string name): a console command that calls QuakeC
static bool QC_Registercommand (qcvm_t *vm)
{
	if (vm->host.register_command)
		vm->host.register_command (vm->ctx, QC_ArgString (vm, 0));
	return true;
}

// float isdemo(): 0 live, 1 a demo, 2 an MVD
static bool QC_Isdemo (qcvm_t *vm)
{
	QC_ReturnFloat (vm, vm->host.is_demo ? vm->host.is_demo (vm->ctx) : 0.0f);
	return true;
}

// float isserver(): whether a local server runs
static bool QC_Isserver (qcvm_t *vm)
{
	QC_LibReturnBool (vm, vm->host.is_server && vm->host.is_server (vm->ctx));
	return true;
}

// float cvars_haveunsaved(): whether archived cvars changed since the config was saved
static bool QC_CvarsHaveunsaved (qcvm_t *vm)
{
	QC_LibReturnBool (vm, vm->host.cvars_have_unsaved && vm->host.cvars_have_unsaved (vm->ctx));
	return true;
}

static const qc_libentry_t	qc_hostcalls[] = {
	{"print", QC_Print, NULL, 0},
	{"dprint", QC_Dprint, NULL, 0},
	{"cprint", QC_Cprint, NULL, 0},
	{"error", QC_ErrorBuiltin, NULL, 0},
	{"objerror", QC_Objerror, NULL, 0},
	{"localcmd", QC_Localcmd, NULL, 0},
	{"cvar", QC_Cvar, NULL, 0},
	{"cvar_string", QC_CvarString, NULL, 0},
	{"cvar_set", QC_CvarSet, NULL, 0},
	{"cvar_type", QC_CvarType, NULL, 0},
	{"cvar_defstring", QC_CvarDefstring, NULL, 0},
	{"cvar_description", QC_CvarDescription, NULL, 0},
	{"registercvar", QC_Registercvar, NULL, 0},
	{"checkextension", QC_Checkextension, NULL, 0},
	{"checkcommand", QC_Checkcommand, NULL, 0},
	{"registercommand", QC_Registercommand, NULL, 0},
	{"isdemo", QC_Isdemo, NULL, 0},
	{"isserver", QC_Isserver, NULL, 0},
	{"cvars_haveunsaved", QC_CvarsHaveunsaved, NULL, 0},
};

bool QC_RegisterHostcalls (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_hostcalls, sizeof(qc_hostcalls) / sizeof(qc_hostcalls[0]));
}
