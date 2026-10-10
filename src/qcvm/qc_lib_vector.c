// qc_lib_vector.c -- vector and angle builtins (docs/spec/builtins.md)
//
// Angles are (pitch, yaw, roll) in degrees. The arithmetic is FTE's operation
// by operation (float products, double trigonometry), so results match it to
// the bit.

#include "qc_lib.h"

#include <math.h>
#include <string.h>

#define QC_PI	3.14159265358979323846

static float QC_Dot (const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void QC_Cross (const float a[3], const float b[3], float out[3])
{
	float	r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};

	memcpy (out, r, sizeof(r));
}

static void QC_Neg (const float v[3], float out[3])
{
	out[0] = -v[0];
	out[1] = -v[1];
	out[2] = -v[2];
}

static void QC_Scale (const float v[3], float s, float out[3])
{
	out[0] = v[0] * s;
	out[1] = v[1] * s;
	out[2] = v[2] * s;
}

// sqrt of a float in double precision, rounded back
static float QC_Fsqrt (float x)
{
	return (float)sqrt ((double)x);
}

// FTE's VectorNormalize: the reciprocal in double precision
static void QC_Normalized (const float v[3], float out[3])
{
	float	len = QC_Fsqrt (QC_Dot (v, v));

	if (len == 0)
		memmove (out, v, sizeof(float) * 3);
	else
		QC_Scale (v, (float)(1.0 / (double)len), out);
}

// degrees to radians as AngleVectors does it (a double product, kept as float)
static float QC_Radians (float deg)
{
	return (float)((double)deg * (QC_PI * 2.0 / 360.0));
}

static void QC_SinCos (float rad, float *s, float *c)
{
	*s = (float)sin ((double)rad);
	*c = (float)cos ((double)rad);
}

void QC_AngleVectors (const float angles[3], float forward[3], float right[3], float up[3])
{
	float	sy, cy, sp, cp, sr, cr;

	QC_SinCos (QC_Radians (angles[1]), &sy, &cy);
	QC_SinCos (QC_Radians (angles[0]), &sp, &cp);
	QC_SinCos (QC_Radians (angles[2]), &sr, &cr);
	forward[0] = cp * cy;
	forward[1] = cp * sy;
	forward[2] = -sp;
	right[0] = -sr * sp * cy + -cr * -sy;
	right[1] = -sr * sp * sy + -cr * cy;
	right[2] = -sr * cp;
	up[0] = cr * sp * cy + -sr * -sy;
	up[1] = cr * sp * sy + -sr * cy;
	up[2] = cr * cp;
}

// AngleVectors for model angles, whose pitch is inverted
static void QC_AngleVectorsMesh (const float angles[3], float forward[3], float right[3], float up[3])
{
	float	a[3] = {-angles[0], angles[1], angles[2]};

	QC_AngleVectors (a, forward, right, up);
}

static float QC_Degrees (float r)
{
	return (float)((double)r * (180.0 / QC_PI));
}

static float QC_Wrap (float d)
{
	return d < 0 ? d + 360 : d;
}

// FTE's VectorAngles with mesh pitch: (pitch, yaw, roll) each in [0, 360),
// positive pitch looking up; up (if not NULL) gives the roll
static void QC_VectorAngles (const float forward[3], const float *up, float out[3])
{
	float	fx = forward[0], fy = forward[1], fz = forward[2], pitch, yaw, roll, sp, cp, sy, cy;
	float	left[3], tup[3];
	double	horizontal;

	if (fy == 0 && fx == 0)
	{
		if (fz > 0)
		{
			pitch = (float)(-QC_PI * 0.5);
			yaw = up ? (float)atan2 ((double)-up[1], (double)-up[0]) : 0;
		}
		else
		{
			pitch = (float)(QC_PI * 0.5);
			yaw = up ? (float)atan2 ((double)up[1], (double)up[0]) : 0;
		}
		roll = 0;
	}
	else
	{
		yaw = (float)atan2 ((double)fy, (double)fx);
		horizontal = sqrt ((double)(fx * fx + fy * fy));
		pitch = (float)-atan2 ((double)fz, horizontal);
		if (up)
		{
			QC_SinCos (pitch, &sp, &cp);
			QC_SinCos (yaw, &sy, &cy);
			left[0] = -sy;
			left[1] = cy;
			left[2] = 0;
			tup[0] = sp * cy;
			tup[1] = sp * sy;
			tup[2] = cp;
			roll = (float)-atan2 ((double)QC_Dot (up, left), (double)QC_Dot (up, tup));
		}
		else
			roll = 0;
	}
	// mesh pitch: FTE multiplies by r_meshpitch (-1) for Quake's models
	out[0] = QC_Wrap (-QC_Degrees (pitch));
	out[1] = QC_Wrap (QC_Degrees (yaw));
	out[2] = QC_Wrap (QC_Degrees (roll));
}

