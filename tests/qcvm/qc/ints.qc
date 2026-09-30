// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Integer, unsigned, 64-bit and double arithmetic (FTE target).

string(int i) itos = #17;
string(__long i) ltos = #18;
string(__double d) dtos = #19;

void(string label, int i) showi = { puts(label, " ", itos(i), "\n"); };
void(string label, __long l) showl = { puts(label, " ", ltos(l), "\n"); };
void(string label, __double d) showd = { puts(label, " ", dtos(d), "\n"); };

int ia, ib, iz, imin;
__uint ua, ub;
__long la, lb;
__double da, db;

void() main =
{
	ia = 1000000; ib = -7; iz = 0; imin = -2147483647i - 1i;
	showi("add", ia + ib);
	showi("sub", ib - ia);
	showi("mul", ia * ib);
	showi("mulwrap", ia * ia);
	showi("div", ia / ib);
	showi("divneg", ib / 2i);
	showi("divzero", ia / iz);
	showi("mindiv", imin / -1i);
	showi("mod", ia % 7i);
	showi("and", ia & 0xffff);
	showi("or", ib | 0x100);
	showi("xor", ia ^ ib);
	showi("shl", ib << 4i);
	showi("shr", ib >> 1i);
	showi("lt", ib < ia);
	showi("eq", ia == 1000000i);
	showi("not", !iz);
	showi("neg", -ia);
	showi("fromf", (int)3.99);
	showi("fromfneg", (int)-3.99);
	show("tof", (float)ib);
	show("mixed", ia + 0.5);
	show("mixed2", 2.5 * ib);
	show("mixedcmp", ia > 999999.5);

	ua = 4000000000u; ub = 3u;
	showi("udiv", ua / ub);
	showi("ushr", ua >> 4u);
	showi("ult", ub < ua);

	la = 123456789012; lb = -5;
	showl("ladd", la + lb);
	showl("lmul", la * 1000);
	showl("ldiv", la / lb);
	showl("land", la & 0xffffffff);
	showl("lshl", lb << 40);
	showl("lshr", la >> 3);
	showi("llt", lb < la);
	showi("ltoi", (int)la);
	showl("itol", (__long)ib);

	da = 0.1; db = 3;
	showd("dadd", da + db);
	showd("ddiv", 1 / db);
	showd("dmul", da * db);
	showi("dlt", da < db);
	show("dtof", (float)(da * 7));
	showd("ftod", (__double)0.1);
};
