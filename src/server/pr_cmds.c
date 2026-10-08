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
// pr_cmds.c -- the server's builtins: those that need the engine, and FTE's
// server versions of error and objerror
//
// A builtin reads its arguments with QC_Arg*, sets its result with QC_Return*
// and returns true; it fails the QuakeC call by returning QC_Error's false.
// The engine's older functions take char * for text they only read, so the
// VM's strings are cast for them.

#include "sv_local.h"

// an entity argument (the world for a number out of range)
static edict_t *PF_ArgEdict (qcvm_t *vm, int i)
{
	return PROG_TO_EDICT (QC_ArgInt (vm, i));
}

static int PF_ArgEdictNum (qcvm_t *vm, int i)
{
	return NUM_FOR_EDICT (PF_ArgEdict (vm, i));
}

// a float argument as an int, truncated as x86 does it
static int PF_ArgTrunc (qcvm_t *vm, int i)
{
	return QC_FloatToInt (QC_ArgFloat (vm, i));
}

static void PF_ReturnEdict (qcvm_t *vm, edict_t *e)
{
	QC_ReturnInt (vm, EDICT_TO_PROG (e));
}

static bool PF_ReturnString (qcvm_t *vm, const char *s)
{
	return QC_ReturnString (vm, s, strlen (s));
}

/*
===============================================================================

						BUILT-IN FUNCTIONS

===============================================================================
*/

static char *PF_VarString (qcvm_t *vm, int first)
{
	int		i;
	static char out[256];
	
	out[0] = 0;
	for (i=first ; i<QC_Argc (vm) ; i++)
	{
		Q_strncatz (out, QC_ArgString (vm, i), sizeof(out));
	}
	return out;
}


/*
=================
PF_errror

This is a TERMINAL error, which will kill off the entire server.
Dumps self.

error(value)
=================
*/
static bool PF_error (qcvm_t *vm)
{
	char	*s;
	edict_t	*ed;
	
	s = PF_VarString(vm, 0);
	Con_Printf ("======SERVER ERROR in %s:\n%s\n", QC_CallerName (vm), s);
	ed = PROG_TO_EDICT(PR_GLOBAL(self));
	ED_Print (ed);

	return QC_HostError (vm, "%s", s);
}

/*
=================
PF_objerror

Dumps out self, then an error message.  Self is removed and the QuakeC that
called it is abandoned, but the level goes on, as in FTE: an entity the progs
can't set up (a map made for other progs) doesn't stop the server.

objerror(value)
=================
*/
static bool PF_objerror (qcvm_t *vm)
{
	char	*s;
	edict_t	*ed;
	size_t	len;

	s = PF_VarString(vm, 0);
	Con_Printf ("======OBJECT ERROR in %s:\n%s\n", QC_CallerName (vm), s);
	ed = PROG_TO_EDICT(PR_GLOBAL(self));
	ED_Print (ed);
	for (len = strlen (s) ; len && s[len - 1] == '\n' ; len--)
		;
	QC_Warning (vm, "Program error: %.*s", (int)len, s);
	ED_Free (ed);
	return QC_Abort (vm, QC_ValWord (0));
}



/*
=================
PF_setorigin

This is the only valid way to move an object without using the physics of the world (setting velocity and waiting).  Directly changing origin will not set internal links correctly, so clipping would be messed up.  This should be called when an object is spawned, and then only if it is teleported.

setorigin (entity, origin)
=================
*/
static bool PF_setorigin (qcvm_t *vm)
{
	edict_t	*e;
	
	e = PF_ArgEdict(vm, 0);
	QC_ArgVector (vm, 1, e->v.origin);
	SV_LinkEdict (e, false);
	return true;
}


/*
=================
PF_setsize

the size box is rotated by the current angle

setsize (entity, minvector, maxvector)
=================
*/
static bool PF_setsize (qcvm_t *vm)
{
	edict_t	*e;
	vec3_t	min, max;
	
	e = PF_ArgEdict(vm, 0);
	QC_ArgVector (vm, 1, min);
	QC_ArgVector (vm, 2, max);
	VectorCopy (min, e->v.mins);
	VectorCopy (max, e->v.maxs);
	VectorSubtract (max, min, e->v.size);
	SV_LinkEdict (e, false);
	return true;
}


/*
=================
PF_setmodel

setmodel(entity, model)
Also sets size, mins, and maxs for inline bmodels; for NetQuake's progs, as
NetQuake does, for the others too: +-16, NetQuake's box for every alias
model, as FTE's server sets it without the models (and none without a model)
=================
*/
static bool PF_setmodel (qcvm_t *vm)
{
	edict_t		*e;
	const char	*m;
	char		**check;
	int			i;
	cmodel_t	*mod;

	e = PF_ArgEdict(vm, 0);
	m = QC_ArgString(vm, 1);

// check to see if model was properly precached
	for (i=0, check = sv.model_precache ; *check ; i++, check++)
		if (!strcmp(*check, m))
			break;

	if (!*check)
		return QC_Error (vm, "no precache: %s", m);
		
	e->v.model = (string_t)QC_ArgWord(vm, 1);
	e->v.modelindex = (float)i;

// if it is an inline model, get the size information for it
	if (m[0] == '*')
	{
		mod = CM_InlineModel (sv.map, (char *)m);
		if (!mod)
			return QC_Error (vm, "no inline model %s", m);
		VectorCopy (mod->mins, e->v.mins);
		VectorCopy (mod->maxs, e->v.maxs);
		VectorSubtract (mod->maxs, mod->mins, e->v.size);
		SV_LinkEdict (e, false);
	}
	else if (pr.nq && sv.models[i])
	{	// NetQuake's: a brush model's box (QuakeWorld's progs size their own)
		VectorCopy (sv.models[i]->mins, e->v.mins);
		VectorCopy (sv.models[i]->maxs, e->v.maxs);
		VectorSubtract (e->v.maxs, e->v.mins, e->v.size);
		SV_LinkEdict (e, false);
	}
	else if (pr.nq)
	{	// no model: no box
		for (i=0 ; i<3 ; i++)
		{
			e->v.mins[i] = *m ? -16.0f : 0;
			e->v.maxs[i] = *m ? 16.0f : 0;
		}
		VectorSubtract (e->v.maxs, e->v.mins, e->v.size);
		SV_LinkEdict (e, false);
	}
	return true;
}

