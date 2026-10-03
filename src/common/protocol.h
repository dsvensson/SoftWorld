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
// protocol.h -- communications protocols

#include "mathlib.h"

#define	MAX_MSGLEN		1450		// max length of a reliable message
#define	MAX_DATAGRAM	1450		// max length of unreliable message

//
// per-level limits
//
#define	MAX_EDICTS		2048		// entity numbers on the wire, with FTE_PEXT_ENTITYDBL2
#define	MAX_LIGHTSTYLES	64
#define	MAX_MODELS		4096		// model numbers: bytes, with FTE_PEXT_MODELDBL shorts
#define	MAX_SOUNDS		256			// sound numbers are sent as bytes

#define	MAX_STYLESTRING	64

//
// stats are integers communicated to the client by the server
//
#define	MAX_STATS			32		// id's, which a server sends
#define	MAX_CL_STATS		256		// FTE's: a server for CSQC registers its own past id's
#define	STAT_HEALTH			0
//define	STAT_FRAGS			1
#define	STAT_WEAPON			2
#define	STAT_AMMO			3
#define	STAT_ARMOR			4
//define	STAT_WEAPONFRAME	5
#define	STAT_SHELLS			6
#define	STAT_NAILS			7
#define	STAT_ROCKETS		8
#define	STAT_CELLS			9
#define	STAT_ACTIVEWEAPON	10
#define	STAT_TOTALSECRETS	11
#define	STAT_TOTALMONSTERS	12
#define	STAT_SECRETS		13		// bumped on client side by svc_foundsecret
#define	STAT_MONSTERS		14		// bumped by svc_killedmonster
#define	STAT_ITEMS			15
#define	STAT_VIEWHEIGHT		16		// Z_EXT_VIEWHEIGHT: the view's height above the origin
#define	STAT_TIME			17		// Z_EXT_SERVERTIME, FTE_PEXT_ACCURATETIMINGS: the server's time in ms


//
// item flags
//
#define	IT_SHOTGUN				1
#define	IT_SUPER_SHOTGUN		2
#define	IT_NAILGUN				4
#define	IT_SUPER_NAILGUN		8

#define	IT_GRENADE_LAUNCHER		16
#define	IT_ROCKET_LAUNCHER		32
#define	IT_LIGHTNING			64
#define	IT_SUPER_LIGHTNING		128

#define	IT_SHELLS				256
#define	IT_NAILS				512
#define	IT_ROCKETS				1024
#define	IT_CELLS				2048

#define	IT_AXE					4096

#define	IT_ARMOR1				8192
#define	IT_ARMOR2				16384
#define	IT_ARMOR3				32768

#define	IT_SUPERHEALTH			65536

#define	IT_KEY1					131072
#define	IT_KEY2					262144

#define	IT_INVISIBILITY			524288

#define	IT_INVULNERABILITY		1048576
#define	IT_SUIT					2097152
#define	IT_QUAD					4194304

#define	IT_SIGIL1				(1<<28)

#define	IT_SIGIL2				(1<<29)
#define	IT_SIGIL3				(1<<30)
#define	IT_SIGIL4				(1<<31)

#define	PROTOCOL_VERSION	28

//
// protocol extensions, each a family (a magic number) and a mask of bits.
// The challenge reply lists the server's as (magic, mask) longs after the
// challenge string; the connect packet the client's, as "0x<magic> 0x<mask>"
// lines after the userinfo, only those the server listed; svc_serverdata the
// ones in use this level, as pairs before PROTOCOL_VERSION.
//
#define	PROTOCOL_VERSION_FTE	(('F'<<0) + ('T'<<8) + ('E'<<16) + ('X'<<24))
#define	PROTOCOL_VERSION_FTE2	(('F'<<0) + ('T'<<8) + ('E'<<16) + ('2'<<24))
#define	PROTOCOL_VERSION_MVD1	(('M'<<0) + ('V'<<8) + ('D'<<16) + ('1'<<24))
// FTE's fragmentation: the packets have an offset after their header, and one
// bigger than the mtu agreed goes in pieces (net_chan.c)
#define	PROTOCOL_VERSION_FRAGMENT	(('F'<<0) + ('R'<<8) + ('A'<<16) + ('G'<<24))

