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
// msg.c -- sized buffers and network message reading/writing

#include "msg.h"
#include "mathlib.h"
#include "protocol.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"

#include <string.h>

usercmd_t	nullcmd;		// guaranteed to be zero

static sizebuf_t	*msg_readbuf;
int			msg_readcount;
bool		msg_badread;

void MSG_WriteChar (sizebuf_t *sb, int c)
{
	byte	*buf;
	

	buf = SZ_GetSpace (sb, 1);
	buf[0] = (byte)c;
}

void MSG_WriteByte (sizebuf_t *sb, int c)
{
	byte	*buf;
	

	buf = SZ_GetSpace (sb, 1);
	buf[0] = (byte)c;
}

void MSG_WriteShort (sizebuf_t *sb, int c)
{
	byte	*buf;
	

	buf = SZ_GetSpace (sb, 2);
	buf[0] = c&0xff;
	buf[1] = (byte)(c>>8);
}

void MSG_WriteLong (sizebuf_t *sb, int c)
{
	byte	*buf;
	
	buf = SZ_GetSpace (sb, 4);
	buf[0] = c&0xff;
	buf[1] = (c>>8)&0xff;
	buf[2] = (c>>16)&0xff;
	buf[3] = c>>24;
}

void MSG_WriteFloat (sizebuf_t *sb, float f)
{
	union
	{
		float	f;
		int	l;
	} dat;
	
	
	dat.f = f;
	dat.l = LittleLong (dat.l);
	
	SZ_Write (sb, &dat.l, 4);
}

void MSG_WriteString (sizebuf_t *sb, char *s)
{
	if (!s)
		SZ_Write (sb, "", 1);
	else
		SZ_Write (sb, s, Q_strlen(s)+1);
}

void MSG_WriteCoord (sizebuf_t *sb, float f)
{
	if (sb->floatcoords)
		MSG_WriteFloat (sb, f);
	else
		MSG_WriteShort (sb, (int)(f*8));
}

void MSG_WriteAngle (sizebuf_t *sb, float f)
{
	if (sb->floatcoords)
		MSG_WriteAngle16 (sb, f);
	else
		MSG_WriteByte (sb, (int)(f*256/360) & 255);
}

void MSG_WriteAngle16 (sizebuf_t *sb, float f)
{
	MSG_WriteShort (sb, (int)(f*65536/360) & 65535);
}

void MSG_WriteDeltaUsercmd (sizebuf_t *buf, usercmd_t *from, usercmd_t *cmd)
{
	int		bits;

//
// send the movement message
//
	bits = 0;
	if (cmd->angles[0] != from->angles[0])
		bits |= CM_ANGLE1;
	if (cmd->angles[1] != from->angles[1])
		bits |= CM_ANGLE2;
	if (cmd->angles[2] != from->angles[2])
		bits |= CM_ANGLE3;
	if (cmd->forwardmove != from->forwardmove)
		bits |= CM_FORWARD;
	if (cmd->sidemove != from->sidemove)
		bits |= CM_SIDE;
	if (cmd->upmove != from->upmove)
		bits |= CM_UP;
	if (cmd->buttons != from->buttons)
		bits |= CM_BUTTONS;
	if (cmd->impulse != from->impulse)
		bits |= CM_IMPULSE;

    MSG_WriteByte (buf, bits);

	if (bits & CM_ANGLE1)
		MSG_WriteAngle16 (buf, cmd->angles[0]);
	if (bits & CM_ANGLE2)
		MSG_WriteAngle16 (buf, cmd->angles[1]);
	if (bits & CM_ANGLE3)
		MSG_WriteAngle16 (buf, cmd->angles[2]);
	
	if (bits & CM_FORWARD)
		MSG_WriteShort (buf, cmd->forwardmove);
	if (bits & CM_SIDE)
	  	MSG_WriteShort (buf, cmd->sidemove);
	if (bits & CM_UP)
		MSG_WriteShort (buf, cmd->upmove);

 	if (bits & CM_BUTTONS)
	  	MSG_WriteByte (buf, cmd->buttons);
 	if (bits & CM_IMPULSE)
	    MSG_WriteByte (buf, cmd->impulse);
	MSG_WriteByte (buf, cmd->msec);
}

void MSG_WriteOrigin (sizebuf_t *sb, float f, unsigned mvdext1)
{
	if (mvdext1 & MVD_PEXT1_FLOATCOORDS)
		MSG_WriteFloat (sb, f);
	else
		MSG_WriteCoord (sb, f);
}

