// test_qc_csprogs.c -- a real CSQC module, KTX's weapon-prediction csprogs.dat
// named by QCVM_CSPROGS (with its .lno beside it), against the stub engine in
// qc_csqc_engine.c (qcvm-rs's tests/all/csprogs.rs)
//
// The scenarios follow what the mod does in a real client: initialisation,
// weapon snapshots, projectile entities, a thousand predicted frames with the
// fire button held, temp entities, sound suppression and error reporting.
// QCVM_PROFILE=1 adds a report of how often each opcode and each straight-line
// opcode pair runs over a thousand frames of play.

#include "qc_csqc_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char	*csprogs_path;

static bool StartClient (cse_client_t *c, qc_progs_t *p, const qc_builtins_t *b)
{
	CSE_EngineInit (&c->host);
	return QT_CHECK (CSE_ClientStart (c, p, b));
}

static bool StartsWith (const char *s, const char *prefix)
{
	return !strncmp (s, prefix, strlen (prefix));
}

static bool EndsWith (const char *s, const char *suffix)
{
	size_t	n = strlen (s), m = strlen (suffix);

	return n >= m && !strcmp (s + n - m, suffix);
}

static bool SceneHas (const cse_scene_t *s, uint32_t e)
{
	uint32_t	i;

	for (i = 0 ; i < s->count ; i++)
		if (s->ents[i] == e)
			return true;
	return false;
}

static qc_ent_t GlobalEnt (qcvm_t *vm, const char *name)
{
	uint32_t	w;

	return CSE_Global (vm, name, QC_EV_ENTITY, &w) ? QC_Globals (vm)[w].u : 0;
}

static void PrintWarnings (const cse_engine_t *h)
{
	uint32_t	i;

	for (i = 0 ; i < h->warnings.count ; i++)
		printf ("  warning: %s\n", h->warnings.items[i]);
}

// Plays frames frames with the fire button held; out gets the frames on which
// the predicted rocket sound played. Returns how many there were.
static uint32_t FireRockets (cse_client_t *c, int32_t frames, int32_t *out, uint32_t max)
{
	uint32_t	n = 0, before, i;
	bool		played;
	int32_t		k;

	for (k = 0 ; k < frames ; k++)
	{
		before = c->host.numsounds;
		CSE_Step (c, true);
		played = false;
		for (i = before ; i < c->host.numsounds ; i++)
			played |= !strcmp (c->host.sounds[i].sample, ROCKET_SOUND);
		if (played)
		{
			if (n < max)
				out[n] = c->frame - 1;
			n++;
		}
	}
	return n;
}

/*
==============================================================================

SCENARIOS

==============================================================================
*/