// Quake 3's PerpendicularVector: a unit vector perpendicular to the unit vector
// src, its smallest axis projected out
static void QC_Perpendicular (const float src[3], float out[3])
{
	float	min = 1, axis[3] = {0, 0, 0}, inv, d, n[3], r[3];
	int		pos = 0, i;

	for (i = 0 ; i < 3 ; i++)
		if (fabsf (src[i]) < min)
		{
			pos = i;
			min = fabsf (src[i]);
		}
	axis[pos] = 1;
	inv = 1 / QC_Dot (src, src);
	d = QC_Dot (src, axis) * inv;
	QC_Scale (src, inv, n);
	for (i = 0 ; i < 3 ; i++)
		r[i] = axis[i] - d * n[i];
	QC_Normalized (r, out);
}

// the surface frame of an entity with a non-zero gravitydir: x, y and up =
// -normalize(gravitydir); false for none (the world's axes)
static bool QC_GravityAxis (const float gravitydir[3], float axis[3][3])
{
	float	t[3];

	if (gravitydir[0] == 0 && gravitydir[1] == 0 && gravitydir[2] == 0)
		return false;
	QC_Neg (gravitydir, t);
	QC_Normalized (t, axis[2]);
	QC_Perpendicular (axis[2], t);
	QC_Normalized (t, axis[0]);
	QC_Cross (axis[2], axis[0], t);
	QC_Normalized (t, axis[1]);
	return true;
}

static void QC_EntityGravity (const qcvm_t *vm, uint32_t e, float out[3])
{
	QC_LibNamedVector (vm, e, "gravitydir", out);
}

// the product of two 3x3 matrices given as rows
static void QC_Concat (const float a[3][3], const float b[3][3], float out[3][3])
{
	float	r[3][3];
	int		i, j;

	for (i = 0 ; i < 3 ; i++)
		for (j = 0 ; j < 3 ; j++)
			r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
	memcpy (out, r, sizeof(r));
}

/*
==============================================================================

THE VIEW GLOBALS

==============================================================================
*/

// the calling progs' v_forward, v_right and v_up
static bool QC_ViewGlobals (qcvm_t *vm, uint32_t g[3])
{
	if (QC_LibGlobal (vm, "v_forward", QC_EV_VECTOR, &g[0]) && QC_LibGlobal (vm, "v_right", QC_EV_VECTOR, &g[1])
		&& QC_LibGlobal (vm, "v_up", QC_EV_VECTOR, &g[2]))
		return true;
	return QC_HostError (vm, "makevectors: one of v_forward, v_right or v_up was not defined");
}

static bool QC_SetView (qcvm_t *vm, const float v[3][3])
{
	uint32_t	g[3];
	int			i, k;

	if (!QC_ViewGlobals (vm, g))
		return false;
	for (i = 0 ; i < 3 ; i++)
		for (k = 0 ; k < 3 ; k++)
			QC_SetS (&vm->mem, (uint64_t)g[i] + (uint64_t)k * 4, QC_FloatBits (v[i][k]));
	return true;
}

static bool QC_GetView (qcvm_t *vm, float v[3][3])
{
	uint32_t	g[3];
	int			i, k;

	if (!QC_ViewGlobals (vm, g))
		return false;
	for (i = 0 ; i < 3 ; i++)
		for (k = 0 ; k < 3 ; k++)
			v[i][k] = QC_BitsFloat (QC_GetS (&vm->mem, (uint64_t)g[i] + (uint64_t)k * 4));
	return true;
}

