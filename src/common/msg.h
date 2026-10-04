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
// msg.h -- sized buffers and network message reading/writing

#include "q_types.h"

typedef struct sizebuf_s
{
	bool	allowoverflow;	// if false, do a Sys_Error
	bool	overflowed;		// set to true if the buffer size failed
	bool	floatcoords;	// FTE_PEXT_FLOATCOORDS: coordinates as floats, angles
							// in 16 bits; for reading, of the buffer being read
	byte	*data;
	int		maxsize;
	int		cursize;
} sizebuf_t;

void	SZ_Clear (sizebuf_t *buf);
void	SZ_Write (sizebuf_t *buf, const void *data, int length);
void	SZ_Print (sizebuf_t *buf, char *data);	// strcats onto the sizebuf

struct usercmd_s;
struct entity_state_s;

extern struct usercmd_s nullcmd;

void	MSG_WriteChar (sizebuf_t *sb, int c);
void	MSG_WriteByte (sizebuf_t *sb, int c);
void	MSG_WriteShort (sizebuf_t *sb, int c);
void	MSG_WriteLong (sizebuf_t *sb, int c);
void	MSG_WriteFloat (sizebuf_t *sb, float f);
void	MSG_WriteString (sizebuf_t *sb, const char *s);
// coordinates and angles in the buffer's encoding; entity and player origins
// as floats with MVD_PEXT1_FLOATCOORDS
void	MSG_WriteCoord (sizebuf_t *sb, float f);
void	MSG_WriteAngle (sizebuf_t *sb, float f);
void	MSG_WriteOrigin (sizebuf_t *sb, float f, unsigned mvdext1);
void	MSG_WriteAngle16 (sizebuf_t *sb, float f);		// whatever the encoding
void	MSG_WriteDeltaUsercmd (sizebuf_t *sb, struct usercmd_s *from, struct usercmd_s *cmd);

// entity deltas (svc_packetentities, FTE's statics and baselines), for the
// FTE and MVD1 protocol extensions in use; the writer's caller makes sure the
// client can take the entity's number and model at all
void	MSG_WriteDeltaEntity (sizebuf_t *sb, const struct entity_state_s *from, const struct entity_state_s *to,
			bool force, unsigned fteext, unsigned mvdext1);
void	MSG_WriteEntityRemove (sizebuf_t *sb, int number);

// reading happens from the buffer given to MSG_BeginReading
extern	int		msg_readcount;
extern	bool	msg_badread;		// set if a read goes beyond end of message

void	MSG_BeginReading (sizebuf_t *buf);
int		MSG_GetReadCount (void);
int		MSG_ReadByte (void);
int		MSG_ReadShort (void);
int		MSG_ReadLong (void);
float	MSG_ReadFloat (void);
char	*MSG_ReadString (void);
char	*MSG_ReadStringLine (void);

float	MSG_ReadCoord (void);
float	MSG_ReadAngle (void);
float	MSG_ReadOrigin (unsigned mvdext1);
float	MSG_ReadAngle16 (void);
void	MSG_ReadDeltaUsercmd (struct usercmd_s *from, struct usercmd_s *cmd);

// after an entity delta's first word, which isn't 0: the rest of its header,
// with its bits in *bits and FTE's in *ext; returns the entity number
int		MSG_ReadEntityHeader (int word, int *bits, int *ext, unsigned fteext);
void	MSG_ReadDeltaEntity (const struct entity_state_s *from, struct entity_state_s *to, int number,
			int bits, int ext, unsigned mvdext1);
