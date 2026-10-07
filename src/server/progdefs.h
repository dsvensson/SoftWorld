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
// progdefs.h -- id's globals and entity fields, as the server reads them
//
// QuakeWorld's and NetQuake's progs lay them out differently, so the server has
// its own layout of each and binds a progs' by name when it loads: the fields
// move to the server's struct (the VM's host_fields, FTE's QC_RegisterFieldVar),
// and the globals are pointed at (FTE's globalptrs_t), the server keeping those
// a progs lacks.

#include "mathlib.h"
#include "pr_comp.h"

// X(type, QuakeC type, name): QuakeWorld's in its order, then NetQuake's own
// (FTE's extension fields)
#define PR_ENTITY_FIELDS(X) \
	X(float, ev_float, modelindex) \
	X(vec3_t, ev_vector, absmin) \
	X(vec3_t, ev_vector, absmax) \
	X(float, ev_float, ltime) \
	X(float, ev_float, lastruntime) \
	X(float, ev_float, movetype) \
	X(float, ev_float, solid) \
	X(vec3_t, ev_vector, origin) \
	X(vec3_t, ev_vector, oldorigin) \
	X(vec3_t, ev_vector, velocity) \
	X(vec3_t, ev_vector, angles) \
	X(vec3_t, ev_vector, avelocity) \
	X(string_t, ev_string, classname) \
	X(string_t, ev_string, model) \
	X(float, ev_float, frame) \
	X(float, ev_float, skin) \
	X(float, ev_float, effects) \
	X(vec3_t, ev_vector, mins) \
	X(vec3_t, ev_vector, maxs) \
	X(vec3_t, ev_vector, size) \
	X(func_t, ev_function, touch) \
	X(func_t, ev_function, use) \
	X(func_t, ev_function, think) \
	X(func_t, ev_function, blocked) \
	X(float, ev_float, nextthink) \
	X(int, ev_entity, groundentity) \
	X(float, ev_float, health) \
	X(float, ev_float, frags) \
	X(float, ev_float, weapon) \
	X(string_t, ev_string, weaponmodel) \
	X(float, ev_float, weaponframe) \
	X(float, ev_float, currentammo) \
	X(float, ev_float, ammo_shells) \
	X(float, ev_float, ammo_nails) \
	X(float, ev_float, ammo_rockets) \
	X(float, ev_float, ammo_cells) \
	X(float, ev_float, items) \
	X(float, ev_float, takedamage) \
	X(int, ev_entity, chain) \
	X(float, ev_float, deadflag) \
	X(vec3_t, ev_vector, view_ofs) \
	X(float, ev_float, button0) \
	X(float, ev_float, button1) \
	X(float, ev_float, button2) \
	X(float, ev_float, impulse) \
	X(float, ev_float, fixangle) \
	X(vec3_t, ev_vector, v_angle) \
	X(string_t, ev_string, netname) \
	X(int, ev_entity, enemy) \
	X(float, ev_float, flags) \
	X(float, ev_float, colormap) \
	X(float, ev_float, team) \
	X(float, ev_float, max_health) \
	X(float, ev_float, teleport_time) \
	X(float, ev_float, armortype) \
	X(float, ev_float, armorvalue) \
	X(float, ev_float, waterlevel) \
	X(float, ev_float, watertype) \
	X(float, ev_float, ideal_yaw) \
	X(float, ev_float, yaw_speed) \
	X(int, ev_entity, aiment) \
	X(int, ev_entity, goalentity) \
	X(float, ev_float, spawnflags) \
	X(string_t, ev_string, target) \
	X(string_t, ev_string, targetname) \
	X(float, ev_float, dmg_take) \
	X(float, ev_float, dmg_save) \
	X(int, ev_entity, dmg_inflictor) \
	X(int, ev_entity, owner) \
	X(vec3_t, ev_vector, movedir) \
	X(string_t, ev_string, message) \
	X(float, ev_float, sounds) \
	X(string_t, ev_string, noise) \
	X(string_t, ev_string, noise1) \
	X(string_t, ev_string, noise2) \
	X(string_t, ev_string, noise3) \
	X(vec3_t, ev_vector, punchangle) \
	X(float, ev_float, idealpitch)

typedef struct
{
#define X(type, qctype, name)	type name;
	PR_ENTITY_FIELDS (X)
#undef X
} entvars_t;

// X(type, QuakeC type, name): id's globals but the parms, QuakeWorld's newmis
// and NetQuake's deathmatch, coop and teamplay among them
#define PR_GLOBALS(X) \
	X(int, ev_entity, self) \
	X(int, ev_entity, other) \
	X(int, ev_entity, world) \
	X(float, ev_float, time) \
	X(float, ev_float, frametime) \
	X(int, ev_entity, newmis) \
	X(float, ev_float, force_retouch) \
	X(string_t, ev_string, mapname) \
	X(float, ev_float, deathmatch) \
	X(float, ev_float, coop) \
	X(float, ev_float, teamplay) \
	X(float, ev_float, serverflags) \
	X(float, ev_float, total_secrets) \
	X(float, ev_float, total_monsters) \
	X(float, ev_float, found_secrets) \
	X(float, ev_float, killed_monsters) \
	X(vec3_t, ev_vector, v_forward) \
	X(vec3_t, ev_vector, v_up) \
	X(vec3_t, ev_vector, v_right) \
	X(float, ev_float, trace_allsolid) \
	X(float, ev_float, trace_startsolid) \
	X(float, ev_float, trace_fraction) \
	X(vec3_t, ev_vector, trace_endpos) \
	X(vec3_t, ev_vector, trace_plane_normal) \
	X(float, ev_float, trace_plane_dist) \
	X(int, ev_entity, trace_ent) \
	X(float, ev_float, trace_inopen) \
	X(float, ev_float, trace_inwater) \
	X(int, ev_entity, msg_entity) \
	X(func_t, ev_function, main) \
	X(func_t, ev_function, StartFrame) \
	X(func_t, ev_function, PlayerPreThink) \
	X(func_t, ev_function, PlayerPostThink) \
	X(func_t, ev_function, ClientKill) \
	X(func_t, ev_function, ClientConnect) \
	X(func_t, ev_function, PutClientInServer) \
	X(func_t, ev_function, ClientDisconnect) \
	X(func_t, ev_function, SetNewParms) \
	X(func_t, ev_function, SetChangeParms)

#define	NUM_SPAWN_PARMS		16

// where each global is: the progs' own, or the server's for a progs without it
typedef struct
{
#define X(type, qctype, name)	type *name;
	PR_GLOBALS (X)
#undef X
	float	*parm[NUM_SPAWN_PARMS];
} globalptrs_t;

#define	PROGHEADER_CRC		54730		// QuakeWorld's progs; NetQuake's are 5927
