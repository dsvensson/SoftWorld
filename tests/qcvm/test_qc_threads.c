// test_qc_threads.c -- QuakeC threads (sleep, fork) resumed by the host with
// QC_RunThreads (qcvm-rs's tests/all/threads.rs), with qc/threads.qc compiled
// by fteqcc when there is one

#include "qc_asm.h"
#include "qc_local.h"
#include "qc_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static qt_text_t	out;

static bool B_Puts (qcvm_t *vm)
{
	int	i;

	for (i = 0 ; i < QC_Argc (vm) ; i++)
		QT_TextAppend (&out, QC_ArgString (vm, i));
	return true;
}

// Rust's {} for an f32: the shortest text that reads back as the same float,
// without an exponent (enough for the values the fixture prints)
static bool B_Ftos (qcvm_t *vm)
{
	float	f = QC_ArgFloat (vm, 0);
	char	text[64];
	int		p;

	if (f == floorf (f) && fabsf (f) < 1e15f)
		snprintf (text, sizeof(text), "%.0f", (double)f);
	else
		for (p = 1 ; p <= 9 ; p++)
		{
			snprintf (text, sizeof(text), "%.*g", p, (double)f);
			if (strtof (text, NULL) == f)
				break;
		}
	return QC_ReturnString (vm, text, strlen (text));
}

static bool B_Abort (qcvm_t *vm)
{
	return QC_Abort (vm, QC_ArgValue (vm, 0));
}

static qc_builtins_t *Builtins (void)
{
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_SSQC);

	QC_BuiltinsSetNumbered (b, 1, "puts", B_Puts);
	QC_BuiltinsSetNumbered (b, 2, "ftos", B_Ftos);
	QC_BuiltinsSetNumbered (b, 211, "abort", B_Abort);
	return b;
}

static float CallFloat (qcvm_t *vm, const char *name)
{
	qc_value_t	ret = {{0}};

	QT_CHECK (QC_Call (vm, QC_FindFunction (vm, name), 0, NULL, &ret));
	return QC_BitsFloat (ret.w[0]);
}

