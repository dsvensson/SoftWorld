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

static void	*SZ_GetSpace (sizebuf_t *buf, int length);

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

void MSG_WriteString (sizebuf_t *sb, const char *s)
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

	if (!to->number || to->number >= MAX_QW_EDICTS)
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

static int MSG_ReadChar (void)
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
		
	// unsigned: the top byte's high bit would overflow an int's shift
	c = (int)((uint32_t)msg_readbuf->data[msg_readcount]
	| (uint32_t)msg_readbuf->data[msg_readcount+1] << 8
	| (uint32_t)msg_readbuf->data[msg_readcount+2] << 16
	| (uint32_t)msg_readbuf->data[msg_readcount+3] << 24);
	
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

/*
==============================================================================

FTE'S REPLACEMENT DELTAS

An entity's update says what changed from what the client has, by UF_ bits,
or (UF_RESET) from its baseline; a player's carries its movement
(UF_PREDINFO). As FTE writes and reads them without PEXT2_PREDINFO,
PEXT2_NEWSIZEENCODING and PEXT2_LERPTIME, which this program doesn't ask for;
the fields it has no use for are read and dropped.

==============================================================================
*/

// the writer's own flags among the bits, on bits it works out itself: the
// movetype or the weapon frame changed
#define	UF_SV_MOVETYPE		UF_EXTEND4
#define	UF_SV_WEAPONFRAME	UF_EXTEND2

void MSG_WriteEntityIndex (sizebuf_t *sb, int number, bool remove)
{
	int		rflag = remove ? 0x8000 : 0;

	if (number >= 0x4000)
	{
		MSG_WriteShort (sb, (number & 0x3fff) | 0x4000 | rflag);
		MSG_WriteByte (sb, number >> 14);
	}
	else
		MSG_WriteShort (sb, number | rflag);
}

int MSG_ReadEntityIndex (bool *remove)
{
	int		number = MSG_ReadShort () & 0xffff;

	*remove = (number & 0x8000) != 0;
	if (number & 0x4000)
		return (number & 0x3fff) | (MSG_ReadByte () << 14);
	return number & 0x7fff;
}

void MSG_WriteBigEntity (sizebuf_t *sb, int number)
{
	if (number >= 0x8000)
	{
		MSG_WriteShort (sb, (number >> 8) | 0x8000);
		MSG_WriteByte (sb, number & 255);
	}
	else
		MSG_WriteShort (sb, number);
}

int MSG_ReadBigEntity (void)
{
	int		number = MSG_ReadShort () & 0xffff;

	if (number & 0x8000)
		return ((number & 0x7fff) << 8) | MSG_ReadByte ();
	return number;
}

uint64_t MSG_ReadUInt64 (void)
{
	uint64_t	r;
	int			b = 0, l = 0x80, v = MSG_ReadByte () & 255;

	for ( ; v & l ; l >>= 1)
	{
		v -= l;
		b++;
	}
	r = (uint64_t)v << (b*8);
	while (b-- > 0)
		r |= (uint64_t)(MSG_ReadByte () & 255) << (b*8);
	return r;
}

// the movement a player's update carries whatever changed
static unsigned MSG_PredictionBits (const entity_state_t *to)
{
	unsigned	bits = 0;

	if (to->movement[0])
		bits |= UFP_FORWARD;
	if (to->movement[1])
		bits |= UFP_SIDE;
	if (to->movement[2])
		bits |= UFP_UP;
	if (to->velocity[0] || to->velocity[1])
		bits |= UFP_VELOCITYXY;
	if (to->velocity[2])
		bits |= UFP_VELOCITYZ;
	if (to->msec)
		bits |= UFP_MSEC;
	return bits;
}

