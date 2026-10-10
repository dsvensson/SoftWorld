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

#include "r_local.h"
#include "r_local.h"

#define MAX_PARTICLES			8192	// default max # of particles at one
										//  time
#define ABSOLUTE_MIN_PARTICLES	512		// no fewer than this no matter what's
										//  on the command line

static int		ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
static int		ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
static int		ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

static particle_t	*active_particles, *free_particles;

static particle_t	*particles;
static int			r_numparticles;

vec3_t			r_pright, r_pup, r_ppn;


/*
===============
R_InitParticles
===============
*/
void R_InitParticles (void)
{
	int		i;

	i = COM_CheckParm ("-particles");

	if (i)
	{
		r_numparticles = (int)(Q_atoi(com_argv[i+1]));
		if (r_numparticles < ABSOLUTE_MIN_PARTICLES)
			r_numparticles = ABSOLUTE_MIN_PARTICLES;
	}
	else
	{
		r_numparticles = MAX_PARTICLES;
	}

	particles = (particle_t *)
			Mem_Calloc ((size_t)r_numparticles, sizeof(particle_t));
}


/*
===============
R_ClearParticles
===============
*/
void R_ClearParticles (void)
{
	int		i;
	
	free_particles = &particles[0];
	active_particles = NULL;

	for (i=0 ;i<r_numparticles ; i++)
		particles[i].next = &particles[i+1];
	particles[r_numparticles-1].next = NULL;
}


void R_ReadPointFile_f (void)
{
	FILE	*f;
	vec3_t	org;
	int		r;
	int		c;
	particle_t	*p;
	char	fname[MAX_OSPATH];

	if (!r_scene.worldmodel)
	{
		Con_Printf ("pointfile: no map is loaded\n");
		return;
	}
	COM_StripExtension (r_scene.worldmodel->name, fname);
	Q_strncatz (fname, ".pts", sizeof(fname));

	COM_FOpenFile (fname, &f);
	if (!f)
	{
		Con_Printf ("couldn't open %s\n", fname);
		return;
	}

	Con_Printf ("Reading %s...\n", fname);
	c = 0;
	for ( ;; )
	{
		r = fscanf (f,"%f %f %f\n", &org[0], &org[1], &org[2]);
		if (r != 3)
			break;
		c++;
		
		if (!free_particles)
		{
			Con_Printf ("Not enough free particles\n");
			break;
		}
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		
		p->die = 99999;
		p->color = (float)((-c)&15);
		p->type = pt_static;
		VectorCopy (vec3_origin, p->vel);
		VectorCopy (org, p->org);
	}

	fclose (f);
	Con_Printf ("%i points read\n", c);
}
	
/*
===============
R_ParticleExplosion

===============
*/
void R_ParticleExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;
	
	for (i=0 ; i<1024 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = (float)(r_scene.time + 5);
		p->color = (float)ramp1[0];
		p->ramp = (float)(rand()&3);
		if (i & 1)
		{
			p->type = pt_explode;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (vec_t)((rand()%512)-256);
			}
		}
		else
		{
			p->type = pt_explode2;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (vec_t)((rand()%512)-256);
			}
		}
	}
}

/*
===============
R_ParticleExplosion2

NetQuake's explosion in a range of colors, colorLength from colorStart
===============
*/
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength)
{
	int			i, j;
	particle_t	*p;

	if (colorLength < 1)
		colorLength = 1;
	for (i=0 ; i<512 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = (float)(r_scene.time + 0.3f);
		p->color = (float)((colorStart + i % colorLength) & 255);
		p->type = pt_blob;
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + ((rand()%32)-16);
			p->vel[j] = (vec_t)((rand()%512)-256);
		}
	}
}

