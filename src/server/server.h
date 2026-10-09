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
#include "msg.h"
#include "net.h"
#include "progs.h"
#include "protocol.h"
#include "world.h"
#include "arena.h"
#include "host.h"
#include "sv_public.h"
// server.h

#define	QW_SERVER

#define	MAX_MASTERS	8				// max recipients for heartbeat packets

typedef enum {RD_NONE, RD_CLIENT, RD_PACKET} redirect_t;


typedef enum {
	ss_dead,			// no map loaded
	ss_loading,			// spawning level edicts
	ss_active			// actively running
} server_state_t;
// some qc commands are only valid before the server has finished
// initializing (precache commands, static sounds / objects, etc)

typedef struct
{
	bool	active;				// false when server is going down
	server_state_t	state;			// precache commands are only valid during load

	double		time;
	
	int			lastcheck;			// used by PF_checkclient
	double		lastchecktime;		// for monster ai 

	bool	paused;				// are we paused?

	//check player/eyes models for hacks
	unsigned	model_player_checksum;
	unsigned	eyes_player_checksum;
	
	char		name[64];			// map name
	char		modelname[MAX_QPATH];		// maps/<name>.bsp, for model_precache[0]
	cmap_t		*map;				// a reference the server holds
	cmodel_t	*worldmodel;
	bool		bigcoords;			// coordinates as floats: the map goes past +-4096
	bool		replacementdeltas;	// FTE's, to the clients that asked (SV_ReplacementDeltas)
	unsigned	map_checksum, map_checksum2;
	movevars_t	movevars;			// player movement settings from the sv_ cvars
	double		frametime;			// seconds the current physics step or move covers
	double		physicstime;		// sv.time of the last physics frame
	char		*model_precache[MAX_MODELS];	// NULL terminated
	char		*sound_precache[MAX_SOUNDS];	// NULL terminated
	char		*lightstyles[MAX_LIGHTSTYLES];
	cmodel_t	*models[MAX_MODELS];	// the world and its inline models, and .bsp models (SV_LoadBrushModel)

	// the stats QuakeC adds after id's (FTE's clientstat and globalstat), by
	// number: a field of each client's entity, or a global
	struct
	{
		uint32_t	type;			// ev_float, ev_entity or QC_EV_INTEGER; 0 none
		bool		global;
		uint32_t	ofs;			// a word of the fields or the globals
	} qcstats[MAX_CL_STATS];

	int			num_edicts;			// increases towards MAX_EDICTS
	edict_t		*edicts;			// can NOT be array indexed, because
									// edict_t is variable sized, but can
									// be used to reference the world ent

	int			vis_rows;			// leafs with visibility, and leaf 0 outside the map
	int			vis_rowbytes;
	byte		**pvs_rows, **phs_rows;	// decompressed when first needed, NULL until then
	byte		*checkpvs;			// what the PF_checkclient target sees

	// added to every client's unreliable buffer each frame, then cleared
	sizebuf_t	datagram;
	byte		datagram_buf[MAX_DATAGRAM];

	// added to every client's reliable buffer each frame, then cleared
	sizebuf_t	reliable_datagram;
	byte		reliable_datagram_buf[MAX_MSGLEN];

	// the multicast buffer is used to send a message to a set of clients
	sizebuf_t	multicast;
	byte		multicast_buf[MAX_MSGLEN];

	// the master buffer is used for building log packets
	sizebuf_t	master;
	byte		master_buf[MAX_DATAGRAM];

	// the signon buffer will be sent to each client as they connect
	// includes the entity baselines, the static entities, etc
	// large levels will have >MAX_DATAGRAM sized signons, so 
	// multiple signon messages are kept
	sizebuf_t	signon;
	int			num_signon_buffers;		// as many as the level's baselines and statics need
	int			max_signon_buffers;
	int			*signon_buffer_size;
	byte		**signon_buffers;		// MAX_DATAGRAM bytes each

	// the level's static entities and the baselines SV_CreateBaseline made,
	// sent to each client at prespawn in the form its extensions allow
	entity_state_t	*static_entities;
	int			num_static_entities, max_static_entities;
	int			num_baselines;		// edicts 0 .. num_baselines - 1

	// the static sounds past svc_spawnstaticsound's 255 (the rest are in the
	// signon), sent at prespawn to the clients with replacement deltas
	struct staticsound_s	*static_sounds;
	int			num_static_sounds, max_static_sounds;

	areanode_t	areanodes[AREA_NODES];	// entities sorted by position
	int			numareanodes;

	int			nailmodel, supernailmodel, playermodel;	// model indices, for compression

	// the particle effects QuakeC named (particleeffectnum), 1 the first
	char		particle_precache[MAX_PARTICLE_PRECACHE][MAX_QPATH];
	int			num_particles;
} server_t;