#define	FTE_PEXT_TRANS				0x00000008	// entity alpha
#define	FTE_PEXT_ACCURATETIMINGS	0x00000040
#define	FTE_PEXT_MODELDBL			0x00001000	// model numbers up to 511 in deltas
#define	FTE_PEXT_ENTITYDBL			0x00002000	// entity numbers up to 1023
#define	FTE_PEXT_ENTITYDBL2			0x00004000	// entity numbers up to 2047
#define	FTE_PEXT_FLOATCOORDS		0x00008000	// coordinates as floats, angles in 16 bits
#define	FTE_PEXT_COLOURMOD			0x00080000	// entity color multipliers
#define	FTE_PEXT_SPAWNSTATIC2		0x00400000	// statics and baselines as deltas
#define	FTE_PEXT_256PACKETENTITIES	0x01000000	// 256 entities in a packet
#define	FTE_PEXT_CHUNKEDDOWNLOADS	0x20000000	// downloads in numbered chunks
#define	DL_CHUNKSIZE				1024		// their size
#define	FTE_PEXT_CSQC				0x40000000	// client-side QuakeC: its entities, events and stats

#define	MVD_PEXT1_FLOATCOORDS		0x00000001	// entity and player origins as floats
#define	MVD_PEXT1_HIGHLAGTELEPORT	0x00000002	// svc_setangle carries a leading byte
#define	MVD_PEXT1_HIDDEN_MESSAGES	0x00000020	// hidden MVD blocks
#define	MVD_PEXT1_WEAPON_PREDICTION	0x00000080
#define	MVD_PEXT1_SIMPLE_PROJECTILE	0x00000100

#define	FTE_PEXT2_VOICECHAT			0x00000002	// svc_fte_voicechat

// the extensions this program speaks: CL_ as a client, SV_ as a server
#define	SV_FTE_EXTENSIONS	(FTE_PEXT_TRANS | FTE_PEXT_ACCURATETIMINGS | FTE_PEXT_MODELDBL | FTE_PEXT_ENTITYDBL | \
	FTE_PEXT_ENTITYDBL2 | FTE_PEXT_FLOATCOORDS | FTE_PEXT_COLOURMOD | FTE_PEXT_SPAWNSTATIC2 | \
	FTE_PEXT_256PACKETENTITIES | FTE_PEXT_CHUNKEDDOWNLOADS)
#define	CL_FTE_EXTENSIONS	(SV_FTE_EXTENSIONS | FTE_PEXT_CSQC)	// the server has no CSQC
#define	SV_MVD1_EXTENSIONS	(MVD_PEXT1_FLOATCOORDS | MVD_PEXT1_HIGHLAGTELEPORT)
#define	CL_MVD1_EXTENSIONS	SV_MVD1_EXTENSIONS

//
// ZQuake's extensions: a client lists them in its "*z_ext" userinfo key, a
// server in its "*z_ext" serverinfo key; both use those both list
//
#define	Z_EXT_PM_TYPE		(1<<0)	// playerinfo carries the movement type (PF_PMC)
#define	Z_EXT_PM_TYPE_NEW	(1<<1)	// with the fly and spectator types
#define	Z_EXT_VIEWHEIGHT	(1<<2)	// STAT_VIEWHEIGHT
#define	Z_EXT_SERVERTIME	(1<<3)	// STAT_TIME now and then
#define	Z_EXT_PITCHLIMITS	(1<<4)	// serverinfo maxpitch and minpitch
#define	Z_EXT_JOIN_OBSERVE	(1<<5)	// the join and observe commands switch sides without reconnecting
#define	Z_EXT_PF_ONGROUND	(1<<6)	// PF_ONGROUND is set in every playerinfo
#define	Z_EXT_VWEP			(1<<7)	// players carry visible weapons
#define	Z_EXT_PF_SOLID		(1<<8)	// PF_SOLID is set in every playerinfo

#define	SV_Z_EXTENSIONS		(Z_EXT_PM_TYPE | Z_EXT_PM_TYPE_NEW | Z_EXT_VIEWHEIGHT | Z_EXT_SERVERTIME | \
	Z_EXT_PITCHLIMITS | Z_EXT_JOIN_OBSERVE | Z_EXT_PF_ONGROUND | Z_EXT_PF_SOLID)
// visible weapons need progs that choose the models: the client only
#define	CL_Z_EXTENSIONS		(SV_Z_EXTENSIONS | Z_EXT_VWEP)

