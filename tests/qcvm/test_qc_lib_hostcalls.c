// test_qc_lib_hostcalls.c -- builtins that forward to the host: printing,
// errors, cvars, commands (qcvm-rs's tests/all/builtins_misc/hostcalls.rs)

#include "qc_harness.h"

#include <stdio.h>
#include <string.h>

// a recorded text, "" when nothing was recorded
static const char *Text (const qt_text_t *t)
{
	return t->text ? t->text : "";
}

static void AddHealth (qc_asm_t *a, void *ctx)
{
	(void)ctx;
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
}

static void TestPrinting (void)
{
	qh_t	*h = QH_Csqc ();

	QT_CHECK (QH_Call (h, "print", ARGS (QH_S (h, "a"), QH_S (h, "b"), QH_S (h, "c\n")), NULL));
	QT_CHECK (QH_Call (h, "print", ARGS (QH_S (h, "single")), NULL));
	QT_EQ_S (Text (&h->host.printed), "abc\nsingle");
	QT_CHECK (QH_Call (h, "cprint", ARGS (QH_S (h, "centre"), QH_S (h, "d")), NULL));
	QT_EQ_S (Text (&h->host.centerprinted), "centred");
	QT_CHECK (QH_Call (h, "localcmd", ARGS (QH_S (h, "map "), QH_S (h, "e1m1\n")), NULL));
	QT_EQ_S (Text (&h->host.localcmds), "map e1m1\n");
	// dprint only prints in developer mode
	QT_CHECK (QH_Call (h, "dprint", ARGS (QH_S (h, "hidden")), NULL));
	QT_EQ_S (Text (&h->host.dprinted), "");
	QC_SetDeveloper (h->vm, true);
	QT_CHECK (QH_Call (h, "dprint", ARGS (QH_S (h, "shown"), QH_S (h, "!")), NULL));
	QT_EQ_S (Text (&h->host.dprinted), "shown!");
	QH_Free (h);
}

// error is fatal, with the message
static void TestError (void)
{
	qh_t	*h = QH_Csqc ();

	QT_EQ_I (QH_Fails (h, "error", ARGS (QH_S (h, "bad "), QH_S (h, "thing"))), QC_ERR_QC);
	QT_EQ_S (QH_ErrorMessage (h), "bad thing");
	// also in developer mode
	QC_SetDeveloper (h->vm, true);
	QT_CHECK (QH_Fails (h, "error", ARGS (QH_S (h, "x"))) != QC_ERR_NONE);
	QH_Free (h);
}

// objerror dumps self and frees it
static void TestObjerror (void)
{
	qh_t		*h = QH_New (QC_NUMBERING_CSQC, NULL, AddHealth, NULL);
	qc_ent_t	e;
	uint32_t	ofs, type, self, health = QC_FloatBits (10);

	QT_CHECK (QC_Spawn (h->vm, &e));
	QT_CHECK (QC_FindField (h->vm, "health", &ofs, &type));
	QT_CHECK (QC_SetField (h->vm, e, ofs, 1, &health));
	QT_CHECK (QC_FindGlobal (h->vm, "self", &self, &type));
	QC_Globals (h->vm)[self].u = e;
	QT_EQ_I (QH_Fails (h, "objerror", ARGS (QH_S (h, "monster "), QH_S (h, "stuck"))), QC_ERR_QC);
	QT_EQ_S (QH_ErrorMessage (h), "monster stuck");
	if (QT_EQ_I (h->host.numdumps, 1))
	{
		QT_EQ_I (h->host.dumps[0].kind, QC_DUMP_OBJERROR);
		QT_EQ_S (h->host.dumps[0].text, "Entity 1:\n{\n\"health\" \"10\"\n}\n");
	}
	QT_CHECK (QC_IsFree (h->vm, e));		// self was freed
	QH_Free (h);
}