typedef enum
{
	cs_free,		// can be reused for a new connection
	cs_zombie,		// client has been disconnected, but don't reuse
					// connection for a couple seconds
	cs_connected,	// has been assigned to a client_t, but not in game yet
	cs_spawned		// client is fully in game
} sv_client_state_t;

typedef struct
{
	// received from client

	// reply
	double				senttime;
	float				ping_time;
	packet_entities_t	entities;
} client_frame_t;

#define MAX_BACK_BUFFERS 4

typedef struct client_s
{
	sv_client_state_t	state;

	int				spectator;			// non-interactive

	bool		sendinfo;			// at end of frame, send info to all
										// this prevents malicious multiple broadcasts
	float			lastnametime;		// time of last name change
	int				lastnamecount;		// time of last name change
	unsigned		checksum;			// checksum for calcs
	bool		drop;				// lose this guy next opportunity
	int				lossage;			// loss percentage

	int				userid;							// identifying number
	char			userinfo[MAX_INFO_STRING];		// infostring

	usercmd_t		lastcmd;			// for filling in big drops and partial predictions
	usercmd_t		nqcmd;				// NetQuake's physics: the newest move, for the world's frame
	double			localtime;			// of last message
	bool			jump_held;			// don't jump again until the button is released

	float			maxspeed;			// localized maxspeed
	float			entgravity;			// localized ent gravity

	edict_t			*edict;				// EDICT_NUM(clientnum+1)
	char			name[32];			// for printing to other people
										// extracted from userinfo
	int				messagelevel;		// for filtering printed messages

	// the datagram is written to after every frame, but only cleared
	// when it is sent out to the client.  overflow is tolerated.
	sizebuf_t		datagram;
	byte			datagram_buf[MAX_DATAGRAM];

	// back buffers for client reliable data
	sizebuf_t	backbuf;
	int			num_backbuf;
	int			backbuf_size[MAX_BACK_BUFFERS];
	byte		backbuf_data[MAX_BACK_BUFFERS][MAX_MSGLEN];

	double			connection_started;	// or time of disconnect for zombies
	bool		send_message;		// set on frames a datagram arived on

// spawn parms are carried from level to level
	float			spawn_parms[NUM_SPAWN_PARMS];
	bool			newparms;			// a new game: SetNewParms makes them as it spawns

// client known data for deltas
	int				old_frags;

	int				stats[MAX_CL_STATS];	// id's, then QuakeC's (the bits of a float)


	client_frame_t	frames[UPDATE_BACKUP];	// updates can be deltad from here

	FILE			*download;			// file being downloaded
	int				downloadsize;		// total bytes
	int				downloadcount;		// bytes sent
	long			downloadbase;		// where the file starts, inside a pak
	char			downloadfn[MAX_QPATH];

	// FTE chunked downloads: the chunk to send with the next datagram, -1
	// none; the file number the client last named; when out-of-band chunks
	// may go again
	int				dlchunk;
	int				dlcookie;
	double			dlcleartime;

	int				spec_track;			// entnum of player tracking

	double			whensaid[10];       // JACK: For floodprots
 	int			whensaidhead;       // Head value for floodprots
 	double			lockedtill;

	bool		upgradewarn;		// did we warn him?

	FILE			*upload;
	char			uploadfn[MAX_QPATH];
	netadr_t		snap_from;
	bool		remote_snap;
 
//===== NETWORK ============
	int				chokecount;
	int				delta_sequence;		// -1 = no compression
	double			rate;				// seconds per byte, the client's rate
	netchan_t		netchan;

	// protocol extensions both ends know, from the connect packet; ZQuake's
	// from the userinfo
	unsigned		fteext;
	unsigned		fteext2;
	unsigned		mvdext1;
	struct svdeltas_s	*deltas;		// FTE_PEXT2_REPLACEMENTDELTAS: what it has and is to be sent (sv_ents.c)
	int				z_ext;
	double			lastservertime;		// host.realtime STAT_TIME went out last

	// MVD1 high-lag teleport: the last view angles the server set, to turn
	// the moves the client sent before it saw them
	bool			teleported;			// a teleport, not a respawn
	int				teleport_outgoing;	// the outgoing sequence they went out in, 0 once seen
	int				teleport_incoming;	// the incoming sequence then
	float			teleport_yaw;		// how far they turned the view

	bool			nolateprecache;		// told it can't take a late precache (SV_LatePrecache)
} client_t;

