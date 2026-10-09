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

#pragma once

#include "pmove.h"
#include "cvar.h"
#include "info.h"
#include "mathlib.h"
#include "msg.h"
#include "net.h"
#include "particles.h"
#include "protocol.h"
#include "q_types.h"
#include "render.h"
#include "vid.h"
// client.h


typedef struct
{
	char		name[16];
	bool	failedload;		// the name isn't a valid skin
	byte		*data;			// 320*200 pixels once loaded
} skin_t;

// player_state_t is the information needed by a player entity
// to do move prediction and to generate a drawable entity
typedef struct
{
	int			messagenum;		// all player's won't be updated each frame

	double		state_time;		// not the same as the packet time,
								// because player commands come asyncronously
	usercmd_t	command;		// last command for prediction

	vec3_t		origin;
	vec3_t		viewangles;		// only for demos, not from server
	vec3_t		velocity;
	int			weaponframe;

	int			modelindex;
	int			frame;
	int			skinnum;
	int			effects;

	int			flags;			// dead, gib, etc
	byte		alpha;			// FTE_PEXT_TRANS: 0 is opaque, else alpha * 254
	byte		colormod[3];	// FTE_PEXT_COLOURMOD: 32 is 1.0; 0 0 0 is unset

	float		waterjumptime;
	bool		onground;		// the server's with Z_EXT_PF_ONGROUND, else predicted
	int			waterlevel;		// predicted: 0 dry .. 3 head under
	bool		jump_held;		// the server's with Z_EXT_PM_TYPE, else predicted
	int			jump_msec;
	int			pm_type;		// pmtype_t: the server's with Z_EXT_PM_TYPE, else a guess
	int			vw_index;		// Z_EXT_VWEP: the weapon model, 0 none
} player_state_t;


#define	MAX_SCOREBOARD		16		// max numbers of players
#define	MAX_SCOREBOARDNAME	16
typedef struct player_info_s
{
	int		userid;
	char	userinfo[MAX_INFO_STRING];

	// scoreboard information
	char	name[MAX_SCOREBOARDNAME];
	float	entertime;
	int		frags;
	int		ping;
	byte	pl;

	// skin information
	int		topcolor;
	int		bottomcolor;

	int		_topcolor;
	int		_bottomcolor;

	int		spectator;
	int		stats[MAX_CL_STATS];	// an MVD's, the player's own
	float	statsf[MAX_CL_STATS];
	byte	translate[256];		// the palette with the player's colors, for the colormap
	pixel_t	palette[256];		// and as the colors themselves, for RGB lighting (skin.c)
	skin_t	*skin;
} player_info_t;


// a frame's entities, as packet_entities_t holds them but as many as FTE's
// replacement deltas bring (CL_FrameEntity makes the room)
typedef struct
{
	int				num_entities;
	int				max_entities;
	entity_state_t	*entities;
} cl_entities_t;

typedef struct
{
	// generated on client side
	usercmd_t	cmd;		// cmd that generated the frame
	double		senttime;	// time cmd was sent off
	int			delta_sequence;		// sequence number to delta from, -1 = full update

	// received from server
	double		receivedtime;	// time message was received, or -1
	player_state_t	playerstate[MAX_CLIENTS];	// message received that reflects performing
							// the usercmd
	cl_entities_t	packet_entities;
	bool	invalid;		// true if the packet_entities delta was invalid
	// FTE's replacement deltas: the players as entities, by number, the
	// base of the next update with packet_entities
	entity_state_t	playerents[MAX_CLIENTS];
	int				numplayerents;
} frame_t;


typedef struct
{
	int		destcolor[3];
	int		percent;		// 0-256
} cshift_t;

#define	CSHIFT_CONTENTS	0
#define	CSHIFT_DAMAGE	1
#define	CSHIFT_BONUS	2
#define	CSHIFT_POWERUP	3
#define	NUM_CSHIFTS		4


//
// client_state_t should hold all pieces of the client state
//


#define	MAX_DEMOS		8
#define	MAX_DEMONAME	16

