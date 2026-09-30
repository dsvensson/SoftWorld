// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Entities and fields of every type.

.float health;
.vector origin;
.string netname;
.entity owner;
.void() think;
.float flags;

entity self;
float counter;

void() bump = { counter = counter + self.health; };

.float(entity e) getter;
float(entity e) get_health = { return e.health; };

void() main =
{
	entity a, b, c;
	.float fld;

	a = spawn();
	b = spawn();
	c = spawn();
	puts("ents ", etos(a), " ", etos(b), " ", etos(c), "\n");

	a.health = 100;
	a.origin = '1 2 3';
	a.netname = "alpha";
	a.owner = b;
	a.think = bump;
	a.getter = get_health;
	b.health = 50;
	b.owner = a;

	show("health", a.health);
	showv("origin", a.origin);
	puts("netname ", a.netname, "\n");
	show("owner_health", a.owner.health);
	show("owner_owner", a.owner.owner.health);
	show("eq", a.owner == b);
	show("ne", a != b);
	show("not_world", !a);

	self = a;
	self.think();
	self.think();
	show("counter", counter);
	show("getter", a.getter(b));

	fld = health;
	b.fld = b.fld + 25;
	show("fieldvar", b.health);

	a.origin_y = 20;
	a.origin += '10 10 10';
	showv("origin2", a.origin);
	a.flags = a.flags | 4;
	a.flags |= 8;
	show("flags", a.flags);

	remove(c);
	c = spawn();
	puts("respawn ", etos(c), "\n");
	show("fresh_health", c.health);
};
