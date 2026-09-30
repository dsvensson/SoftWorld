// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Hexen 2 target: compound assignments on globals and fields (the *STORE and *STOREP opcodes)
// and the CALLnH calling convention.

.float health;
.vector velocity;
.float flags;

float g;
vector gv;

float(float a, float b, float c) three = { return a * 100 + b * 10 + c; };

void() main =
{
	entity e;

	g = 10;
	g += 5; show("addstore", g);
	g -= 2; show("substore", g);
	g *= 3; show("mulstore", g);
	g /= 13; show("divstore", g);
	g = 12;
	g |= 3; show("bitset", g);
	g (-) 5; show("bitclr", g);
	gv = '1 2 3';
	gv += '10 10 10'; showv("addstore_v", gv);
	gv -= '1 1 1'; showv("substore_v", gv);
	gv *= 2; showv("mulstore_v", gv);

	e = spawn();
	e.health = 100;
	e.health += 25; show("addstorep", e.health);
	e.health -= 50; show("substorep", e.health);
	e.health *= 2; show("mulstorep", e.health);
	e.health /= 3; show("divstorep", e.health);
	e.flags |= 6; show("bitsetp", e.flags);
	e.flags (-) 2; show("bitclrp", e.flags);
	e.velocity = '1 2 3';
	e.velocity += '1 1 1'; showv("addstorep_v", e.velocity);
	e.velocity -= '0 1 2'; showv("substorep_v", e.velocity);
	e.velocity *= 3; showv("mulstorep_v", e.velocity);
	show("three", three(1, 2, 3));
};
