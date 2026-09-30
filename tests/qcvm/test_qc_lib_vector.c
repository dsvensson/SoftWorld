// test_qc_lib_vector.c -- vector and angle builtins (qcvm-rs's
// tests/all/builtins_misc/vector.rs)

#include "qc_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI	3.14159265358979323846

static void Setup (qc_asm_t *a, void *ctx)
{
	static const char	*floats[] = {"ideal_yaw", "yaw_speed", "idealpitch", "pitch_speed"};
	size_t				i;

	(void)ctx;
	QA_Field (a, "angles", QC_EV_VECTOR, NULL);
	QA_Field (a, "gravitydir", QC_EV_VECTOR, NULL);
	for (i = 0 ; i < sizeof(floats) / sizeof(floats[0]) ; i++)
		QA_Field (a, floats[i], QC_EV_FLOAT, NULL);
}

static qh_t *Harness (void)
{
	return QH_New (QC_NUMBERING_CSQC, NULL, Setup, NULL);
}

// a global of the main progs' word
static uint32_t Global (qh_t *h, const char *name)
{
	uint32_t	word = 0, type;

	if (!QC_FindGlobal (h->vm, name, &word, &type))
		printf ("  no global %s\n", name);
	return word;
}

static uint32_t Field (qh_t *h, const char *name)
{
	uint32_t	ofs = 0, type;

	if (!QC_FindField (h->vm, name, &ofs, &type))
		printf ("  no field %s\n", name);
	return ofs;
}

// v_forward, v_right and v_up
static void View (qh_t *h, float v[3][3])
{
	static const char	*names[3] = {"v_forward", "v_right", "v_up"};
	qc_word_t			*g = QC_Globals (h->vm);
	uint32_t			word;
	int					i, k;

	for (i = 0 ; i < 3 ; i++)
	{
		word = Global (h, names[i]);
		for (k = 0 ; k < 3 ; k++)
			v[i][k] = g[word + (uint32_t)k].f;
	}
}

static bool Close (const float a[3], const float b[3])
{
	int	k;

	for (k = 0 ; k < 3 ; k++)
		if (!(fabsf (a[k] - b[k]) < 1e-5f))
			return false;
	return true;
}

static void CheckClose (const float a[3], float x, float y, float z, int line)
{
	float	b[3] = {x, y, z};

	if (!QT_Check (Close (a, b), "close", __FILE__, line))
		printf ("  (%.9g %.9g %.9g) != (%.9g %.9g %.9g)\n", (double)a[0], (double)a[1], (double)a[2], (double)x,
			(double)y, (double)z);
}
#define CLOSE(a, x, y, z)	CheckClose ((a), (x), (y), (z), __LINE__)

// exactly equal (as floats: -0 == 0)
static void CheckEq (const float a[3], float x, float y, float z, int line)
{
	if (!QT_Check (a[0] == x && a[1] == y && a[2] == z, "equal", __FILE__, line))
		printf ("  (%.9g %.9g %.9g) != (%.9g %.9g %.9g)\n", (double)a[0], (double)a[1], (double)a[2], (double)x,
			(double)y, (double)z);
}
#define EQ3(a, x, y, z)	CheckEq ((a), (x), (y), (z), __LINE__)

static void Vec (qh_t *h, float out[3], const char *name, int argc, const qc_value_t *args)
{
	QH_Vector (h, out, name, argc, args);
}

static void Call (qh_t *h, const char *name, int argc, const qc_value_t *args)
{
	QH_Raw (h, name, argc, args);
}

static void TestLengthsAndNormals (void)
{
	qh_t	*h = Harness ();
	float	v[3];

	QT_EQ_F (QH_Float (h, "vlen", ARGS (V (3, 4, 0))), 5);
	QT_EQ_F (QH_Float (h, "vlen", ARGS (V (0, 0, 0))), 0);
	Vec (h, v, "normalize", ARGS (V (3, 4, 0)));
	EQ3 (v, 0.6f, 0.8f, 0);
	Vec (h, v, "normalize", ARGS (V (0, 0, -2)));
	EQ3 (v, 0, 0, -1);
	Vec (h, v, "normalize", ARGS (V (0, 0, 0)));
	EQ3 (v, 0, 0, 0);
	Vec (h, v, "crossproduct", ARGS (V (1, 0, 0), V (0, 1, 0)));
	EQ3 (v, 0, 0, 1);
	Vec (h, v, "crossproduct", ARGS (V (1, 2, 3), V (4, 5, 6)));
	EQ3 (v, -3, 6, -3);
	QH_Free (h);
}

