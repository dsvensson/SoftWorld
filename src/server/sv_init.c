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

#include "sv_local.h"

server_static_t	svs;				// persistant server info
server_t		sv;					// local server
static arena_t		sv_level_arena;		// memory that lives as long as the current level



/*
================
SV_ModelIndex

================
*/
int SV_ModelIndex (const char *name)
{
	int		i;
	
	if (!name || !name[0])
		return 0;

	for (i=0 ; i<MAX_MODELS && sv.model_precache[i] ; i++)
		if (!strcmp(sv.model_precache[i], name))
			return i;
	if (i==MAX_MODELS || !sv.model_precache[i])
		SV_Error ("SV_ModelIndex: model %s not precached", name);
	return i;
}

/*
================
SV_LevelString

A copy of text, for as long as the level lasts
================
*/
char *SV_LevelString (const char *s)
{
	size_t	len = strlen (s) + 1;
	char	*copy = Arena_Alloc (&sv_level_arena, len);

	memcpy (copy, s, len);
	return copy;
}

/*
================
SV_NewSignonBuffer

Starts another signon buffer, from the level's memory
================
*/
static void SV_NewSignonBuffer (void)
{
	int		max, *sizes;
	byte	**buffers;

	if (sv.num_signon_buffers == sv.max_signon_buffers)
	{
		max = sv.max_signon_buffers ? sv.max_signon_buffers * 2 : 8;
		sizes = Arena_Alloc (&sv_level_arena, (size_t)max * sizeof(*sizes));
		buffers = Arena_Alloc (&sv_level_arena, (size_t)max * sizeof(*buffers));
		if (sv.num_signon_buffers)
		{
			memcpy (sizes, sv.signon_buffer_size, (size_t)sv.num_signon_buffers * sizeof(*sizes));
			memcpy (buffers, sv.signon_buffers, (size_t)sv.num_signon_buffers * sizeof(*buffers));
		}
		sv.signon_buffer_size = sizes;
		sv.signon_buffers = buffers;
		sv.max_signon_buffers = max;
	}
	sv.signon_buffers[sv.num_signon_buffers] = Arena_Alloc (&sv_level_arena, MAX_DATAGRAM);
	sv.signon.data = sv.signon_buffers[sv.num_signon_buffers];
	sv.signon.maxsize = MAX_DATAGRAM;
	sv.signon.cursize = 0;
	sv.num_signon_buffers++;
}

/*
================
SV_NewStatic

Another static entity of the level, cleared. Its number only makes it a
valid delta; it stays below 512, so every client can read it.
================
*/
entity_state_t *SV_NewStatic (void)
{
	entity_state_t	*s;
	int		max;

	if (sv.num_static_entities == sv.max_static_entities)
	{
		max = sv.max_static_entities ? sv.max_static_entities * 2 : 64;
		s = Arena_Alloc (&sv_level_arena, (size_t)max * sizeof(*s));
		if (sv.num_static_entities)
			memcpy (s, sv.static_entities, (size_t)sv.num_static_entities * sizeof(*s));
		sv.static_entities = s;
		sv.max_static_entities = max;
	}
	s = &sv.static_entities[sv.num_static_entities];
	memset (s, 0, sizeof(*s));
	s->number = 1 + sv.num_static_entities % 511;
	sv.num_static_entities++;
	return s;
}

staticsound_t *SV_NewStaticSound (void)
{
	staticsound_t	*s;
	int		max;

	if (sv.num_static_sounds == sv.max_static_sounds)
	{
		max = sv.max_static_sounds ? sv.max_static_sounds * 2 : 64;
		s = Arena_Alloc (&sv_level_arena, (size_t)max * sizeof(*s));
		if (sv.num_static_sounds)
			memcpy (s, sv.static_sounds, (size_t)sv.num_static_sounds * sizeof(*s));
		sv.static_sounds = s;
		sv.max_static_sounds = max;
	}
	s = &sv.static_sounds[sv.num_static_sounds++];
	memset (s, 0, sizeof(*s));
	return s;
}

