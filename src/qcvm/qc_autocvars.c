// qc_autocvars.c -- keeping the progs' autocvars (autocvar_<name> globals) in
// step with the host's cvars

#include "qc_lib.h"

#include <stdio.h>
#include <stdlib.h>

// sets an autocvar global of progs pr from the host's cvar, parsed by its type;
// false when out of memory
static bool QC_SetAutocvar (qcvm_t *vm, uint32_t pr, const qc_def_t *d, const char *cvar)
{
	uint64_t	at = vm->progs[pr].gbase + (uint64_t)d->ofs * 4;
	const char	*text;
	size_t		used;
	float		v[3];
	uint32_t	s;

	if (!vm->host.cvar_string || !(text = vm->host.cvar_string (vm->ctx, cvar)))
		return true;
	switch (d->type)
	{
	case QC_EV_FLOAT:
		QC_SetS (&vm->mem, at, QC_FloatBits ((float)QC_Strtod (text, &used)));
		break;
	case QC_EV_INTEGER:
	case QC_EV_UINT:
		// C's atoi: strtol truncated to an int
		QC_SetS (&vm->mem, at, (uint32_t)QC_Strtol (text, 10));
		break;
	case QC_EV_VECTOR:
		QC_ParseVector (text, v);
		QC_SetS (&vm->mem, at, QC_FloatBits (v[0]));
		QC_SetS (&vm->mem, at + 4, QC_FloatBits (v[1]));
		QC_SetS (&vm->mem, at + 8, QC_FloatBits (v[2]));
		break;
	case QC_EV_STRING:
		if (!(s = QC_NewTemp (vm, text, strlen (text))))
			return false;
		QC_SetS (&vm->mem, at, s);
		break;
	default:
		break;
	}
	return true;
}

bool QC_SyncAutocvars (qcvm_t *vm)
{
	const qc_progs_t	*p;
	const char			*name;
	uint32_t			pr, i;

	for (pr = 0 ; pr < vm->numprogs ; pr++)
	{
		p = vm->progs[pr].progs;
		for (i = 0 ; i < p->numglobaldefs ; i++)
		{
			name = QC_Cstr (p, p->globaldefs[i].name);
			if (!strncmp (name, "autocvar_", 9) && !QC_SetAutocvar (vm, pr, &p->globaldefs[i], name + 9))
				return false;
		}
	}
	return true;
}

// by name, as FTE finds them when a cvar changes (the first global of the name)
bool QC_SyncAutocvar (qcvm_t *vm, const char *cvar)
{
	const qc_def_t	*d;
	uint32_t		pr;
	size_t			len = strlen (cvar);
	char			*name = malloc (len + 10);
	bool			ok = true;

	if (!name)
		return QC_Fail (vm, QC_ERR_OUT_OF_MEMORY, QC_RES_TEMP_STRINGS, NULL);
	memcpy (name, "autocvar_", 9);
	memcpy (name + 9, cvar, len + 1);
	for (pr = 0 ; ok && pr < vm->numprogs ; pr++)
		if ((d = QC_GlobalDefRaw (vm->progs[pr].progs, name)))
			ok = QC_SetAutocvar (vm, pr, d, cvar);
	free (name);
	return ok;
}