static void TestMakevectors (void)
{
	qh_t	*h = Harness ();
	float	v[3][3];

	Call (h, "makevectors", ARGS (V (0, 0, 0)));
	View (h, v);
	EQ3 (v[0], 1, 0, 0);
	EQ3 (v[1], 0, -1, 0);
	EQ3 (v[2], 0, 0, 1);
	Call (h, "makevectors", ARGS (V (0, 90, 0)));
	View (h, v);
	CLOSE (v[0], 0, 1, 0);
	CLOSE (v[1], 1, 0, 0);
	CLOSE (v[2], 0, 0, 1);
	// positive pitch looks down
	Call (h, "makevectors", ARGS (V (30, 0, 0)));
	View (h, v);
	CLOSE (v[0], 0.8660254f, 0, -0.5f);
	CLOSE (v[2], 0.5f, 0, 0.8660254f);
	// roll tilts right and up
	Call (h, "makevectors", ARGS (V (0, 0, 90)));
	View (h, v);
	CLOSE (v[1], 0, 0, -1);
	CLOSE (v[2], 0, -1, 0);
	QH_Free (h);
}

// makevectors without the view globals is fatal, even in developer mode
static void TestMakevectorsWithoutView (void)
{
	qc_asm_t		*a = QA_New ();
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	qcvm_t			*vm;
	qc_func_t		f;
	qc_value_t		arg = V (0, 0, 0), ret;

	QA_Builtin (a, "makevectors", 1, 1);
	vm = QA_CreateVM (a, NULL, b, NULL, NULL);
	f = QC_FindFunction (vm, "makevectors");
	QT_CHECK (!QC_Call (vm, f, 1, &arg, &ret));
	QT_EQ_I (QC_LastError (vm)->kind, QC_ERR_HOST);
	QT_CHECK (QT_Contains (QC_LastError (vm)->message ? QC_LastError (vm)->message : "", "v_forward"));
	QC_SetDeveloper (vm, true);
	QT_CHECK (!QC_Call (vm, f, 1, &arg, &ret));
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	QA_Free (a);
}

static void TestVectoyaw (void)
{
	static const float	cases[][4] = {{1, 0, 0, 0}, {0, 1, 0, 90}, {-1, 0, 0, 180}, {0, -1, 0, 270}, {0, 0, 1, 0},
		{0, 0, 0, 0},
		// 5.71 degrees truncates to 5; -5.71 to -5, i.e. 355
		{1, 0.1f, 0, 5}, {1, -0.1f, 0, 355}};
	qh_t	*h = Harness ();
	float	diagonal;
	size_t	i;

	for (i = 0 ; i < sizeof(cases) / sizeof(cases[0]) ; i++)
		if (!QT_EQ_F (QH_Float (h, "vectoyaw", ARGS (V (cases[i][0], cases[i][1], cases[i][2]))), cases[i][3]))
			printf ("  vectoyaw(%g %g %g)\n", (double)cases[i][0], (double)cases[i][1], (double)cases[i][2]);
	// the degrees are computed in double precision, then truncated
	diagonal = (float)trunc (atan2 (1.0, 1.0) * 180.0 / PI);
	QT_EQ_F (QH_Float (h, "vectoyaw", ARGS (V (1, 1, 5))), diagonal);
	QH_Free (h);
}

// vectoyaw measures in the reference entity's surface frame
static void TestVectoyawSurface (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	e = 0;
	uint32_t	grav = Field (h, "gravitydir"), down[3], up[3];

	QT_CHECK (QC_Spawn (h->vm, &e));
	// no gravitydir: the world frame
	QT_EQ_F (QH_Float (h, "vectoyaw", ARGS (V (0, 1, 0), W (e))), 90);
	// normal gravity: still the world frame
	down[0] = QC_FloatBits (0);
	down[1] = QC_FloatBits (0);
	down[2] = QC_FloatBits (-1);
	QC_SetField (h->vm, e, grav, 3, down);
	QT_EQ_F (QH_Float (h, "vectoyaw", ARGS (V (0, 1, 0), W (e))), 90);
	// upside down: y is mirrored
	up[0] = QC_FloatBits (0);
	up[1] = QC_FloatBits (0);
	up[2] = QC_FloatBits (1);
	QC_SetField (h->vm, e, grav, 3, up);
	QT_EQ_F (QH_Float (h, "vectoyaw", ARGS (V (0, 1, 0), W (e))), 270);
	QH_Free (h);
}