unsigned MSG_ReplacementBits (const entity_state_t *from, const entity_state_t *to)
{
	unsigned	bits = 0;

	if (from->pmovetype != to->pmovetype)
		bits |= UF_PREDINFO | UF_SV_MOVETYPE;
	if (from->weaponframe != to->weaponframe)
		bits |= UF_PREDINFO | UF_SV_WEAPONFRAME;
	// the client takes an update without movement as none: one that stops
	// moving says so
	if (MSG_PredictionBits (to) || MSG_PredictionBits (from))
		bits |= UF_PREDINFO;
	// a moving player's place goes with its movement, changed or not
	if ((bits & UF_PREDINFO) && (from->velocity[0] || from->velocity[1] || from->velocity[2]))
		bits |= UF_ORIGINXY | UF_ORIGINZ | UF_ANGLESXZ | UF_ANGLESY;

	if (to->origin[0] != from->origin[0] || to->origin[1] != from->origin[1])
		bits |= UF_ORIGINXY;
	if (to->origin[2] != from->origin[2])
		bits |= UF_ORIGINZ;
	if (to->angles[0] != from->angles[0] || to->angles[2] != from->angles[2])
		bits |= UF_ANGLESXZ;
	if (to->angles[1] != from->angles[1])
		bits |= UF_ANGLESY;
	if (to->modelindex != from->modelindex)
		bits |= UF_MODEL;
	if (to->frame != from->frame)
		bits |= UF_FRAME;
	if (to->skinnum != from->skinnum)
		bits |= UF_SKIN;
	if (to->colormap != from->colormap)
		bits |= UF_COLORMAP;
	if (to->effects != from->effects)
		bits |= UF_EFFECTS;
	if (to->dpflags != from->dpflags)
		bits |= UF_FLAGS;
	if (to->alpha != from->alpha)
		bits |= UF_ALPHA;
	if (to->scale != from->scale)
		bits |= UF_SCALE;
	if (memcmp (to->colormod, from->colormod, sizeof(to->colormod)))
		bits |= UF_COLORMOD;
	if (to->traileffect != from->traileffect || to->emiteffect != from->emiteffect)
		bits |= UF_TRAILEFFECT;
	return bits;
}

void MSG_WriteReplacement (sizebuf_t *sb, unsigned bits, const entity_state_t *to, unsigned mvdext1)
{
	unsigned	predbits = 0;
	int			i;

	if (bits & UF_SV_MOVETYPE)
		predbits |= UFP_MOVETYPE;
	if (bits & UF_SV_WEAPONFRAME)
		predbits |= UFP_WEAPONFRAME;
	bits &= ~(UF_SV_MOVETYPE | UF_SV_WEAPONFRAME | UF_EXTEND1 | UF_EXTEND3 | UF_16BIT | UF_EFFECTS2);

	if (((bits & UF_MODEL) && to->modelindex > 255) || ((bits & UF_SKIN) && to->skinnum > 255)
		|| ((bits & UF_FRAME) && to->frame > 255))
		bits |= UF_16BIT;
	if (bits & UF_EFFECTS)
	{
		if (to->effects & 0xffff0000)
			bits |= UF_EFFECTS2;
		else if (to->effects & 0x0000ff00)
			bits = (bits & ~UF_EFFECTS) | UF_EFFECTS2;
	}
	if (bits & 0xff000000)
		bits |= UF_EXTEND3;
	if (bits & 0x00ff0000)
		bits |= UF_EXTEND2;
	if (bits & 0x0000ff00)
		bits |= UF_EXTEND1;

	MSG_WriteByte (sb, bits & 255);
	if (bits & UF_EXTEND1)
		MSG_WriteByte (sb, (bits >> 8) & 255);
	if (bits & UF_EXTEND2)
		MSG_WriteByte (sb, (bits >> 16) & 255);
	if (bits & UF_EXTEND3)
		MSG_WriteByte (sb, (bits >> 24) & 255);

	if (bits & UF_FRAME)
	{
		if (bits & UF_16BIT)
			MSG_WriteShort (sb, to->frame);
		else
			MSG_WriteByte (sb, to->frame);
	}
	// floats with MVD_PEXT1_FLOATCOORDS, as FTE's (its EZPEXT1_FLOATENTCOORDS)
	if (bits & UF_ORIGINXY)
	{
		MSG_WriteOrigin (sb, to->origin[0], mvdext1);
		MSG_WriteOrigin (sb, to->origin[1], mvdext1);
	}
	if (bits & UF_ORIGINZ)
		MSG_WriteOrigin (sb, to->origin[2], mvdext1);
	// a player's angles more precisely
	if (bits & UF_ANGLESXZ)
	{
		if (bits & UF_PREDINFO)
		{
			MSG_WriteAngle16 (sb, to->angles[0]);
			MSG_WriteAngle16 (sb, to->angles[2]);
		}
		else
		{
			MSG_WriteAngle (sb, to->angles[0]);
			MSG_WriteAngle (sb, to->angles[2]);
		}
	}
	if (bits & UF_ANGLESY)
	{
		if (bits & UF_PREDINFO)
			MSG_WriteAngle16 (sb, to->angles[1]);
		else
			MSG_WriteAngle (sb, to->angles[1]);
	}
	if ((bits & (UF_EFFECTS | UF_EFFECTS2)) == (UF_EFFECTS | UF_EFFECTS2))
		MSG_WriteLong (sb, to->effects);
	else if (bits & UF_EFFECTS2)
		MSG_WriteShort (sb, to->effects);
	else if (bits & UF_EFFECTS)
		MSG_WriteByte (sb, to->effects);

	if (bits & UF_PREDINFO)
	{
		predbits |= MSG_PredictionBits (to);
		MSG_WriteByte (sb, predbits);
		for (i=0 ; i<3 ; i++)
			if (predbits & (UFP_FORWARD << i))
				MSG_WriteShort (sb, to->movement[i]);
		if (predbits & UFP_MOVETYPE)
			MSG_WriteByte (sb, to->pmovetype);
		if (predbits & UFP_VELOCITYXY)
		{
			MSG_WriteShort (sb, to->velocity[0]);
			MSG_WriteShort (sb, to->velocity[1]);
		}
		if (predbits & UFP_VELOCITYZ)
			MSG_WriteShort (sb, to->velocity[2]);
		if (predbits & UFP_MSEC)
			MSG_WriteByte (sb, to->msec);
		if (predbits & UFP_WEAPONFRAME)
		{
			if (to->weaponframe > 127)
			{
				MSG_WriteByte (sb, 128 | (to->weaponframe & 127));
				MSG_WriteByte (sb, to->weaponframe >> 7);
			}
			else
				MSG_WriteByte (sb, to->weaponframe);
		}
	}

	if (bits & UF_MODEL)
	{
		if (bits & UF_16BIT)
			MSG_WriteShort (sb, to->modelindex);
		else
			MSG_WriteByte (sb, to->modelindex);
	}
	if (bits & UF_SKIN)
	{
		if (bits & UF_16BIT)
			MSG_WriteShort (sb, to->skinnum);
		else
			MSG_WriteByte (sb, to->skinnum);
	}
	if (bits & UF_COLORMAP)
		MSG_WriteByte (sb, to->colormap & 255);
	if (bits & UF_FLAGS)
		MSG_WriteByte (sb, to->dpflags);
	// FTE's alpha: 255 opaque (this program's 0)
	if (bits & UF_ALPHA)
		MSG_WriteByte (sb, to->alpha ? to->alpha : 255);
	if (bits & UF_SCALE)
		MSG_WriteByte (sb, to->scale);
	// the trail, its top bit an emitted effect after it
	if (bits & UF_TRAILEFFECT)
	{
		if (to->emiteffect)
		{
			MSG_WriteShort (sb, (to->traileffect & PC_INDEX) | 0x8000);
			MSG_WriteShort (sb, to->emiteffect & PC_INDEX);
		}
		else
			MSG_WriteShort (sb, to->traileffect & PC_INDEX);
	}
	if (bits & UF_COLORMOD)
	{
		for (i=0 ; i<3 ; i++)
			MSG_WriteByte (sb, to->colormod[i]);
	}
}