typedef enum {
ca_disconnected, 	// full screen console with no connection
ca_demostart,		// starting up a demo
ca_connected,		// netchan_t established, waiting for svc_serverdata
ca_onserver,		// processing data lists, donwloading, etc
ca_active			// everything is in, so frames can be rendered
} cactive_t;

typedef enum {key_game, key_console, key_message, key_menu} keydest_t;

typedef enum {
	dl_none,
	dl_model,
	dl_sound,
	dl_skin,
	dl_vwep_model,
	dl_csprogs,
	dl_single
} dltype_t;		// download type

//
// the client_static_t structure is persistant through an arbitrary number
// of server connections
//
typedef struct
{
// connection information
	cactive_t	state;

// network stuff
	netchan_t	netchan;
	sizebuf_t	net_message;	// the packet being read
	netadr_t	net_from;		// and who sent it
	byte		net_message_buf[MAX_FRAGMENTED];	// a packet, or one put together of FTE's fragments

// private userinfo for sending to masterless servers
	char		userinfo[MAX_INFO_STRING];

	char		servername[MAX_OSPATH];	// name of server from original connect

	int			qport;

	FILE		*download;		// file transfer from server
	char		downloadtempname[MAX_OSPATH];
	char		downloadname[MAX_OSPATH];		// the server's name for it
	char		downloadlocalname[MAX_OSPATH];	// where it goes
	int			downloadnumber;
	dltype_t	downloadtype;
	int			downloadpercent;

// demo loop control
	int			demonum;		// -1 = don't play demos
	char		demos[MAX_DEMOS][MAX_DEMONAME];		// when not playing

// demo recording info must be here, because record is started before
// entering a map (and clearing client_state_t)
	bool	demorecording;
	bool	demoplayback;
	bool	mvdplayback;		// the demo is an MVD (cl_mvd.c)
	bool	timedemo;
	FILE		*demofile;
	float		td_lastframe;		// to meter out one message a frame
	int			td_startframe;		// host_framecount at start
	float		td_starttime;		// realtime at second frame of timedemo

	int			challenge;

// protocol extensions: from the challenge, those both ends know, asked for
// in the connect packet; from svc_serverdata, those in use
	unsigned	fteext;
	unsigned	fteext2;
	unsigned	mvdext1;
	int			fragmtu;		// FTE's fragmentation: the server's mtu, then the one asked for; 0 for none

	float		latency;		// rolling average

// independent physics: commands are made and sent at the physics rate,
// frames are drawn as fast as allowed
	bool		physframe;		// this frame makes and sends a command
	double		physframetime;	// seconds the command covers
	double		physaccum;		// time not yet covered by a command

	double		frametime;		// seconds since the last drawn frame, at most 0.2
	int			framecount;		// frames drawn, never reset
	int			fps_count;		// frames drawn, for show_fps

	keydest_t	key_dest;		// where key events go
	byte		*basepal;		// gfx/palette.lmp
	byte		*colormap;		// gfx/colormap.lmp
} client_static_t;

extern client_static_t	cls;

#define	STATIC_BLOCK	64				// static entities (torches, etc) come in blocks that never move
#define	MAX_VISEDICTS		4096
#define NET_TIMINGS			256
#define NET_TIMINGSMASK		255