void SV_SignonRoom (int size)
{
	if (sv.signon.cursize + size <= sv.signon.maxsize)
		return;

	sv.signon_buffer_size[sv.num_signon_buffers-1] = sv.signon.cursize;
	SV_NewSignonBuffer ();
}

/*
================
SV_FlushSignon

Moves to the next signon buffer if needed
================
*/
void SV_FlushSignon (void)
{
	if (sv.signon.cursize < sv.signon.maxsize - 512)
		return;

	sv.signon_buffer_size[sv.num_signon_buffers-1] = sv.signon.cursize;
	SV_NewSignonBuffer ();
}

/*
================
SV_SetCoordEncoding

Coordinates go out as floats (FTE_PEXT_FLOATCOORDS) when the map's geometry
goes past +-4096, where the standard 1/8 unit shorts end (walls at 4096 keep
everything inside them in range), or when sv_bigcoords asks for it.
Everything the server writes this level, and what it reads, uses it; the
clients' own buffers switch with serverdata (SV_New_f).
================
*/
static void SV_SetCoordEncoding (void)
{
	sv.bigcoords = sv_bigcoords.value != 0 || CM_Extent (sv.map) > 4096;
	if (sv.bigcoords)
		Con_Printf ("%s uses float coordinates\n", sv.modelname);

	sv.datagram.floatcoords = sv.bigcoords;
	sv.reliable_datagram.floatcoords = sv.bigcoords;
	sv.multicast.floatcoords = sv.bigcoords;
	sv.signon.floatcoords = sv.bigcoords;
	svs.net_message.floatcoords = sv.bigcoords;
}

/*
================
SV_CreateBaseline

Entity baselines are used to compress the update messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
static void SV_CreateBaseline (void)
{
	edict_t			*svent;
	int				entnum;

	for (entnum = 0; entnum < sv.num_edicts ; entnum++)
	{
		svent = EDICT_NUM(entnum);
		if (svent->free)
			continue;
		// create baselines for all player slots,
		// and any other edict that has a visible model
		if (entnum > MAX_CLIENTS && !svent->v.modelindex)
			continue;

	//
	// create entity baseline
	//
		svent->baseline.number = entnum;
		VectorCopy (svent->v.origin, svent->baseline.origin);
		VectorCopy (svent->v.angles, svent->baseline.angles);
		svent->baseline.frame = (int)svent->v.frame;
		svent->baseline.skinnum = (int)svent->v.skin;
		if (entnum > 0 && entnum <= MAX_CLIENTS)
		{
			svent->baseline.colormap = entnum;
			svent->baseline.modelindex = SV_ModelIndex("progs/player.mdl");
		}
		else
		{
			svent->baseline.colormap = 0;
			svent->baseline.modelindex =
				SV_ModelIndex(PR_GetString(svent->v.model));
		}
		SV_EntityLook (svent, &svent->baseline);
	}

	// sent to each client at prespawn
	sv.num_baselines = sv.num_edicts;
}


/*
================
SV_SaveSpawnparms

Grabs the current state of the progs serverinfo flags 
and each client for saving across the
transition to another level: as parms says
================
*/
static void SV_SaveSpawnparms (spawnparms_t parms)
{
	int		i, j;

	if (parms == SPAWNPARMS_NEW)
		svs.serverflags = 0;
	if (!sv.state)
		return;		// no progs loaded yet

	// serverflags is the only game related thing maintained
	if (parms == SPAWNPARMS_CHANGE)
		svs.serverflags = (int)PR_GLOBAL(serverflags);

	for (i=0, host_client = svs.clients ; i<MAX_CLIENTS ; i++, host_client++)
	{
		if (host_client->state != cs_spawned)
			continue;

		// needs to reconnect
		host_client->state = cs_connected;
		host_client->newparms = parms == SPAWNPARMS_NEW;
		if (parms != SPAWNPARMS_CHANGE)
			continue;

		// call the progs to get default spawn parms for the new client
		PR_GLOBAL(self) = EDICT_TO_PROG(host_client->edict);
		PR_ExecuteProgram (PR_GLOBAL(SetChangeParms));
		for (j=0 ; j<NUM_SPAWN_PARMS ; j++)
			host_client->spawn_parms[j] = PR_PARM(j);
	}
}