/*
===============
R_BlobExplosion

===============
*/
void R_BlobExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;
	
	for (i=0 ; i<1024 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = (float)(r_scene.time + 1 + (rand()&8)*0.05f);

		if (i & 1)
		{
			p->type = pt_blob;
			p->color = (float)(66 + rand()%6);
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (vec_t)((rand()%512)-256);
			}
		}
		else
		{
			p->type = pt_blob2;
			p->color = (float)(150 + rand()%6);
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (vec_t)((rand()%512)-256);
			}
		}
	}
}

/*
===============
R_RunParticleEffect

===============
*/
void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count)
{
	int			i, j;
	particle_t	*p;
	int			scale;

	if (count > 130)
		scale = 3;
	else if (count > 20)
		scale = 2;
	else
		scale = 1;

	for (i=0 ; i<count ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = (float)(r_scene.time + 0.1f*(rand()%5));
		p->color = (float)((color&~7) + (rand()&7));
		p->type = pt_grav;
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + scale*((rand()&15)-8);
			p->vel[j] = dir[j]*15;// + (rand()%300)-150;
		}
	}
}


/*
===============
R_LavaSplash

===============
*/
void R_LavaSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i++)
		for (j=-16 ; j<16 ; j++)
			for (k=0 ; k<1 ; k++)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;
		
				p->die = (float)(r_scene.time + 2 + (rand()&31) * 0.02f);
				p->color = (float)(224 + (rand()&7));
				p->type = pt_grav;
				
				dir[0] = (vec_t)(j*8 + (rand()&7));
				dir[1] = (vec_t)(i*8 + (rand()&7));
				dir[2] = 256;
	
				p->org[0] = org[0] + dir[0];
				p->org[1] = org[1] + dir[1];
				p->org[2] = org[2] + (rand()&63);
	
				VectorNormalize (dir);						
				vel = (float)(50 + (rand()&63));
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_TeleportSplash

===============
*/
void R_TeleportSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i+=4)
		for (j=-16 ; j<16 ; j+=4)
			for (k=-24 ; k<32 ; k+=4)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;
		
				p->die = (float)(r_scene.time + 0.2f + (rand()&7) * 0.02f);
				p->color = (float)(7 + (rand()&7));
				p->type = pt_grav;
				
				dir[0] = (vec_t)(j*8);
				dir[1] = (vec_t)(i*8);
				dir[2] = (vec_t)(k*8);
	
				p->org[0] = org[0] + i + (rand()&3);
				p->org[1] = org[1] + j + (rand()&3);
				p->org[2] = org[2] + k + (rand()&3);
	
				VectorNormalize (dir);						
				vel = (float)(50 + (rand()&63));
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_RocketTrail

