// wl_hyprland.c -- what the compositor says of direct scanout and tearing. The
// Wayland protocols don't tell whether a compositor would scan a window out, or let
// it tear, only that it did (presentation feedback's zero-copy); Hyprland tells
// through its IPC socket, and why not: its settings, and what blocks them on the
// display now.

#include "print.h"
#include "wl_local.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
================
WL_Hyprland

Hyprland's reply to request, NUL-terminated; false if Hyprland isn't the
compositor, or didn't answer in time
================
*/
static bool WL_Hyprland (const char *request, char *reply, size_t size)
{
	const char			*signature = getenv ("HYPRLAND_INSTANCE_SIGNATURE");
	const char			*runtime = getenv ("XDG_RUNTIME_DIR");
	struct sockaddr_un	address = {.sun_family = AF_UNIX};
	struct pollfd		p;
	size_t				used = 0;
	ssize_t				got;
	int					s;

	if (!signature || !runtime || size < 2)
		return false;
	if (snprintf (address.sun_path, sizeof(address.sun_path), "%s/hypr/%s/.socket.sock", runtime, signature)
		>= (int)sizeof(address.sun_path))
		return false;
	s = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return false;
	if (connect (s, (struct sockaddr *)&address, sizeof(address)) < 0
		|| write (s, request, strlen (request)) != (ssize_t)strlen (request))
	{
		close (s);
		return false;
	}

	p.fd = s;
	p.events = POLLIN;
	while (used < size - 1 && poll (&p, 1, 100) > 0)
	{
		got = read (s, reply + used, size - 1 - used);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		used += (size_t)got;
	}
	close (s);
	reply[used] = 0;
	return used > 0;
}

// the number after "key": in a reply, or -1
static int WL_JsonInt (const char *json, const char *key)
{
	const char	*at = strstr (json, key);

	if (!at)
		return -1;
	at += strlen (key);
	while (*at == ' ' || *at == ':')
		at++;
	if (!strncmp (at, "true", 4))
		return 1;
	if (!strncmp (at, "false", 5))
		return 0;
	return *at >= '0' && *at <= '9' ? atoi (at) : -1;
}

// the list after "key": in a reply, as "A, B", lowercased
static void WL_JsonList (const char *json, const char *key, char *out, size_t size)
{
	const char	*at = strstr (json, key);
	size_t		len = 0;

	out[0] = 0;
	if (!at || !(at = strchr (at, '[')))
		return;
	for (at++ ; *at && *at != ']' && len < size - 3 ; at++)
	{
		if (*at == '"')
			continue;
		if (*at == ',')
		{
			out[len++] = ',';
			out[len++] = ' ';
			continue;
		}
		out[len++] = (char)(*at >= 'A' && *at <= 'Z' ? *at - 'A' + 'a' : *at);
	}
	out[len] = 0;
}

/*
================
WL_PrintScanout

Whether the compositor scans windows out, and lets them tear; with Hyprland,
its settings, and what blocks each on the focused display now
================
*/
void WL_PrintScanout (void)
{
	static char	reply[16384];
	char		blocked[256];
	const char	*monitor;
	int			scanout, tearing;

	if (!WL_Hyprland ("j/getoption render:direct_scanout", reply, sizeof(reply)))
	{
		Con_Printf ("Direct scanout: the compositor doesn't say whether it scans windows out; "
			"it is reported when it does\n");
		return;
	}
	scanout = WL_JsonInt (reply, "\"int\"");
	if (scanout == 0)
		Con_Printf ("Direct scanout: off in Hyprland's settings (render:direct_scanout 0; 2 scans out "
			"fullscreen games)\n");
	else if (scanout == 1)
		Con_Printf ("Direct scanout: Hyprland scans out fullscreen windows (render:direct_scanout 1)\n");
	else if (scanout == 2)
		Con_Printf ("Direct scanout: Hyprland scans out fullscreen games (render:direct_scanout 2)\n");

	if (WL_Hyprland ("j/getoption general:allow_tearing", reply, sizeof(reply)))
	{
		tearing = WL_JsonInt (reply, "\"bool\"");
		if (tearing == 0)
			Con_Printf ("Tearing: off in Hyprland's settings (general:allow_tearing false): vid_vsync 0 "
				"doesn't tear\n");
		else if (tearing == 1)
			Con_Printf ("Tearing: Hyprland lets fullscreen windows tear (general:allow_tearing true)\n");
	}

	// the display the game is on, as it is now
	if (!WL_Hyprland ("j/monitors", reply, sizeof(reply)))
		return;
	monitor = strstr (reply, "\"focused\": true");
	if (!monitor)
		return;
	WL_JsonList (monitor, "\"directScanoutBlockedBy\"", blocked, sizeof(blocked));
	if (blocked[0])
		Con_Printf ("  on this display, scanout is blocked by: %s\n", blocked);
	WL_JsonList (monitor, "\"tearingBlockedBy\"", blocked, sizeof(blocked));
	if (blocked[0])
		Con_Printf ("  and tearing by: %s\n", blocked);
}