/*
==================
MSG_WriteDeltaEntity

The fields of an entity that differ from a state the client has: its
baseline, the state it last acknowledged, or nothing. FTE's extensions widen
the entity and model numbers and add alpha and color; MVD_PEXT1_FLOATCOORDS
sends the origin as floats.
==================
*/
void MSG_WriteDeltaEntity (sizebuf_t *sb, const entity_state_t *from, const entity_state_t *to,
	bool force, unsigned fteext, unsigned mvdext1)
{
	int		bits, ext, i;
	float	miss;

	bits = ext = 0;
	for (i=0 ; i<3 ; i++)
	{
		miss = to->origin[i] - from->origin[i];
		if (miss < -0.1f || miss > 0.1f)
			bits |= U_ORIGIN1<<i;
	}
	if (to->angles[0] != from->angles[0])
		bits |= U_ANGLE1;
	if (to->angles[1] != from->angles[1])
		bits |= U_ANGLE2;
	if (to->angles[2] != from->angles[2])
		bits |= U_ANGLE3;
	if (to->colormap != from->colormap)
		bits |= U_COLORMAP;
	if (to->skinnum != from->skinnum)
		bits |= U_SKIN;
	if (to->frame != from->frame)
		bits |= U_FRAME;
	if (to->effects != from->effects)
		bits |= U_EFFECTS;
	if (to->modelindex != from->modelindex)
	{
		bits |= U_MODEL;
		if (to->modelindex > 255)
		{
			ext |= U_FTE_MODELDBL;
			if (to->modelindex > 511)
				bits &= ~U_MODEL;		// the whole number as a short instead
		}
	}
	if (to->alpha != from->alpha && (fteext & FTE_PEXT_TRANS))
		ext |= U_FTE_TRANS;
	if (memcmp (to->colormod, from->colormod, sizeof(to->colormod)) && (fteext & FTE_PEXT_COLOURMOD))
		ext |= U_FTE_COLOURMOD;

	if (bits & 511)
		bits |= U_MOREBITS;
	if (to->flags & U_SOLID)
		bits |= U_SOLID;
	if (!bits && !ext && !force)
		return;		// nothing to send

	if (!to->number || to->number >= MAX_EDICTS)
		Sys_Error ("MSG_WriteDeltaEntity: entity number %i", to->number);
	if (to->number & 512)
		ext |= U_FTE_ENTITYDBL;
	if (to->number & 1024)
		ext |= U_FTE_ENTITYDBL2;
	if (ext & 0xff00)
		ext |= U_FTE_YETMORE;
	if (ext & 0xff)
		bits |= U_EVENMORE | U_MOREBITS;

	MSG_WriteShort (sb, (to->number & 511) | (bits & ~511));
	if (bits & U_MOREBITS)
		MSG_WriteByte (sb, bits & 255);
	if (bits & U_EVENMORE)
		MSG_WriteByte (sb, ext & 255);
	if (ext & U_FTE_YETMORE)
		MSG_WriteByte (sb, ext >> 8);

	if (bits & U_MODEL)
		MSG_WriteByte (sb, to->modelindex & 255);
	else if (ext & U_FTE_MODELDBL)
		MSG_WriteShort (sb, to->modelindex);
	if (bits & U_FRAME)
		MSG_WriteByte (sb, to->frame);
	if (bits & U_COLORMAP)
		MSG_WriteByte (sb, to->colormap);
	if (bits & U_SKIN)
		MSG_WriteByte (sb, to->skinnum);
	if (bits & U_EFFECTS)
		MSG_WriteByte (sb, to->effects);
	if (bits & U_ORIGIN1)
		MSG_WriteOrigin (sb, to->origin[0], mvdext1);
	if (bits & U_ANGLE1)
		MSG_WriteAngle (sb, to->angles[0]);
	if (bits & U_ORIGIN2)
		MSG_WriteOrigin (sb, to->origin[1], mvdext1);
	if (bits & U_ANGLE2)
		MSG_WriteAngle (sb, to->angles[1]);
	if (bits & U_ORIGIN3)
		MSG_WriteOrigin (sb, to->origin[2], mvdext1);
	if (bits & U_ANGLE3)
		MSG_WriteAngle (sb, to->angles[2]);

	// opaque goes out as 255, which every reader takes as opaque; FTE reads 0
	// as invisible
	if (ext & U_FTE_TRANS)
		MSG_WriteByte (sb, to->alpha ? to->alpha : 255);
	if (ext & U_FTE_COLOURMOD)
	{
		for (i=0 ; i<3 ; i++)
			MSG_WriteByte (sb, to->colormod[0] | to->colormod[1] | to->colormod[2] ? to->colormod[i] : 32);
	}
}