A trail from start to end: id's types (0 rocket, 1 grenade, 2 blood, 3 and
5 tracers, 4 slight blood, 6 vore), a particle every 3 units (slight blood's
6). id's stepped a unit for each 3 it counted, so only the first third of a
stretch had any; as FTE does it, they are spaced along all of it, and *carry
(if carry isn't NULL) is how far into the stretch the next is due, then how
far into the next stretch: a trail drawn a stretch at a time stays even.
===============
*/
void R_RocketTrail (const vec3_t start, const vec3_t end, int type, float *carry)
{
	vec3_t	vec, at;
	float	len, d, spacing = type == 4 ? 6.0f : 3.0f;
	int			j;
	particle_t	*p;

	VectorSubtract (end, start, vec);
	len = VectorNormalize (vec);
	for (d = carry ? *carry : 0 ; d < len ; d += spacing)
	{
		if (!free_particles)
			continue;		// the spacing goes on
		for (j=0 ; j<3 ; j++)
			at[j] = start[j] + vec[j] * d;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		
		VectorCopy (vec3_origin, p->vel);
		p->die = (float)(r_scene.time + 2);

		if (type == 4)
		{	// slight blood
			p->type = pt_slowgrav;
			p->color = (float)(67 + (rand()&3));
			for (j=0 ; j<3 ; j++)
				p->org[j] = at[j] + ((rand()%6)-3);
		}
		else if (type == 2)
		{	// blood
			p->type = pt_slowgrav;
			p->color = (float)(67 + (rand()&3));
			for (j=0 ; j<3 ; j++)
				p->org[j] = at[j] + ((rand()%6)-3);
		}
		else if (type == 6)
		{	// voor trail
			p->color = (float)(9*16 + 8 + (rand()&3));
			p->type = pt_static;
			p->die = (float)(r_scene.time + 0.3f);
			for (j=0 ; j<3 ; j++)
				p->org[j] = at[j] + ((rand()&15)-8);
		}
		else if (type == 1)
		{	// smoke smoke
			p->ramp = (float)((rand()&3) + 2);
			p->color = (float)(ramp3[(int)p->ramp]);
			p->type = pt_fire;
			for (j=0 ; j<3 ; j++)
				p->org[j] = at[j] + ((rand()%6)-3);
		}
		else if (type == 0)
		{	// rocket trail
			p->ramp = (float)((rand()&3));
			p->color = (float)(ramp3[(int)p->ramp]);
			p->type = pt_fire;
			for (j=0 ; j<3 ; j++)
				p->org[j] = at[j] + ((rand()%6)-3);
		}
		else if (type == 3 || type == 5)
		{	// tracer
			static int tracercount;

			p->die = (float)(r_scene.time + 0.5f);
			p->type = pt_static;
			if (type == 3)
				p->color = (float)(52 + ((tracercount&4)<<1));
			else
				p->color = (float)(230 + ((tracercount&4)<<1));
			
			tracercount++;

			VectorCopy (at, p->org);
			if (tracercount & 1)
			{
				p->vel[0] = 30*vec[1];
				p->vel[1] = 30*-vec[0];
			}
			else
			{
				p->vel[0] = 30*-vec[1];
				p->vel[1] = 30*vec[0];
			}
			
		}
	}
	if (carry)
		*carry = d - len;
}


/*
===============
R_DrawParticles
===============
*/
void R_DrawParticles (void)
{
	particle_t		*p, *kill;
	float			grav;
	int				i;
	float			time2, time3;
	float			time1;
	float			dvel;
	float			frametime;
	D_StartParticles ();

	VectorScale (vright, xscaleshrink, r_pright);
	VectorScale (vup, yscaleshrink, r_pup);
	VectorCopy (vpn, r_ppn);

	frametime = (float)r_scene.frametime;
	time3 = frametime * 15;
	time2 = frametime * 10; // 15;
	time1 = frametime * 5;
	grav = frametime * 800 * 0.05f;
	dvel = 4*frametime;
	
	for ( ;; ) 
	{
		kill = active_particles;
		if (kill && kill->die < r_scene.time)
		{
			active_particles = kill->next;
			kill->next = free_particles;
			free_particles = kill;
			continue;
		}
		break;
	}

	for (p=active_particles ; p ; p=p->next)
	{
		for ( ;; )
		{
			kill = p->next;
			if (kill && kill->die < r_scene.time)
			{
				p->next = kill->next;
				kill->next = free_particles;
				free_particles = kill;
				continue;
			}
			break;
		}

		D_DrawParticle (p);

		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;
		
		switch (p->type)
		{
		case pt_static:
			break;
		case pt_fire:
			p->ramp += time1;
			if (p->ramp >= 6)
				p->die = -1;
			else
				p->color = (float)(ramp3[(int)p->ramp]);
			p->vel[2] += grav;
			break;

		case pt_explode:
			p->ramp += time2;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = (float)(ramp1[(int)p->ramp]);
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_explode2:
			p->ramp += time3;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = (float)(ramp2[(int)p->ramp]);
			for (i=0 ; i<3 ; i++)
				p->vel[i] -= p->vel[i]*frametime;
			p->vel[2] -= grav;
			break;

		case pt_blob:
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_blob2:
			for (i=0 ; i<2 ; i++)
				p->vel[i] -= p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_slowgrav:
		case pt_grav:
			p->vel[2] -= grav;
			break;
		}
	}

	D_EndParticles ();
}