/*
=================
PF_bprint

broadcast print to everyone on server

bprint(level, value), or NetQuake's bprint(value)
=================
*/
static bool PF_bprint (qcvm_t *vm)
{
	char		*s;
	int			level;

	level = pr.nq ? PRINT_HIGH : PF_ArgTrunc(vm, 0);

	s = PF_VarString(vm, pr.nq ? 0 : 1);
	SV_BroadcastPrintf (level, "%s", s);
	return true;
}

/*
=================
PF_sprint

single print to a specific client

sprint(clientent, level, value), or NetQuake's sprint(clientent, value)
=================
*/
static bool PF_sprint (qcvm_t *vm)
{
	char		*s;
	client_t	*client;
	int			entnum;
	int			level;

	entnum = PF_ArgEdictNum(vm, 0);
	level = pr.nq ? PRINT_HIGH : PF_ArgTrunc(vm, 1);

	s = PF_VarString(vm, pr.nq ? 1 : 2);
	
	if (entnum < 1 || entnum > MAX_CLIENTS)
	{
		Con_Printf ("tried to sprint to a non-client\n");
		return true;
	}
		
	client = &svs.clients[entnum-1];
	
	SV_ClientPrintf (client, level, "%s", s);
	return true;
}


/*
=================
PF_centerprint

single print to a specific client

centerprint(clientent, value)
=================
*/
static bool PF_centerprint (qcvm_t *vm)
{
	char		*s;
	int			entnum;
	client_t	*cl;
	
	entnum = PF_ArgEdictNum(vm, 0);
	s = PF_VarString(vm, 1);
	
	if (entnum < 1 || entnum > MAX_CLIENTS)
	{
		Con_Printf ("tried to sprint to a non-client\n");
		return true;
	}
		
	cl = &svs.clients[entnum-1];

	ClientReliableWrite_Begin (cl, svc_centerprint, 2 + (int)strlen(s));
	ClientReliableWrite_String (cl, s);
	if (sv_mvd)
	{
		sizebuf_t	*msg = SV_MVDMessage ();

		MSG_WriteByte (msg, svc_centerprint);
		MSG_WriteString (msg, s);
		SV_MVDSingle (cl, msg->data, msg->cursize);
	}
	return true;
}


/*
=================
PF_ambientsound

=================
*/
static bool PF_ambientsound (qcvm_t *vm)
{
	char		**check;
	const char	*samp;
	vec3_t		pos;
	float 		vol, attenuation;
	int			i, soundnum;
	staticsound_t	*s;

	QC_ArgVector (vm, 0, pos);
	samp = QC_ArgString(vm, 1);
	vol = QC_ArgFloat(vm, 2);
	attenuation = QC_ArgFloat(vm, 3);
	
// check to see if samp was properly precached
	for (soundnum=0, check = sv.sound_precache ; *check ; check++, soundnum++)
		if (!strcmp(*check,samp))
			break;
			
	if (!*check)
	{
		Con_Printf ("no precache: %s\n", samp);
		return true;
	}
	if (soundnum >= MAX_QW_SOUNDS)
	{	// past svc_spawnstaticsound's byte: kept for the clients that read more
		s = SV_NewStaticSound ();
		VectorCopy (pos, s->origin);
		s->sound = soundnum;
		s->volume = QC_FloatToInt (vol*255);
		s->attenuation = QC_FloatToInt (attenuation*64);
		return true;
	}

// add an svc_spawnambient command to the level signon packet

	MSG_WriteByte (&sv.signon,svc_spawnstaticsound);
	for (i=0 ; i<3 ; i++)
		MSG_WriteCoord(&sv.signon, pos[i]);

	MSG_WriteByte (&sv.signon, soundnum);

	MSG_WriteByte (&sv.signon, QC_FloatToInt (vol*255));
	MSG_WriteByte (&sv.signon, QC_FloatToInt (attenuation*64));
	return true;
}