// a client can leave the server in one of four ways:
// dropping properly by quiting or disconnecting
// timing out if no valid messages are received for timeout.value seconds
// getting kicked off by the server operator
// a program error, like an overflowed reliable buffer

//=============================================================================


#define	STATFRAMES	100
typedef struct
{
	double	active;
	double	idle;
	int		count;
	int		packets;

	double	latched_active;
	double	latched_idle;
	int		latched_packets;
} svstats_t;

// MAX_CHALLENGES is made large to prevent a denial
// of service attack that could cycle all of them
// out before legitimate users connected
#define	MAX_CHALLENGES	1024

typedef struct
{
	netadr_t	adr;
	int			challenge;
	int			time;
} challenge_t;

typedef struct
{
	int			spawncount;			// number of servers spawned since start,
									// used to check late spawns
	client_t	clients[MAX_CLIENTS];
	int			serverflags;		// episode completion information
	
	double		last_heartbeat;
	int			heartbeat_sequence;
	svstats_t	stats;

	char		info[MAX_SERVERINFO_STRING];

	// log messages are used so that fraglog processes can get stats
	int			logsequence;	// the message currently being filled
	double		logtime;		// time of last swap
	sizebuf_t	log[2];
	byte		log_buf[2][MAX_DATAGRAM];

	challenge_t	challenges[MAX_CHALLENGES];	// to prevent invalid IPs from connecting

	netadr_t	master_adr[MAX_MASTERS];	// heartbeats go here
	char		localmodels[MAX_MODELS][6];	// inline model names for precache
	char		localinfo[MAX_LOCALINFO_STRING+1];	// info for QuakeC only
	FILE		*logfile;
	FILE		*fraglogfile;
	redirect_t	redirected;					// where console output goes

	bool		attract;					// a showcase's level (sv_attract.c): closed to the
											// network, its rules its own
	int			port;						// UDP port, opened with the first map
	sizebuf_t	net_message;				// the packet being read
	netadr_t	net_from;					// and who sent it
	byte		net_message_buf[MAX_UDP_PACKET];

	struct
	{
		int		messages;		// this many ...
		int		persecond;		// ... in this many seconds is flooding
		int		secondsdead;	// seconds a flooder is muted
		char	msg[255];		// what they are told
	} floodprot;
} server_static_t;

//=============================================================================

// edict->movetype values
#define	MOVETYPE_NONE			0		// never moves
#define	MOVETYPE_ANGLENOCLIP	1
#define	MOVETYPE_ANGLECLIP		2
#define	MOVETYPE_WALK			3		// gravity
#define	MOVETYPE_STEP			4		// gravity, special edge handling
#define	MOVETYPE_FLY			5
#define	MOVETYPE_TOSS			6		// gravity
#define	MOVETYPE_PUSH			7		// no clip to world, push and crush
#define	MOVETYPE_NOCLIP			8
#define	MOVETYPE_FLYMISSILE		9		// extra size to monsters
#define	MOVETYPE_BOUNCE			10
#define	MOVETYPE_LOCK			15		// the server moves the player and turns the view (mvdsv)

// edict->solid values
#define	SOLID_NOT				0		// no interaction with other objects
#define	SOLID_TRIGGER			1		// touch on edge, but not blocking
#define	SOLID_BBOX				2		// touch on edge, block
#define	SOLID_SLIDEBOX			3		// touch on edge, but not an onground
#define	SOLID_BSP				4		// bsp clip, touch on edge, block

// edict->deadflag values
#define	DEAD_NO					0
#define	DEAD_DYING				1
#define	DEAD_DEAD				2

#define	DAMAGE_NO				0
#define	DAMAGE_YES				1
#define	DAMAGE_AIM				2

