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

static void AutocvarGlobals (qc_asm_t *a, void *ctx)
{
	uint32_t	zero[3] = {0, 0, 0};

	(void)ctx;
	QA_Global1 (a, "autocvar_f", QC_EV_FLOAT, QC_FloatBits (2.5f));
	QA_Global1 (a, "autocvar_i", QC_EV_INTEGER, 3);
	QA_Global (a, "autocvar_v", QC_EV_VECTOR, zero, 3);
	QA_Global1 (a, "autocvar_s", QC_EV_STRING, QA_String (a, "default"));
	QA_Global1 (a, "autocvar_unset", QC_EV_FLOAT, QC_FloatBits (7.0f));
}

static qc_word_t *Global (qh_t *h, const char *name)
{
	uint32_t	word = 0;

	QT_CHECK (QC_FindGlobal (h->vm, name, &word, NULL));
	return &QC_Globals (h->vm)[word];
}

// QC_SyncAutocvars copies the host's cvars into autocvar_* globals, parsed by
// type; cvars the host lacks keep the progs' default, and syncing again picks
// up changes
static void TestAutocvarsFollowTheHostsCvars (void)
{
	qh_t		*h = QH_New (QC_NUMBERING_CSQC, NULL, AutocvarGlobals, NULL);
	qc_word_t	*v;

	QH_SetCvar (h, "f", "0.25x");
	QH_SetCvar (h, "i", " -12abc");
	QH_SetCvar (h, "v", "'1 2 3'");
	QH_SetCvar (h, "s", "hello");
	QT_CHECK (QC_SyncAutocvars (h->vm));
	QT_EQ_F (Global (h, "autocvar_f")->f, 0.25);
	QT_EQ_I (Global (h, "autocvar_i")->i, -12);
	v = Global (h, "autocvar_v");
	QT_EQ_F (v[0].f, 1);
	QT_EQ_F (v[1].f, 2);
	QT_EQ_F (v[2].f, 3);
	QT_EQ_S (QC_String (h->vm, Global (h, "autocvar_s")->u), "hello");
	QT_EQ_F (Global (h, "autocvar_unset")->f, 7);

	// a changed cvar is picked up by the next sync; the string survives collections
	QH_SetCvar (h, "f", "9");
	QT_CHECK (QC_SyncAutocvars (h->vm));
	QC_CollectGarbage (h->vm);
	QT_EQ_F (Global (h, "autocvar_f")->f, 9);
	QT_EQ_S (QC_String (h->vm, Global (h, "autocvar_s")->u), "hello");

	// QC_SyncAutocvar updates one cvar's autocvar only
	QH_SetCvar (h, "f", "4");
	QH_SetCvar (h, "i", "5");
	QH_SetCvar (h, "s", "bye");
	QT_CHECK (QC_SyncAutocvar (h->vm, "i"));
	QT_EQ_I (Global (h, "autocvar_i")->i, 5);
	QT_EQ_F (Global (h, "autocvar_f")->f, 9);
	QT_EQ_S (QC_String (h->vm, Global (h, "autocvar_s")->u), "hello");
	QT_CHECK (QC_SyncAutocvar (h->vm, "s"));
	QT_EQ_S (QC_String (h->vm, Global (h, "autocvar_s")->u), "bye");
	QT_EQ_F (Global (h, "autocvar_f")->f, 9);
	// a cvar with no autocvar, or one the host lacks, changes nothing
	QT_CHECK (QC_SyncAutocvar (h->vm, "nothing"));
	QT_CHECK (QC_SyncAutocvar (h->vm, "unset"));
	QT_EQ_F (Global (h, "autocvar_unset")->f, 7);
	QH_Free (h);
}

int main (void)
{
	TestPrinting ();
	TestError ();
	TestObjerror ();
	TestCvars ();
	TestRegistercvar ();
	TestExtensionsCommandsAndFlags ();
	TestAutocvarsFollowTheHostsCvars ();
	return QT_Finish ("lib_hostcalls", "the builtins the host answers forward to it");
}
