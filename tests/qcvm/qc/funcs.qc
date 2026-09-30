// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Function values, variadic QuakeC functions, many parameters, struct returns, dynamic local
// memory via alloca (FTE target with PUSH, fte_6614).

string(int i) itos = #17;

typedef struct
{
	float x, y, z, w;
	string tag;
} quad_t;

float(float a, float b) add = { return a + b; };
float(float a, float b) mul = { return a * b; };

float(float(float, float) op, float a, float b) apply = { return op(a, b); };

float(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j)
	ten = { return a + b + c + d + e + f + g + h + i * 100 + j * 1000; };

float(float first, ... count) count_args =
{
	float i, total = first;
	for (i = 0; i < count; i++)
		total += va_arg(i, float);
	return count * 1000 + total;
};

quad_t(float base) make_quad =
{
	quad_t q;
	q.x = base; q.y = base + 1; q.z = base + 2; q.w = base + 3;
	q.tag = "made";
	return q;
};

float(float n) dyn_array =
{
	float *arr = alloca(n * 4);
	float i, s = 0;
	for (i = 0; i < n; i++)
		arr[i] = i * 2;
	for (i = 0; i < n; i++)
		s += arr[i];
	return s;
};

float(float depth) recurse_dyn =
{
	float *buf = alloca((depth + 1) * 4);
	buf[depth] = depth;
	if (depth > 0)
		return buf[depth] + recurse_dyn(depth - 1);
	return buf[depth];
};

var float(float a, float b) chosen = add;

void() main =
{
	quad_t q;

	show("apply_add", apply(add, 3, 4));
	show("apply_mul", apply(mul, 3, 4));
	chosen = mul;
	show("var_fn", chosen(5, 6));
	show("fn_eq", chosen == mul);
	show("ten", ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
	show("varargs0", count_args(1));
	show("varargs3", count_args(1, 10, 20, 30));
	q = make_quad(10);
	puts("quad ", ftos(q.x), " ", ftos(q.y), " ", ftos(q.z), " ", ftos(q.w), " ", q.tag, "\n");
	show("dyn", dyn_array(5));
	show("recurse_dyn", recurse_dyn(4));
};