/*
==============================================================================

BUILTINS

==============================================================================
*/

// void makevectors(vector angles): v_forward, v_right and v_up
static bool QC_Makevectors (qcvm_t *vm)
{
	float	angles[3], v[3][3];

	QC_ArgVector (vm, 0, angles);
	QC_AngleVectors (angles, v[0], v[1], v[2]);
	return QC_SetView (vm, v);
}

// vector normalize(vector v): '0 0 0' stays zero
static bool QC_Normalize (qcvm_t *vm)
{
	float	v[3], len, r[3] = {0, 0, 0};

	QC_ArgVector (vm, 0, v);
	len = QC_Fsqrt (QC_Dot (v, v));
	if (len != 0)
		QC_Scale (v, 1 / len, r);
	QC_ReturnVector (vm, r);
	return true;
}

static bool QC_Vlen (qcvm_t *vm)
{
	float	v[3];

	QC_ArgVector (vm, 0, v);
	QC_ReturnFloat (vm, QC_Fsqrt (QC_Dot (v, v)));
	return true;
}

// float vectoyaw(vector v, optional entity reference): whole degrees in [0,
// 360); in the reference's surface frame if its gravitydir isn't zero
static bool QC_Vectoyaw (qcvm_t *vm)
{
	float	v[3], x, y, g[3], axis[3][3], yaw;

	QC_ArgVector (vm, 0, v);
	x = v[0];
	y = v[1];
	if (QC_Argc (vm) >= 2)
	{
		QC_EntityGravity (vm, QC_LibEntArg (vm, 1), g);
		if (QC_GravityAxis (g, axis))
		{
			x = QC_Dot (v, axis[0]);
			y = QC_Dot (v, axis[1]);
		}
	}
	if (x == 0 && y == 0)
		yaw = 0;
	else
	{
		yaw = (float)QC_DoubleToInt (atan2 ((double)y, (double)x) * 180 / QC_PI);
		if (yaw < 0)
			yaw += 360;
	}
	QC_ReturnFloat (vm, yaw);
	return true;
}

// vector vectoangles(vector forward, optional vector up): not truncated, each
// in [0, 360), positive pitch looking up
static bool QC_Vectoangles (qcvm_t *vm)
{
	float	forward[3], up[3], r[3];

	QC_ArgVector (vm, 0, forward);
	QC_ArgVector (vm, 1, up);
	QC_VectorAngles (forward, QC_Argc (vm) >= 2 ? up : NULL, r);
	QC_ReturnVector (vm, r);
	return true;
}

// void vectorvectors(vector dir): v_forward = normalize(dir), a horizontal
// v_right and v_up from them
static bool QC_Vectorvectors (qcvm_t *vm)
{
	float	dir[3], v[3][3], t[3];

	QC_ArgVector (vm, 0, dir);
	QC_Normalized (dir, v[0]);
	if (v[0][0] == 0 && v[0][1] == 0)
	{
		v[1][0] = 0;
		v[1][1] = v[0][2] != 0 ? -1.0f : 0.0f;
		v[1][2] = 0;
	}
	else
	{
		t[0] = v[0][1];
		t[1] = -v[0][0];
		t[2] = 0;
		QC_Normalized (t, v[1]);
	}
	QC_Cross (v[1], v[0], v[2]);
	return QC_SetView (vm, v);
}

static bool QC_Crossproduct (qcvm_t *vm)
{
	float	a[3], b[3], r[3];

	QC_ArgVector (vm, 0, a);
	QC_ArgVector (vm, 1, b);
	QC_Cross (a, b, r);
	QC_ReturnVector (vm, r);
	return true;
}

// the view vectors turned by trans (rows forward, left, up)
static bool QC_RotateView (qcvm_t *vm, const float trans[3][3], const float base[3][3])
{
	float	r[3][3];

	QC_Concat (trans, base, r);
	QC_Neg (r[1], r[1]);
	return QC_SetView (vm, r);
}