// the pre-flight check an engine makes before enabling CSQC, and every global
// and field the engine reads or writes resolving with the engine's type
static void TestEveryCalledBuiltinIsBoundAndHandlesResolve (qc_progs_t *p, const qc_builtins_t *b)
{
	static const char	*floats[] = {"time", "cltime", "frametime", "player_localentnum", "player_localnum",
		"clientcommandframe", "servercommandframe", "intermission", "trace_fraction", "input_timelength",
		"input_buttons", "input_impulse"};
	static const char	*vectors[] = {"pmove_org", "view_angles", "input_angles", "trace_endpos",
		"trace_plane_normal", "v_forward", "v_right", "v_up"};
	static const char	*vfields[] = {"origin", "angles", "velocity"};
	static const char	*ffields[] = {"modelindex", "modelflags", "drawmask", "renderflags", "dimension_hit"};
	qc_config_t			config;
	qcvm_t				*vm;
	qc_unbound_t		unbound[64];
	qc_definfo_t		def;
	uint32_t			i, n, w, value;
	bool				projectiles = false, debug = false;

	QC_DefaultConfig (&config, QC_CSQC);
	vm = QC_Create (p, b, &config, NULL, NULL, NULL);
	if (!QT_CHECK (vm != NULL))
		return;

	// the mod's debug build declares hundreds of builtins, but only those it calls matter
	QT_CHECK (QC_ProgsCalledBuiltins (p, NULL, 0) > 30);
	QT_CHECK (QC_UnboundBuiltins (vm, false, NULL, 0) > 100);
	n = QC_UnboundBuiltins (vm, true, unbound, 64);
	if (!QT_EQ_U (n, 0))
		for (i = 0 ; i < n && i < 64 ; i++)
			printf ("  unbound builtin the progs calls: %s #%u\n", unbound[i].name, unbound[i].number);

	for (i = 0 ; i < sizeof(floats) / sizeof(floats[0]) ; i++)
		if (!QT_CHECK (CSE_Global (vm, floats[i], QC_EV_FLOAT, &w)))
			printf ("  float global %s\n", floats[i]);
	for (i = 0 ; i < sizeof(vectors) / sizeof(vectors[0]) ; i++)
		if (!QT_CHECK (CSE_Global (vm, vectors[i], QC_EV_VECTOR, &w)))
			printf ("  vector global %s\n", vectors[i]);
	QT_CHECK (CSE_Global (vm, "self", QC_EV_ENTITY, &w));
	// type mismatches are rejected
	QT_CHECK (!CSE_Global (vm, "self", QC_EV_FLOAT, &w));
	for (i = 0 ; i < sizeof(vfields) / sizeof(vfields[0]) ; i++)
		if (!QT_CHECK (CSE_Field (vm, vfields[i], QC_EV_VECTOR, &w)))
			printf ("  vector field %s\n", vfields[i]);
	for (i = 0 ; i < sizeof(ffields) / sizeof(ffields[0]) ; i++)
		if (!QT_CHECK (CSE_Field (vm, ffields[i], QC_EV_FLOAT, &w)))
			printf ("  float field %s\n", ffields[i]);
	QT_CHECK (CSE_Field (vm, "predraw", QC_EV_FUNCTION, &w));

	// autocvars with their compiled defaults
	for (i = 0 ; QC_ProgsGlobalDefAt (p, i, &def) ; i++)
	{
		if (strncmp (def.name, "autocvar_", 9) || !QC_ProgsInitialGlobal (p, def.ofs, &value))
			continue;
		if (!strcmp (def.name + 9, "cl_predict_projectiles"))
			projectiles = QT_EQ_F (CSE_Float (value), 1.0);
		else if (!strcmp (def.name + 9, "cl_predict_debug"))
			debug = QT_EQ_F (CSE_Float (value), 0.0);
	}
	QT_CHECK (projectiles);
	QT_CHECK (debug);
	QC_Destroy (vm);
}

