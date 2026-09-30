// qc_csqc_engine.h -- a stub engine for KTX's weapon-prediction csprogs.dat
// (qcvm-rs's tests/all/support/csqc_engine.rs), for test_qc_csprogs
//
// It implements the engine builtins the progs calls: it records what they did,
// feeds scripted network messages to the Read* builtins and scripted input
// frames to getinputstate, and models the world as a floor plane at z = 0 for
// traceline. The standard builtins supply every other one. A cse_client_t
// drives the progs the way a client does.
#pragma once

#include "qcvm.h"
#include "qc_test.h"

#define MASK_ENGINE			1
#define RF_VIEWMODEL		1.0f
#define VF_ORIGIN			11.0f
#define STAT_HEALTH			0
#define STAT_ITEMS			15
#define STAT_KTX_GRAVITY	32
#define IT_ROCKET_LAUNCHER	32u
#define TE_LIGHTNING2		6
#define CHAN_WEAPON			1.0f
#define EZCSQC_WEAPONINFO	1
#define EZCSQC_PROJECTILE	2
#define ROCKET_LAUNCHER		7
#define ROCKET_SOUND		"weapons/sgun1.wav"
#define ROCKET_MODEL		"progs/missile.mdl"
#define FPS					60.0f
// input frames between sending a command and the server acknowledging it (100 ms)
#define LATENCY				6
// our entity number on the server
#define PLAYER_ENT			1.0f

#define CSE_MAX_STATS		256

// a growable list of strings
typedef struct
{
	char		**items;
	uint32_t	count, size;
} cse_strings_t;

// keys and their text values
typedef struct
{
	char		**keys, **values;
	uint32_t	count, size;
} cse_pairs_t;

// one input frame, as getinputstate reports it
typedef struct
{
	bool	present;
	float	timelength;
	float	angles[3];
	float	buttons;
	float	impulse;
} cse_input_t;

// a sound played: entity, channel, sample
typedef struct
{
	uint32_t	ent;
	float		chan;
	char		*sample;
} cse_sound_t;

// the entities a scene held
typedef struct
{
	uint32_t	*ents;
	uint32_t	count, size;
} cse_scene_t;

// statement counts, collected from trace lines while tracing is on: each
// opcode, and each pair where the second ran straight after the first
typedef struct cse_profile_s cse_profile_t;

typedef struct
{
	cse_strings_t	log;				// every engine builtin call, in order
	qt_text_t		printed;
	cse_strings_t	warnings;
	cse_pairs_t		cvars;
	float			stats[CSE_MAX_STATS];
	cse_pairs_t		serverkeys;
	cse_pairs_t		playerkeys;			// values as text

	uint8_t			*net;				// the network message the Read* builtins consume
	size_t			netlen, netpos;
	bool			net_underflow;		// a Read* ran past the end of the message

	cse_input_t		*inputs;			// by frame
	uint32_t		numinputs;

	cse_strings_t	models;				// model index i is models[i - 1]
	cse_strings_t	sounds_precached;
	cse_strings_t	effects;
	cse_scene_t		scene;				// entities added since clearscene
	cse_sound_t		*sounds;
	uint32_t		numsounds, soundsize;
	float			view_origin[3];
	cse_scene_t		*rendered;			// what the scene held at each renderscene
	uint32_t		numrendered, renderedsize;

	// resolved once the progs is loaded
	bool			has_handles;
	uint32_t		drawmask_f;
	bool			has_predraw;
	uint32_t		predraw_f;

	cse_profile_t	*profile;			// or NULL
} cse_engine_t;

void	CSE_EngineInit (cse_engine_t *e);
void	CSE_EngineFree (cse_engine_t *e);

void	CSE_Push (cse_strings_t *list, const char *text);
void	CSE_FreeStrings (cse_strings_t *list);
bool	CSE_Contains (const cse_strings_t *list, const char *text);
void	CSE_SetPair (cse_pairs_t *pairs, const char *key, const char *value);
const char	*CSE_Pair (const cse_pairs_t *pairs, const char *key);	// NULL if none

// a precached model's index (1 for the first), 0 if not precached
uint32_t	CSE_ModelIndex (const cse_engine_t *e, const char *name);
size_t		CSE_NetLeft (const cse_engine_t *e);

// the report of the counts, the busiest first
void	CSE_ProfileStart (cse_engine_t *e);
void	CSE_ProfileReport (cse_engine_t *e, uint32_t top);

// the standard builtins plus the stub engine's, numbered for CSQC
qc_builtins_t	*CSE_Builtins (void);

// csprogs.dat at path, with the .lno beside it if there is one; NULL if unreadable
qc_progs_t	*CSE_LoadProgram (const char *path);

// the host callbacks, with the engine as their context
extern const qc_host_t	cse_host;

/*
------------------------------------------------------------------------------
typed access, as qcvm-rs's typed handles: a missing name or another type
reads as zero and ignores writes
------------------------------------------------------------------------------
*/