// what the client reads in recordings as well: FTE's voice chat is skipped
#define	CL_FTE_READABLE		CL_FTE_EXTENSIONS
#define	CL_FTE2_READABLE	FTE_PEXT2_VOICECHAT
#define	CL_MVD1_READABLE	(CL_MVD1_EXTENSIONS | MVD_PEXT1_HIDDEN_MESSAGES)	// hidden blocks are skipped

#define QW_CHECK_HASH 0x5157

//=========================================

#define	PORT_MASTER	27000
#define	PORT_SERVER	27500

//=========================================

// out of band message id bytes

// M = master, S = server, C = client, A = any
// the second character will allways be \n if the message isn't a single
// byte long (?? not true anymore?)

#define	S2C_CHALLENGE		'c'
#define	S2C_CONNECTION		'j'
#define	A2A_PING			'k'	// respond with an A2A_ACK
#define	A2A_ACK				'l'	// general acknowledgement without info
#define	A2A_NACK			'm'	// [+ comment] general failure
#define A2A_ECHO			'e' // for echoing
#define	A2C_PRINT			'n'	// print a message on client

#define	S2M_HEARTBEAT		'a'	// + serverinfo + userlist + fraglist
#define	A2C_CLIENT_COMMAND	'B'	// + command line
#define	S2M_SHUTDOWN		'C'


//==================
// note that there are some defs.qc that mirror to these numbers
// also related to svc_strings[] in cl_parse
//==================

//
// server to client
//
#define	svc_bad				0
#define	svc_nop				1
#define	svc_disconnect		2
#define	svc_updatestat		3	// [byte] [byte]
//define	svc_version			4	// [long] server version
#define	svc_setview			5	// [short] entity number
#define	svc_sound			6	// <see code>
//define	svc_time			7	// [float] server time
#define	svc_print			8	// [byte] id [string] null terminated string
#define	svc_stufftext		9	// [string] stuffed into client's console buffer
								// the string should be \n terminated
#define	svc_setangle		10	// [angle3] set the view angle to this absolute value
	
#define	svc_serverdata		11	// [long] protocol ...
#define	svc_lightstyle		12	// [byte] [string]
//define	svc_updatename		13	// [byte] [string]
#define	svc_updatefrags		14	// [byte] [short]
//define	svc_clientdata		15	// <shortbits + data>
#define	svc_stopsound		16	// <see code>
//define	svc_updatecolors	17	// [byte] [byte] [byte]
//define	svc_particle		18	// [vec3] <variable>
#define	svc_damage			19
	
#define	svc_spawnstatic		20
//	svc_spawnbinary		21
#define	svc_spawnbaseline	22
	
#define	svc_temp_entity		23	// variable
#define	svc_setpause		24	// [byte] on / off
//	svc_signonnum		25	// [byte]  used for the signon sequence

#define	svc_centerprint		26	// [string] to put in center of the screen

#define	svc_killedmonster	27
#define	svc_foundsecret		28

#define	svc_spawnstaticsound	29	// [coord3] [byte] samp [byte] vol [byte] aten

#define	svc_intermission	30		// [vec3_t] origin [vec3_t] angle
#define	svc_finale			31		// [string] text

#define	svc_cdtrack			32		// [byte] track
#define svc_sellscreen		33

#define	svc_smallkick		34		// set client punchangle to 2
#define	svc_bigkick			35		// set client punchangle to 4

#define	svc_updateping		36		// [byte] [short]
#define	svc_updateentertime	37		// [byte] [float]

#define	svc_updatestatlong	38		// [byte] [long]

#define	svc_muzzleflash		39		// [short] entity

#define	svc_updateuserinfo	40		// [byte] slot [long] uid
									// [string] userinfo

#define	svc_download		41		// [short] size [size bytes]
#define	svc_playerinfo		42		// variable
#define	svc_nails			43		// [byte] num [48 bits] xyzpy 12 12 12 4 8 
#define	svc_chokecount		44		// [byte] packets choked
#define	svc_modellist		45		// [strings]
#define	svc_soundlist		46		// [strings]
#define	svc_packetentities	47		// [...]
#define	svc_deltapacketentities	48		// [...]
#define svc_maxspeed		49		// maxspeed change, for prediction
#define svc_entgravity		50		// gravity change, for prediction
#define svc_setinfo			51		// setinfo on a client
#define svc_serverinfo		52		// serverinfo
#define svc_updatepl		53		// [byte] [byte]