/*
==================
MSG_WriteEntityRemove

An entity that left the client's view
==================
*/
void MSG_WriteEntityRemove (sizebuf_t *sb, int number)
{
	int		ext;

	ext = 0;
	if (number & 512)
		ext |= U_FTE_ENTITYDBL;
	if (number & 1024)
		ext |= U_FTE_ENTITYDBL2;
	if (!ext)
	{
		MSG_WriteShort (sb, number | U_REMOVE);
		return;
	}
	MSG_WriteShort (sb, (number & 511) | U_REMOVE | U_MOREBITS);
	MSG_WriteByte (sb, U_EVENMORE);
	MSG_WriteByte (sb, ext);
}

void MSG_BeginReading (sizebuf_t *buf)
{
	msg_readbuf = buf;
	msg_readcount = 0;
	msg_badread = false;
}

int MSG_GetReadCount(void)
{
	return msg_readcount;
}

int MSG_ReadChar (void)
{
	int	c;
	
	if (msg_readcount+1 > msg_readbuf->cursize)
	{
		msg_badread = true;
		return -1;
	}
		
	c = (signed char)msg_readbuf->data[msg_readcount];
	msg_readcount++;
	
	return c;
}

int MSG_ReadByte (void)
{
	int	c;
	
	if (msg_readcount+1 > msg_readbuf->cursize)
	{
		msg_badread = true;
		return -1;
	}
		
	c = (unsigned char)msg_readbuf->data[msg_readcount];
	msg_readcount++;
	
	return c;
}

int MSG_ReadShort (void)
{
	int	c;
	
	if (msg_readcount+2 > msg_readbuf->cursize)
	{
		msg_badread = true;
		return -1;
	}
		
	c = (short)(msg_readbuf->data[msg_readcount]
	+ (msg_readbuf->data[msg_readcount+1]<<8));
	
	msg_readcount += 2;
	
	return c;
}

int MSG_ReadLong (void)
{
	int	c;
	
	if (msg_readcount+4 > msg_readbuf->cursize)
	{
		msg_badread = true;
		return -1;
	}
		
	c = msg_readbuf->data[msg_readcount]
	+ (msg_readbuf->data[msg_readcount+1]<<8)
	+ (msg_readbuf->data[msg_readcount+2]<<16)
	+ (msg_readbuf->data[msg_readcount+3]<<24);
	
	msg_readcount += 4;
	
	return c;
}

float MSG_ReadFloat (void)
{
	union
	{
		byte	b[4];
		float	f;
		int	l;
	} dat;
	
	dat.b[0] =	msg_readbuf->data[msg_readcount];
	dat.b[1] =	msg_readbuf->data[msg_readcount+1];
	dat.b[2] =	msg_readbuf->data[msg_readcount+2];
	dat.b[3] =	msg_readbuf->data[msg_readcount+3];
	msg_readcount += 4;
	
	dat.l = LittleLong (dat.l);

	return dat.f;	
}

char *MSG_ReadString (void)
{
	static char	string[2048];
	int		l,c;
	
	l = 0;
	do
	{
		c = MSG_ReadChar ();
		if (c == -1 || c == 0)
			break;
		string[l] = (char)c;
		l++;
	} while (l < (int)sizeof(string)-1);
	
	string[l] = 0;
	
	return string;
}

char *MSG_ReadStringLine (void)
{
	static char	string[2048];
	int		l,c;
	
	l = 0;
	do
	{
		c = MSG_ReadChar ();
		if (c == -1 || c == 0 || c == '\n')
			break;
		string[l] = (char)c;
		l++;
	} while (l < (int)sizeof(string)-1);
	
	string[l] = 0;
	
	return string;
}

float MSG_ReadCoord (void)
{
	if (msg_readbuf->floatcoords)
		return MSG_ReadFloat ();
	return MSG_ReadShort() * (1.0f/8);
}

float MSG_ReadAngle (void)
{
	if (msg_readbuf->floatcoords)
		return MSG_ReadAngle16 ();
	return (float)(MSG_ReadChar() * (360.0/256));
}

float MSG_ReadAngle16 (void)
{
	return (float)(MSG_ReadShort() * (360.0/65536));
}

/*
==================
MSG_ReadEntityHeader

The entity number needs FTE's bits, which follow the flags
==================
*/
int MSG_ReadEntityHeader (int word, int *bits, int *ext, unsigned fteext)
{
	int		number;

	number = word & 511;
	*bits = word & ~511;
	*ext = 0;
	if (*bits & U_MOREBITS)
	{
		*bits |= MSG_ReadByte () & 255;
		if ((*bits & U_EVENMORE) && fteext)
		{
			*ext = MSG_ReadByte () & 255;
			if (*ext & U_FTE_YETMORE)
				*ext |= (MSG_ReadByte () & 255) << 8;
			if (*ext & U_FTE_ENTITYDBL)
				number += 512;
			if (*ext & U_FTE_ENTITYDBL2)
				number += 1024;
		}
	}
	return number;
}

