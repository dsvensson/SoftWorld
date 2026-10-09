/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// test_part.c -- the scripted particles (src/particles) on scripts the test
// writes: FTE's r_part blocks, namespaces loaded when named, weak scripts,
// chains of +<name> and assoc, model effects, aliases, id's TE_EXPLOSION2
// colors, DarkPlaces' effectinfo stages; an effect's particles run into the
// batches the renderer draws, in the order they blend; the built-in weather

#include "p_local.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define	mkdir(path, mode)	_mkdir (path)
#else
#include <sys/stat.h>
#endif

static int	failures;

//
// the platform, as far as the module needs it
//

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

// the scripts are loose files: there, or not
int Sys_FileTime (char *path)
{
	FILE	*f = fopen (path, "rb");

	if (!f)
		return -1;
	fclose (f);
	return 1;
}
void Sys_mkdir (char *path) { (void)path; }
bool Sys_ListDir (const char *path, void (*entry) (void *ctx, const char *name, bool isdir), void *ctx)
{
	(void)path;
	(void)entry;
	(void)ctx;
	return false;
}
double Sys_DoubleTime (void) { return 0; }
void *Sys_ReserveMemory (size_t size) { return calloc (1, size); }
void Sys_CommitMemory (void *base, size_t size) { (void)base; (void)size; }
int Sys_NumCores (void) { return 1; }
void Sys_SetWorkers (int workers) { (void)workers; }
void Sys_Parallel (int count, void (*job) (void *ctx, int index), void *ctx)
{
	for (int i = 0 ; i < count ; i++)
		job (ctx, i);
}
viddef_t vid;
void VID_Update (void) { }

//
// the client, as far as the module asks
//

static int	changes;

static float TestTrace (const vec3_t start, const vec3_t end, vec3_t impact, vec3_t normal, int *entnum)
{
	(void)start;
	VectorCopy (end, impact);
	VectorSet (normal, 0, 0, 1);
	*entnum = 0;
	return 1;
}

static int TestContents (const vec3_t p)
{
	(void)p;
	return CONTENTS_EMPTY;
}

static void TestDlight (int key, const vec3_t org, float radius, float time, float decay, const vec3_t rgb)
{
	(void)key;
	(void)org;
	(void)radius;
	(void)time;
	(void)decay;
	(void)rgb;
}

static void TestChanged (void)
{
	changes++;
}

static void Check (bool ok, const char *what)
{
	if (!ok)
	{
		printf ("FAILED %s\n", what);
		failures++;
	}
}

static void WriteFile (const char *path, const char *text)
{
	FILE	*f = fopen (path, "wb");

	if (!f)
	{
		printf ("can't write %s\n", path);
		exit (1);
	}
	fputs (text, f);
	fclose (f);
}

static const char	test_cfg[] =
	"// the test's r_particledesc\n"
	"r_part spark1\n"
	"{\n"
	"	texture ball\n"
	"	count 10\n"
	"	die 1\n"
	"	scale 2\n"
	"	rgb 255 128 0\n"
	"	alpha 0.5\n"
	"	cliptype splash		// made, never defined\n"
	"}\n"
	"r_part +spark1\n"
	"{\n"
	"	count 3\n"
	"	die 1\n"
	"}\n"
	"r_part +spark1\n"
	"{\n"
	"	count 2\n"
	"	die 1\n"
	"	blend invmodc\n"
	"}\n"
	"r_part chain1\n"
	"{\n"
	"	count 1\n"
	"	die 1\n"
	"	assoc chain2\n"
	"}\n"
	"r_part chain2\n"
	"{\n"
	"	count 1\n"
	"	die 1\n"
	"	assoc chain1\n"		// a loop: refused
	"}\n"
	"r_part te_explosion2\n"
	"{\n"
	"	count 4\n"
	"	die 1\n"
	"	colorindex 0 1\n"
	"}\n"
	"r_effect progs/missile.mdl spark1 replace forwards\n"
	"r_trail progs/missile.mdl chain1\n"
	"r_partredirect myspark spark1\n"
	"r_part namespace t2\n"
	"r_part x\n"
	"{\n"
	"	count 7\n"
	"}\n";

// loaded weakly when t2.y is named: its x doesn't replace the strong one
static const char	t2_cfg[] =
	"r_part x\n"
	"{\n"
	"	count 9\n"
	"}\n"
	"r_part y\n"
	"{\n"
	"	count 1\n"
	"}\n";