static void TestInitAndEntityUpdates (qc_progs_t *p, const qc_builtins_t *b)
{
	cse_client_t	c = {0};
	cse_snapshot_t	snap = {ROCKET_LAUNCHER, 1, 20, 0, 0, 50, 0};
	cse_msg_t		msg = {0};
	const float		origin[3] = {100, 200, 50}, velocity[3] = {0, 1000, 0};
	const char		*s, *printed;
	qc_ent_t		viewweapon, proj;
	uint32_t		ofs, predraw, count;
	int16_t			model;
	float			o[3];
	qc_value_t		ret;
	bool			ok;

	if (!StartClient (&c, p, b))
	{
		CSE_ClientFree (&c);
		return;
	}

	// CSQC_Init precached every weapon and spawned the view weapon
	QT_CHECK (CSE_ModelIndex (&c.host, "progs/v_rock2.mdl") != 0);
	QT_CHECK (CSE_ModelIndex (&c.host, ROCKET_MODEL) != 0);
	QT_CHECK (CSE_Contains (&c.host.sounds_precached, ROCKET_SOUND));
	viewweapon = GlobalEnt (c.vm, "viewweapon");
	QT_CHECK (viewweapon != 0);
	QT_EQ_F (CSE_FieldF (c.vm, viewweapon, "renderflags"), RF_VIEWMODEL);
	QT_EQ_F (CSE_FieldF (c.vm, viewweapon, "dimension_hit"), 254);
	QT_EQ_F (CSE_FieldF (c.vm, viewweapon, "dimension_solid"), 255);		// the CSQC spawn default
	QT_CHECK (QC_IsProtected (c.vm, 0));

	// the first weapon snapshot announces prediction, once
	CSE_SnapshotMsg (&snap, &msg);
	ok = CSE_EntUpdate (&c, 500, &msg);
	CSE_OK (&c, ok);
	CSE_SnapshotMsg (&snap, &msg);
	ok = CSE_EntUpdate (&c, 500, &msg);
	CSE_OK (&c, ok);
	printed = c.host.printed.text ? c.host.printed.text : "";
	for (count = 0, s = printed ; (s = strstr (s, "CSQC Antilag ready")) ; s++)
		count++;
	if (!QT_EQ_U (count, 1))
		printf ("%s", printed);

	// a server projectile: its model by index, drawn through the engine mask with a predraw
	model = (int16_t)CSE_ModelIndex (&c.host, ROCKET_MODEL);
	CSE_ProjectileMsg (&msg, origin, velocity, model, 2);
	ok = CSE_EntUpdate (&c, 600, &msg);
	CSE_OK (&c, ok);
	proj = CSE_Ent (&c, 600);
	QT_EQ_F (CSE_FieldF (c.vm, proj, "modelindex"), model);
	QT_EQ_F (CSE_FieldF (c.vm, proj, "modelflags"), 1);
	QT_EQ_F (CSE_FieldF (c.vm, proj, "drawmask"), MASK_ENGINE);
	QT_EQ_F (CSE_FieldF (c.vm, proj, "dimension_hit"), 254);
	CSE_FieldV (c.vm, proj, "origin", o);
	QT_CHECK (o[0] == 100 && o[1] == 200 && o[2] == 50);
	predraw = 0;
	QT_CHECK (CSE_Field (c.vm, "predraw", QC_EV_FUNCTION, &ofs) && QC_GetField (c.vm, proj, ofs, 1, &predraw));
	QT_CHECK (predraw != 0);

	// the projectile flies on in its predraw and is drawn; removal frees it
	CSE_Render (&c, false);
	CSE_Render (&c, false);
	QT_CHECK (c.host.numrendered > 0 && SceneHas (&c.host.rendered[c.host.numrendered - 1], proj));
	CSE_FieldV (c.vm, proj, "origin", o);
	QT_CHECK (o[1] > 200);
	CSE_EntRemove (&c, 600);

	// temp entities are passed back to the engine
	CSE_MsgShort (CSE_MsgByte (&msg, TE_LIGHTNING2), 1);
	free (c.host.net);
	c.host.net = msg.data;
	c.host.netlen = msg.len;
	c.host.netpos = 0;
	msg = (cse_msg_t){0};
	ret = (qc_value_t){{0}};
	ok = CSE_Call (&c, "CSQC_Parse_TempEntity", 0, NULL, &ret);
	CSE_OK (&c, ok);
	QT_EQ_F (CSE_Float (ret.w[0]), 0);
	QT_EQ_U (CSE_NetLeft (&c.host), 0);

	ok = CSE_Call (&c, "CSQC_Input_Frame", 0, NULL, NULL);
	CSE_OK (&c, ok);
	CSE_ClientFree (&c);
}