/*
=================
PF_sound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
allready running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.

=================
*/
static bool PF_sound (qcvm_t *vm)
{
	const char	*sample;
	int			channel;
	edict_t		*entity;
	int 		volume;
	float attenuation;
		
	entity = PF_ArgEdict(vm, 0);
	channel = PF_ArgTrunc(vm, 1);
	sample = QC_ArgString(vm, 2);
	volume = QC_FloatToInt (QC_ArgFloat(vm, 3) * 255);
	attenuation = QC_ArgFloat(vm, 4);

	if (volume < 0 || volume > 255)
		return QC_Error (vm, "sound: volume = %i", volume);
	if (!(attenuation >= 0 && attenuation <= 4))
		return QC_Error (vm, "sound: attenuation = %f", attenuation);
	if (channel < 0 || channel > 15)
		return QC_Error (vm, "sound: channel = %i", channel);
	
	SV_StartSound (entity, channel, sample, volume, attenuation);
	return true;
}

/*
=================
PF_break

break()
=================
*/
static bool PF_break (qcvm_t *vm)
{
	(void)vm;
	Con_Printf ("break statement\n");
	return true;
}

/*
=================
PF_traceline

Used for use tracing and shot targeting
Traces are blocked by bbox and exact bsp entityes, and also slide box entities
if the tryents flag is set.

traceline (vector1, vector2, tryents)
=================
*/
static bool PF_traceline (qcvm_t *vm)
{
	vec3_t	v1, v2;
	trace_t	trace;
	int		nomonsters;
	edict_t	*ent;

	QC_ArgVector (vm, 0, v1);
	QC_ArgVector (vm, 1, v2);
	nomonsters = PF_ArgTrunc(vm, 2);
	ent = PF_ArgEdict(vm, 3);

	trace = SV_Move (v1, vec3_origin, vec3_origin, v2, nomonsters, ent);

	PR_GLOBAL(trace_allsolid) = trace.allsolid;
	PR_GLOBAL(trace_startsolid) = trace.startsolid;
	PR_GLOBAL(trace_fraction) = trace.fraction;
	PR_GLOBAL(trace_inwater) = trace.inwater;
	PR_GLOBAL(trace_inopen) = trace.inopen;
	VectorCopy (trace.endpos, PR_GLOBAL(trace_endpos));
	VectorCopy (trace.plane.normal, PR_GLOBAL(trace_plane_normal));
	PR_GLOBAL(trace_plane_dist) =  trace.plane.dist;	
	if (trace.ent)
		PR_GLOBAL(trace_ent) = EDICT_TO_PROG(trace.ent);
	else
		PR_GLOBAL(trace_ent) = EDICT_TO_PROG(sv.edicts);
	return true;
}

//============================================================================

static int PF_newcheckclient (int check)
{
	int		i;
	edict_t	*ent;
	vec3_t	org;

// cycle to the next one

	if (check < 1)
		check = 1;
	if (check > MAX_CLIENTS)
		check = MAX_CLIENTS;

	if (check == MAX_CLIENTS)
		i = 1;
	else
		i = check + 1;

	for ( ;  ; i++)
	{
		if (i == MAX_CLIENTS+1)
			i = 1;

		ent = EDICT_NUM(i);

		if (i == check)
			break;	// didn't find anything else

		if (ent->free)
			continue;
		if (ent->v.health <= 0)
			continue;
		if ((int)ent->v.flags & FL_NOTARGET)
			continue;

	// anything that is a client, or has a client as an enemy
		break;
	}

// get the PVS for the entity
	VectorAdd (ent->v.origin, ent->v.view_ofs, org);
	memcpy (sv.checkpvs, CM_LeafPVS (sv.map, CM_Leafnum (sv.map, CM_PointInLeaf (sv.map, org))), (size_t)sv.vis_rowbytes);

	return i;
}

/*
=================
PF_checkclient

Returns a client (or object that has a client enemy) that would be a
valid target.

If there are more than one valid options, they are cycled each frame

If (self.origin + self.viewofs) is not in the PVS of the current target,
it is not returned at all.

name checkclient (void)
=================
*/
#define	MAX_CHECK	16
static bool PF_checkclient (qcvm_t *vm)
{
	edict_t	*ent, *self;
	int		l;
	vec3_t	view;
	
// find a new check if on a new frame
	if (sv.time - sv.lastchecktime >= 0.1)
	{
		sv.lastcheck = PF_newcheckclient (sv.lastcheck);
		sv.lastchecktime = sv.time;
	}

// return check if it might be visible	
	ent = EDICT_NUM(sv.lastcheck);
	if (ent->free || ent->v.health <= 0)
	{
		PF_ReturnEdict (vm, sv.edicts);
		return true;
	}

// if current entity can't possibly see the check entity, return 0
	self = PROG_TO_EDICT(PR_GLOBAL(self));
	VectorAdd (self->v.origin, self->v.view_ofs, view);
	l = CM_Leafnum (sv.map, CM_PointInLeaf (sv.map, view)) - 1;
	if ( (l<0) || !(sv.checkpvs[l>>3] & (1<<(l&7)) ) )
	{
		PF_ReturnEdict (vm, sv.edicts);
		return true;
	}

// might be able to see it
	PF_ReturnEdict (vm, ent);
	return true;
}

//============================================================================