// edict->flags
#define	FL_FLY					1
#define	FL_SWIM					2
#define	FL_GLIMPSE				4
#define	FL_CLIENT				8
#define	FL_INWATER				16
#define	FL_MONSTER				32
#define	FL_GODMODE				64
#define	FL_NOTARGET				128
#define	FL_ITEM					256
#define	FL_ONGROUND				512
#define	FL_PARTIALGROUND		1024	// not all corners are valid
#define	FL_WATERJUMP			2048	// player jumping out of water

// entity effects

//define	EF_BRIGHTFIELD			1
#define	EF_MUZZLEFLASH 			2		// NetQuake's progs': svc_muzzleflash for QuakeWorld's
#define	EF_BRIGHTLIGHT 			4
#define	EF_DIMLIGHT 			8


#define	SPAWNFLAG_NOT_EASY			256
#define	SPAWNFLAG_NOT_MEDIUM		512
#define	SPAWNFLAG_NOT_HARD			1024
#define	SPAWNFLAG_NOT_DEATHMATCH	2048

#define	MULTICAST_ALL			0
#define	MULTICAST_PHS			1
#define	MULTICAST_PVS			2

#define	MULTICAST_ALL_R			3
#define	MULTICAST_PHS_R			4
#define	MULTICAST_PVS_R			5

//============================================================================

extern	cvar_t	sv_mintic, sv_maxtic;
extern	cvar_t	sv_csqc_progname;
extern	cvar_t	sv_bigcoords;
extern	cvar_t	sv_replacementdeltas;
extern	cvar_t	sv_websocket;
extern	cvar_t	sv_public, sv_webrtc_room;
extern	cvar_t	sv_maxdrate;
extern	cvar_t	sv_maxpitch, sv_minpitch;
extern	cvar_t	pm_ktjump, pm_bunnyspeedcap, pm_slidefix, pm_airstep, pm_pground, pm_rampjump;

#define	SV_BIGCOORDS_REFUSAL	"This map goes past the standard coordinates of +-4096:\n" \
	"it needs a client with FTE float coordinates (ezQuake, FTE, SoftWorld).\n"

extern	cvar_t	sv_maxspeed;


extern	cvar_t	teamplay, deathmatch, coop, skill;

extern	server_static_t	svs;				// persistant server info
extern	server_t		sv;					// local server

extern	client_t	*host_client;

extern	edict_t		*sv_player;

extern	bool		sv_allow_cheats;	// -cheats: god, noclip, give, setpos




//===========================================================

//
// sv_main.c
//
void SV_DropClient (client_t *drop);

int SV_CalcPing (client_t *cl);
void SV_FullClientUpdate (client_t *client, sizebuf_t *buf);
void SV_FullClientUpdateToClient (client_t *client, client_t *cl);
void SV_LogPrint (const char *msg);
info_charset_t SV_InfoCharset (void);
void SV_SendServerInfoChange (char *key, char *value);

int SV_ModelIndex (const char *name);
char *SV_LevelString (const char *s);		// a copy for the level's life

bool SV_CheckBottom (edict_t *ent);
bool SV_movestep (edict_t *ent, vec3_t move, bool relink);

bool SV_MoveToGoal (qcvm_t *vm);		// the movetogoal builtin
void SV_ChangeYaw (edict_t *ent);


void SV_InitOperatorCommands (void);

void SV_ExtractFromUserinfo (client_t *cl);


//
// sv_init.c
//
// what a new level does with the players' spawn parms (FTE's map, changelevel
// and restart)
typedef enum
{
	SPAWNPARMS_CHANGE,	// the progs' SetChangeParms makes them: changelevel, QuakeWorld's map
	SPAWNPARMS_NEW,		// a new game, without serverflags: NetQuake's map
	SPAWNPARMS_KEEP		// those the level began with, again: restart
} spawnparms_t;

void SV_SpawnServer (char *server, spawnparms_t parms, struct cmap_s *built);	// built: adopted, or NULL
void SV_GotoLevel (const char *name, spawnparms_t parms, struct cmap_s *built);