//
// the client_state_t structure is wiped completely at every
// server signon
//
typedef struct
{
	int			servercount;	// server identification for prespawns

	char		serverinfo[MAX_SERVERINFO_STRING];
	int			z_ext;			// the ZQuake extensions in use: the server's "*z_ext" and ours
	float		minpitch, maxpitch;	// how far the view may look down and up

	int			parsecount;		// server message counter
	int			validsequence;	// this is the sequence number of the last good
								// packetentity_t we got.  If this is 0, we can't
								// render a frame yet
	int			movemessages;	// since connecting to this server
								// throw out the first couple, so the player
								// doesn't accidentally do something the 
								// first frame

	int			spectator;

	double		last_ping_request;	// while showing scoreboard
	double		last_servermessage;

// sentcmds[cl.netchan.outgoing_sequence & UPDATE_MASK] = cmd
	frame_t		frames[UPDATE_BACKUP];

// information for local display
	int			stats[MAX_CL_STATS];	// health, etc
	float		statsf[MAX_CL_STATS];	// the same as floats: FTE's svc_fte_updatestatfloat's
	char		*statsstr[MAX_CL_STATS];	// FTE's string stats (Mem_), NULL for none
	float		item_gettime[32];	// cl.time of aquiring item, for blinking
	float		faceanimtime;		// use anim frame if cl.time < this

	cshift_t	cshifts[NUM_CSHIFTS];	// color shifts for damage, powerups and content types

// the client maintains its own idea of view angles, which are
// sent to the server each frame.  And only reset at level change
// and teleport times
	vec3_t		viewangles;

// the client simulates or interpolates movement to get these values
	double		time;			// this is the time value that the client
								// is rendering at.  allways <= realtime
	vec3_t		simorg;
	vec3_t		simvel;
	vec3_t		simangles;

// pitch drifting vars
	float		pitchvel;
	bool	nodrift;
	float		driftmove;
	double		laststop;


	float		crouch;			// local amount for smoothing stepups

	bool	paused;			// send over by server

	float		punchangle;		// temporar yview kick from weapon firing
	
	int			intermission;	// don't change view angle, full screen, etc: 1 scores, 2 finale,
								// 3 cutscene (a finale "/.", FTE's)
	int			completed_time;	// latched ffrom time at intermission start
	double		completed_leveltime;	// CL_LevelTime then, for single player's
	
//
// information that is static for the entire time connected to a server
//
	char		model_name[MAX_MODELS][MAX_QPATH];
	char		sound_name[MAX_SOUNDS][MAX_QPATH];
	char		particle_name[MAX_PARTICLE_PRECACHE][MAX_QPATH];	// svc_fte_precache's effects

	struct model_s		*model_precache[MAX_MODELS];

	// Z_EXT_VWEP: a player model without a weapon, then the weapons, from the
	// server's //vwep list ("-" none)
#define	MAX_VWEP_MODELS	32
	char		vw_model_name[MAX_VWEP_MODELS][MAX_QPATH];
	struct model_s		*vw_model_precache[MAX_VWEP_MODELS];
	bool		vwep_enabled;
	struct cmodel_s	*clipmodels[MAX_MODELS];	// the world and its inline models, for prediction
	unsigned	map_checksum2;		// the server checks it on prespawn
	struct cmap_s	*map;			// a reference the client holds
	movevars_t	movevars;			// from the server, for prediction
	struct sfx_s		*sound_precache[MAX_SOUNDS];

	char		levelname[40];	// for display on solo scoreboard
	int			playernum;

// refresh related state
	struct model_s	*worldmodel;	// cl_entitites[0].model
	int			num_entities;	// stored bottom up in cl_entities array
	int			num_statics;
	entity_t	**static_blocks;	// STATIC_BLOCK entities each, heap; efrags point into them

	int			cdtrack;		// cd audio

	entity_t	viewent;		// weapon model

// all player information
	player_info_t	players[MAX_CLIENTS];

	int			cmdtime_msec;	// sum of the msec of every command sent
	bool		onground;		// predicted
	int			waterlevel;		// predicted, for cl_smartjump
	playermove_t	pmove;		// the prediction's player movement; physents are set up by cl_ents.c

	entity_state_t	baselines[MAX_EDICTS];
	lightstyle_t	lightstyles[MAX_LIGHTSTYLES];
	dlight_t	dlights[MAX_DLIGHTS];

// refresh list
// this is double buffered so the last frame
// can be scanned for oldorigins of trailing objects
	entity_t	visedicts_list[2][MAX_VISEDICTS];
	entity_t	*visedicts, *oldvisedicts;
	int			numvisedicts, oldnumvisedicts;

	int			spikeindex, playerindex, flagindex;	// model indices, for effects
	int			h_playerindex, gib1index, gib2index, gib3index;	// and the filters' (FTE's)
	int			rocketindex, grenadeindex;

	int			viewplayer;		// whose view is drawn: playernum, or who an MVD follows
	float		mvd_server_time;	// an MVD's serverdata: the server's clock when it began
	player_state_t	mvd_prev[MAX_CLIENTS];	// an MVD's players as last sent; the next deltas from them

	int			parsecountmod;		// frame the last packet filled
	double		parsecounttime;		// realtime the packet's command was sent
	int			packet_latency[NET_TIMINGS];	// for the net graph
} client_state_t;