// protocol extensions
#define	svc_fte_spawnstatic2	21	// an entity delta from nothing
#define	svc_nails2			54		// [byte] num, each [byte] entity [48 bits] xyzpy (MVD)
#define	svc_fte_soundlistshort	56	// svc_soundlist with a [short] start
#define	svc_fte_modellistshort	60	// svc_modellist with a [short] start
#define	svc_fte_spawnbaseline2	66	// an entity delta from nothing
#define	svc_fte_csqcentities	76	// CSQC's entities: [short] number, (0x8000 removed) or its data, ..., [short] 0
#define	svc_fte_updatestatstring	78	// [byte] stat [string]
#define	svc_fte_updatestatfloat	79	// [byte] stat [float]
#define	svc_fte_cgamepacket	83		// CSQC's own message, all it reads (CSQC_Parse_Event)
#define	svc_fte_voicechat	84		// [byte] [byte] [byte] [short] n [n bytes]
#define	svc_fte_cgamepacket_sized	90	// svc_fte_cgamepacket with a [short] size first
#define	svc_fte_csqcentities_sized	92	// svc_fte_csqcentities with a [short] size after each number


//==============================================

//
// client to server
//
#define	clc_bad			0
#define	clc_nop 		1
//define	clc_doublemove	2
#define	clc_move		3		// [[usercmd_t]
#define	clc_stringcmd	4		// [string] message
#define	clc_delta		5		// [byte] sequence number, requests delta compression of message
#define clc_tmove		6		// teleport request, spectator only
#define clc_upload		7		// teleport request, spectator only


//==============================================

// playerinfo flags from server
// playerinfo allways sends: playernum, flags, origin[] and framenumber

#define	PF_MSEC			(1<<0)
#define	PF_COMMAND		(1<<1)
#define	PF_VELOCITY1	(1<<2)
#define	PF_VELOCITY2	(1<<3)
#define	PF_VELOCITY3	(1<<4)
#define	PF_MODEL		(1<<5)
#define	PF_SKINNUM		(1<<6)
#define	PF_EFFECTS		(1<<7)
#define	PF_WEAPONFRAME	(1<<8)		// only sent for view player
#define	PF_DEAD			(1<<9)		// don't block movement any more
#define	PF_GIB			(1<<10)		// offset the view height differently
#define	PF_PMC_SHIFT	11			// Z_EXT_PM_TYPE: how the player moves, 3 bits
#define	PF_PMC_MASK		7
#define	PF_EXTRA_PFS	(1<<15)		// FTE_PEXT_TRANS: a byte of flags 16-23 follows
#define	PF_TRANS		(1<<17)		// FTE_PEXT_TRANS: a byte of alpha, after the weapon frame
#define	PF_COLOURMOD	(1<<19)		// FTE_PEXT_COLOURMOD: three bytes of color, after the alpha
#define	PF_ONGROUND		(1<<22)		// ZQuake; bit 14 on the wire without FTE_PEXT_TRANS
#define	PF_SOLID		(1<<23)		// ZQuake; bit 15 on the wire without FTE_PEXT_TRANS

// an MVD's playerinfo flags: what is sent; the rest is as last sent
#define	DF_ORIGIN		(1<<0)		// three bits, one per axis
#define	DF_ANGLES		(1<<3)		// three bits
#define	DF_EFFECTS		(1<<6)
#define	DF_SKINNUM		(1<<7)
#define	DF_DEAD			(1<<8)
#define	DF_GIB			(1<<9)
#define	DF_WEAPONFRAME	(1<<10)
#define	DF_MODEL		(1<<11)

// the PF_PMC codes; 3 and up need Z_EXT_PM_TYPE_NEW
#define	PMC_NORMAL				0	// or dead, with PF_DEAD
#define	PMC_NORMAL_JUMP_HELD	1
#define	PMC_OLD_SPECTATOR		2
#define	PMC_SPECTATOR			3
#define	PMC_FLY					4
#define	PMC_NONE				5
#define	PMC_LOCK				6

//==============================================

// if the high bit of the client to server byte is set, the low bits are
// client move cmd bits
// ms and angle2 are allways sent, the others are optional
#define	CM_ANGLE1 	(1<<0)
#define	CM_ANGLE3 	(1<<1)
#define	CM_FORWARD	(1<<2)
#define	CM_SIDE		(1<<3)
#define	CM_UP		(1<<4)
#define	CM_BUTTONS	(1<<5)
#define	CM_IMPULSE	(1<<6)
#define	CM_ANGLE2 	(1<<7)

//==============================================