static void TestPredictsAThousandFramesOfFire (qc_progs_t *p, const qc_builtins_t *b)
{
	cse_client_t	c = {0};
	int32_t			sounds[64];
	uint32_t		n, i, spawned = 0, alive = 0, ofs, v, peak = 0, live, start;
	qc_ent_t		viewweapon, e;
	char			addentity[32];
	bool			before = true, after = true, first_is_render, has_add1 = false, has_add = false;

	if (!StartClient (&c, p, b))
	{
		CSE_ClientFree (&c);
		return;
	}
	n = FireRockets (&c, 1000, sounds, 64);

	// One predicted shot per refire interval (0.8 s at 60 fps = 48 frames),
	// matching the server shot for shot, except the server's first shot, which
	// came before any snapshot. Replay walks the same input frames every render,
	// so this also checks that a shot's effects fire once.
	if (!QT_EQ_U (n + 1, c.server.shots))
		for (i = 0 ; i < n && i < 64 ; i++)
			printf ("  shot on frame %d\n", sounds[i]);
	for (i = 1 ; i < n && i < 64 ; i++)
		if (sounds[i] - sounds[i - 1] < 48 || sounds[i] - sounds[i - 1] > 49)
			break;
	if (!QT_CHECK (i >= n || i >= 64))
		printf ("  shots on frames %d and %d\n", sounds[i - 1], sounds[i]);

	// every predicted shot spawned one local rocket; they expire in their
	// predraw (removing themselves mid-walk) after about four frames, long
	// before the next shot
	for (i = 0 ; i < c.host.log.count ; i++)
		spawned += StartsWith (c.host.log.items[i], "setmodel") && EndsWith (c.host.log.items[i], "missile.mdl");
	QT_EQ_U (spawned, n);
	if (QT_CHECK (CSE_Field (c.vm, "is_local", QC_EV_FLOAT, &ofs)))
		for (e = 0 ; e < QC_NumEdicts (c.vm) ; e++)
			alive += !QC_IsFree (c.vm, e) && QC_GetField (c.vm, e, ofs, 1, &v) && CSE_Float (v) == 1.0f;
	if (!QT_CHECK (alive <= 1))
		printf ("  %u local rockets still alive\n", alive);

	// once the first snapshot has arrived (frame 6) we draw the view weapon
	// ourselves; before that the engine's is used
	viewweapon = GlobalEnt (c.vm, "viewweapon");
	QT_CHECK (c.host.numrendered > 6);
	for (i = 0 ; i < c.host.numrendered ; i++)
	{
		if (i < 6)
			before &= !SceneHas (&c.host.rendered[i], viewweapon);
		else
			after &= SceneHas (&c.host.rendered[i], viewweapon);
	}
	QT_CHECK (before);
	QT_CHECK (after);
	// the engine's view model before that
	QT_CHECK (CSE_Contains (&c.host.log, "addentities 2"));
	// the last frame, back to its clearscene
	for (start = c.host.log.count ; start > 0 && strcmp (c.host.log.items[start - 1], "clearscene") ; start--)
		;
	first_is_render = c.host.log.count > start && StartsWith (c.host.log.items[c.host.log.count - 1], "renderscene");
	snprintf (addentity, sizeof(addentity), "addentity #%u", viewweapon);
	for (i = start ; i < c.host.log.count ; i++)
	{
		has_add1 |= !strcmp (c.host.log.items[i], "addentities 1");
		has_add |= !strcmp (c.host.log.items[i], addentity);
	}
	QT_CHECK (first_is_render);
	QT_CHECK (has_add1);
	QT_CHECK (has_add);

	// Temp strings are collected as frames return to the engine: the count
	// stays below the collector's trigger (half the initial 1024-slot table)
	// however long the game runs.
	for (i = 0 ; i < 2000 ; i++)
	{
		CSE_Step (&c, false);
		live = QC_NumTempStrings (c.vm);
		peak = live > peak ? live : peak;
	}
	if (!QT_CHECK (peak <= 512))
		printf ("  %u live temps\n", peak);
	if (!QT_EQ_U (c.host.warnings.count, 0))
		PrintWarnings (&c.host);
	CSE_ClientFree (&c);
}

static float EventSound (cse_client_t *c, const char *sample)
{
	qc_value_t	args[8], ret = {{0}};
	bool		ok;

	args[0] = QC_ValFloat (PLAYER_ENT);
	args[1] = QC_ValFloat (CHAN_WEAPON);
	args[2] = QC_ValWord (QC_TempString (c->vm, sample, strlen (sample)));
	args[3] = QC_ValFloat (1);
	args[4] = QC_ValFloat (0);
	args[5] = QC_ValVector (0, 0, 0);
	args[6] = QC_ValFloat (0);
	args[7] = QC_ValFloat (0);
	ok = CSE_Call (c, "CSQC_Event_Sound", 8, args, &ret);
	CSE_OK (c, ok);
	return CSE_Float (ret.w[0]);
}

static void TestSuppressesTheServersEchoOfAPredictedSound (qc_progs_t *p, const qc_builtins_t *b)
{
	cse_client_t	c = {0};
	int32_t			sounds[16];
	uint32_t		n;

	if (!StartClient (&c, p, b))
	{
		CSE_ClientFree (&c);
		return;
	}
	n = FireRockets (&c, 60, sounds, 16);
	// the host's copy of the sample name (a temp string) matches the progs'
	// constant; the predicted play left one token, which the first echo consumes
	QT_EQ_U (n, 1);
	QT_EQ_F (EventSound (&c, ROCKET_SOUND), 1);				// the echo of the predicted shot is dropped
	QT_EQ_F (EventSound (&c, ROCKET_SOUND), 0);				// the token was consumed
	QT_EQ_F (EventSound (&c, "weapons/grenade.wav"), 0);		// sounds not predicted play
	CSE_ClientFree (&c);
}

