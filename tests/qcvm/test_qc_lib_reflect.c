// test_qc_lib_reflect.c -- field reflection, and the savegame-style entity text
// of eprint and coredump (qcvm-rs's tests/all/builtins_misc/reflect.rs)

#include "qc_harness.h"

#include <stdio.h>
#include <string.h>

static void Setup (qc_asm_t *a, void *ctx)
{
	static const char	*named[] = {"findentityfield", "entityfieldref", NULL};

	(void)ctx;
	QA_Field (a, "classname", QC_EV_STRING, NULL);
	QA_Field (a, "health", QC_EV_FLOAT, NULL);
	QA_Field (a, "origin", QC_EV_VECTOR, NULL);
	QA_Field (a, "owner", QC_EV_ENTITY, NULL);
	QA_Field (a, "think", QC_EV_FUNCTION, NULL);
	QA_Field (a, "watched", QC_EV_FIELD, NULL);
	QA_Field (a, "count", QC_EV_INTEGER, NULL);
	QA_Field (a, "target", QC_EV_POINTER, NULL);
	// a saved global (flag 0x8000) and an unsaved one
	QA_Global1 (a, "score", QC_EV_FLOAT | 0x8000, QC_FloatBits (3.5f));
	QA_Global1 (a, "scratch", QC_EV_FLOAT, QC_FloatBits (1.0f));
	QA_Function (a, "monster_think", NULL, 0, 0);
	QA_Emit (a, QOP_DONE, 0, 0, 0);
	QH_Named (a, (void *)named);
}

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, Setup, NULL);
}

// a field's offset
static uint32_t Field (qh_t *h, const char *name)
{
	uint32_t	ofs = 0, type;

	QT_CHECK (QC_FindField (h->vm, name, &ofs, &type));
	return ofs;
}

static void SetWord (qh_t *h, qc_ent_t e, const char *name, uint32_t v)
{
	QT_CHECK (QC_SetField (h->vm, e, Field (h, name), 1, &v));
}

static void SetVector (qh_t *h, qc_ent_t e, const char *name, float x, float y, float z)
{
	uint32_t	v[3] = {QC_FloatBits (x), QC_FloatBits (y), QC_FloatBits (z)};

	QT_CHECK (QC_SetField (h->vm, e, Field (h, name), 3, v));
}

static qc_ent_t Spawn (qh_t *h)
{
	qc_ent_t	e = 0;

	QT_CHECK (QC_Spawn (h->vm, &e));
	return e;
}

// the last dump, which must be of a kind
static const char *LastDump (qh_t *h, qc_dumpkind_t kind)
{
	const qh_dump_t	*d;

	if (!QT_CHECK (h->host.numdumps > 0))
		return "";
	d = &h->host.dumps[h->host.numdumps - 1];
	QT_EQ_I (d->kind, kind);
	return d->text;
}

// the field table: every field, a vector four times (v, v_x, v_y, v_z)
static void TestFieldTableReflection (void)
{
	static const float	outside[] = {11, -1, 1e9f};
	qh_t	*h = Harness ();
	size_t	i;

	QT_EQ_F (QH_Float (h, "numentityfields", NOARGS), 11);
	QT_EQ_S (QH_String (h, "entityfieldname", ARGS (F (0))), "classname");
	QT_EQ_S (QH_String (h, "entityfieldname", ARGS (F (2))), "origin");
	QT_EQ_S (QH_String (h, "entityfieldname", ARGS (F (4))), "origin_y");
	QT_EQ_F (QH_Float (h, "entityfieldtype", ARGS (F (2))), 3);
	QT_EQ_F (QH_Float (h, "entityfieldtype", ARGS (F (3))), 2);
	QT_EQ_F (QH_Float (h, "entityfieldtype", ARGS (F (10))), 7);
	QT_EQ_F (QH_Float (h, "findentityfield", ARGS (QH_S (h, "origin_y"))), 4);
	QT_EQ_F (QH_Float (h, "findentityfield", ARGS (QH_S (h, "health"))), 1);
	QT_EQ_F (QH_Float (h, "findentityfield", ARGS (QH_S (h, "nope"))), 0);
	QT_EQ_I (QH_Int (h, "entityfieldref", ARGS (F (4))), (int32_t)Field (h, "origin_y"));
	// out of range: null, 0, 0
	for (i = 0 ; i < sizeof(outside) / sizeof(outside[0]) ; i++)
	{
		QT_EQ_U (QH_Word (h, "entityfieldname", ARGS (F (outside[i]))), 0);
		QT_EQ_F (QH_Float (h, "entityfieldtype", ARGS (F (outside[i]))), 0);
		QT_EQ_I (QH_Int (h, "entityfieldref", ARGS (F (outside[i]))), 0);
	}
	QH_Free (h);
}