static void TestCvars (void)
{
	qh_t	*h = QH_Csqc ();

	QH_SetCvar (h, "sv_gravity", "800");
	QT_EQ_F (QH_Float (h, "cvar", ARGS (QH_S (h, "sv_gravity"))), 800);
	QT_EQ_F (QH_Float (h, "cvar", ARGS (QH_S (h, "nope"))), 0);
	QT_EQ_S (QH_String (h, "cvar_string", ARGS (QH_S (h, "sv_gravity"))), "800");
	QT_CHECK (QH_OptString (h, "cvar_string", ARGS (QH_S (h, "nope"))) == NULL);
	QT_CHECK (QH_Call (h, "cvar_set", ARGS (QH_S (h, "sv_gravity"), QH_S (h, "100")), NULL));
	QT_EQ_S (QH_Cvar (h, "sv_gravity"), "100");
	QT_EQ_F (QH_Float (h, "cvar_type", ARGS (QH_S (h, "sv_gravity"))), 1);
	QT_EQ_F (QH_Float (h, "cvar_type", ARGS (QH_S (h, "nope"))), 0);
	// the test host has empty defaults and no descriptions
	QT_EQ_S (QH_OptString (h, "cvar_defstring", ARGS (QH_S (h, "sv_gravity"))), "");
	QT_CHECK (QH_OptString (h, "cvar_defstring", ARGS (QH_S (h, "nope"))) == NULL);
	QT_CHECK (QH_OptString (h, "cvar_description", ARGS (QH_S (h, "sv_gravity"))) == NULL);
	QH_Free (h);
}

// registercvar takes the value with two arguments
static void TestRegistercvar (void)
{
	qh_t	*h = QH_Csqc ();

	// FTE ignores the value unless flags are passed too; this passes it on
	QT_EQ_F (QH_Float (h, "registercvar", ARGS (QH_S (h, "my_cvar"), QH_S (h, "5"))), 1);
	QT_EQ_S (QH_Cvar (h, "my_cvar"), "5");
	QT_EQ_F (QH_Float (h, "registercvar", ARGS (QH_S (h, "my_cvar"), QH_S (h, "6"))), 0);	// it exists
	QT_EQ_S (QH_Cvar (h, "my_cvar"), "5");
	QT_EQ_F (QH_Float (h, "registercvar", ARGS (QH_S (h, "other"), QH_S (h, "7"), F (32))), 1);
	QT_EQ_F (QH_Float (h, "registercvar", ARGS (QH_S (h, "bare"))), 1);
	QT_EQ_S (QH_Cvar (h, "bare"), "");
	QH_Free (h);
}

static void TestExtensionsCommandsAndFlags (void)
{
	qh_t	*h = QH_Csqc ();

	// the default host reports the extensions the standard library implements
	QT_EQ_F (QH_Float (h, "checkextension", ARGS (QH_S (h, "DP_QC_ASINACOSATANATAN2TAN"))), 1);
	QT_EQ_F (QH_Float (h, "checkextension", ARGS (QH_S (h, "NOT_AN_EXTENSION"))), 0);
	QT_EQ_F (QH_Float (h, "checkcommand", ARGS (QH_S (h, "quit"))), 0);
	QT_CHECK (QH_Call (h, "registercommand", ARGS (QH_S (h, "mycmd")), NULL));
	if (QT_EQ_I (h->host.numcommands, 1))
		QT_EQ_S (h->host.commands[0], "mycmd");
	QT_EQ_F (QH_Float (h, "isdemo", NOARGS), 0);
	QT_EQ_F (QH_Float (h, "isserver", NOARGS), 0);
	QT_EQ_F (QH_Float (h, "cvars_haveunsaved", NOARGS), 0);
	QH_Free (h);
}

// later: autocvars_follow_the_hosts_cvars (autocvars, Phase 14l)

int main (void)
{
	TestPrinting ();
	TestError ();
	TestObjerror ();
	TestCvars ();
	TestRegistercvar ();
	TestExtensionsCommandsAndFlags ();
	return QT_Finish ("lib_hostcalls", "the builtins the host answers forward to it");
}