/*
=================
PF_stuffcmd

Sends text over to the client's execution buffer

stuffcmd (clientent, value)
=================
*/
static bool PF_stuffcmd (qcvm_t *vm)
{
	int			entnum;
	const char	*str;
	client_t	*cl;
	
	entnum = PF_ArgEdictNum(vm, 0);
	if (entnum < 1 || entnum > MAX_CLIENTS)
		return QC_Error (vm, "Parm 0 not a client");
	str = QC_ArgString(vm, 1);	
	
	cl = &svs.clients[entnum-1];

	if (strcmp(str, "disconnect\n") == 0) {
		// so long and thanks for all the fish
		cl->drop = true;
		return true;
	}

	ClientReliableWrite_Begin (cl, svc_stufftext, 2+(int)strlen(str));
	ClientReliableWrite_String (cl, str);
	if (sv_mvd)
	{
		sizebuf_t	*msg = SV_MVDMessage ();

		MSG_WriteByte (msg, svc_stufftext);
		MSG_WriteString (msg, str);
		SV_MVDSingle (cl, msg->data, msg->cursize);
	}
	return true;
}

static bool PR_CheckEmptyString (qcvm_t *vm, const char *s)
{
	if (s[0] <= ' ')
		return QC_Error (vm, "Bad string");
	return true;
}

static bool PF_precache_file (qcvm_t *vm)
{	// precache_file is only used to copy files with qcc, it does nothing
	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	return true;
}

static bool PF_precache_sound (qcvm_t *vm)
{
	const char	*s;
	int			i;
	
	if (sv.state != ss_loading)
		return QC_Error (vm, "PF_Precache_*: Precache can only be done in spawn functions");
		
	s = QC_ArgString(vm, 0);
	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	if (!PR_CheckEmptyString (vm, s))
		return false;
	
	for (i=0 ; i<MAX_SOUNDS-1 ; i++)		// the last stays NULL, ending the list
	{
		if (!sv.sound_precache[i])
		{
			sv.sound_precache[i] = SV_LevelString (s);
			return true;
		}
		if (!strcmp(sv.sound_precache[i], s))
			return true;
	}
	return QC_Error (vm, "PF_precache_sound: overflow");
}

static bool PF_precache_model (qcvm_t *vm)
{
	const char	*s;
	int			i;
	
	if (sv.state != ss_loading)
		return QC_Error (vm, "PF_Precache_*: Precache can only be done in spawn functions");
		
	s = QC_ArgString(vm, 0);
	QC_ReturnWord (vm, QC_ArgWord (vm, 0));
	if (!PR_CheckEmptyString (vm, s))
		return false;

	for (i=0 ; i<MAX_MODELS-1 ; i++)		// the last stays NULL, ending the list
	{
		if (!sv.model_precache[i])
		{
			sv.model_precache[i] = SV_LevelString (s);
			SV_LoadBrushModel (i);
			return true;
		}
		if (!strcmp(sv.model_precache[i], s))
			return true;
	}
	return QC_Error (vm, "PF_precache_model: overflow");
}


/*
===============
PF_walkmove

float(float yaw, float dist) walkmove
===============
*/
static bool PF_walkmove (qcvm_t *vm)
{
	edict_t	*ent;
	float	yaw, dist;
	vec3_t	move;
	int 	oldself;
	bool	moved;
	
	ent = PROG_TO_EDICT(PR_GLOBAL(self));
	yaw = QC_ArgFloat(vm, 0);
	dist = QC_ArgFloat(vm, 1);
	
	if ( !( (int)ent->v.flags & (FL_ONGROUND|FL_FLY|FL_SWIM) ) )
	{
		QC_ReturnFloat (vm, 0);
		return true;
	}

	yaw = (float)(yaw*Q_PI*2 / 360);
	
	move[0] = cosf(yaw)*dist;
	move[1] = sinf(yaw)*dist;
	move[2] = 0;

// save self, because SV_movestep may call other progs
	oldself = PR_GLOBAL(self);
	
	moved = SV_movestep(ent, move, true);
	
// restore self
	PR_GLOBAL(self) = oldself;
	QC_ReturnFloat (vm, moved);
	return true;
}

/*
===============
PF_droptofloor

void() droptofloor
===============
*/
static bool PF_droptofloor (qcvm_t *vm)
{
	edict_t		*ent;
	vec3_t		end;
	trace_t		trace;
	
	ent = PROG_TO_EDICT(PR_GLOBAL(self));

	VectorCopy (ent->v.origin, end);
	end[2] -= 256;
	
	trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, false, ent);

	if (trace.fraction == 1 || trace.allsolid)
		QC_ReturnFloat (vm, 0);
	else
	{
		VectorCopy (trace.endpos, ent->v.origin);
		SV_LinkEdict (ent, false);
		ent->v.flags = (float)((int)ent->v.flags | FL_ONGROUND);
		ent->v.groundentity = EDICT_TO_PROG(trace.ent);
		QC_ReturnFloat (vm, 1);
	}
	return true;
}

/*
===============
PF_lightstyle

void(float style, string value) lightstyle

The styles are the server's copies, for QuakeC may change them every frame
===============
*/
static char	*pr_lightstyles[MAX_LIGHTSTYLES];

void PR_ClearLightstyles (void)
{
	int		i;

	for (i = 0 ; i < MAX_LIGHTSTYLES ; i++)
	{
		Mem_Free (pr_lightstyles[i]);
		pr_lightstyles[i] = NULL;
	}
}

