// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The STATE opcode ([frame, think] functions) and self/time handling.

entity self;
float time;
.float frame;
.float nextthink;
.void() think;

void() anim2;
void() anim1 = [5, anim2] { puts("anim1\n"); };
void() anim2 = [6, anim1] { puts("anim2\n"); };

void() main =
{
	entity e = spawn();
	self = e;
	time = 10;
	anim1();
	show("frame", e.frame);
	show("nextthink", e.nextthink);
	show("think_is_anim2", e.think == anim2);
	time = 20;
	e.think();
	show("frame2", e.frame);
	show("nextthink2", e.nextthink);
	show("think_is_anim1", e.think == anim1);
};