//
// sv_attract.c: a showcase's levels
//
void SV_AttractStop (void);			// before the user's own level: the showcase goes
void SV_AttractQuiet (bool quiet);	// what the server says to the debug output only, in a showcase
void SV_AttractRedirectEnded (void);	// a redirect of the server's own ended (SV_EndRedirect)
bool SV_AttractProgs (char *name, size_t size, fs_chain_t **dir);
const char *SV_AttractCvar (const char *name);	// the showcase's rules, NULL for the cvar's own
void SV_AttractCvarSet (const char *name, const char *value);
void SV_AttractPlace (client_t *cl);	// the spectator held where the showcase looks from
bool SV_NQPhysics (const client_t *cl);	// it moves as NetQuake's players do (sv_phys.c)
void SV_FlushSignon (void);
void SV_LoadBrushModel (int index);	// a precached .bsp's hulls, as sv.models[index]
void SV_FreeBrushModels (void);
void SV_SignonRoom (int size);		// the signon with room for size more: the next buffer if need be

// where QuakeC's Write builtins write
#define	MSG_BROADCAST	0		// unreliable to all
#define	MSG_ONE			1		// reliable to one (msg_entity)
#define	MSG_ALL			2		// reliable to all
#define	MSG_INIT		3		// write to the init string
#define	MSG_MULTICAST	4		// for multicast()

// NetQuake's progs' writes, in QuakeWorld's words (sv_nqmsg.c): a write of a
// kind to dest (MSG_BROADCAST to MSG_INIT; one for MSG_ONE), the frame's end
// before its messages go out, and a level's start
typedef enum
{
	NQW_BYTE, NQW_CHAR, NQW_SHORT, NQW_LONG, NQW_COORD, NQW_ANGLE, NQW_STRING, NQW_ENTITY
} nqwrite_t;
void SV_NQWrite (int dest, client_t *one, nqwrite_t kind, float value, const char *string);
void SV_NQEndFrame (void);
void SV_NQNewLevel (void);
entity_state_t *SV_NewStatic (void);

typedef struct staticsound_s
{
	vec3_t	origin;
	int		sound, volume, attenuation;	// attenuation 64 times it
} staticsound_t;

staticsound_t *SV_NewStaticSound (void);
byte *SV_LeafPVS (int leafnum);
byte *SV_LeafPHS (int leafnum);
void SV_CheckVisWiden (void);


//
// sv_phys.c
//
void SV_ProgStartFrame (void);
void SV_Physics (void);
bool SV_RunThink (edict_t *ent);
void SV_RunNewmis (void);
void SV_SetMoveVars(void);

//
// sv_send.c
//
void SV_SendClientMessages (void);
// the player's fixangle in msg, or reliable with msg NULL (the spawn's)
void SV_SendFixangle (client_t *client, sizebuf_t *msg);

void SV_Multicast (vec3_t origin, int to);
// to the clients with the FTE2 extensions fteext2 alone (none: QTV too)
void SV_MulticastExt (vec3_t origin, int to, unsigned fteext2);
// the FTE extensions MVDs and QTV carry, mvdsv's; float coordinates on the maps that need them
#define	MVD_FTE_EXTENSIONS	(FTE_PEXT_256PACKETENTITIES | FTE_PEXT_MODELDBL | FTE_PEXT_ENTITYDBL | \
	FTE_PEXT_ENTITYDBL2 | FTE_PEXT_SPAWNSTATIC2 | FTE_PEXT_TRANS | FTE_PEXT_COLOURMOD)
// to the clients with the FTE extensions with and the FTE2 extensions with2,
// and without those of without; MVDs and QTV get it when their extensions
// (MVD_FTE_EXTENSIONS) would
void SV_MulticastProtExt (vec3_t origin, int to, unsigned with, unsigned without, unsigned with2);
// the broadcasts (sv.reliable_datagram and sv.datagram) to the clients, now:
// each frame, and when another message wouldn't fit (FTE's)
void SV_FlushBroadcasts (void);
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume,
    float attenuation);
void SV_ClientPrintf (client_t *cl, int level, char *fmt, ...);
void SV_BroadcastPrintf (int level, char *fmt, ...);
void SV_BroadcastCommand (char *fmt, ...);
void SV_SendMessagesToAll (void);
void SV_FindModelNumbers (void);
void SV_LatePrecache (int kind, int index, const char *name);	// PC_MODEL or PC_SOUND
void SV_PrintToClient (client_t *cl, int level, const char *string);	// whatever its messagelevel
// the stats a client is shown: its player's, or the one's a spectator tracks
void SV_ClientStats (const client_t *client, int stats[MAX_STATS]);

//
// sv_part.c
//
int SV_ParticleEffect (const char *name);	// particleeffectnum's
void SV_WriteParticle (const client_t *client, sizebuf_t *msg, int i);	// prespawn's: its name

