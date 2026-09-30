// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Control flow: loops, break/continue, switch, recursion, calls with many arguments.

float(float n) fib =
{
	if (n < 2)
		return n;
	return fib(n - 1) + fib(n - 2);
};

float(float n) fact = { return n <= 1 ? 1 : n * fact(n - 1); };

float(float a, float b, float c, float d, float e, float f, float g, float h) sum8 =
{
	return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
};

vector(vector a, float s, vector b) mix = { return a * s + b * (1 - s); };

float(float x) classify =
{
	switch (x)
	{
	case 1:
		return 100;
	case 2:
	case 3:
		return 200;
	case 10:
		return 300;
	default:
		return -1;
	}
};

float(string s) classify_s =
{
	switch (s)
	{
	case "alpha":
		return 1;
	case "beta":
		return 2;
	default:
		return 0;
	}
};

float(float x) sign = { if (x > 0) return 1; else if (x < 0) return -1; return 0; };

void() main =
{
	float i, total, j;

	total = 0;
	for (i = 0; i < 10; i++)
		total += i;
	show("for", total);

	total = 0;
	i = 0;
	while (1)
	{
		i++;
		if (i > 20)
			break;
		if (i & 1)
			continue;
		total = total + i;
	}
	show("while", total);

	total = 0;
	i = 5;
	do { total = total * 2 + i; i--; } while (i > 0);
	show("dowhile", total);

	total = 0;
	for (i = 0; i < 5; i++)
		for (j = 0; j < i; j++)
			total += i * j;
	show("nested", total);

	show("fib20", fib(20));
	show("fact10", fact(10));
	show("sum8", sum8(1, 2, 3, 4, 5, 6, 7, 8));
	showv("mix", mix('1 2 3', 0.25, '10 20 30'));
	for (i = 0; i < 12; i++)
		puts(ftos(classify(i)), " ");
	puts("\n");
	show("s_alpha", classify_s("alpha"));
	show("s_beta", classify_s("beta"));
	show("s_none", classify_s("gamma"));
	show("sign", sign(-3) * 100 + sign(0) * 10 + sign(7));
};
