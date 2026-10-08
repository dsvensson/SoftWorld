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

#include "pr_comp.h"			// the values QuakeC holds
#include "progdefs.h"			// id's globals and fields
#include "qcvm.h"
#include "cvar.h"
#include "link.h"
#include "mathlib.h"
#include "protocol.h"

// An entity: a block of the VM's entity memory, the server's own part first
// (QuakeC can't reach it), then the progs' fields. Blocks are pr.edict_size
// apart and never move.
#define	MAX_ENT_LEAFS	16
typedef struct edict_s
{
	bool	free;				// the VM's slot is free: the VM knows, this is its copy
	link_t		area;				// linked to a division node or leaf
	
	int			num_leafs;
	int			leafnums[MAX_ENT_LEAFS];	// visibility bit numbers

	entity_state_t	baseline;
	float		alpha;				// FTE's alpha and colormod map keys, for progs
	vec3_t		colormod;			// without such fields
	
	entvars_t	v;					// id's fields, where the server has them
// the progs' other fields come immediately after
} edict_t;
#define	EDICT_FROM_AREA(l) STRUCT_FROM_LINK(l,edict_t,area)

//============================================================================

// the QuakeC virtual machine
typedef struct
{
	qcvm_t			*vm;				// NULL between maps
	qc_progs_t		*progs;
	qc_builtins_t	*builtins;			// the server's, made once
	char			name[MAX_QPATH];	// progs.dat or qwprogs.dat (or sv_progs')
	bool			nq;					// NetQuake's progs (a header CRC but QuakeWorld's)
	globalptrs_t	g;					// id's globals (PR_GLOBAL)
	float			*globals;			// the progs' globals, where the VM keeps them
	int				edict_size;			// in bytes: the step from an entity to the next

	// optional QuakeC fields, as offsets into an edict's fields; 0 without
	int				fofs_alpha;			// float
	int				fofs_colormod;		// vector
	int				fofs_gravity;		// float
	int				fofs_maxspeed;		// float
	int				fofs_teleported;	// int
	int				fofs_teleport_time;	// float

	// optional QuakeC functions
	func_t			SpectatorConnect;
	func_t			SpectatorThink;
	func_t			SpectatorDisconnect;
	func_t			ParseClientCommand;	// SV_ParseClientCommand, KRIMZON_SV_PARSECLIENTCOMMAND's
} pr_state_t;

extern	pr_state_t	pr;

// one of id's globals, the progs' or the server's for a progs without it
#define	PR_GLOBAL(name)	(*pr.g.name)
#define	PR_PARM(i)		(*pr.g.parm[i])


//============================================================================

void PR_Init (void);
void PR_ResetStack (void);	// after an error left QuakeC running

void PR_ExecuteProgram (func_t fnum);
void PR_ExecuteProgramString (func_t fnum, const char *s);	// fnum (s)
void PR_RunThreads (void);
void PR_LoadProgs (void);
void PR_FreeProgs (void);			// the VM gone, until the next map
void PR_ClearLightstyles (void);	// PF_lightstyle's copies

// qw-qc's qwprogs.dat as the program was built with it (qwprogs_data.c, which
// cmake/qcprogs.cmake makes): the game when the game directory has none
extern const unsigned char	sv_qwprogs[];
extern const size_t			sv_qwprogs_size;

// the server's builtins, in the registry the VM binds them from
void PR_InitBuiltins (qc_builtins_t *b);

edict_t *ED_Alloc (void);
void ED_Free (edict_t *ed);

void ED_Print (edict_t *ed);


void ED_LoadFromFile (char *data);


edict_t *EDICT_NUM(int n);
int NUM_FOR_EDICT(edict_t *e);
edict_t *PROG_TO_EDICT (int n);		// out of range: the world, as FTE has it

#define	NEXT_EDICT(e) ((edict_t *)( (byte *)e + pr.edict_size))

// entities are their numbers
#define	EDICT_TO_PROG(e) NUM_FOR_EDICT(e)

//============================================================================

#define	E_FLOAT(e,o) (((float*)&e->v)[o])
#define	E_INT(e,o) (*(int *)&((float*)&e->v)[o])
#define	E_VECTOR(e,o) (&((float*)&e->v)[o])
#define	E_STRING(e,o) (PR_GetString(*(string_t *)&((float*)&e->v)[o]))


void ED_PrintEdicts (void);
void ED_PrintNum (int ent);

//
// strings
//

// the text of a string reference ("" if none), good until QuakeC runs again
const char *PR_GetString (int num);

// a reference to the server's own text, which QuakeC reads where it is
string_t PR_SetString (const char *s);