//
// cvars
//
extern  cvar_t	cl_warncmd;
extern	cvar_t	cl_upspeed;
extern	cvar_t	cl_forwardspeed;
extern	cvar_t	cl_backspeed;
extern	cvar_t	cl_sidespeed;

extern	cvar_t	cl_movespeedkey;

extern	cvar_t	cl_yawspeed;
extern	cvar_t	cl_pitchspeed;

extern	cvar_t	cl_anglespeedkey;

extern	cvar_t	cl_shownet;
extern	cvar_t	hudstyle;
extern	cvar_t	cl_hudswap;

extern	cvar_t	cl_pitchdriftspeed;
extern	cvar_t	lookspring;
extern	cvar_t	lookstrafe;
extern	cvar_t	freelook;
extern	cvar_t	sensitivity;

extern	cvar_t	m_pitch;
extern	cvar_t	m_yaw;
extern	cvar_t	m_forward;
extern	cvar_t	m_side;

extern cvar_t		_windowed_mouse;

extern	cvar_t	name;


extern	client_state_t	cl;

// the static entity i (0 .. cl.num_statics-1)
static inline entity_t *CL_StaticEntity (int i)
{
	return &cl.static_blocks[i / STATIC_BLOCK][i % STATIC_BLOCK];
}

//=============================================================================


//
// cl_main
//
dlight_t *CL_AllocDlight (int key);
void	CL_DecayLights (void);


void CL_NextDemo (void);

void CL_BeginServerConnect(void);
bool CL_ConsoleForced (void);	// the console fills the screen and takes the keys: out of a game


extern char emodel_name[], pmodel_name[], prespawn_name[], modellist_name[], soundlist_name[];

//
// cl_input
//
typedef struct
{
	int		down[2];		// key nums holding it down
	int		state;			// low bit is down state
} kbutton_t;


void CL_InitInput (void);
void CL_SendCmd (void);

void CL_ParseTEnt (void);
void CL_UpdateTEnts (void);

void CL_ClearState (void);


char *Key_KeynumToString (int keynum);
int Key_StringToKeynum (const char *str);	// -1 if none
int Key_ToFTE (int keynum);		// as FTE's QuakeC numbers keys, -1 if it has none
int Key_FromFTE (int code);		// -1 if none

//
// cl_demo.c
//
void CL_StopPlayback (void);
bool CL_GetMessage (void);
void CL_RecordPacket (void);	// a packet CL_GetMessage left to the netchan (FTE's fragments)
void CL_WriteDemoCmd (usercmd_t *pcmd);

void CL_Stop_f (void);
void CL_Record_f (void);
void CL_ReRecord_f (void);
void CL_PlayDemo_f (void);
void CL_TimeDemo_f (void);
void CL_CompleteDemo (const char *partial, void (*add) (void *ctx, const char *candidate), void *ctx);

//
// cl_fchecks.c: replies to f_version, f_system and f_modified asked in chat
//
void CL_InitFChecks (void);
void CL_FCheckRequest (const char *line);

//
// cl_fmod.c: f_modified, whether the files checked are the originals
//
void CL_InitFMod (void);
void CL_FModResponse (void);
const char *CL_FModText (void);
void CL_InitDemo (void);

