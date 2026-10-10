// func_bob.qc -- a brush model that moves gently back and forth, as
// dumptruck_ds' progs_dump has it (from Arcane Dimensions), and KTX: the same
// keys and the same travel. Theirs adds to the velocity each tenth of a
// second and lets it go, so whatever is lost or gained (a step more one way
// than the other, the two clocks it compares, a moment blocked) is never made
// good, and it drifts away over a game. This one works out where it should be
// from how far into its cycle it is, and moves there.

float BOB_NONSOLID_MG1 = 1;		// Dimension of the Machine's non-solid flag
float BOB_NONSOLID     = 4;		// progs_dump's

float BOB_STEP = 0.1;			// the think's step, and progs_dump's

/*
================
func_bob_place

How far along its travel it is at t into its cycle, 0 to 1: away for count
seconds and back for count, starting and stopping gently
================
*/
float(float t) func_bob_place =
{
	local float	x;

	t = t - self.count * 2 * floor (t / (self.count * 2));
	if (t < self.count)
		x = t / self.count;
	else
		x = 2 - t / self.count;
	if (x < 0)
		x = 0;
	else if (x > 1)
		x = 1;
	return x * x * (3 - 2 * x);
};

/*
================
func_bob_think

To where it should be at the next think, by its own time, which stands
while it's blocked
================
*/
void() func_bob_think =
{
	local vector	goal;

	goal = self.pos1 - self.movedir * (self.distance * func_bob_place (self.ltime + BOB_STEP - self.cnt));
	self.velocity = (goal - self.origin) * (1 / BOB_STEP);
	self.nextthink = self.ltime + BOB_STEP;
};

void() func_bob_on =
{
	self.use = SUB_Null;
	self.cnt = self.ltime + BOB_STEP + self.delay;		// its cycle starts
	self.think = func_bob_think;
	self.nextthink = self.cnt;
};

/*
================
func_bob_travel

How far progs_dump's would go each way: a half cycle speeding up, its
velocity grown by height, then height times waitmin, height times waitmin
squared... each step, and a half cycle slowing down, the velocity times
waitmin2 each step
================
*/
float() func_bob_travel =
{
	local float	steps, i, speed, add, travel;

	steps = floor (self.count * 0.5 / BOB_STEP + 0.5);
	add = self.height;
	for (i = 0 ; i < steps ; i = i + 1)
	{
		add = add * self.waitmin;
		speed = speed + add;
		travel = travel + speed * BOB_STEP;
	}
	for (i = 0 ; i < steps ; i = i + 1)
	{
		speed = speed * self.waitmin2;
		travel = travel + speed * BOB_STEP;
	}
	return travel;
};

/*QUAKED func_bob (0 .5 .8) ? NONSOLID_MG1 X NONSOLID
A bmodel that gently moves back and forth
-------- KEYS --------
targetname : trigger entity, with "style" 1
angle    : direction of movement, use "360" for angle 0
height   : direction intensity (def=8)
count    : direction cycle timer (def=2s, minimum=1s)
waitmin  : Speed up scale (def=1) 1+=non linear
waitmin2 : Slow down scale (def=0.75)
delay    : Starting time delay (def=0, -1=random)
style    : If set to 1, starts off and waits for trigger
-------- SPAWNFLAGS --------
NONSOLID_MG1, NONSOLID : can be passed through
*/
void() func_bob =
{
	self.movetype = MOVETYPE_PUSH;
	if (self.spawnflags & (BOB_NONSOLID | BOB_NONSOLID_MG1))
		self.solid = SOLID_NOT;
	else
		self.solid = SOLID_BSP;

	setmodel (self, self.model);
	setsize (self, self.mins, self.maxs);

	SetMovedir ();
	self.movedir = normalize (self.movedir);

	if (self.height <= 0)
		self.height = 8;		// Direction intensity
	if (self.count < 1)
		self.count = 2;			// Direction switch timer
	if (self.waitmin <= 0)
		self.waitmin = 1;		// Speed up
	if (self.waitmin2 <= 0)
		self.waitmin2 = 0.75;	// Slow down
	if (self.delay < 0)
		self.delay = random () + random () + random ();

	self.distance = func_bob_travel ();
	self.pos1 = self.origin;

	if (self.style & 1)
		self.use = func_bob_on;
	else
		func_bob_on ();
};