float MSG_ReadOrigin (unsigned mvdext1)
{
	if (mvdext1 & MVD_PEXT1_FLOATCOORDS)
		return MSG_ReadFloat ();
	return MSG_ReadCoord ();
}

/*
==================
MSG_ReadDeltaEntity
==================
*/
void MSG_ReadDeltaEntity (const entity_state_t *from, entity_state_t *to, int number, int bits, int ext,
	unsigned mvdext1)
{
	int		i;

	*to = *from;
	to->number = number;
	to->flags = bits;

	if (bits & U_MODEL)
	{
		to->modelindex = MSG_ReadByte ();
		if (ext & U_FTE_MODELDBL)
			to->modelindex += 256;
	}
	else if (ext & U_FTE_MODELDBL)
		to->modelindex = MSG_ReadShort () & 0xffff;
	if (to->modelindex >= MAX_MODELS)
		to->modelindex = 0;		// past anything a server can precache
	if (bits & U_FRAME)
		to->frame = MSG_ReadByte ();
	if (bits & U_COLORMAP)
		to->colormap = MSG_ReadByte ();
	if (bits & U_SKIN)
		to->skinnum = MSG_ReadByte ();
	if (bits & U_EFFECTS)
		to->effects = MSG_ReadByte ();
	if (bits & U_ORIGIN1)
		to->origin[0] = MSG_ReadOrigin (mvdext1);
	if (bits & U_ANGLE1)
		to->angles[0] = MSG_ReadAngle ();
	if (bits & U_ORIGIN2)
		to->origin[1] = MSG_ReadOrigin (mvdext1);
	if (bits & U_ANGLE2)
		to->angles[1] = MSG_ReadAngle ();
	if (bits & U_ORIGIN3)
		to->origin[2] = MSG_ReadOrigin (mvdext1);
	if (bits & U_ANGLE3)
		to->angles[2] = MSG_ReadAngle ();
	if (ext & U_FTE_TRANS)
		to->alpha = (byte)MSG_ReadByte ();
	if (ext & U_FTE_COLOURMOD)
	{
		for (i=0 ; i<3 ; i++)
			to->colormod[i] = (byte)MSG_ReadByte ();
	}
}

void MSG_ReadDeltaUsercmd (usercmd_t *from, usercmd_t *move)
{
	int bits;

	memcpy (move, from, sizeof(*move));

	bits = MSG_ReadByte ();
		
// read current angles
	if (bits & CM_ANGLE1)
		move->angles[0] = MSG_ReadAngle16 ();
	if (bits & CM_ANGLE2)
		move->angles[1] = MSG_ReadAngle16 ();
	if (bits & CM_ANGLE3)
		move->angles[2] = MSG_ReadAngle16 ();
		
// read movement
	if (bits & CM_FORWARD)
		move->forwardmove = (short)MSG_ReadShort ();
	if (bits & CM_SIDE)
		move->sidemove = (short)MSG_ReadShort ();
	if (bits & CM_UP)
		move->upmove = (short)MSG_ReadShort ();
	
// read buttons
	if (bits & CM_BUTTONS)
		move->buttons = (byte)MSG_ReadByte ();

	if (bits & CM_IMPULSE)
		move->impulse = (byte)MSG_ReadByte ();

// read time to run command
	move->msec = (byte)MSG_ReadByte ();
}

void SZ_Clear (sizebuf_t *buf)
{
	buf->cursize = 0;
	buf->overflowed = false;
}

void *SZ_GetSpace (sizebuf_t *buf, int length)
{
	void	*data;
	
	if (buf->cursize + length > buf->maxsize)
	{
		if (!buf->allowoverflow)
			Sys_Error ("SZ_GetSpace: overflow without allowoverflow set (%d)", buf->maxsize);
		
		if (length > buf->maxsize)
			Sys_Error ("SZ_GetSpace: %i is > full buffer size", length);
			
		Sys_Printf ("SZ_GetSpace: overflow\n");	// because Con_Printf may be redirected
		SZ_Clear (buf); 
		buf->overflowed = true;
	}

	data = buf->data + buf->cursize;
	buf->cursize += length;
	
	return data;
}

void SZ_Write (sizebuf_t *buf, void *data, int length)
{
	Q_memcpy (SZ_GetSpace(buf,length),data,length);		
}

void SZ_Print (sizebuf_t *buf, char *data)
{
	int		len;
	
	len = Q_strlen(data)+1;

	if (!buf->cursize || buf->data[buf->cursize-1])
		Q_memcpy ((byte *)SZ_GetSpace(buf, len),data,len); // no trailing 0
	else
		Q_memcpy ((byte *)SZ_GetSpace(buf, len-1)-1,data,len); // write over trailing 0
}