//
// cl_mvd.c
//
void CL_InitMVD (void);
void CL_MVDStart (byte *data, size_t size);	// takes the data
void CL_MVDStop (void);
void CL_MVDAdvance (void);			// once a frame, before reading
bool CL_GetMVDMessage (void);
double CL_MVDTime (void);			// the moment drawn, in demo seconds
bool CL_MVDSkipMessage (void);		// for another player than the one followed
int CL_MVDStatTarget (void);		// whose stats a stat message sets, -1 nobody's
int CL_MVDTracking (void);			// the player followed, -1 none
bool CL_MVDFlying (void);			// the viewer flies the camera instead
void CL_MVDView (void);				// the view from the player followed
double CL_MVDFrameTime (void);		// the frame last read, in demo seconds
void CL_MVDTogglePause (void);
void CL_MVDHint (const char *s);	// a "//at" stufftext
void CL_MVDButtons (bool attack, bool jump);	// pressed since the last frame
void CL_FlyMove (float move[3]);	// cl_input.c: the movement keys, for a flown camera
void CL_LerpMVDPlayers (void);		// cl_ents.c: aims the players' trails, once a frame
bool CL_PlayerPlace (int slot, vec3_t origin, vec3_t angles);	// an MVD's player at the moment played
void CL_MVDFixAngle (int slot);		// the player's view was set: no turn to it
void CL_MVDStartStream (void);		// QTV: played as it arrives
void CL_MVDFeed (const byte *bytes, int len);
void CL_MVDStreamClosed (void);
void CL_MVDActive (void);			// the level went active: a file's is scanned for seeking
bool CL_MVDNewLevel (void);			// a serverdata; true: a scan ends before it
bool CL_MVDQuiet (void);			// a scan or a seek: nothing is shown or heard
void CL_MVDAnnouncements (const char *s);	// a stufftext: KTX's lines about items

//
// cl_items.c
//
void CL_InitItems (void);
void CL_ItemsClear (void);
void CL_ItemsMarker (const char *text, double time);	// "//ktx" lines said at time
void CL_DrawItemTimers (void);
void CL_LinkItems (void);			// rings and ghosts where items are missing

//
// cl_qtv.c
//
void CL_InitQTV (void);
void CL_QTVFrame (void);			// once a frame, before the MVD is read
void CL_QTVStop (void);
void CL_DumpTimedemoFrame (void);	// when timedemo_dump asks for it

//
// cl_parse.c
//
int CL_CalcNet (void);
void CL_ParseServerMessage (void);
void CL_FreeStatStrings (void);		// FTE's string stats, before cl is cleared
void CL_ProcessUserInfo (int slot, player_info_t *player);	// name, colors, skin from the userinfo
double CL_ScoreClock (void);		// what the scoreboard's times count on
double CL_LevelTime (void);			// the level's time, the server's (STAT_TIME) when it says
void CL_RequestNextDownload (void);
bool CL_IsUploading(void);
void CL_NextUpload(void);
void CL_StartUpload (byte *data, int size);
void CL_StopUpload(void);

//
// cl_csqc.c: client-side QuakeC, as FTE's client runs it
//
extern	cvar_t	cl_nocsqc, cl_download_csprogs;
void	CSQC_RegisterVariables (void);
// the csprogs the server offers (csprogsname NULL: none), before the models
// load; whether CSQC runs
bool	CSQC_Init (bool anycsqc, const char *csprogsname, unsigned checksum, size_t size);
void	CSQC_WorldLoaded (void);	// after the world model
void	CSQC_Announce (void);		// then: tells the server whether CSQC runs (enablecsqc)
void	CSQC_Shutdown (void);		// at serverdata, disconnect and quit
bool	CSQC_Inited (void);
void	CSQC_ParseEntities (bool sized);	// svc_fte_csqcentities(_sized)
void	CSQC_ParseEvent (bool sized);		// svc_fte_cgamepacket(_sized)
bool	CSQC_ParseTempEntity (void);		// svc_temp_entity, if CSQC took it
// a sound the server plays, if CSQC took it (CSQC_Event_Sound)
bool	CSQC_EventSound (int ent, int channel, const char *sample, float vol, float attenuation, const vec3_t pos);
void	CSQC_InputFrame (usercmd_t *cmd);	// a command about to be sent, as CSQC changes it
// draws the view if CSQC does (CSQC_UpdateView), and whether it asked for the
// status bar; false to draw it as the client does
bool	CSQC_DrawView (bool *sbar);
bool	CSQC_DrawHud (void);		// simple CSQC's status bar in place of the client's
bool	CSQC_DrawScores (void);		// and its scores
bool	CSQC_DrawsHud (void);		// it has CSQC_DrawHud
bool	CSQC_DrawsView (void);
void	CSQC_ParticlesChanged (void);	// the scripts changed: its effects found again
// a matching csprogs is here already (none needs downloading)
bool	CSQC_CheckDownload (const char *csprogsname, unsigned checksum, size_t size);