// vectoangles doesn't truncate, and pitches up
static void TestVectoangles (void)
{
	qh_t	*h = Harness ();
	float	v[3];

	Vec (h, v, "vectoangles", ARGS (V (1, 0, 0)));
	EQ3 (v, 0, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (0, 0, 1)));
	EQ3 (v, 90, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (0, 0, -1)));
	EQ3 (v, 270, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (0, 0, 0)));
	EQ3 (v, 270, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (-1, 0, 0)));
	EQ3 (v, 0, 180, 0);
	Vec (h, v, "vectoangles", ARGS (V (1, 1, 0)));
	CLOSE (v, 0, 45, 0);
	Vec (h, v, "vectoangles", ARGS (V (1, 0, 1)));
	CLOSE (v, 45, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (1, 0, -1)));
	CLOSE (v, 315, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (0, -1, 0)));
	CLOSE (v, 0, 270, 0);
	// not truncated
	Vec (h, v, "vectoangles", ARGS (V (1, 0.1f, 0)));
	if (!QT_CHECK (fabsf (v[1] - 5.710593f) < 1e-4f))
		printf ("  yaw %.9g\n", (double)v[1]);
	QH_Free (h);
}

// vectoangles with an up vector sets the roll
static void TestVectoanglesRoll (void)
{
	qh_t	*h = Harness ();
	float	v[3], view[3][3];
	int		k;

	Vec (h, v, "vectoangles", ARGS (V (1, 0, 0), V (0, 0, 1)));
	CLOSE (v, 0, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (1, 0, 0), V (0, 0, -1)));
	CLOSE (v, 0, 0, 180);
	Vec (h, v, "vectoangles", ARGS (V (1, 0, 0), V (0, 1, 0)));
	CLOSE (v, 0, 0, 270);
	// straight up or down: the up vector gives the yaw
	Vec (h, v, "vectoangles", ARGS (V (0, 0, 1), V (-1, 0, 0)));
	CLOSE (v, 90, 0, 0);
	Vec (h, v, "vectoangles", ARGS (V (0, 0, 1), V (0, -1, 0)));
	CLOSE (v, 90, 90, 0);
	Vec (h, v, "vectoangles", ARGS (V (0, 0, -1), V (0, 1, 0)));
	CLOSE (v, 270, 90, 0);
	// a round trip through makevectors (whose pitch sign is the opposite)
	Call (h, "makevectors", ARGS (V (-20, 30, 40)));
	View (h, view);
	Vec (h, v, "vectoangles", ARGS (V (view[0][0], view[0][1], view[0][2]), V (view[2][0], view[2][1], view[2][2])));
	for (k = 0 ; k < 3 ; k++)
		if (!QT_CHECK (fabsf (v[k] - (float)(20 + 10 * k)) < 1e-3f))
			printf ("  angle %d is %.9g\n", k, (double)v[k]);
	QH_Free (h);
}

static void TestVectorvectors (void)
{
	qh_t	*h = Harness ();
	float	v[3][3];

	Call (h, "vectorvectors", ARGS (V (2, 0, 0)));
	View (h, v);
	EQ3 (v[0], 1, 0, 0);
	EQ3 (v[1], 0, -1, 0);
	EQ3 (v[2], 0, 0, 1);
	Call (h, "vectorvectors", ARGS (V (0, 0, 5)));
	View (h, v);
	EQ3 (v[0], 0, 0, 1);
	EQ3 (v[1], 0, -1, 0);
	EQ3 (v[2], -1, 0, 0);
	Call (h, "vectorvectors", ARGS (V (0, 0, 0)));
	View (h, v);
	EQ3 (v[0], 0, 0, 0);
	EQ3 (v[1], 0, 0, 0);
	EQ3 (v[2], 0, 0, 0);
	Call (h, "vectorvectors", ARGS (V (3, 4, 0)));
	View (h, v);
	CLOSE (v[0], 0.6f, 0.8f, 0);
	CLOSE (v[1], 0.8f, -0.6f, 0);
	CLOSE (v[2], 0, 0, 1);
	QH_Free (h);
}