// entityfieldref gives the field of an index, to read the field with
static void TestEntityfieldrefReadsFieldsByIndex (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	e = Spawn (h);
	float		index;

	SetWord (h, e, "health", QC_FloatBits (42));
	index = QH_Float (h, "findentityfield", ARGS (QH_S (h, "health")));
	QT_EQ_U ((uint32_t)QH_Int (h, "entityfieldref", ARGS (F (index))), Field (h, "health"));
	QH_Free (h);
}

// fills an entity with a value of each type
static void Fill (qh_t *h, qc_ent_t e)
{
	static const char	text[] = "mon\"st\\er\nx";

	SetWord (h, e, "classname", QC_Intern (h->vm, text, sizeof(text) - 1));
	SetWord (h, e, "health", QC_FloatBits (50));
	SetVector (h, e, "origin", 1, 2.5f, -3);
	SetWord (h, e, "owner", 2);
	SetWord (h, e, "think", QC_FindFunction (h->vm, "monster_think"));
	SetWord (h, e, "watched", Field (h, "health"));
	SetWord (h, e, "count", (uint32_t)-5);
	SetWord (h, e, "target", 0x1234);
}

// eprint writes the fields as a savegame does
static void TestEprint (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	e = Spawn (h), e2;

	Spawn (h);
	Fill (h, e);
	QT_CHECK (QH_Call (h, "eprint", ARGS (W (e)), NULL));
	QT_EQ_S (LastDump (h, QC_DUMP_ENTITY), "Entity 1:\n{\n"
		"\"classname\" \"mon\\\"st\\\\er\\nx\"\n"
		"\"health\" \"50\"\n"
		"\"origin\" \"1 2.5 -3\"\n"
		"\"owner\" \"2\"\n"
		"\"think\" \"0:monster_think\"\n"
		"\"watched\" \"health\"\n"
		"\"count\" \"-5\"\n"
		"\"target\" \"0x1234\"\n"
		"}\n\n");

	// zero fields are left out; integral floats and vectors print as integers,
	// others with %f and %g
	e2 = Spawn (h);
	SetWord (h, e2, "health", QC_FloatBits (0.5f));
	SetVector (h, e2, "origin", 1, -2, 1e10f);
	QT_CHECK (QH_Call (h, "eprint", ARGS (W (e2)), NULL));
	QT_EQ_S (LastDump (h, QC_DUMP_ENTITY), "Entity 3:\n{\n\"health\" \"0.500000\"\n\"origin\" \"1 -2 1e+10\"\n}\n\n");
	SetVector (h, e2, "origin", 0.1f, 100000, 1234567);
	SetWord (h, e2, "health", QC_FloatBits (-0.0f));
	QT_CHECK (QH_Call (h, "eprint", ARGS (W (e2)), NULL));
	// -0 isn't all zero bits, and prints as the integer 0
	QT_EQ_S (LastDump (h, QC_DUMP_ENTITY), "Entity 3:\n{\n\"health\" \"0\"\n\"origin\" \"0.1 100000 1.23457e+06\"\n}\n\n");
	QH_Free (h);
}

// eprint of a bad reference prints the world
static void TestEprintBadReference (void)
{
	qh_t	*h = Harness ();

	QT_CHECK (QH_Call (h, "eprint", ARGS (W (77)), NULL));
	QT_EQ_S (LastDump (h, QC_DUMP_ENTITY), "Entity 0:\n{\n}\n\n");
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_S (QH_WarningText (h, 0), "bad entity index 77");
	QH_Free (h);
}

// coredump lists the saved globals and the entities in use
static void TestCoredump (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	e = Spawn (h), doomed;
	const char	*text;

	Fill (h, e);
	doomed = Spawn (h);
	QC_Remove (h->vm, doomed, false);
	QT_CHECK (QH_Call (h, "coredump", NOARGS, NULL));
	text = LastDump (h, QC_DUMP_COREDUMP);
	if (!QT_CHECK (!strncmp (text, "general {\n", 10)))
		printf ("%s\n", text);
	QT_CHECK (QT_Contains (text, "\"numentities\" \"3\"\n"));
	QT_CHECK (QT_Contains (text, "stacktrace {\n"));
	// only the globals marked for saving
	QT_CHECK (QT_Contains (text, "globals 0 {\n\"score\" \"3.500000\"\n}\n"));
	QT_CHECK (!QT_Contains (text, "scratch"));
	QT_CHECK (QT_Contains (text, "entity 0{\n}\n"));
	QT_CHECK (QT_Contains (text, "entity 1{\n\"classname\""));
	QT_CHECK (QT_Contains (text, "\"health\" \"50\"\n"));
	// free entities are left out
	QT_CHECK (!QT_Contains (text, "entity 2{"));
	QH_Free (h);
}

int main (void)
{
	TestFieldTableReflection ();
	TestEntityfieldrefReadsFieldsByIndex ();
	TestEprint ();
	TestEprintBadReference ();
	TestCoredump ();
	return QT_Finish ("lib_reflect", "fields are found and written as FTE writes them");
}