static void TestDebugOutputAndProjectileHandOff (qc_progs_t *p, const qc_builtins_t *b)
{
	static const char	*needles[] = {"wpred: ", "proj: predicted progs/missile.mdl on frame ", "proj: took over, "};
	cse_client_t		c = {0};
	const float			velocity[3] = {0, 1000, 0};
	cse_msg_t			msg = {0};
	const char			*printed;
	qc_ent_t			local;
	int16_t				model;
	float				at[3];
	uint32_t			i;
	bool				ok;

	CSE_EngineInit (&c.host);
	CSE_SetPair (&c.host.cvars, "cl_predict_debug", "2");
	if (!QT_CHECK (CSE_ClientStart (&c, p, b)))
	{
		CSE_ClientFree (&c);
		return;
	}
	while (!CSE_LocalProjectile (&c))
	{
		if (!QT_CHECK (c.frame < 120))
		{
			printf ("  no rocket predicted\n");
			CSE_ClientFree (&c);
			return;
		}
		CSE_Step (&c, true);
	}

	// the server's copy of our rocket arrives near the predicted one and takes it over
	model = (int16_t)CSE_ModelIndex (&c.host, ROCKET_MODEL);
	local = CSE_LocalProjectile (&c);
	CSE_FieldV (c.vm, local, "origin", at);
	CSE_ProjectileMsg (&msg, at, velocity, model, (int16_t)PLAYER_ENT);
	ok = CSE_EntUpdate (&c, 700, &msg);
	CSE_OK (&c, ok);
	QT_CHECK (QC_IsFree (c.vm, local));		// the local rocket was handed over
	printed = c.host.printed.text ? c.host.printed.text : "";
	for (i = 0 ; i < sizeof(needles) / sizeof(needles[0]) ; i++)
		if (!QT_CHECK (strstr (printed, needles[i]) != NULL))
			printf ("  missing \"%s\" in:\n%s\n", needles[i], printed);
	if (!QT_EQ_U (c.host.warnings.count, 0))
		PrintWarnings (&c.host);
	CSE_ClientFree (&c);
}

static void TestUnknownEntityKindIsAQCErrorWithABacktrace (qc_progs_t *p, const qc_builtins_t *b)
{
	cse_client_t		c = {0};
	cse_msg_t			msg = {0};
	const qc_error_t	*err;
	const qc_btframe_t	*frame = NULL;
	char				lnopath[1024], *dot, text[4096];
	uint8_t				*lno;
	size_t				lnosize;
	uint32_t			i;
	bool				has_lines;

	snprintf (lnopath, sizeof(lnopath), "%s", csprogs_path);
	dot = strrchr (lnopath, '.');
	if (dot && !strchr (dot, '/'))
		*dot = 0;
	strncat (lnopath, ".lno", sizeof(lnopath) - strlen (lnopath) - 1);
	lno = QT_LoadFile (lnopath, &lnosize);
	has_lines = lno != NULL;
	free (lno);

	if (!StartClient (&c, p, b))
	{
		CSE_ClientFree (&c);
		return;
	}
	CSE_MsgByte (&msg, 7);
	QT_CHECK (!CSE_EntUpdate (&c, 900, &msg));
	err = QC_LastError (c.vm);
	QT_EQ_U (err->kind, QC_ERR_QC);
	QT_EQ_S (err->message ? err->message : "", "csqc: unknown entity type 7\n");
	for (i = 0 ; i < err->backtrace.count ; i++)
		if (!strcmp (err->backtrace.frames[i].name, "CSQC_Ent_Update"))
		{
			frame = &err->backtrace.frames[i];
			break;
		}
	if (QT_CHECK (frame != NULL))
	{
		QT_EQ_S (frame->file, "main.qc");
		if (has_lines)
			QT_CHECK (frame->line > 1);
	}
	else
		printf ("%s", QC_BacktraceText (&err->backtrace, text, sizeof(text)));
	// the VM is usable after the error
	CSE_Render (&c, false);
	CSE_ClientFree (&c);
}

