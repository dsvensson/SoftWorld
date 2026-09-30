// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Float and vector arithmetic, comparisons and logic.

float zero, one, two, negzero, big, third;
vector va, vb;

void() main =
{
	zero = 0; one = 1; two = 2; negzero = -0; big = 16777217; third = one / 3;
	va = '1 2 3'; vb = '-4 0.5 10';

	show("add", one + two);
	show("sub", one - 2.5);
	show("mul", 3 * 0.1);
	show("div", one / 8);
	show("third", third);
	show("big", big);
	show("chain", (one + two) * (two - 0.5) / 4);
	showv("vadd", va + vb);
	showv("vsub", va - vb);
	show("dot", va * vb);
	showv("scale", va * 2.5);
	showv("scale2", 0.5 * vb);
	show("eq", one == 1);
	show("ne", one != 1);
	show("lt", one < two);
	show("le", two <= two);
	show("gt", one > two);
	show("ge", one >= two);
	show("veq", va == '1 2 3');
	show("vne", va != vb);
	show("negzero_eq", negzero == zero);
	show("not", !zero);
	show("notneg", !negzero);
	show("notv", !va);
	show("and", one && two);
	show("or", zero || negzero);
	show("bitand", 7 & 12);
	show("bitor", 5 | 10);
	show("bitand_frac", 6.9 & 3.1);
	show("ternary", (one > two) ? 10 : 20);
	show("floor", floor(-1.5));
	show("sqrt", sqrt(2));
	show("pow", pow(2, 10));
	show("vlen", vlen('3 4 0'));
	showv("normalize", normalize('3 4 0'));
	showv("comp", '1 2 3' + [one, two, one + two]);
	va_x = 7;
	show("va_x", va_x);
	va_z += 5;
	showv("va", va);
};
