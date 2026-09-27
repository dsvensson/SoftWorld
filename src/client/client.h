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
	byte	translate[256];		// the palette with the player's colors, for the colormap
	pixel_t	palette[256];		// and as the colors themselves, for RGB lighting (skin.c)
	skin_t	*skin;
} player_info_t;


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
	packet_entities_t	packet_entities;
	bool	invalid;		// true if the packet_entities delta was invalid
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
	byte		net_message_buf[MAX_UDP_PACKET];

// private userinfo for sending to masterless servers
	char		userinfo[MAX_INFO_STRING];

	char		servername[MAX_OSPATH];	// name of server from original connect

	int			qport;

	FILE		*download;		// file transfer from server
	char		downloadtempname[MAX_OSPATH];
	char		downloadname[MAX_OSPATH];
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
	unsigned	mvdext1;

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
#define	MAX_VISEDICTS		1024
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
	
	int			intermission;	// don't change view angle, full screen, etc
	int			completed_time;	// latched ffrom time at intermission start
	
//
// information that is static for the entire time connected to a server
//
	char		model_name[MAX_MODELS][MAX_QPATH];
	char		sound_name[MAX_SOUNDS][MAX_QPATH];

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
extern	cvar_t	cl_sbar;
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

void Cmd_ForwardToServer (void);

//=============================================================================


//
// cl_main
//
dlight_t *CL_AllocDlight (int key);
void	CL_DecayLights (void);

void CL_WriteConfiguration (void);


void CL_Disconnect_f (void);
void CL_NextDemo (void);

void CL_BeginServerConnect(void);


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

void CL_ReadPackets (void);

void CL_BaseMove (usercmd_t *cmd);


float CL_KeyState (kbutton_t *key);
char *Key_KeynumToString (int keynum);

//
// cl_demo.c
//
void CL_StopPlayback (void);
bool CL_GetMessage (void);
void CL_WriteDemoCmd (usercmd_t *pcmd);

void CL_Stop_f (void);
void CL_Record_f (void);
void CL_ReRecord_f (void);
void CL_PlayDemo_f (void);
void CL_TimeDemo_f (void);
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
void CL_MVDView (void);				// the view from the player followed
double CL_MVDFrameTime (void);		// the frame last read, in demo seconds
void CL_MVDTogglePause (void);
void CL_MVDHint (const char *s);	// a "//at" stufftext
void CL_MVDButtons (bool attack, bool jump);	// pressed since the last frame
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
void CL_ProcessUserInfo (int slot, player_info_t *player);	// name, colors, skin from the userinfo
double CL_ScoreClock (void);		// what the scoreboard's times count on
void CL_NewTranslation (int slot);
void CL_RequestNextDownload (void);
bool CL_IsUploading(void);
void CL_NextUpload(void);
void CL_StartUpload (byte *data, int size);
void CL_StopUpload(void);

//
// cl_download.c
//
bool CL_CheckOrDownloadFile (char *filename);
void CL_Download_f (void);
void CL_ParseDownload (void);
bool CL_ParseChunkPacket (void);		// an out-of-band chunk
int CL_DownloadRequests (void);			// how many chunks to ask for this frame
int CL_WriteDownloadRequests (sizebuf_t *buf, int want);	// returns how many went in
void CL_StopDownload (void);
const char *CL_DownloadSpeed (void);	// for the download bar, in a fixed width

//
// view.c
//
void V_StartPitchDrift (void);
void V_StopPitchDrift (void);

void V_RenderView (void);
void V_UpdateBlend (void);
void V_ParseDamage (void);
void V_SetContentsColor (int contents);


//
// cl_tent
//
void CL_InitTEnts (void);
void CL_ClearTEnts (void);

//
// cl_ents.c
//
void CL_SetSolidPlayers (int playernum);
void CL_SetUpPlayerPrediction(bool dopred);
void CL_EmitEntities (void);
void CL_ClearProjectiles (void);
void CL_ResetSmoothing (void);		// a new level, or a new connection
extern cvar_t	cl_nolerp;
void CL_ParseProjectiles (bool numbered);

// the model with that number, NULL if there is none
struct model_s *CL_Model (int index);

// what the serverinfo tells: the ZQuake extensions, the pitch limits
void CL_ProcessServerInfo (void);

// the view's height above the player's origin
float CL_ViewHeight (void);
void CL_ParsePacketEntities (bool delta);
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
byte	*Skin_Cache (skin_t *skin);
byte	*Skin_ForPlayer (player_info_t *info);	// NULL if the skin can't be loaded
void	Skin_Skins_f (void);
void	Skin_AllSkins_f (void);
void	Skin_NextDownload (void);
void	Skin_Colors (player_info_t *player);	// translate and palette from the player's colors
int		Skin_ColorIndex (int color);			// a player's color on the scoreboard: a palette index

#define RSSHOT_WIDTH 320
#define RSSHOT_HEIGHT 200