// void rotatevectorsbyangle(vector angles): the view vectors turned by model angles
static bool QC_Rotatevectorsbyangle (qcvm_t *vm)
{
	float	angles[3], t[3][3], v[3][3];

	QC_ArgVector (vm, 0, angles);
	QC_AngleVectorsMesh (angles, t[0], t[1], t[2]);
	QC_Neg (t[1], t[1]);
	if (!QC_GetView (vm, v))
		return false;
	QC_Neg (v[1], v[1]);
	return QC_RotateView (vm, t, v);
}

// void rotatevectorsbyvectors(vector forward, vector right, vector up)
static bool QC_Rotatevectorsbyvectors (qcvm_t *vm)
{
	float	base[3][3], v[3][3];

	QC_ArgVector (vm, 0, base[0]);
	QC_ArgVector (vm, 1, base[1]);
	QC_Neg (base[1], base[1]);
	QC_ArgVector (vm, 2, base[2]);
	if (!QC_GetView (vm, v))
		return false;
	QC_Neg (v[1], v[1]);
	return QC_RotateView (vm, v, base);
}

// Quake's anglemod: the angle quantised to 1/65536 of a turn, in [0, 360)
static float QC_Anglemod16 (float a)
{
	int32_t	steps = QC_DoubleToInt ((double)a * (65536.0 / 360.0)) & 0xFFFF;

	return (float)((360.0 / 65536.0) * (double)steps);
}

// a turn limited to +-speed with C's comparisons (a NaN passes through)
static float QC_Limit (float delta, float speed)
{
	if (delta > 0)
		return delta > speed ? speed : delta;
	return delta < -speed ? -speed : delta;
}

// turns current toward ideal by at most speed, Quake's way; false if already there
static bool QC_Turn (float current, float ideal, float speed, float *out)
{
	float	delta;

	current = QC_Anglemod16 (current);
	if (current == ideal)
		return false;
	delta = ideal - current;
	if (ideal > current)
	{
		if (delta >= 180)
			delta -= 360;
	}
	else if (delta <= -180)
		delta += 360;
	*out = QC_Anglemod16 (current + QC_Limit (delta, speed));
	return true;
}

// the surface frame's turn, which doesn't compare ideal with current first
static float QC_ClampTurn (float delta, float speed)
{
	if (delta > 180)
		delta -= 360;
	else if (delta < -180)
		delta += 360;
	return QC_Limit (delta, speed);
}

static bool QC_RequiredField (qcvm_t *vm, const char *builtin, const char *name, uint32_t *f)
{
	int64_t	ofs = QC_LibField (vm, name);

	*f = 0;
	if (ofs < 0)
		return QC_Error (vm, "%s: the progs has no .%s field", builtin, name);
	*f = (uint32_t)ofs;
	return true;
}

// the calling progs' self
static bool QC_SelfEntity (qcvm_t *vm, const char *builtin, uint32_t *e)
{
	uint32_t	g;

	*e = 0;
	if (!QC_LibGlobal (vm, "self", QC_EV_ENTITY, &g))
		return QC_Error (vm, "%s: the progs has no `self` global", builtin);
	*e = QC_GetS (&vm->mem, g);
	if (*e >= vm->mem.num_edicts)
		*e = 0;
	return true;
}