// rotatevectorsbyangle uses model pitch
static void TestRotateByAngle (void)
{
	qh_t	*h = Harness ();
	float	rotated[3][3], direct[3][3];
	int		i;

	Call (h, "makevectors", ARGS (V (0, 0, 0)));
	Call (h, "rotatevectorsbyangle", ARGS (V (0, 90, 0)));
	View (h, rotated);
	Call (h, "makevectors", ARGS (V (0, 90, 0)));
	View (h, direct);
	for (i = 0 ; i < 3 ; i++)
		CLOSE (rotated[i], direct[i][0], direct[i][1], direct[i][2]);
	// the pitch is negated (model angles): rotating the identity by pitch 30 looks up
	Call (h, "makevectors", ARGS (V (0, 0, 0)));
	Call (h, "rotatevectorsbyangle", ARGS (V (30, 0, 0)));
	View (h, rotated);
	Call (h, "makevectors", ARGS (V (-30, 0, 0)));
	View (h, direct);
	for (i = 0 ; i < 3 ; i++)
		CLOSE (rotated[i], direct[i][0], direct[i][1], direct[i][2]);
	// rotations compose: yaw 90, then yaw 90 more
	Call (h, "makevectors", ARGS (V (0, 90, 0)));
	Call (h, "rotatevectorsbyangle", ARGS (V (0, 90, 0)));
	View (h, rotated);
	CLOSE (rotated[0], -1, 0, 0);
	QH_Free (h);
}

// rotatevectorsbyvectors applies a basis
static void TestRotateByVectors (void)
{
	qh_t	*h = Harness ();
	float	basis[3][3], v[3][3];
	int		i;

	Call (h, "makevectors", ARGS (V (0, 90, 0)));
	View (h, basis);
	Call (h, "makevectors", ARGS (V (0, 0, 0)));
	Call (h, "rotatevectorsbyvectors", ARGS (V (basis[0][0], basis[0][1], basis[0][2]),
		V (basis[1][0], basis[1][1], basis[1][2]), V (basis[2][0], basis[2][1], basis[2][2])));
	View (h, v);
	for (i = 0 ; i < 3 ; i++)
		CLOSE (v[i], basis[i][0], basis[i][1], basis[i][2]);
	Call (h, "rotatevectorsbyvectors", ARGS (V (basis[0][0], basis[0][1], basis[0][2]),
		V (basis[1][0], basis[1][1], basis[1][2]), V (basis[2][0], basis[2][1], basis[2][2])));
	View (h, v);
	CLOSE (v[0], -1, 0, 0);
	CLOSE (v[1], 0, 1, 0);
	QH_Free (h);
}

static void SetFloatField (qh_t *h, qc_ent_t e, const char *name, float v)
{
	uint32_t	w = QC_FloatBits (v);

	QC_SetField (h->vm, e, Field (h, name), 1, &w);
}

static void SetVectorField (qh_t *h, qc_ent_t e, const char *name, float x, float y, float z)
{
	uint32_t	w[3] = {QC_FloatBits (x), QC_FloatBits (y), QC_FloatBits (z)};

	QC_SetField (h->vm, e, Field (h, name), 3, w);
}

// sets up self with angles and turning parameters
static qc_ent_t Turner (qh_t *h, float pitch, float yaw, float roll, const char *ideal, float idealv,
	const char *speed, float speedv)
{
	qc_ent_t	e = 0;

	QT_CHECK (QC_Spawn (h->vm, &e));
	QC_Globals (h->vm)[Global (h, "self")].u = e;
	SetVectorField (h, e, "angles", pitch, yaw, roll);
	SetFloatField (h, e, ideal, idealv);
	SetFloatField (h, e, speed, speedv);
	return e;
}