bool	CSE_Global (qcvm_t *vm, const char *name, uint32_t type, uint32_t *word);
bool	CSE_Field (const qcvm_t *vm, const char *name, uint32_t type, uint32_t *ofs);
void	CSE_SetGlobalF (qcvm_t *vm, const char *name, float v);
void	CSE_SetGlobalV (qcvm_t *vm, const char *name, float x, float y, float z);
void	CSE_SetGlobalE (qcvm_t *vm, const char *name, qc_ent_t e);
float	CSE_FieldF (const qcvm_t *vm, qc_ent_t e, const char *name);
void	CSE_FieldV (const qcvm_t *vm, qc_ent_t e, const char *name, float out[3]);
void	CSE_SetFieldF (qcvm_t *vm, qc_ent_t e, const char *name, float v);
float	CSE_Float (uint32_t bits);

/*
------------------------------------------------------------------------------
network messages
------------------------------------------------------------------------------
*/

typedef struct
{
	uint8_t	*data;
	size_t	len, size;
} cse_msg_t;

cse_msg_t	*CSE_MsgByte (cse_msg_t *m, uint8_t v);
cse_msg_t	*CSE_MsgShort (cse_msg_t *m, int16_t v);
cse_msg_t	*CSE_MsgFloat (cse_msg_t *m, float v);
cse_msg_t	*CSE_MsgCoords (cse_msg_t *m, const float v[3]);

// a weapon snapshot as the server sends it (every section present)
typedef struct
{
	uint8_t	weapon;
	uint8_t	generation;
	uint8_t	rockets;
	float	attack_finished;
	float	client_time;
	uint8_t	ping_ms;
	uint8_t	predflags;
} cse_snapshot_t;

void	CSE_SnapshotMsg (const cse_snapshot_t *s, cse_msg_t *m);
// a server projectile update with every section present
void	CSE_ProjectileMsg (cse_msg_t *m, const float origin[3], const float velocity[3], int16_t model,
			int16_t owner);

/*
------------------------------------------------------------------------------
the client
------------------------------------------------------------------------------
*/

// The server's side of the weapon: it processes our input frames LATENCY frames
// late, fires the rocket launcher whenever the button is held and the refire
// time has passed, and sends a weapon snapshot every sixth frame.
typedef struct
{
	int32_t		acked;			// the last input frame processed, or -1
	float		clock;
	float		attack_finished;
	uint32_t	shots;
} cse_server_t;

typedef struct
{
	uint16_t	num;
	qc_ent_t	e;
} cse_entmap_t;

typedef struct
{
	qcvm_t			*vm;
	cse_engine_t	host;
	cse_entmap_t	*ents;			// CSQC entities by network entity number
	uint32_t		numents, entsize;
	int32_t			frame;
	cse_server_t	server;
} cse_client_t;

// Creates the VM (with c->host, set up by CSE_EngineInit and the caller) and
// runs CSE_ClientInit; false if the VM couldn't be made.
bool	CSE_ClientStart (cse_client_t *c, qc_progs_t *progs, const qc_builtins_t *builtins);
// what the engine does when the progs is loaded: autocvars, CSQC_Init, then
// the world becomes read-only
void	CSE_ClientInit (cse_client_t *c);
void	CSE_ClientFree (cse_client_t *c);

// a check that a call succeeded, printing the VM's error and backtrace if not
bool	CSE_Report (const cse_client_t *c, bool ok, const char *what, const char *file, int line);
#define CSE_OK(c, ok)	CSE_Report ((c), (ok), #ok, __FILE__, __LINE__)

qc_func_t	CSE_Func (cse_client_t *c, const char *name);		// checked to exist
bool		CSE_Call (cse_client_t *c, const char *name, int argc, const qc_value_t *args, qc_value_t *ret);
float		CSE_Time (const cse_client_t *c);
qc_ent_t	CSE_Ent (const cse_client_t *c, uint16_t entnum);	// 0 if none

// Delivers a CSQC entity update for network entity entnum (the message is
// consumed); false (QC_LastError) on a QuakeC error.
bool	CSE_EntUpdate (cse_client_t *c, uint16_t entnum, cse_msg_t *msg);
void	CSE_EntRemove (cse_client_t *c, uint16_t entnum);

// a video frame: this frame's input and the globals the engine keeps
void	CSE_BeginFrame (cse_client_t *c, bool attack);
// finishes a video frame by rendering it
void	CSE_Draw (cse_client_t *c);
void	CSE_Render (cse_client_t *c, bool attack);
// one frame of play: the server catches up with our input (and every sixth
// frame sends a weapon snapshot), then we render
void	CSE_Step (cse_client_t *c, bool attack);

// the first local (predicted) projectile alive, or 0
qc_ent_t	CSE_LocalProjectile (const cse_client_t *c);