// the log of 120 frames of fire, taken from the engine
static cse_strings_t RunAndTakeLog (cse_client_t *c)
{
	int32_t			sounds[8];
	cse_strings_t	log;

	FireRockets (c, 120, sounds, 8);
	log = c->host.log;
	c->host.log = (cse_strings_t){0};
	return log;
}

static bool SameLog (const cse_strings_t *a, const cse_strings_t *b)
{
	uint32_t	i;

	if (a->count != b->count)
	{
		printf ("  %u lines, not %u\n", a->count, b->count);
		return false;
	}
	for (i = 0 ; i < a->count ; i++)
		if (strcmp (a->items[i], b->items[i]))
		{
			printf ("  line %u: \"%s\", not \"%s\"\n", i, a->items[i], b->items[i]);
			return false;
		}
	return true;
}

static void TestRunsAreReproducibleAcrossVMsAndResets (qc_progs_t *p, const qc_builtins_t *b)
{
	cse_client_t	a = {0}, c = {0};
	cse_strings_t	log_a, log_b, log_c;

	if (!StartClient (&a, p, b) || !StartClient (&c, p, b))
	{
		CSE_ClientFree (&a);
		CSE_ClientFree (&c);
		return;
	}
	log_a = RunAndTakeLog (&a);
	log_b = RunAndTakeLog (&c);
	QT_CHECK (log_a.count > 500);
	// two VMs sharing one program
	QT_CHECK (SameLog (&log_a, &log_b));

	// a VM after QC_Reset
	QT_CHECK (QC_Reset (a.vm));
	CSE_EngineFree (&a.host);
	CSE_EngineInit (&a.host);
	CSE_ClientInit (&a);
	log_c = RunAndTakeLog (&a);
	QT_CHECK (SameLog (&log_c, &log_b));

	CSE_FreeStrings (&log_a);
	CSE_FreeStrings (&log_b);
	CSE_FreeStrings (&log_c);
	CSE_ClientFree (&a);
	CSE_ClientFree (&c);
}

// how often each opcode and each straight-line opcode pair runs over a
// thousand frames of play, for choosing interpreter fast paths: a report, not
// a check
static void StatementProfile (qc_progs_t *p, const qc_builtins_t *b)
{
	cse_client_t	c = {0};
	int32_t			sounds[32];

	if (!StartClient (&c, p, b))
	{
		CSE_ClientFree (&c);
		return;
	}
	FireRockets (&c, 60, sounds, 32);
	CSE_ProfileStart (&c.host);
	QC_SetTrace (c.vm, true);
	FireRockets (&c, 1000, sounds, 32);
	QC_SetTrace (c.vm, false);
	printf ("over 1000 frames: ");
	CSE_ProfileReport (&c.host, 30);
	CSE_ClientFree (&c);
}

int main (void)
{
	qc_progs_t		*p;
	qc_builtins_t	*b;
	const char		*profile = getenv ("QCVM_PROFILE");

	csprogs_path = getenv ("QCVM_CSPROGS");
	if (!csprogs_path || !*csprogs_path)
	{
		printf ("csprogs: skipped (QCVM_CSPROGS names no csprogs.dat)\n");
		return QT_SKIP;
	}
	p = CSE_LoadProgram (csprogs_path);
	b = CSE_Builtins ();
	if (!QT_CHECK (p != NULL) || !QT_CHECK (b != NULL))
	{
		QC_ReleaseProgs (p);
		QC_BuiltinsFree (b);
		return QT_Finish ("csprogs", "");
	}

	TestEveryCalledBuiltinIsBoundAndHandlesResolve (p, b);
	TestInitAndEntityUpdates (p, b);
	TestPredictsAThousandFramesOfFire (p, b);
	TestSuppressesTheServersEchoOfAPredictedSound (p, b);
	TestDebugOutputAndProjectileHandOff (p, b);
	TestUnknownEntityKindIsAQCErrorWithABacktrace (p, b);
	TestRunsAreReproducibleAcrossVMsAndResets (p, b);
	if (profile && *profile && strcmp (profile, "0"))
		StatementProfile (p, b);

	QC_ReleaseProgs (p);
	QC_BuiltinsFree (b);
	return QT_Finish ("csprogs", "KTX's csprogs runs against the stub engine: init, entities, prediction, errors");
}
