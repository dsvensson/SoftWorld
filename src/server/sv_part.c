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
// sv_part.c -- the particle effects QuakeC names (particleeffectnum): their
// numbers, and their names sent as FTE's server sends them, to the clients
// that take its particle messages (FTE_PEXT_CSQC): at prespawn, and at once
// for one named later

#include "sv_local.h"

// DarkPlaces' own effects, numbered first, as its effects are numbered
static const char	*sv_dpeffects[] =
{
	"TE_GUNSHOT", "TE_GUNSHOTQUAD", "TE_SPIKE", "TE_SPIKEQUAD", "TE_SUPERSPIKE", "TE_SUPERSPIKEQUAD",
	"TE_WIZSPIKE", "TE_KNIGHTSPIKE", "TE_EXPLOSION", "TE_EXPLOSIONQUAD", "TE_TAREXPLOSION", "TE_TELEPORT",
	"TE_LAVASPLASH", "TE_SMALLFLASH", "TE_FLAMEJET", "EF_FLAME", "TE_BLOOD", "TE_SPARK", "TE_PLASMABURN",
	"TE_TEI_G3", "TE_TEI_SMOKE", "TE_TEI_BIGEXPLOSION", "TE_TEI_PLASMAHIT", "EF_STARDUST", "TR_ROCKET",
	"TR_GRENADE", "TR_BLOOD", "TR_WIZSPIKE", "TR_SLIGHTBLOOD", "TR_KNIGHTSPIKE", "TR_VORESPIKE",
	"TR_NEHAHRASMOKE", "TR_NEXUIZPLASMA", "TR_GLOWTRAIL", "SVC_PARTICLE",
};

// the effect's name for a client, at prespawn and when it comes late
static void SV_WriteParticleName (sizebuf_t *msg, int i)
{
	MSG_WriteByte (msg, svc_fte_precache);
	MSG_WriteShort (msg, PC_PARTICLE | i);
	MSG_WriteString (msg, sv.particle_precache[i]);
}

// the number of an effect, given one if it has none: 0 when there are too many
static int SV_AddParticleEffect (const char *name)
{
	int		i;

	for (i = 1 ; i <= sv.num_particles ; i++)
		if (!strcmp (sv.particle_precache[i], name))
			return i;
	if (i == MAX_PARTICLE_PRECACHE || strlen (name) >= MAX_QPATH)
		return 0;
	Q_strncpyz (sv.particle_precache[i], name, sizeof(sv.particle_precache[i]));
	sv.num_particles = i;
	if (sv.state == ss_active)
	{
		SV_WriteParticleName (&sv.multicast, i);
		SV_MulticastProtExt (vec3_origin, MULTICAST_ALL_R, FTE_PEXT_CSQC, 0, 0);
	}
	return i;
}

/*
=================
SV_ParticleEffect

particleeffectnum's number for a name, 0 for none. The first, when it is
effectinfo.<anything>, numbers effectinfo.txt's effects first in their order,
after DarkPlaces' own, as QuakeSpasm-Spiked numbers them for DarkPlaces'
clients
=================
*/
int SV_ParticleEffect (const char *name)
{
	char	*data, *s;
	size_t	i;

	if (!*name)
		return 0;
	if (!sv.num_particles && !strncmp (name, "effectinfo.", 11) && (data = (char *)FS_LoadFile ("effectinfo.txt", NULL)))
	{
		for (i = 0 ; i < sizeof(sv_dpeffects) / sizeof(sv_dpeffects[0]) ; i++)
			SV_AddParticleEffect (sv_dpeffects[i]);
		for (s = data ; (s = COM_Parse (s)) ; )
		{
			if (!strcmp (com_token, "effect") && (s = COM_Parse (s)))
				SV_AddParticleEffect (com_token);
			while (s && *s && *s != '\n')	// the rest of the line
				s++;
		}
		Mem_Free (data);
	}
	return SV_AddParticleEffect (name);
}

// prespawn's: the effect i's name, to a client that takes them
void SV_WriteParticle (const client_t *client, sizebuf_t *msg, int i)
{
	if (client->fteext & FTE_PEXT_CSQC)
		SV_WriteParticleName (msg, i);
}