static bool PF_lightstyle (qcvm_t *vm)
{
	int			style;
	const char	*val;
	client_t	*client;
	int			j;
	size_t		len;
	
	style = PF_ArgTrunc(vm, 0);
	val = QC_ArgString(vm, 1);
	if (style < 0 || style >= MAX_LIGHTSTYLES)
	{
		QC_Warning (vm, "lightstyle: bad style %i", style);
		return true;
	}

// change the string in sv
	len = strlen (val) + 1;
	Mem_Free (pr_lightstyles[style]);
	pr_lightstyles[style] = Mem_Alloc (len);
	memcpy (pr_lightstyles[style], val, len);
	sv.lightstyles[style] = pr_lightstyles[style];
	
// send message to all clients on this server
	if (sv.state != ss_active)
		return true;
	
	for (j=0, client = svs.clients ; j<MAX_CLIENTS ; j++, client++)
		if ( client->state == cs_spawned )
		{
			ClientReliableWrite_Begin (client, svc_lightstyle, (int)len+2);
			ClientReliableWrite_Char (client, style);
			ClientReliableWrite_String (client, pr_lightstyles[style]);
		}
	if (sv_mvd)
	{
		sizebuf_t	*msg = SV_MVDMessage ();

		MSG_WriteByte (msg, svc_lightstyle);
		MSG_WriteByte (msg, style);
		MSG_WriteString (msg, pr_lightstyles[style]);
		SV_MVDAll (msg->data, msg->cursize);
	}
	return true;
}

/*
=============
PF_checkbottom
=============
*/
static bool PF_checkbottom (qcvm_t *vm)
{
	QC_ReturnFloat (vm, SV_CheckBottom (PF_ArgEdict(vm, 0)));
	return true;
}

/*
=============
PF_pointcontents
=============
*/
static bool PF_pointcontents (qcvm_t *vm)
{
	vec3_t	v;
	
	QC_ArgVector (vm, 0, v);
	QC_ReturnFloat (vm, (float)SV_PointContents (v));
	return true;
}

/*
=============
PF_aim

Pick a vector for the player to shoot along
vector aim(entity, missilespeed)
=============
*/
//cvar_t	sv_aim = {.name = "sv_aim", .string = "0.93"};
cvar_t	sv_aim = {.name = "sv_aim", .string = "2",
	.description = "Autoaim: the cosine of the widest angle off the view a target is aimed at; above 1 turns it off. "
		"A player's noaim userinfo key opts out."};
static bool PF_aim (qcvm_t *vm)
{
	edict_t	*ent, *check, *bestent;
	vec3_t	start, dir, end, bestdir;
	int		i, j;
	trace_t	tr;
	float	dist, bestdist;
	char	*noaim;

	ent = PF_ArgEdict(vm, 0);

	VectorCopy (ent->v.origin, start);
	start[2] += 20;

// noaim option
	i = NUM_FOR_EDICT(ent);
	if (i>0 && i<MAX_CLIENTS)
	{
		noaim = Info_ValueForKey (svs.clients[i-1].userinfo, "noaim");
		if (atoi(noaim) > 0)
		{
			QC_ReturnVector (vm, PR_GLOBAL(v_forward));
			return true;
		}
	}

// try sending a trace straight
	VectorCopy (PR_GLOBAL(v_forward), dir);
	VectorMA (start, 2048, dir, end);
	tr = SV_Move (start, vec3_origin, vec3_origin, end, false, ent);
	if (tr.ent && tr.ent->v.takedamage == DAMAGE_AIM
	&& (!teamplay.value || ent->v.team <=0 || ent->v.team != tr.ent->v.team) )
	{
		QC_ReturnVector (vm, PR_GLOBAL(v_forward));
		return true;
	}


// try all possible entities
	VectorCopy (dir, bestdir);
	bestdist = sv_aim.value;
	bestent = NULL;
	
	check = NEXT_EDICT(sv.edicts);
	for (i=1 ; i<sv.num_edicts ; i++, check = NEXT_EDICT(check) )
	{
		if (check->v.takedamage != DAMAGE_AIM)
			continue;
		if (check == ent)
			continue;
		if (teamplay.value && ent->v.team > 0 && ent->v.team == check->v.team)
			continue;	// don't aim at teammate
		for (j=0 ; j<3 ; j++)
			end[j] = check->v.origin[j]
			+ 0.5f*(check->v.mins[j] + check->v.maxs[j]);
		VectorSubtract (end, start, dir);
		VectorNormalize (dir);
		dist = DotProduct (dir, PR_GLOBAL(v_forward));
		if (dist < bestdist)
			continue;	// to far to turn
		tr = SV_Move (start, vec3_origin, vec3_origin, end, false, ent);
		if (tr.ent == check)
		{	// can shoot at this one
			bestdist = dist;
			bestent = check;
		}
	}
	
	if (bestent)
	{
		VectorSubtract (bestent->v.origin, ent->v.origin, dir);
		dist = DotProduct (dir, PR_GLOBAL(v_forward));
		VectorScale (PR_GLOBAL(v_forward), dist, end);
		end[2] = dir[2];
		VectorNormalize (end);
		QC_ReturnVector (vm, end);
	}
	else
	{
		QC_ReturnVector (vm, bestdir);
	}
	return true;
}

