// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Strings: concatenation, comparison and conversion.

string greeting;
string empty;
string names[3];

void() main =
{
	string a, b;

	greeting = strcat("hello", " ", "world");
	puts(greeting, "\n");
	show("cmp_eq", greeting == "hello world");
	show("cmp_ne", greeting != "hello world");
	show("empty_eq", empty == "");
	show("not_empty", !empty);
	show("not_str", !greeting);
	a = ftos(12.5);
	b = strcat(a, "!");
	puts(a, " ", b, "\n");
	show("stof", stof("  -3.25abc"));
	showv("stov", stov("'1 2.5 -3'"));
	names[0] = "zero"; names[1] = "one"; names[2] = strcat("tw", "o");
	puts(names[0], ",", names[1], ",", names[2], "\n");
	show("strcmp", strcmp("abc", "abd"));
	a = "x";
	b = strcat(a, a);
	show("eq_temp", b == "xx");
	if (greeting)
		puts("truthy\n");
	if (!empty)
		puts("null is false\n");
};
