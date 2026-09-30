// SPDX-License-Identifier: MIT OR Apache-2.0
//
// QuakeC threads: sleep, fork and abort (FTE target).

void(string s, ...) puts = #1;
string(float f) ftos = #2;
float(optional float sleeptime) fork = #210;
void(optional __variant ret) abort = #211;
void(float sleeptime) sleep = #212;

float time;
entity self;

void(float n) looper =
{
	float i;
	string label;
	for (i = 0; i < n; i++)
	{
		label = ftos(i * 10);
		puts("loop ", ftos(i), " at ", ftos(time), "\n");
		sleep(1);
		puts("  label ", label, "\n");
	}
	puts("loop done\n");
};

float(float depth) nested =
{
	if (depth > 0)
		return nested(depth - 1) + 1;
	sleep(0.5);
	return 100;
};

void() main =
{
	looper(3);
	puts("main continues\n");
	if (fork(2))
	{
		puts("forked child at ", ftos(time), "\n");
		abort();
	}
	puts("parent after fork\n");
	puts("nested ", ftos(nested(3)), "\n");
};