unsigned MSG_ReadReplacementBits (void)
{
	unsigned	bits = MSG_ReadByte () & 255;

	if (bits & UF_EXTEND1)
		bits |= (unsigned)(MSG_ReadByte () & 255) << 8;
	if (bits & UF_EXTEND2)
		bits |= (unsigned)(MSG_ReadByte () & 255) << 16;
	if (bits & UF_EXTEND3)
		bits |= (unsigned)(MSG_ReadByte () & 255) << 24;
	return bits;
}

void MSG_ReadReplacement (unsigned bits, entity_state_t *to, unsigned mvdext1)
{
	unsigned	predbits;
	int			i, n;

	if (bits & UF_FRAME)
		to->frame = (bits & UF_16BIT) ? MSG_ReadShort () & 0xffff : MSG_ReadByte ();
	if (bits & UF_ORIGINXY)
	{
		to->origin[0] = MSG_ReadOrigin (mvdext1);
		to->origin[1] = MSG_ReadOrigin (mvdext1);
	}
	if (bits & UF_ORIGINZ)
		to->origin[2] = MSG_ReadOrigin (mvdext1);
	if (bits & UF_ANGLESXZ)
	{
		to->angles[0] = (bits & UF_PREDINFO) ? MSG_ReadAngle16 () : MSG_ReadAngle ();
		to->angles[2] = (bits & UF_PREDINFO) ? MSG_ReadAngle16 () : MSG_ReadAngle ();
	}
	if (bits & UF_ANGLESY)
		to->angles[1] = (bits & UF_PREDINFO) ? MSG_ReadAngle16 () : MSG_ReadAngle ();
	if ((bits & (UF_EFFECTS | UF_EFFECTS2)) == (UF_EFFECTS | UF_EFFECTS2))
		to->effects = MSG_ReadLong ();
	else if (bits & UF_EFFECTS2)
		to->effects = MSG_ReadShort () & 0xffff;
	else if (bits & UF_EFFECTS)
		to->effects = MSG_ReadByte ();

	// the movement is the update's, there or not
	memset (to->movement, 0, sizeof(to->movement));
	memset (to->velocity, 0, sizeof(to->velocity));
	to->msec = 0;
	if (bits & UF_PREDINFO)
	{
		predbits = MSG_ReadByte () & 255;
		for (i=0 ; i<3 ; i++)
			if (predbits & (UFP_FORWARD << i))
				to->movement[i] = (short)MSG_ReadShort ();
		if (predbits & UFP_MOVETYPE)
			to->pmovetype = (byte)MSG_ReadByte ();
		if (predbits & UFP_VELOCITYXY)
		{
			to->velocity[0] = (short)MSG_ReadShort ();
			to->velocity[1] = (short)MSG_ReadShort ();
		}
		if (predbits & UFP_VELOCITYZ)
			to->velocity[2] = (short)MSG_ReadShort ();
		if (predbits & UFP_MSEC)
			to->msec = (byte)MSG_ReadByte ();
		if (predbits & UFP_WEAPONFRAME)
		{
			to->weaponframe = MSG_ReadByte () & 255;
			if (to->weaponframe & 128)
				to->weaponframe = (to->weaponframe & 127) | (MSG_ReadByte () << 7);
		}
	}

	if (bits & UF_MODEL)
		to->modelindex = (bits & UF_16BIT) ? MSG_ReadShort () & 0xffff : MSG_ReadByte ();
	if (bits & UF_SKIN)
		to->skinnum = (bits & UF_16BIT) ? (short)MSG_ReadShort () : MSG_ReadByte ();
	if (bits & UF_COLORMAP)
		to->colormap = MSG_ReadByte ();
	if (bits & UF_SOLID)
		MSG_ReadShort ();
	if (bits & UF_FLAGS)
		to->dpflags = (byte)MSG_ReadByte ();
	// FTE's alpha: 255 opaque, 0 unseen (this program's opaque, so nearly unseen)
	if (bits & UF_ALPHA)
	{
		i = MSG_ReadByte ();
		to->alpha = (byte)(i == 255 ? 0 : i ? i : 1);
	}
	if (bits & UF_SCALE)
		to->scale = (byte)MSG_ReadByte ();
	if (bits & UF_BONEDATA)
	{
		i = MSG_ReadByte ();
		if (i & 0x80)
			for (n = (MSG_ReadByte () & 255) * 7 ; n > 0 ; n--)
				MSG_ReadShort ();
		if (i & 0x40)
		{
			MSG_ReadByte ();
			MSG_ReadShort ();
		}
	}
	if (bits & UF_DRAWFLAGS)
	{
		if ((MSG_ReadByte () & 7) >= 6)		// Hexen 2's MLS_ADDLIGHT and up: a light
			MSG_ReadByte ();
	}
	if (bits & UF_TAGINFO)
	{
		MSG_ReadBigEntity ();
		MSG_ReadByte ();
	}
	if (bits & UF_LIGHT)
	{
		for (i=0 ; i<4 ; i++)
			MSG_ReadShort ();
		MSG_ReadByte ();
		MSG_ReadByte ();
	}
	// effects past the precaches' are none, as FTE has them
	if (bits & UF_TRAILEFFECT)
	{
		i = MSG_ReadShort () & 0xffff;
		n = (i & 0x8000) ? MSG_ReadShort () & PC_INDEX : 0;
		i &= PC_INDEX;
		to->traileffect = (unsigned short)(i < MAX_PARTICLE_PRECACHE ? i : 0);
		to->emiteffect = (unsigned short)(n < MAX_PARTICLE_PRECACHE ? n : 0);
	}
	if (bits & UF_COLORMOD)
	{
		for (i=0 ; i<3 ; i++)
			to->colormod[i] = (byte)MSG_ReadByte ();
	}
	if (bits & UF_GLOW)
	{
		for (i=0 ; i<5 ; i++)
			MSG_ReadByte ();
	}
	if (bits & UF_FATNESS)
		MSG_ReadChar ();
	if (bits & UF_MODELINDEX2)
	{
		if (bits & UF_16BIT)
			MSG_ReadShort ();
		else
			MSG_ReadByte ();
	}
	if (bits & UF_GRAVITYDIR)
	{
		MSG_ReadByte ();
		MSG_ReadByte ();
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

static void *SZ_GetSpace (sizebuf_t *buf, int length)
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

void SZ_Write (sizebuf_t *buf, const void *data, int length)
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