/*
================
SV_InitVis

Room for the visibility rows of the level. A PHS (potentially hearable set)
row is the PVS rows of every leaf a leaf sees; for a big map building them
all takes minutes, so each is built when a multicast first needs it.
================
*/
static void SV_InitVis (void)
{
	int		num;

	// a row for every leaf with visibility, and one for leaf 0 outside the map
	num = CM_NumVisLeafs (sv.map) + 1;
	sv.vis_rows = num;
	sv.vis_rowbytes = ((num + 31) >> 5) * 4;
	sv.checkpvs = Arena_Alloc (&sv_level_arena, (size_t)sv.vis_rowbytes);
	sv.pvs_rows = Arena_Alloc (&sv_level_arena, (size_t)num * sizeof(*sv.pvs_rows));
	sv.phs_rows = Arena_Alloc (&sv_level_arena, (size_t)num * sizeof(*sv.phs_rows));
}

/*
================
SV_LeafPVS

The leafs leafnum sees, bit n for leaf n+1; leaf 0 (outside the map) sees
everything
================
*/
byte *SV_LeafPVS (int leafnum)
{
	byte	*row;

	if (leafnum < 0 || leafnum >= sv.vis_rows)
		leafnum = 0;
	if (!sv.pvs_rows[leafnum])
	{
		row = Arena_Alloc (&sv_level_arena, (size_t)sv.vis_rowbytes);
		memcpy (row, CM_LeafPVS (sv.map, leafnum), (size_t)sv.vis_rowbytes);
		sv.pvs_rows[leafnum] = row;
	}
	return sv.pvs_rows[leafnum];
}

/*
================
SV_LeafPHS

The leafs leafnum can hear: the PVS of every leaf it sees
================
*/
byte *SV_LeafPHS (int leafnum)
{
	unsigned	*row, *src;
	byte		*pvs;
	int			j, l, rowwords = sv.vis_rowbytes / 4;

	if (leafnum < 0 || leafnum >= sv.vis_rows)
		leafnum = 0;
	if (!sv.phs_rows[leafnum])
	{
		pvs = SV_LeafPVS (leafnum);
		row = Arena_Alloc (&sv_level_arena, (size_t)sv.vis_rowbytes);
		memcpy (row, pvs, (size_t)sv.vis_rowbytes);
		// bit j is leaf j+1
		for (j = 0 ; j < sv.vis_rows - 1 ; j++)
		{
			if (!(pvs[j>>3] & (1<<(j&7))))
				continue;
			src = (unsigned *)SV_LeafPVS (j + 1);
			for (l = 0 ; l < rowwords ; l++)
				row[l] |= src[l];
		}
		sv.phs_rows[leafnum] = (byte *)row;
	}
	return sv.phs_rows[leafnum];
}

static unsigned SV_CheckModel(char *mdl)
{
	byte	*buf;
	int		len;
	unsigned short crc;

	buf = FS_LoadFile (mdl, &len);
	if (!buf)
		SV_Error ("SV_CheckModel: couldn't load %s", mdl);
	crc = CRC_Block(buf, len);
	Mem_Free (buf);

	return crc;
}

/*
================
SV_SpawnServer

Change the server to a new map, taking all connected
clients along with it, their spawn parms as parms says.

Called by the map, changelevel and restart commands (SV_GotoLevel).
================
*/
/*
================
SV_PublishCsprogs

The client-side progs the server offers, as FTE's server does: the checksum
(the folded MD4) and size of sv_csqc_progname in the serverinfo, and its name
when it isn't csprogs.dat; none of them without the file
================
*/
static void SV_PublishCsprogs (void)
{
	const char	*name = sv_csqc_progname.string;
	byte		*data = *name ? FS_LoadFile (name, NULL) : NULL;
	int			size = com_filesize;
	char		checksum[32], length[32];

	*checksum = *length = 0;
	if (data)
	{
		snprintf (checksum, sizeof(checksum), "0x%x", Com_BlockChecksum (data, size));
		snprintf (length, sizeof(length), "0x%x", (unsigned)size);
		Mem_Free (data);
	}
	Info_SetValueForStarKey (svs.info, "*csprogs", checksum, MAX_SERVERINFO_STRING, SV_InfoCharset ());
	Info_SetValueForStarKey (svs.info, "*csprogssize", length, MAX_SERVERINFO_STRING, SV_InfoCharset ());
	Info_SetValueForStarKey (svs.info, "*csprogsname", data && strcmp (name, "csprogs.dat") ? name : "",
		MAX_SERVERINFO_STRING, SV_InfoCharset ());
}