static const char	effectinfo_txt[] =
	"/* DarkPlaces'\n"
	"   effects */\n"
	"effect TE_GUNSHOT\n"
	"countabsolute 1\n"
	"type decal\n"
	"tex 8 16\n"
	"size 1 2\n"
	"alpha 32 96 0\n"
	"\n"
	"effect TE_GUNSHOT\n"
	"count 1 // a comment\n"
	"type static\n"
	"color 0x101010 0x707070\n"
	"size 1 2\n"
	"alpha 64 96 48\n"
	"velocityjitter 8 8 4\n"
	"gravity 0.01\n"
	"time 1 2\n";

static int	TypeIndex (const char *name)
{
	return P_FindParticleType (name);
}

static void TestScripts (void)
{
	int			spark1, plus0, plus1, chain1, chain2, splash, x, y, ret, gun, gun2;
	unsigned	flags;

	changes = 0;
	Cvar_Set ("r_particledesc", "test");
	Check (changes > 0, "r_particledesc's change reloads");

	spark1 = TypeIndex ("spark1");
	Check (spark1 >= 0 && p_types[spark1].loaded == 2, "spark1 loaded strongly");
	Check (TypeIndex ("test.spark1") == spark1, "spark1 in its namespace");
	Check (TypeIndex ("myspark") == spark1, "r_partredirect's alias");
	if (spark1 < 0)
		return;
	Check (p_types[spark1].looks.type == PT_NORMAL, "a textured type is normal");
	Check (p_types[spark1].rgb[1] > 0.5f && p_types[spark1].rgb[1] < 0.51f, "rgb is in bytes");
	Check (fabsf (p_types[spark1].alphachange + 0.5f) < 1e-6f, "alphachange is the alpha gone by die");

	// +spark1 twice: a chain after it
	plus0 = p_types[spark1].assoc;
	Check (plus0 >= 0 && !strcmp (p_types[plus0].name, "+0spark1"), "+spark1 follows spark1");
	plus1 = plus0 >= 0 ? p_types[plus0].assoc : -1;
	Check (plus1 >= 0 && p_types[plus1].looks.blendmode == RPB_INVMODC, "the second +spark1 follows the first");
	Check (plus0 >= 0 && p_types[plus0].looks.type == PT_SPARK, "a type without an image or size is a spark");

	// cliptype makes the type it names, unloaded
	splash = p_types[spark1].cliptype;
	Check (splash >= 0 && !p_types[splash].loaded && !strcmp (p_types[splash].config, "test"),
		"cliptype's type made in the namespace");
	Check (TypeIndex ("splash") == P_INVALID, "an unloaded type isn't found");

	chain1 = TypeIndex ("chain1");
	chain2 = TypeIndex ("chain2");
	Check (chain1 >= 0 && chain2 >= 0 && p_types[chain1].assoc == chain2, "assoc");
	Check (chain2 >= 0 && p_types[chain2].assoc == P_INVALID, "an assoc that loops is refused");

	Check (P_ModelTrail ("progs/missile.mdl") == chain1, "r_trail");
	Check (P_ModelEmit ("progs/missile.mdl", &flags) == spark1 && flags == (P_EMITREPLACE | P_EMITFORWARDS), "r_effect");
	Check (P_ModelTrail ("progs/player.mdl") == P_INVALID, "no r_trail");

	// t2.y loads t2.cfg, weakly
	x = TypeIndex ("t2.x");
	Check (x >= 0 && p_types[x].count == 7, "namespace t2 in the strong script");
	y = TypeIndex ("t2.y");
	Check (y >= 0 && p_types[y].loaded == 1, "t2.cfg loaded weakly when t2.y is named");
	x = TypeIndex ("t2.x");
	Check (x >= 0 && p_types[x].count == 7, "the weak script doesn't replace x");

	// te_explosion2 in a palette range
	ret = TypeIndex ("te_explosion2_32_8");
	Check (ret >= 0 && p_types[ret].colorindex == 32 && p_types[ret].colorrand == 8, "te_explosion2_32_8's colors");
	Check (ret >= 0 && p_types[ret].count == 4, "te_explosion2_32_8 is te_explosion2");
	Check (TypeIndex ("te_explosion2_32_8") == ret, "te_explosion2_32_8 made once");

	// effectinfo.txt's namespace, two stages of an effect
	gun = TypeIndex ("effectinfo.TE_GUNSHOT");
	Check (gun >= 0 && p_types[gun].loaded == 1, "effectinfo loaded weakly when named");
	if (gun < 0)
		return;
	gun2 = p_types[gun].assoc;
	Check (p_types[gun].looks.type == PT_CDECAL && p_types[gun].looks.blendmode == RPB_INVMODC, "a decal stage");
	Check (gun2 >= 0 && !strcmp (p_types[gun2].name, "0+TE_GUNSHOT"), "the second stage follows the first");
	if (gun2 < 0)
		return;
	Check (p_types[gun2].looks.blendmode == RPB_PREMUL && p_types[gun2].looks.premul == 2, "static adds");
	Check (fabsf (p_types[gun2].alpha - 0.25f) < 1e-6f && fabsf (p_types[gun2].alpharand - 0.125f) < 1e-6f,
		"alpha is a 256th");
	Check (fabsf (p_types[gun2].rgb[0] - 16 / 255.0f) < 1e-6f && fabsf (p_types[gun2].rgbrand[0] - 96 / 255.0f) < 1e-6f,
		"color is a range");
	Check (fabsf (p_types[gun2].gravity - 8) < 1e-4f, "gravity is the world's");
	Check (p_types[gun2].die == 2 && p_types[gun2].randdie == 1, "time from one to the other");
	// without particlefont.tga, all of the image made in its place
	Check (p_types[gun].s1 == 0 && p_types[gun].s2 == 1 && p_types[gun].randsmax == 1, "a missing image's cells");
}