//
// cl_download.c
//
bool CL_CheckOrDownloadFile (char *filename);
bool CL_CheckOrDownloadFileAs (const char *remote, const char *local);	// saved under another name
void CL_Download_f (void);
void CL_ParseDownload (void);
bool CL_ParseChunkPacket (void);		// an out-of-band chunk
int CL_DownloadRequests (void);			// how many chunks to ask for this frame
int CL_WriteDownloadRequests (sizebuf_t *buf, int want);	// returns how many went in
void CL_StopDownload (void);
const char *CL_DownloadSpeed (void);	// for the download bar, in a fixed width
void CL_InitDownloads (void);
bool CL_Downloading (void);				// a file on its way, from the server or the web
void CL_DownloadFrame (void);			// a web download's progress, and its end

//
// view.c
//
void V_StartPitchDrift (void);
void V_StopPitchDrift (void);

void V_RenderView (void);
void V_UpdateBlend (void);
void V_ParseDamage (void);


//
// cl_part.c
//
void CL_InitParticles (void);
void CL_NewMapParticles (void);		// a level's start
void CL_ReloadParticles (void);		// a new game directory
void CL_RunParticles (void);		// once a frame, into r_scene

// the scripted effects the client starts by name, P_INVALID where no script
// has one: temporary entities', beams' (and the ends where they hit), and
// id's trails in R_RocketTrail's order
typedef enum
{
	PT_GUNSHOT, PT_GUNSHOTQUAD, PT_QWGUNSHOT, PT_SPIKE, PT_SPIKEQUAD, PT_SUPERSPIKE, PT_SUPERSPIKEQUAD,
	PT_BULLET, PT_SUPERBULLET, PT_WIZSPIKE, PT_KNIGHTSPIKE,
	PT_EXPLOSION, PT_EXPLOSIONQUAD, PT_TEI_BIGEXPLOSION, PT_TAREXPLOSION, PT_LAVASPLASH, PT_TELEPORT,
	PT_BLOOD, PT_QWBLOOD, PT_LIGHTNINGBLOOD, PT_SPARK, PT_FLAMEJET, PT_PLASMABURN, PT_SMALLFLASH,
	PT_TEI_SMOKE, PT_TEI_PLASMAHIT, PT_TEI_G3, PT_NEXBEAM, PT_RAILTRAIL,
	PT_LIGHTNING1, PT_LIGHTNING1_END, PT_LIGHTNING2, PT_LIGHTNING2_END, PT_LIGHTNING3, PT_LIGHTNING3_END,
	PT_BEAM, PT_BEAM_END,
	PT_TR_ROCKET, PT_TR_GRENADE, PT_TR_BLOOD, PT_TR_WIZSPIKE, PT_TR_SLIGHTBLOOD, PT_TR_KNIGHTSPIKE, PT_TR_VORESPIKE,
	PT_NUM
} cl_effect_t;

int CL_Effect (cl_effect_t effect);
const char *CL_EffectName (cl_effect_t effect);
int CL_ParticleEffect (int index);		// the server's list's effect index (svc_fte_precache)
void CL_PrecacheParticle (int index);	// it came
void CL_PrecacheModelEffects (int index);	// a model came: its r_trail and r_effect
// a model's scripted effects (r_trail, r_effect), P_INVALID none; emitflags P_EMIT*
int CL_ModelTrail (int modelindex);
int CL_ModelEmit (int modelindex, unsigned *emitflags);
// the first brush entity a line hits (the fraction of the way), as particles hit them
float CL_PartTrace (const vec3_t start, const vec3_t end, vec3_t impact, vec3_t normal, int *entnum);

//
// cl_tent
//

