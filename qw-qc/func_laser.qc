// func_laser.qc -- a toggleable laser that hurts to touch, as progs_dump has
// it, from Copper, and KTX. A helper entity flickers its alpha, as a pusher
// (a solid laser) can't think for itself; KTX's flickered the helper's, which
// isn't drawn, and the laser kept still.

float LASER_START_OFF = 1;
float LASER_SOLID     = 2;

// the laser's alpha, the helper's own (its base) within a fifth either way
void() laser_helper_think =
{
	if (!(self.owner.spawnflags & LASER_START_OFF))
		self.owner.alpha = self.alpha * 0.8 + self.alpha * random () * 0.4;
	self.nextthink = time + 0.05;
};

void() init_laser_noise =
{
	sound (self.owner, CHAN_VOICE, self.owner.noise, 1, ATTN_NORM);
	self.think = laser_helper_think;
	self.nextthink = time + 0.05;
};

void() func_laser_touch =
{
	// from Copper -- dumptruck_ds
	if (other.movetype == MOVETYPE_NOCLIP)
		return;

	if (other.takedamage && self.attack_finished < time)
	{
		T_Damage (other, self, self, self.dmg);
		// add "zap" sound when damage is dealt
		sound (self, CHAN_WEAPON, self.noise2, 1, ATTN_NORM);
		self.attack_finished = time + 0.333;
	}
};

// on, where it is, or off, far above the map
void() func_laser_use =
{
	if (self.spawnflags & LASER_START_OFF)
	{
		setorigin (self, '0 0 0');
		self.spawnflags = self.spawnflags - LASER_START_OFF;
		sound (self, CHAN_VOICE, self.noise, 1, ATTN_NORM);
	}
	else
	{
		sound (self, CHAN_VOICE, self.noise1, 1, ATTN_NORM);
		setorigin (self, '0 0 9000');
		self.spawnflags = self.spawnflags + LASER_START_OFF;
	}
};

/*QUAKED func_laser (0 .5 .8) ? START_OFF LASER_SOLID
A toggleable laser, hurts to touch, can be used to block players
START_OFF: Laser starts off.
LASER_SOLID: Laser blocks movement while turned on.
Keys:
"dmg" damage to do on touch. default 1
"alpha" approximate alpha you want the laser drawn at. default 0.5. alpha will vary by 20% of this value.
"noise" sound when turned on, "noise1" when turned off, "noise2" when it hurts
*/
void() func_laser =
{
	local entity	helper;

	setmodel (self, self.model);

	precache_sound ("buttons/switch02.wav");
	precache_sound ("buttons/switch04.wav");
	precache_sound ("misc/null.wav");

	if (self.spawnflags & LASER_SOLID)
	{
		self.solid = SOLID_BSP;			// so you can shoot between lasers in a single bmodel
		self.movetype = MOVETYPE_PUSH;	// required because of SOLID_BSP
	}
	else
	{
		self.solid = SOLID_TRIGGER;
		self.movetype = MOVETYPE_NONE;
	}

	if (!self.alpha)
		self.alpha = 0.5;
	if (!self.dmg)
		self.dmg = 1;

	self.use = func_laser_use;
	self.touch = func_laser_touch;

	if (self.spawnflags & LASER_START_OFF)
		setorigin (self, '0 0 9000');
	else
		setorigin (self, '0 0 0');

	if (self.noise)
		precache_sound (self.noise);
	else
		self.noise = "buttons/switch02.wav";
	if (self.noise1)
		precache_sound (self.noise1);
	else
		self.noise1 = "buttons/switch04.wav";
	if (self.noise2)
		precache_sound (self.noise2);
	else
		self.noise2 = "misc/null.wav";

	// a second entity to flicker the alpha, since MOVETYPE_PUSH doesn't
	// support think functions
	helper = spawn ();
	helper.owner = self;
	helper.alpha = self.alpha;
	helper.nextthink = time + 2;
	helper.think = init_laser_noise;
};