static void Angles (qh_t *h, qc_ent_t e, float out[3])
{
	uint32_t	w[3] = {0, 0, 0};
	int			k;

	QC_GetField (h->vm, e, Field (h, "angles"), 3, w);
	for (k = 0 ; k < 3 ; k++)
		out[k] = QC_BitsFloat (w[k]);
}

// changeyaw turns self, with 16-bit angles
static void TestChangeyaw (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	e;
	float		a[3];

	e = Turner (h, 0, 0, 0, "ideal_yaw", 90, "yaw_speed", 20);
	Call (h, "changeyaw", NOARGS);
	// 20 degrees, quantised to 1/65536 of a turn
	Angles (h, e, a);
	EQ3 (a, 0, 19.995117f, 0);

	// the short way round, through 0
	e = Turner (h, 0, 10, 0, "ideal_yaw", 350, "yaw_speed", 5);
	Call (h, "changeyaw", NOARGS);
	Angles (h, e, a);
	QT_EQ_F (a[1], 4.993286f);
	e = Turner (h, 0, 350, 0, "ideal_yaw", 10, "yaw_speed", 45);
	Call (h, "changeyaw", NOARGS);
	Angles (h, e, a);
	QT_EQ_F (a[1], 9.997559f);

	// already there: nothing changes (not even the quantisation)
	e = Turner (h, 1, 90, 2, "ideal_yaw", 90, "yaw_speed", 5);
	Call (h, "changeyaw", NOARGS);
	Angles (h, e, a);
	EQ3 (a, 1, 90, 2);
	QH_Free (h);
}

// changeyaw turns in the gravity frame
static void TestChangeyawGravity (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	e;
	float		a[3];

	e = Turner (h, 0, 0, 0, "ideal_yaw", 90, "yaw_speed", 20);
	// upside down: turning left in the surface frame turns right in the world
	SetVectorField (h, e, "gravitydir", 0, 0, 1);
	Call (h, "changeyaw", NOARGS);
	Angles (h, e, a);
	if (!QT_CHECK (fabsf (a[1] - 340) < 0.02f))
		printf ("  yaw %.9g\n", (double)a[1]);
	if (!QT_CHECK (fabsf (a[2] - 180) < 0.02f))
		printf ("  roll %.9g\n", (double)a[2]);
	if (!QT_CHECK (fabsf (a[0]) < 0.02f || fabsf (a[0] - 360) < 0.02f))
		printf ("  pitch %.9g\n", (double)a[0]);
	QH_Free (h);
}

// changepitch turns the entity passed
static void TestChangepitch (void)
{
	qh_t		*h = Harness ();
	qc_ent_t	other, e = 0;
	float		a[3];

	other = Turner (h, 0, 0, 0, "idealpitch", 30, "pitch_speed", 10);
	// self is now other; turn a different entity
	QT_CHECK (QC_Spawn (h->vm, &e));
	SetFloatField (h, e, "idealpitch", 30);
	SetFloatField (h, e, "pitch_speed", 10);
	Call (h, "changepitch", ARGS (W (e)));
	Angles (h, e, a);
	EQ3 (a, 9.997559f, 0, 0);
	// FTE turns self instead; this turns the argument
	Angles (h, other, a);
	EQ3 (a, 0, 0, 0);
	// without an argument it turns self
	Call (h, "changepitch", NOARGS);
	Angles (h, other, a);
	EQ3 (a, 9.997559f, 0, 0);
	QH_Free (h);
}

// turning needs the fields
static void TestTurningNeedsFields (void)
{
	qh_t	*h = QH_Csqc ();

	QT_EQ_I (QH_Fails (h, "changeyaw", NOARGS), QC_ERR_BUILTIN);
	QT_CHECK (QT_Contains (QH_ErrorMessage (h), "angles"));
	QH_Free (h);
}

int main (void)
{
	TestLengthsAndNormals ();
	TestMakevectors ();
	TestMakevectorsWithoutView ();
	TestVectoyaw ();
	TestVectoyawSurface ();
	TestVectoangles ();
	TestVectoanglesRoll ();
	TestVectorvectors ();
	TestRotateByAngle ();
	TestRotateByVectors ();
	TestChangeyaw ();
	TestChangeyawGravity ();
	TestChangepitch ();
	TestTurningNeedsFields ();
	return QT_Finish ("lib_vector", "the vector and angle builtins give FTE's results");
}