static void TestRun (void)
{
	const r_partscene_t	*scene;
	p_frame_t			frame = {.time = 1, .realtime = 1, .frametime = 0.01f};
	vec3_t				org = {0, 0, 0};
	int					spark1 = TypeIndex ("spark1"), i, sprites = 0, sparks = 0, last;
	bool				ordered = true, firstdark;
	float				alpha = 0;

	P_ClearParticles ();
	P_RunFrame (&frame);
	Check (P_RunEffect (org, NULL, 1, spark1, NULL), "spark1 runs");
	Check (!P_RunEffect (org, NULL, 1, TypeIndex ("splash"), NULL), "an unloaded effect doesn't");
	frame.time = 1.1;
	scene = P_RunFrame (&frame);
	Check (scene != NULL, "a scene of particles");
	if (!scene)
		return;

	// spark1's 10 sprites and +0's 3 lines blend; +1's 2 lines darken, first
	last = -1;
	for (i = 0 ; i < scene->numbatches ; i++)
	{
		const r_partbatch_t	*b = &scene->batches[i];
		int					pass = b->blend == RPB_INVMODC ? 0 : 1;

		if (pass < last)
			ordered = false;
		last = pass;
		if (b->type == RPT_SPRITE)
		{
			sprites += b->count;
			alpha = scene->parts[b->first].rgba[3];
		}
		else if (b->type == RPT_SPARK)
			sparks += b->count;
	}
	firstdark = scene->numbatches > 0 && scene->batches[0].blend == RPB_INVMODC;
	Check (sprites == 10, "spark1's sprites");
	Check (sparks == 5, "+0's and +1's lines");
	Check (ordered && firstdark, "darkening first, then blending");
	Check (fabsf (alpha - 0.45f) < 1e-4f, "alpha fades over the frame");

	// a second on, all dead and the list empty
	frame.time = 2.2;
	P_RunFrame (&frame);
	frame.time = 2.3;
	Check (P_RunFrame (&frame) == NULL && !p_runlist, "the dead gone");
}

// DarkPlaces' weather with no script of it: the built-in te_rain, in the
// palette colour asked for
static void TestWeather (void)
{
	vec3_t	mins = {-64, -64, 0}, maxs = {64, 64, 64}, dir = {0, 0, -400};
	int		rain;

	P_ClearParticles ();
	Check (P_FindParticleType ("te_rain") == P_INVALID, "no te_rain in the scripts");
	P_RunWeather (mins, maxs, dir, 20, 15, "rain");
	rain = P_FindParticleType ("builtin.te_rain");
	Check (rain >= 0 && p_types[rain].colorindex == 15, "the built-in te_rain, in colour 15");
	Check (rain >= 0 && p_types[rain].particles != NULL, "it rained");
	Check (rain >= 0 && p_types[rain].cliptype == P_FindParticleType ("builtin.rainsplash"), "it splashes");
}

int main (void)
{
	static byte	palette[768];
	p_host_t	host =
	{
		.palette = palette,
		.trace = TestTrace,
		.contents = TestContents,
		.dlight = TestDlight,
		.changed = TestChanged,
	};
	const char	*base = "test_part_data";

	// loose files outside pak files are only found at the top of a
	// shareware game directory (<name>.cfg, as well as particles/<name>.cfg)
	mkdir (base, 0777);
	mkdir ("test_part_data/id1", 0777);
	WriteFile ("test_part_data/id1/test.cfg", test_cfg);
	WriteFile ("test_part_data/id1/t2.cfg", t2_cfg);
	WriteFile ("test_part_data/id1/effectinfo.txt", effectinfo_txt);

	COM_Init (base);
	P_Init (&host);
	TestScripts ();
	TestRun ();
	TestWeather ();
	P_Shutdown ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("all passed\n");
	return 0;
}