/*
==============
SV_ChangeYaw

Turns an entity toward its ideal_yaw, at most yaw_speed
This was a major timewaster in progs, so it was converted to C
==============
*/
void SV_ChangeYaw (edict_t *ent)
{
	float		ideal, current, move, speed;
	
	current = anglemod( ent->v.angles[1] );
	ideal = ent->v.ideal_yaw;
	speed = ent->v.yaw_speed;
	
	if (current == ideal)
		return;
	move = ideal - current;
	if (ideal > current)
	{
		if (move >= 180)
			move = move - 360;
	}
	else
	{
		if (move <= -180)
			move = move + 360;
	}
	if (move > 0)
	{
		if (move > speed)
			move = speed;
	}
	else
	{
		if (move < -speed)
			move = -speed;
	}
	
	ent->v.angles[1] = anglemod (current + move);
}

/*
===============================================================================

MESSAGE WRITING

===============================================================================
*/

// the buffer to write to, or NULL after QC_Error
static sizebuf_t *WriteDest (qcvm_t *vm)
{
	int		dest;

	dest = PF_ArgTrunc(vm, 0);
	switch (dest)
	{
	case MSG_BROADCAST:
		return &sv.datagram;
	
	case MSG_ALL:
		return &sv.reliable_datagram;
	
	case MSG_INIT:
		if (sv.state != ss_loading)
		{
			QC_Error (vm, "PF_Write_*: MSG_INIT can only be written in spawn functions");
			return NULL;
		}
		return &sv.signon;

	case MSG_MULTICAST:
		return &sv.multicast;

	default:
		QC_Error (vm, "WriteDest: bad destination");
		return NULL;
	}
}

// the client of msg_entity, or NULL after QC_Error
static client_t *Write_GetClient (qcvm_t *vm)
{
	int		entnum;
	edict_t	*ent;

	ent = PROG_TO_EDICT(PR_GLOBAL(msg_entity));
	entnum = NUM_FOR_EDICT(ent);
	if (entnum < 1 || entnum > MAX_CLIENTS)
	{
		QC_Error (vm, "WriteDest: not a client");
		return NULL;
	}
	return &svs.clients[entnum-1];
}

// where a Write* builtin writes: to a buffer, or for msg_entity's client
// (MSG_ONE, *one) a message of its own, which PF_WriteDone gives to the
// client's reliable stream and to its view on QTV; NULL after QC_Error
static sizebuf_t *PF_WriteTo (qcvm_t *vm, client_t **one)
{
	static byte			data[MAX_MSGLEN];
	static sizebuf_t	msg;

	*one = NULL;
	if (QC_ArgFloat(vm, 0) != MSG_ONE)
		return WriteDest (vm);
	if (!(*one = Write_GetClient (vm)))
		return NULL;
	msg = (sizebuf_t){.data = data, .maxsize = sizeof(data), .allowoverflow = true,
		.floatcoords = (*one)->netchan.message.floatcoords};
	return &msg;
}

static bool PF_WriteDone (client_t *one, sizebuf_t *msg)
{
	if (one)
	{
		ClientReliableCheckBlock (one, msg->cursize);
		ClientReliableWrite_SZ (one, msg->data, msg->cursize);
		SV_MVDSingle (one, msg->data, msg->cursize);
	}
	return true;
}

// whether a write is a NetQuake progs' (to NetQuake's destinations, all but
// multicast's), which sv_nqmsg.c writes as QuakeWorld's
static bool PF_WriteIsNQ (qcvm_t *vm)
{
	return pr.nq && PF_ArgTrunc(vm, 0) != MSG_MULTICAST;
}

// a NetQuake progs' write, to sv_nqmsg.c; false after QC_Error
static bool PF_WriteNQ (qcvm_t *vm, nqwrite_t kind, float value, const char *string)
{
	int			dest = PF_ArgTrunc(vm, 0);
	client_t	*one = NULL;

	if (dest < MSG_BROADCAST || dest > MSG_INIT)
		return QC_Error (vm, "WriteDest: bad destination");
	if (dest == MSG_INIT && sv.state != ss_loading)
		return QC_Error (vm, "PF_Write_*: MSG_INIT can only be written in spawn functions");
	if (dest == MSG_ONE && !(one = Write_GetClient (vm)))
		return false;
	SV_NQWrite (dest, one, kind, value, string);
	return true;
}

