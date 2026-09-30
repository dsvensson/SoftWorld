/*
Copyright (C) 2001-2002 A Nourai

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
// cl_fchecks.c -- replies to what players ask of each other's clients in chat,
// as ezQuake replies (its fchecks.c): f_version, f_system and f_modified. A
// request is a chat line of "name: " and the request alone (a number after it
// aside); each kind is replied to once in 20 seconds at most, and not while
// spectating or watching a demo. The colors are ezQuake's, in braces, which
// chat shows them in.

#include "cl_local.h"
#include "version.h"

#define FCHECK_INTERVAL		20		// seconds between replies of a kind

// f_system tells what it is asked, or "disabled"
static cvar_t	allow_f_system = {.name = "allow_f_system", .string = "1", .archive = true,
	.description = "Answers a player's f_system in chat with this computer's memory, CPU and GPU.",
	.values = (const cvar_value_t[]){{"0", "Answers \"disabled\""}, {"1", "Answers with the memory, CPU and GPU"}, {0}}};

// the platforms' colors in f_version
static const struct
{
	const char	*platform;		// the start of Sys_Platform's name
	const char	*color;
} fcheck_platforms[] =
{
	{"Win", "&c4af"},			// light blue
	{"Linux", "&cfa0"},			// orange
	{"MacOSX", "&caaa"},		// silver
};

// what a vendor's name in the CPU or GPU is colored
static const struct
{
	const char	*vendor;
	const char	*color;
	bool		gpu;			// only in the GPU's name
} fcheck_vendors[] =
{
	{"Intel", "&c06f", false},
	{"AMD", "&cf00", false},
	{"Ryzen", "&cf00", false},
	{"Radeon", "&cf00", true},
	{"RX ", "&cf00", true},
	{"NVIDIA", "&c0f0", true},
	{"GeForce", "&c0f0", true},
	{"RTX", "&c0f0", true},
	{"GTX", "&c0f0", true},
};

/*
================
CL_FCheckMatch

A chat line from a player or spectator (not to their team) that is request
alone: after it only a number and spaces
================
*/
static bool CL_FCheckMatch (const char *line, const char *request)
{
	const char	*sender;
	size_t		len;
	int			i, offset = -1, state;

	for (i = 0 ; i < MAX_CLIENTS && offset < 0 ; i++)
	{
		if (!cl.players[i].name[0])
			continue;
		sender = Info_ValueForKey (cl.players[i].userinfo, "name");
		len = strlen (sender);
		if (len > 31)
			len = 31;
		if (!strncmp (line, sender, len) && line[len] == ':' && line[len + 1] == ' ')
			offset = (int)len + 2;
	}
	if (offset < 0)
		return false;

	// the text as it is typed, the other charset aside
	line += offset;
	for (len = 0 ; request[len] ; len++)
		if ((line[len] & 127) != request[len])
			return false;
	for (line += len, state = 0 ; *line ; line++)
	{
		if (isdigit (*line & 127) && state <= 1)
			state = 1;
		else if (isspace (*line & 127))
			state = state == 1 ? 2 : state;
		else
			return false;
	}
	return true;
}

// a name as a command's argument: nothing that would end it or the command
static void CL_FCheckClean (char *s)
{
	for ( ; *s ; s++)
		if (*s == ';' || *s == '"' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static const char *CL_FCheckVendorColor (const char *device, bool gpu)
{
	size_t	i;
	char	lower[128], vendor[16];
	int		j;

	for (j = 0 ; device[j] && j < (int)sizeof(lower) - 1 ; j++)
		lower[j] = (char)tolower ((byte)device[j]);
	lower[j] = 0;
	for (i = 0 ; i < sizeof(fcheck_vendors) / sizeof(fcheck_vendors[0]) ; i++)
	{
		if (fcheck_vendors[i].gpu && !gpu)
			continue;
		for (j = 0 ; fcheck_vendors[i].vendor[j] ; j++)
			vendor[j] = (char)tolower ((byte)fcheck_vendors[i].vendor[j]);
		vendor[j] = 0;
		if (strstr (lower, vendor))
			return fcheck_vendors[i].color;
	}
	return "";
}

/*
================
CL_FCheckVersion

"SoftWorld 3.06 Linux64:SW", the name ice white, the version cyan, the
platform in its color and the renderer (software) gray
================
*/
static void CL_FCheckVersion (void)
{
	const char	*platform = Sys_Platform (), *color = "&c888";
	size_t		i;

	for (i = 0 ; i < sizeof(fcheck_platforms) / sizeof(fcheck_platforms[0]) ; i++)
		if (!strncmp (platform, fcheck_platforms[i].platform, strlen (fcheck_platforms[i].platform)))
			color = fcheck_platforms[i].color;
	Cbuf_AddText (va ("say {&ccefSoftWorld&r &c0ff%4.2f&r %s%s&r:&c888SW&r}\n", VERSION, color, platform));
}

/*
================
CL_FCheckSystem

"16000MB, the CPU (its MHz), the GPU", the CPU and GPU colored by their
vendors; or "disabled" by allow_f_system
================
*/
static void CL_FCheckSystem (void)
{
	static sys_info_t	info;
	static bool			known;
	char				text[512], cpu[128], gpu[128];
	const char			*color;
	size_t				len;

	if (!allow_f_system.value)
	{
		Cbuf_AddText ("say {disabled}\n");
		return;
	}
	if (!known)
	{
		Sys_SystemInfo (&info);
		known = true;
	}
	Q_strncpyz (cpu, info.cpu, sizeof(cpu));
	Q_strncpyz (gpu, VID_GPUName (), sizeof(gpu));
	CL_FCheckClean (cpu);
	CL_FCheckClean (gpu);

	len = (size_t)snprintf (text, sizeof(text), "%uMB", info.memory);
	if (cpu[0] && len < sizeof(text))
	{
		color = CL_FCheckVendorColor (cpu, false);
		len += (size_t)snprintf (text + len, sizeof(text) - len, ", %s%s%s", color, cpu, color[0] ? "&r" : "");
		if (info.mhz && len < sizeof(text))
			len += (size_t)snprintf (text + len, sizeof(text) - len, " (%dMHz)", info.mhz);
	}
	if (gpu[0] && len < sizeof(text))
	{
		color = CL_FCheckVendorColor (gpu, true);
		snprintf (text + len, sizeof(text) - len, ", %s%s%s", color, gpu, color[0] ? "&r" : "");
	}
	Cbuf_AddText (va ("say {%s}\n", text));
}

static struct
{
	const char	*request;
	void		(*reply) (void);
	double		replied;		// when last, 0 for never
} fchecks[] =
{
	{"f_version", CL_FCheckVersion, 0},
	{"f_system", CL_FCheckSystem, 0},
	{"f_modified", CL_FModResponse, 0},
};

/*
================
CL_FCheckRequest

A chat line as the server sent it: replied to if it asks what this client is
================
*/
void CL_FCheckRequest (const char *line)
{
	size_t	i;

	if (cls.state != ca_active || cls.demoplayback || cl.spectator)
		return;
	for (i = 0 ; i < sizeof(fchecks) / sizeof(fchecks[0]) ; i++)
	{
		if (fchecks[i].replied && host.realtime - fchecks[i].replied < FCHECK_INTERVAL)
			continue;
		if (!CL_FCheckMatch (line, fchecks[i].request))
			continue;
		fchecks[i].reply ();
		fchecks[i].replied = host.realtime;
	}
}

void CL_InitFChecks (void)
{
	Cvar_RegisterVariable (&allow_f_system);
	CL_InitFMod ();
}