//
// sv_user.c
//
void SV_ExecuteClientMessage (client_t *cl);
// a client's command as the engine runs it, QuakeC handing it back (clientcommand)
void SV_ClientCommand (client_t *cl, const char *s);
// the chunk a chunked download asked for, on the datagram or out of band
void SV_DownloadDatagram (client_t *cl, sizebuf_t *msg);
void SV_UserInit (void);
void SV_TogglePause (const char *msg);
// a static entity and an entity's baseline, as the client's protocol takes them
void SV_WriteStatic (const client_t *client, sizebuf_t *msg, const entity_state_t *s);
void SV_WriteBaseline (const client_t *client, sizebuf_t *msg, int entnum);


//
// svonly.c
//
void SV_BeginRedirect (redirect_t rd);
void SV_EndRedirect (void);
void SV_AbandonRedirect (void);	// the server going down: what it collected goes to the console

//
// sv_ents.c
//
void SV_WriteEntitiesToClient (client_t *client, sizebuf_t *msg);
// FTE's replacement deltas: a client message has acknowledged packets (what
// those before it carried, unacknowledged, is sent again); the client gone
// whether the client gets FTE's replacement deltas this level
bool SV_ReplacementDeltas (const client_t *client);
void SV_DeltasAcked (client_t *client);
void SV_DeltasUnsent (client_t *client);	// the packet with the deltas didn't go
void SV_FreeDeltas (client_t *client);
void SV_WriteDelta (const client_t *client, const entity_state_t *from, const entity_state_t *to, sizebuf_t *msg,
	bool force);
bool SV_EntityFits (const client_t *client, int number, int modelindex);
void SV_EntityLook (const edict_t *ent, entity_state_t *s);
void SV_ClientBaseline (const client_t *client, const edict_t *ent, entity_state_t *base);
void SV_EntityState (const edict_t *ent, int number, entity_state_t *state);
// the entities, as a delta from a frame the client has, or whole without one
void SV_EmitPacketEntities (const client_t *client, const packet_entities_t *from, const packet_entities_t *to,
	sizebuf_t *msg);

//
// sv_mvd.c: the game as an MVD for QTV's viewers, written while any watch
// (sv_mvd). The game's events go to everybody, to a player's view (a player's
// only), or to the views of a mask's players; SV_MVDMessage is a buffer to
// write one in first.
//
extern bool sv_mvd;
sizebuf_t *SV_MVDMessage (void);
void SV_MVDAll (const void *data, int length);
void SV_MVDSingle (const client_t *cl, const void *data, int length);
void SV_MVDMultiple (unsigned mask, const void *data, int length);
void SV_MVDPrint (const client_t *cl, int level, const char *text);	// cl NULL: to everybody
void SV_MVDFrame (void);		// after the clients' messages
void SV_MVDEndLevel (void);		// before a level spawns
void SV_MVDNewLevel (void);		// after
void SV_MVDInit (void);

// sv_main.c: whether a client may join (spectator false) or observe; prints
// the reason to it when not
bool SV_CanSwitchSide (client_t *cl, bool spectator);
int SV_BoundRate (int rate);

// sv_user.c: how the client's player moves (pmtype_t)
int SV_PMTypeForClient (const client_t *cl);
void SV_SetChannelRate (client_t *cl);	// before sending to it

// sv_user.c: the view angles are being set for the client (MVD1 high-lag
// teleport): returns what it is told, 1 a teleport, 2 a respawn
int SV_NoteFixangle (client_t *cl);

//
// sv_nchan.c
//

void ClientReliableCheckBlock(client_t *cl, int maxsize);
void ClientReliable_FinishWrite(client_t *cl);
void ClientReliableWrite_Begin(client_t *cl, int c, int maxsize);
void ClientReliableWrite_Angle(client_t *cl, float f);
void ClientReliableWrite_Byte(client_t *cl, int c);
void ClientReliableWrite_Char(client_t *cl, int c);
void ClientReliableWrite_Float(client_t *cl, float f);
void ClientReliableWrite_Coord(client_t *cl, float f);
void ClientReliableWrite_Long(client_t *cl, int c);
void ClientReliableWrite_Short(client_t *cl, int c);
void ClientReliableWrite_String(client_t *cl, const char *s);
void ClientReliableWrite_SZ(client_t *cl, void *data, int len);


[[noreturn]] void SV_Error (char *error, ...);