static bool PF_WriteByte (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_BYTE, (float)PF_ArgTrunc(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteByte (sb, PF_ArgTrunc(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteChar (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_CHAR, (float)PF_ArgTrunc(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteChar (sb, PF_ArgTrunc(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteShort (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_SHORT, (float)PF_ArgTrunc(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteShort (sb, PF_ArgTrunc(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteLong (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_LONG, (float)PF_ArgTrunc(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteLong (sb, PF_ArgTrunc(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteAngle (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_ANGLE, QC_ArgFloat(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteAngle (sb, QC_ArgFloat(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteCoord (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_COORD, QC_ArgFloat(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteCoord (sb, QC_ArgFloat(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteString (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_STRING, 0, QC_ArgString(vm, 1));
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteString (sb, QC_ArgString(vm, 1));
	return PF_WriteDone (one, sb);
}

static bool PF_WriteEntity (qcvm_t *vm)
{
	client_t	*one;
	sizebuf_t	*sb;

	if (PF_WriteIsNQ (vm))
		return PF_WriteNQ (vm, NQW_ENTITY, (float)PF_ArgEdictNum(vm, 1), NULL);
	if (!(sb = PF_WriteTo (vm, &one)))
		return false;
	MSG_WriteShort (sb, PF_ArgEdictNum(vm, 1));
	return PF_WriteDone (one, sb);
}

//=============================================================================

// kept with the level's static entities, which each client gets at prespawn
static bool PF_makestatic (qcvm_t *vm)
{
	edict_t	*ent;
	entity_state_t	*s;

	ent = PF_ArgEdict(vm, 0);

	s = SV_NewStatic ();
	s->modelindex = SV_ModelIndex(PR_GetString(ent->v.model));
	s->frame = QC_FloatToInt (ent->v.frame);
	s->colormap = QC_FloatToInt (ent->v.colormap);
	s->skinnum = QC_FloatToInt (ent->v.skin);
	VectorCopy (ent->v.origin, s->origin);
	VectorCopy (ent->v.angles, s->angles);
	SV_EntityLook (ent, s);

// throw the entity away now
	ED_Free (ent);
	return true;
}

//=============================================================================

/*
==============
PF_setspawnparms
==============
*/
static bool PF_setspawnparms (qcvm_t *vm)
{
	int		i;
	client_t	*client;

	i = PF_ArgEdictNum(vm, 0);
	if (i < 1 || i > MAX_CLIENTS)
		return QC_Error (vm, "Entity is not a client");

	// copy spawn parms out of the client_t
	client = svs.clients + (i-1);

	for (i=0 ; i< NUM_SPAWN_PARMS ; i++)
		PR_PARM(i) = client->spawn_parms[i];
	return true;
}

/*
==============
PF_changelevel
==============
*/
static bool PF_changelevel (qcvm_t *vm)
{
	static	int	last_spawncount;

// make sure we don't issue two changelevels
	if (svs.spawncount == last_spawncount)
		return true;
	last_spawncount = svs.spawncount;
	
	Cbuf_AddText (va("changelevel %s\n", QC_ArgString(vm, 0)));
	return true;
}


/*
==============
PF_logfrag

logfrag (killer, killee)
==============
*/
static bool PF_logfrag (qcvm_t *vm)
{
	int		e1, e2;
	char	*s;

	e1 = PF_ArgEdictNum(vm, 0);
	e2 = PF_ArgEdictNum(vm, 1);
	
	if (e1 < 1 || e1 > MAX_CLIENTS
	|| e2 < 1 || e2 > MAX_CLIENTS)
		return true;
	
	s = va("\\%s\\%s\\\n",svs.clients[e1-1].name, svs.clients[e2-1].name);

	SZ_Print (&svs.log[svs.logsequence&1], s);
	if (svs.fraglogfile) {
		fprintf (svs.fraglogfile, "%s", s);
		fflush (svs.fraglogfile);
	}
	return true;
}


/*
==============
PF_infokey

string(entity e, string key) infokey
==============
*/
static bool PF_infokey (qcvm_t *vm)
{
	int			e1;
	const char	*value;
	const char	*key;
	char		ov[256];

	e1 = PF_ArgEdictNum(vm, 0);
	key = QC_ArgString(vm, 1);

	if (e1 == 0) {
		if ((value = Info_ValueForKey (svs.info, key)) == NULL ||
			!*value)
			value = Info_ValueForKey(svs.localinfo, key);
	} else if (e1 <= MAX_CLIENTS) {
		if (!strcmp(key, "ip")) {
			Q_strncpyz(ov, NET_BaseAdrToString (svs.clients[e1-1].netchan.remote_address), sizeof(ov));
			value = ov;
		} else if (!strcmp(key, "ping")) {
			int ping = SV_CalcPing (&svs.clients[e1-1]);
			snprintf(ov, sizeof(ov), "%d", ping);
			value = ov;
		} else
			value = Info_ValueForKey (svs.clients[e1-1].userinfo, key);
	} else
		value = "";

	return PF_ReturnString (vm, value);
}

/*
==============
PF_multicast

void(vector where, float set) multicast
==============
*/
static bool PF_multicast (qcvm_t *vm)
{
	vec3_t	o;
	int		to;

	QC_ArgVector (vm, 0, o);
	to = PF_ArgTrunc(vm, 1);
	if (to < MULTICAST_ALL || to > MULTICAST_PVS_R)
		return QC_Error (vm, "multicast: bad to:%i", to);

	SV_Multicast (o, to);
	return true;
}

/*
==============
PF_particle

void(vector org, vector dir, float color, float count) particle: NetQuake's.
QuakeWorld's protocol has no particles, so as FTE's server sends them to its
clients: blood (color 73) and lightning's blood (225) as their temp entities,
the rest not at all
==============
*/
static bool PF_particle (qcvm_t *vm)
{
	vec3_t	org;
	int		color, count;

	QC_ArgVector (vm, 0, org);
	color = PF_ArgTrunc(vm, 2) & 255;
	count = PF_ArgTrunc(vm, 3);
	count = count < 0 ? 0 : count > 255 ? 255 : count;
	if (color != 73 && color != 225)
		return true;

	MSG_WriteByte (&sv.multicast, svc_temp_entity);
	if (color == 73)
	{
		MSG_WriteByte (&sv.multicast, TE_BLOOD);
		MSG_WriteByte (&sv.multicast, count < 10 ? 1 : (count + 10) / 20);
	}
	else
		MSG_WriteByte (&sv.multicast, TE_LIGHTNINGBLOOD);
	MSG_WriteCoord (&sv.multicast, org[0]);
	MSG_WriteCoord (&sv.multicast, org[1]);
	MSG_WriteCoord (&sv.multicast, org[2]);
	SV_Multicast (org, MULTICAST_PVS);
	return true;
}

// stat num follows a word of each client's entity or of the globals, from
// num up (three stats for a vector); false after QC_Error
static bool PF_AddStat (qcvm_t *vm, const char *builtin, int num, int type, bool global, uint32_t ofs)
{
	int		i, n = type == ev_vector ? 3 : 1;

	if (type == ev_vector)
		type = ev_float;
	if (type == ev_string)
	{
		QC_Warning (vm, "%s: string stats aren't sent", builtin);
		return true;
	}
	if (type != ev_float && type != ev_entity && type != QC_EV_INTEGER)
		return QC_Error (vm, "%s: stat type %i", builtin, type);
	if (num < MAX_STATS || num + n > MAX_CL_STATS)
		return QC_Error (vm, "%s: stat %i isn't from %i to %i", builtin, num, MAX_STATS, MAX_CL_STATS - n);
	if (!global && ofs + n > QC_FieldWords (vm))
		return QC_Error (vm, "%s: not a field", builtin);
	for (i = 0 ; i < n ; i++)
	{
		sv.qcstats[num + i].type = (uint32_t)type;
		sv.qcstats[num + i].global = global;
		sv.qcstats[num + i].ofs = ofs + (uint32_t)i;
	}
	return true;
}

/*
==============
PF_clientstat

void(float num, float type, .__variant field) clientstat: FTE's. Stat num
(from 32: id's are below) is the field of each client's entity, as type
(EV_FLOAT, EV_VECTOR as three, EV_ENTITY, EV_INTEGER), for the clients that
run CSQC
==============
*/
static bool PF_clientstat (qcvm_t *vm)
{
	return PF_AddStat (vm, "clientstat", PF_ArgTrunc(vm, 0), PF_ArgTrunc(vm, 1), false, QC_ArgWord(vm, 2));
}

/*
==============
PF_globalstat

void(float num, float type, string global) globalstat: FTE's. Stat num is the
global of the name, for every client
==============
*/
static bool PF_globalstat (qcvm_t *vm)
{
	const char	*name = QC_ArgString(vm, 2);
	uint32_t	word;

	if (!QC_FindGlobal (vm, name, &word, NULL))
	{
		QC_Warning (vm, "globalstat: no global %s", name);
		return true;
	}
	return PF_AddStat (vm, "globalstat", PF_ArgTrunc(vm, 0), PF_ArgTrunc(vm, 1), true, word);
}


// the engine's builtins, by id's numbers, over FTE's standard ones (those that
// need no engine: QC_BuiltinsStandard); numbers neither has fail when called
static const struct
{
	uint32_t		number;
	const char		*name;
	qc_builtin_t	func;
} pr_builtin[] =
{
	{2, "setorigin", PF_setorigin},
	{3, "setmodel", PF_setmodel},
	{4, "setsize", PF_setsize},
	{6, "break", PF_break},
	{8, "sound", PF_sound},
	{10, "error", PF_error},
	{11, "objerror", PF_objerror},
	{16, "traceline", PF_traceline},
	{17, "checkclient", PF_checkclient},
	{19, "precache_sound", PF_precache_sound},
	{20, "precache_model", PF_precache_model},
	{21, "stuffcmd", PF_stuffcmd},
	{23, "bprint", PF_bprint},
	{24, "sprint", PF_sprint},
	{32, "walkmove", PF_walkmove},
	{34, "droptofloor", PF_droptofloor},
	{35, "lightstyle", PF_lightstyle},
	{40, "checkbottom", PF_checkbottom},
	{41, "pointcontents", PF_pointcontents},
	{44, "aim", PF_aim},
	{48, "particle", PF_particle},
	{52, "WriteByte", PF_WriteByte},
	{53, "WriteChar", PF_WriteChar},
	{54, "WriteShort", PF_WriteShort},
	{55, "WriteLong", PF_WriteLong},
	{56, "WriteCoord", PF_WriteCoord},
	{57, "WriteAngle", PF_WriteAngle},
	{58, "WriteString", PF_WriteString},
	{59, "WriteEntity", PF_WriteEntity},
	{67, "movetogoal", SV_MoveToGoal},
	{68, "precache_file", PF_precache_file},
	{69, "makestatic", PF_makestatic},
	{70, "changelevel", PF_changelevel},
	{73, "centerprint", PF_centerprint},
	{74, "ambientsound", PF_ambientsound},
	{75, "precache_model2", PF_precache_model},
	{76, "precache_sound2", PF_precache_sound},		// precache_sound2 is different only for qcc
	{77, "precache_file2", PF_precache_file},
	{78, "setspawnparms", PF_setspawnparms},
	{79, "logfrag", PF_logfrag},
	{80, "infokey", PF_infokey},
	{82, "multicast", PF_multicast},
	{232, "clientstat", PF_clientstat},
	{233, "globalstat", PF_globalstat},
};

void PR_InitBuiltins (qc_builtins_t *b)
{
	size_t	i;

	for (i = 0 ; i < sizeof(pr_builtin)/sizeof(pr_builtin[0]) ; i++)
		if (!QC_BuiltinsSetNumbered (b, pr_builtin[i].number, pr_builtin[i].name, pr_builtin[i].func))
			Sys_Error ("PR_InitBuiltins: out of memory");
}
