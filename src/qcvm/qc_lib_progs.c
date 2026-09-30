// qc_lib_progs.c -- addprogs: loading further progs into the VM (docs/spec/builtins.md)

#include "qc_lib.h"

#include <stdlib.h>

// float addprogs(string progsname): loads a progs through the host's
// load_progs and returns its number, or -1 if the host can't provide it or it
// doesn't fit; errors of its init function fail the call
static bool QC_AddProgsBuiltin (qcvm_t *vm)
{
	char		*name = QC_LibDup (QC_ArgString (vm, 0)), text[256];
	qc_progs_t	*p;
	uint32_t	pr;
	bool		ok;

	if (!name)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	p = vm->host.load_progs ? vm->host.load_progs (vm->ctx, name) : NULL;
	if (!p)
	{
		free (name);
		QC_ReturnFloat (vm, -1);
		return true;
	}
	ok = QC_AddProgs (vm, p, &pr);
	QC_ReleaseProgs (p);
	if (ok)
		QC_ReturnFloat (vm, (float)pr);
	else if (vm->error.kind == QC_ERR_OUT_OF_MEMORY)
	{
		QC_ErrorText (&vm->error, text, sizeof(text));
		QC_ClearError (&vm->error);
		QC_Warning (vm, "addprogs %s: %s", name, text);
		QC_ReturnFloat (vm, -1);
		ok = true;
	}
	free (name);
	return ok;
}

static const qc_libentry_t	qc_progs[] = {
	{"addprogs", QC_AddProgsBuiltin, NULL, 0},
};

bool QC_RegisterProgs (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_progs, sizeof(qc_progs) / sizeof(qc_progs[0]));
}