void SV_SpawnServer (char *server, spawnparms_t parms)
{
	edict_t		*ent;
	int			i;
	cmap_t		*oldmap;

	Con_DPrintf ("SpawnServer: %s\n",server);

	// the old level's last events out to QTV's viewers
	SV_MVDEndLevel ();

	// the first map opens the server's port; a listen server can do without
	if (NET_SocketAddress (NS_SERVER).type == NA_INVALID && !NET_OpenSocket (NS_SERVER, svs.port))
	{
		if (host.dedicated)
			Sys_Error ("Couldn't open UDP port %i", svs.port);
		Con_Printf ("Couldn't open UDP port %i: only this client can join\n", svs.port);
	}
	// and at its number on TCP, browsers' clients over WebSocket and, a
	// public server's, QTV's viewers; without them if it can't
	NET_ListenTCP (svs.port, sv_websocket.value != 0, sv_public.value != 0);
	// and clients over WebRTC, through a broker's room: its name, or the
	// invitation code
	NET_HostRTC (sv_public.value ? sv_webrtc_room.string : NULL);
	
	SV_SaveSpawnparms (parms);

	svs.spawncount++;		// any partially connected client will be
							// restarted

	oldmap = sv.map;		// freed once the new map is loaded, which may be the same

	sv.state = ss_dead;

	if (!sv_level_arena.name)
		Arena_Init (&sv_level_arena, "server level");
	Arena_Reset (&sv_level_arena);

	// wipe the entire per-level structure
	memset (&sv, 0, sizeof(sv));
	SV_NQNewLevel ();

	sv.datagram.maxsize = sizeof(sv.datagram_buf);
	sv.datagram.data = sv.datagram_buf;
	sv.datagram.allowoverflow = true;

	sv.reliable_datagram.maxsize = sizeof(sv.reliable_datagram_buf);
	sv.reliable_datagram.data = sv.reliable_datagram_buf;
	
	sv.multicast.maxsize = sizeof(sv.multicast_buf);
	sv.multicast.data = sv.multicast_buf;
	
	sv.master.maxsize = sizeof(sv.master_buf);
	sv.master.data = sv.master_buf;
	
	SV_NewSignonBuffer ();

	Q_strncpyz (sv.name, server, sizeof(sv.name));

	// NetQuake's rules: coop has no deathmatch, and a skill of 0 to 3
	if (coop.value && deathmatch.value)
		Cvar_Set ("deathmatch", "0");
	i = (int)(skill.value + 0.5f);
	Cvar_SetValue ("skill", (float)(i < 0 ? 0 : i > 3 ? 3 : i));

	// load progs to get entity field count
	// which determines how big each edict is
	PR_LoadProgs ();
	SV_PublishCsprogs ();

	// the VM's entities, of which the slots at the start are the clients'
	sv.edicts = (edict_t *)QC_Edicts (pr.vm);
	sv.num_edicts = MAX_CLIENTS+1;
	for (i=0 ; i<MAX_CLIENTS ; i++)
	{
		QC_ClaimEdict (pr.vm, (qc_ent_t)(i+1));
		ent = EDICT_NUM(i+1);
		svs.clients[i].edict = ent;
//ZOID - make sure we update frags right
		svs.clients[i].old_frags = 0;
	}

	sv.time = 1.0;
	
	Q_strncpyz (sv.name, server, sizeof(sv.name));
	snprintf (sv.modelname, sizeof(sv.modelname), "maps/%s.bsp", server);
	sv.map = CM_LoadMap (sv.modelname, &sv.map_checksum, &sv.map_checksum2);
	if (oldmap)
		CM_FreeMap (oldmap);
	if (!sv.map)
		SV_Error ("Couldn't load %s", sv.modelname);
	sv.worldmodel = CM_WorldModel (sv.map);
	SV_SetCoordEncoding ();
	// model numbers go out as bytes: the world and its inline models must
	// leave room in the precache list
	if (CM_NumInlineModels (sv.map) + 1 >= MAX_MODELS)
		SV_Error ("%s has %i brush models, more than the protocol's %i", sv.modelname,
			CM_NumInlineModels (sv.map), MAX_MODELS - 2);
	SV_InitVis ();

	//
	// clear physics interaction links
	//
	SV_ClearWorld ();
	
	sv.sound_precache[0] = "";

	sv.model_precache[0] = "";
	sv.model_precache[1] = sv.modelname;
	sv.models[1] = sv.worldmodel;
	for (i=1 ; i<CM_NumInlineModels (sv.map) ; i++)
	{
		sv.model_precache[1+i] = svs.localmodels[i];
		sv.models[i+1] = CM_InlineModel (sv.map, svs.localmodels[i]);
	}

	//check player/eyes models for hacks
	sv.model_player_checksum = SV_CheckModel("progs/player.mdl");
	sv.eyes_player_checksum = SV_CheckModel("progs/eyes.mdl");

	//
	// spawn the rest of the entities on the map
	//	

	// precache and static commands can be issued during
	// map initialization
	sv.state = ss_loading;

	ent = EDICT_NUM(0);
	ent->free = false;
	ent->v.model = PR_SetString(sv.modelname);
	ent->v.modelindex = 1;		// world model
	ent->v.solid = SOLID_BSP;
	ent->v.movetype = MOVETYPE_PUSH;

	PR_GLOBAL(mapname) = PR_SetString(sv.name);
	// serverflags are for cross level information (sigils)
	PR_GLOBAL(serverflags) = (float)svs.serverflags;
	// the rules, which NetQuake's progs read as globals
	PR_GLOBAL(deathmatch) = deathmatch.value;
	PR_GLOBAL(coop) = coop.value;
	PR_GLOBAL(teamplay) = teamplay.value;
	
	// run the frame start qc function to let progs check cvars; QuakeWorld's
	// only, as FTE has it: NetQuake's count their frames from the first after
	// the spawn functions (Copper's precache only before it)
	if (!pr.nq)
		SV_ProgStartFrame ();

	// load and spawn all other entities
	ED_LoadFromFile (CM_EntityString (sv.map));

	// look up some model indexes for specialized message compression
	SV_FindModelNumbers ();

	// FTE's replacement deltas where QuakeWorld's have no room: NetQuake's
	// progs (their levels are the big ones), more entities than 2048 or
	// sounds than 256; QuakeWorld's own levels stay QuakeWorld's
	sv.replacementdeltas = sv_replacementdeltas.value || pr.nq || sv.num_edicts > MAX_QW_EDICTS
		|| sv.sound_precache[MAX_QW_SOUNDS];

	// all spawning is completed, any further precache statements
	// or prog writes to the signon message are errors
	sv.state = ss_active;
	// and QuakeC's writes to the world are skipped, with a warning
	QC_SetProtected (pr.vm, 0, true);
	
	// run two frames to allow everything to settle
	sv.frametime = 0.1;
	SV_Physics ();
	SV_Physics ();

	// save movement vars
	SV_SetMoveVars();

	// create a baseline for more efficient communications
	SV_CreateBaseline ();
	sv.signon_buffer_size[sv.num_signon_buffers-1] = sv.signon.cursize;

	// what the spawn functions broadcast is for no one: the clients get the
	// level whole at its serverdata (Copper's monsters send their count to
	// all as each spawns)
	SZ_Clear (&sv.reliable_datagram);
	SZ_Clear (&sv.datagram);

	Info_SetValueForKey (svs.info, "map", sv.name, MAX_SERVERINFO_STRING, SV_InfoCharset ());
	SV_MVDNewLevel ();
	Con_DPrintf ("Server spawned.\n");
}