// a temporary entity as the server sent it
typedef struct
{
	int		type;
	int		ent;			// a beam's
	vec3_t	pos;			// its origin, a box's min, a beam's start
	vec3_t	pos2;			// a beam's end, a box's max, a color (0 to 1)
	vec3_t	vel;			// a velocity or a direction
	int		count;
	int		color, colors;	// palette colors: the first and how many from it
	float	time;			// TEDP_CUSTOMFLASH's
} tent_t;

void CL_RunTEnt (const tent_t *te);		// what it shows
void CL_InitTEnts (void);
void CL_ClearTEnts (void);
// a beam of a kind (TE_LIGHTNING1 to 3, TE_BEAM) from ent, for 0.2 seconds
void CL_AddBeam (int type, int ent, const vec3_t start, const vec3_t end);

//
// cl_ents.c
//
void CL_SetSolidPlayers (int playernum);
p_trailstate_t **CL_EntityTrailState (int entnum);	// its trail's, NULL for none
void CL_SetUpPlayerPrediction(bool dopred);
void CL_EmitEntities (void);
void CL_ClearProjectiles (void);
void CL_ResetSmoothing (void);		// a new level, or a new connection

// an entity's animation frame, and the one it turns from (r_lerpframes)
typedef struct
{
	int		frame, oldframe;
	double	changed;		// when frame came
} framelerp_t;
void CL_FrameArrived (framelerp_t *f, int frame, bool snap);	// the frame an update has
void CL_FrameBlend (const framelerp_t *f, entity_t *ent);		// the frames to draw between
extern cvar_t	cl_nolerp;
void CL_ParseProjectiles (bool numbered);

// the model with that number, NULL if there is none
struct model_s *CL_Model (int index);

// what the serverinfo tells: the ZQuake extensions, the pitch limits
void CL_ProcessServerInfo (void);

// the view's height above the player's origin
float CL_ViewHeight (void);
void CL_ParsePacketEntities (bool delta);
void CL_ParseReplacementEntities (void);	// FTE's svc_fte_updateentities
// the entity at index of a frame's, with room made for it
entity_state_t *CL_FrameEntity (cl_entities_t *pack, int index);
void CL_SetSolidEntities (void);
void CL_ParsePlayerinfo (void);

//
// cl_pred.c
//
void CL_InitPrediction (void);
// repredict runs the player move; otherwise only the view is interpolated
void CL_PredictMove (bool repredict);
// the next position is a jump (teleport, respawn): don't interpolate to it
void CL_DisableLerpMove (void);
// commands at the physics rate, frames at the render rate
bool CL_IndependentPhysics (void);
extern cvar_t	cl_physfps;
void CL_PredictUsercmd (player_state_t *from, player_state_t *to, usercmd_t *u);

//
// cl_cam.c
//
int Cam_TrackNum (void);		// the player the camera follows, or -1
int Cam_ViewEntity (void);		// whose eyes the view is: their entity, 0 for a free camera
bool Cam_DrawViewModel(void);
bool Cam_DrawPlayer(int playernum);
void Cam_Track(usercmd_t *cmd);
void Cam_FinishMove(usercmd_t *cmd);
void Cam_Reset(void);
void CL_InitCam(void);

//
// skin.c
//

typedef struct
{
    char	manufacturer;
    char	version;
    char	encoding;
    char	bits_per_pixel;
    unsigned short	xmin,ymin,xmax,ymax;
    unsigned short	hres,vres;
    unsigned char	palette[48];
    char	reserved;
    char	color_planes;
    unsigned short	bytes_per_line;
    unsigned short	palette_type;
    char	filler[58];
    unsigned char	data;			// unbounded
} pcx_t;


void	Skin_Find (player_info_t *sc);
byte	*Skin_ForPlayer (player_info_t *info);	// NULL if the skin can't be loaded
void	Skin_Skins_f (void);
void	Skin_AllSkins_f (void);
void	Skin_NextDownload (void);
void	Skin_Colors (player_info_t *player);	// translate and palette from the player's colors
int		Skin_ColorIndex (int color);			// a player's color on the scoreboard: a palette index

#define RSSHOT_WIDTH 320
#define RSSHOT_HEIGHT 200