// changeyaw for an entity standing on a surface given by its gravity axis: the
// turn is made in the surface's frame, where pitch and roll are zero
static void QC_ChangeyawOnSurface (const float angles[3], const float surf[3][3], float ideal, float speed,
	float out[3])
{
	float	f[3], r[3], u[3], fs[3], us[3], rel[3], delta, yaw, a[3], f2[3], u2[3], fw[3], uw[3];
	int		k;

	QC_AngleVectorsMesh (angles, f, r, u);
	for (k = 0 ; k < 3 ; k++)
	{
		fs[k] = QC_Dot (f, surf[k]);
		us[k] = QC_Dot (u, surf[k]);
	}
	QC_VectorAngles (fs, us, rel);
	delta = QC_ClampTurn (ideal - QC_Anglemod16 (rel[1]), speed);
	yaw = QC_Anglemod16 (rel[1] + delta);
	a[0] = 0;
	a[1] = yaw;
	a[2] = 0;
	QC_AngleVectors (a, f2, r, u2);
	for (k = 0 ; k < 3 ; k++)
	{
		fw[k] = f2[0] * surf[0][k] + f2[1] * surf[1][k] + f2[2] * surf[2][k];
		uw[k] = u2[0] * surf[0][k] + u2[1] * surf[1][k] + u2[2] * surf[2][k];
	}
	QC_VectorAngles (fw, uw, out);
	for (k = 0 ; k < 3 ; k++)
		out[k] = QC_Anglemod16 (out[k]);
}

// void changeyaw(): self.angles_y toward self.ideal_yaw by at most
// self.yaw_speed; in its surface frame for a non-zero gravitydir
static bool QC_Changeyaw (qcvm_t *vm)
{
	uint32_t	e, af, idf, sf;
	float		angles[3], ideal, speed, g[3], surf[3][3], r[3], yaw;

	if (!QC_SelfEntity (vm, "changeyaw", &e) || !QC_RequiredField (vm, "changeyaw", "angles", &af)
		|| !QC_RequiredField (vm, "changeyaw", "ideal_yaw", &idf)
		|| !QC_RequiredField (vm, "changeyaw", "yaw_speed", &sf))
		return false;
	ideal = QC_LibFloat (vm, e, idf);
	speed = QC_LibFloat (vm, e, sf);
	QC_LibVector (vm, e, af, angles);
	QC_EntityGravity (vm, e, g);
	if (QC_GravityAxis (g, surf))
	{
		QC_ChangeyawOnSurface (angles, surf, ideal, speed, r);
		QC_LibSetVector (vm, e, af, r);
		return true;
	}
	if (QC_Turn (angles[1], ideal, speed, &yaw))
		QC_LibSetWord (vm, e, af + 1, QC_FloatBits (yaw));
	return true;
}

// void changepitch(entity e): e.angles_x toward e.idealpitch by at most
// e.pitch_speed. FTE turns self whatever the argument; this turns the entity
// passed (self without one).
static bool QC_Changepitch (qcvm_t *vm)
{
	uint32_t	e, af, idf, sf;
	float		pitch;

	if (QC_Argc (vm) > 0)
		e = QC_LibEntArg (vm, 0);
	else if (!QC_SelfEntity (vm, "changepitch", &e))
		return false;
	if (!QC_RequiredField (vm, "changepitch", "angles", &af)
		|| !QC_RequiredField (vm, "changepitch", "idealpitch", &idf)
		|| !QC_RequiredField (vm, "changepitch", "pitch_speed", &sf))
		return false;
	if (QC_Turn (QC_LibFloat (vm, e, af), QC_LibFloat (vm, e, idf), QC_LibFloat (vm, e, sf), &pitch))
		QC_LibSetWord (vm, e, af, QC_FloatBits (pitch));
	return true;
}

static const qc_libentry_t	qc_vector[] = {
	{"makevectors", QC_Makevectors, NULL, 0},
	{"normalize", QC_Normalize, NULL, 0},
	{"vlen", QC_Vlen, NULL, 0},
	{"vectoyaw", QC_Vectoyaw, NULL, 0},
	{"vectoangles", QC_Vectoangles, NULL, 0},
	{"vectorvectors", QC_Vectorvectors, NULL, 0},
	{"crossproduct", QC_Crossproduct, NULL, 0},
	{"rotatevectorsbyangle", QC_Rotatevectorsbyangle, NULL, 0},
	{"rotatevectorsbyvectors", QC_Rotatevectorsbyvectors, NULL, 0},
	{"changeyaw", QC_Changeyaw, NULL, 0},
	{"changepitch", QC_Changepitch, NULL, 0},
};

bool QC_RegisterVector (qc_builtins_t *b)
{
	return QC_LibRegister (b, qc_vector, sizeof(qc_vector) / sizeof(qc_vector[0]));
}