// the first 16 bits of a packetentities update holds 9 bits
// of entity number and 7 bits of flags
#define	U_ORIGIN1	(1<<9)
#define	U_ORIGIN2	(1<<10)
#define	U_ORIGIN3	(1<<11)
#define	U_ANGLE2	(1<<12)
#define	U_FRAME		(1<<13)
#define	U_REMOVE	(1<<14)		// REMOVE this entity, don't add it
#define	U_MOREBITS	(1<<15)

// if MOREBITS is set, these additional flags are read in next
#define	U_ANGLE1	(1<<0)
#define	U_ANGLE3	(1<<1)
#define	U_MODEL		(1<<2)
#define	U_COLORMAP	(1<<3)
#define	U_SKIN		(1<<4)
#define	U_EFFECTS	(1<<5)
#define	U_SOLID		(1<<6)		// the entity should be solid for prediction
#define	U_EVENMORE	(1<<7)		// with FTE extensions: a byte of FTE's bits follows

// FTE's bits, in the byte after the MOREBITS byte and, with U_FTE_YETMORE, the
// one after that
#define	U_FTE_TRANS			(1<<1)		// a byte of alpha, after the angles
#define	U_FTE_MODELDBL		(1<<3)		// the model number + 256, or a short without U_MODEL
#define	U_FTE_ENTITYDBL		(1<<5)		// the entity number + 512
#define	U_FTE_ENTITYDBL2	(1<<6)		// the entity number + 1024
#define	U_FTE_YETMORE		(1<<7)		// bits 8-15 follow
#define	U_FTE_COLOURMOD		(1<<10)		// three bytes of color, after the alpha

//==============================================

// a sound with no channel is a local only sound
// the sound field has bits 0-2: channel, 3-12: entity
#define	SND_VOLUME		(1<<15)		// a byte
#define	SND_ATTENUATION	(1<<14)		// a byte

#define DEFAULT_SOUND_PACKET_VOLUME 255
#define DEFAULT_SOUND_PACKET_ATTENUATION 1.0

// svc_print messages have an id, so messages can be filtered
#define	PRINT_LOW			0
#define	PRINT_MEDIUM		1
#define	PRINT_HIGH			2
#define	PRINT_CHAT			3	// also go to chat buffer

//
// temp entity events
//
#define	TE_SPIKE			0
#define	TE_SUPERSPIKE		1
#define	TE_GUNSHOT			2
#define	TE_EXPLOSION		3
#define	TE_TAREXPLOSION		4
#define	TE_LIGHTNING1		5
#define	TE_LIGHTNING2		6
#define	TE_WIZSPIKE			7
#define	TE_KNIGHTSPIKE		8
#define	TE_LIGHTNING3		9
#define	TE_LAVASPLASH		10
#define	TE_TELEPORT			11
#define	TE_BLOOD			12
#define	TE_LIGHTNINGBLOOD	13


/*
==========================================================

  ELEMENTS COMMUNICATED ACROSS THE NET

==========================================================
*/

#define	MAX_CLIENTS		32

#define	UPDATE_BACKUP	64	// copies of entity_state_t to keep buffered
							// must be power of two
#define	UPDATE_MASK		(UPDATE_BACKUP-1)

// entity_state_t is the information conveyed from the server
// in an update message
typedef struct entity_state_s
{
	int		number;			// edict index

	int		flags;			// nolerp, etc
	vec3_t	origin;
	vec3_t	angles;
	int		modelindex;
	int		frame;
	int		colormap;
	int		skinnum;
	int		effects;
	byte	alpha;			// FTE_PEXT_TRANS: 0 and 255 are opaque, else alpha * 254
	byte	colormod[3];	// FTE_PEXT_COLOURMOD: 32 is 1.0; 0 0 0 is unset
} entity_state_t;


// entities in a packet, not counting nails: 256 with FTE_PEXT_256PACKETENTITIES,
// 300 in an MVD
#define	MAX_PACKET_ENTITIES	256
#define	STD_PACKET_ENTITIES	64
#define	MAX_MVD_PACKET_ENTITIES	300
typedef struct
{
	int		num_entities;
	entity_state_t	entities[MAX_MVD_PACKET_ENTITIES];
} packet_entities_t;

typedef struct usercmd_s
{
	byte	msec;
	vec3_t	angles;
	short	forwardmove, sidemove, upmove;
	byte	buttons;
	byte	impulse;
} usercmd_t;