// a resumed thread that ends with abort, or suspends again, leaves nothing
// behind for the next, unrelated host call
static void TestResumedThreadsDoNotLeakReturnValues (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_SSQC);
	qc_config_t		config;
	qcvm_t			*vm;
	uint32_t		sleep_g, abort_g, zero, n99, n42, ran, i;
	static const char	*workers[] = {"then_abort", "sleep_twice"};

	QA_Global (a, "time", QC_EV_FLOAT, NULL, 0);
	sleep_g = QA_Global1 (a, "sleep_g", QC_EV_FUNCTION, QA_Builtin (a, "sleep", 212, 1));
	abort_g = QA_Global1 (a, "abort_g", QC_EV_FUNCTION, QA_Builtin (a, "abort", 211, 1));
	zero = QA_Float (a, 0);
	n99 = QA_Float (a, 99);
	n42 = QA_Float (a, 42);
	QA_Function (a, "then_abort", NULL, 0, 0);
	QA_Emit (a, QOP_STORE_F, zero, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, sleep_g, 0, 0);
	QA_Emit (a, QOP_STORE_F, n99, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, abort_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_Function (a, "sleep_twice", NULL, 0, 0);
	QA_Emit (a, QOP_STORE_F, zero, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, sleep_g, 0, 0);
	QA_Emit (a, QOP_STORE_F, zero, QA_PARM (0), 0);
	QA_Emit (a, QOP_CALL1, sleep_g, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QA_Function (a, "answer", NULL, 0, 0);
	QA_Emit (a, QOP_RETURN, n42, 0, 0);

	QC_BuiltinsSetNumbered (b, 211, "abort", B_Abort);
	QC_DefaultConfig (&config, QC_SSQC);
	vm = QA_CreateVM (a, &config, b, NULL, NULL);
	for (i = 0 ; vm && i < 2 ; i++)
	{
		QT_CHECK (QC_Call (vm, QC_FindFunction (vm, workers[i]), 0, NULL, NULL));
		ran = 0;
		QT_CHECK (QC_RunThreads (vm, &ran));
		if (!QT_EQ_U (ran, 1))
			printf ("  %s\n", workers[i]);
		if (!QT_EQ_F (CallFloat (vm, "answer"), 42))
			printf ("  after %s\n", workers[i]);
	}
	// sleep_twice is asleep again
	if (vm)
		QT_EQ_U (QC_SleepingThreads (vm), 1);
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

// the compiled threads.qc, named on the command line ("" without fteqcc)
static const char	*threads_dat = "";

static qcvm_t *FixtureVM (const qc_builtins_t *b, const qc_config_t *config)
{
	qc_progs_t	*p;
	qcvm_t		*vm;
	uint8_t		*data;
	size_t		size;

	data = QT_LoadFile (threads_dat, &size);
	if (!QT_CHECK (data != NULL))
		return NULL;
	p = QC_LoadProgs (data, size, NULL);
	free (data);
	if (!QT_CHECK (p != NULL))
		return NULL;
	vm = QC_Create (p, b, config, NULL, NULL, NULL);
	QC_ReleaseProgs (p);
	QT_CHECK (vm != NULL);
	return vm;
}

static void TestSleepForkAndNestedSleeps (void)
{
	static const char	expected[] = "loop 0 at 0\n  label 0\nloop 1 at 1\n  label 10\nloop 2 at 2\n  label 20\n"
		"loop done\nmain continues\nparent after fork\nnested 103\nforked child at 5\n";
	qc_builtins_t	*b = Builtins ();
	qc_config_t		config;
	qcvm_t			*vm;
	uint32_t		time = 0;
	int				step;

	QC_DefaultConfig (&config, QC_SSQC);
	vm = FixtureVM (b, &config);
	if (vm && QT_CHECK (QC_FindGlobal (vm, "time", &time, NULL)))
	{
		out = (qt_text_t){0};
		QT_CHECK (QC_Call (vm, QC_FindFunction (vm, "main"), 0, NULL, NULL));
		QT_EQ_U (QC_SleepingThreads (vm), 1);
		for (step = 1 ; step <= 12 ; step++)
		{
			QC_Globals (vm)[time].f = (float)step * 0.5f;
			QT_CHECK (QC_RunThreads (vm, NULL));
			// temp strings referred to only by sleeping threads survive collections
			QC_CollectGarbage (vm);
		}
		QT_EQ_U (QC_SleepingThreads (vm), 0);
		QT_EQ_S (out.text ? out.text : "", expected);
		QT_TextFree (&out);
	}
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
}

// suspended threads are limited by the memory their snapshots hold
static void TestThreadMemoryIsLimited (void)
{
	qc_builtins_t		*b = Builtins ();
	qc_config_t			config;
	const qc_error_t	*e;
	qcvm_t				*vm;

	QC_DefaultConfig (&config, QC_SSQC);
	config.limits.thread_bytes = 8;
	vm = FixtureVM (b, &config);
	if (vm)
	{
		QT_CHECK (!QC_Call (vm, QC_FindFunction (vm, "main"), 0, NULL, NULL));
		e = QC_LastError (vm);
		QT_EQ_U (e->kind, QC_ERR_OUT_OF_MEMORY);
		QT_EQ_U (e->value, QC_RES_THREADS);
		QT_EQ_U (QC_SleepingThreads (vm), 0);
		QT_TextFree (&out);
	}
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
}

int main (int argc, char **argv)
{
	if (argc > 1)
		threads_dat = argv[1];
	TestResumedThreadsDoNotLeakReturnValues ();
	if (*threads_dat)
	{
		TestSleepForkAndNestedSleeps ();
		TestThreadMemoryIsLimited ();
	}
	else
		printf ("threads: skipping threads.qc (no fteqcc to compile it)\n");
	return QT_Finish ("threads", "threads sleep, fork and wake by time");
}
