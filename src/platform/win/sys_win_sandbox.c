// sys_win_sandbox.c -- the programs with a client run in an AppContainer, as
// macOS's run in the App Sandbox (sys_mac_gui.m). A process enters a container
// only when it is created, so the program started is the launcher: it lets the
// container into the game directory, starts itself again inside it as the game,
// and waits for it.
//
// The container ("SoftWorld", softworld.exe's and softworld-client.exe's both)
// opens only what it is let into, by an entry for it in the permissions of a
// directory that everything in it inherits: the game directory (the one
// -basedir names, or else the one the first start asks for and the next ones
// remember) with the places its links lead to, which the permissions don't
// follow, and the program's own directory, to run it. sys_forget_sandbox
// forgets the game directory: the next start takes the container out of the
// directories it was let into and asks again. What is remembered is sandbox.cfg
// in the container's own folder, which the launcher and the game both write.
//
// The container may not confine the cursor either: the launcher does it for
// the game (THE CURSOR, below).
//
// The container can't reach this computer's own addresses (a server or a QTV
// proxy on 127.0.0.1) unless an administrator exempts it, and no firewall rule
// lets players in to its server: the first start offers both, through the UAC
// prompt of the program run again with -sandbox-network.

#define COBJMACROS

#include "cmd.h"
#include "print.h"
#include "win_local.h"

#include <aclapi.h>
#include <netfw.h>
#include <sddl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <userenv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SB_CONTAINER	L"SoftWorld"
#define SB_PATH			1024		// wide characters of a path
#define SB_MAXGRANTED	16
#define SB_GAMERIGHTS	(FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE)
#define SB_EXERIGHTS	(FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)
#define SB_NETWORKARG	"-sandbox-network"

// the capabilities the game runs with: the internet both ways, and the home or
// work network both ways
static const wchar_t	*sb_capabilities[] = {L"S-1-15-3-1", L"S-1-15-3-2", L"S-1-15-3-3"};
#define SB_NUMCAPS	(sizeof(sb_capabilities) / sizeof(sb_capabilities[0]))

// sandbox.cfg: a line of each, in UTF-8
typedef struct
{
	wchar_t	basedir[SB_PATH];					// "basedir <dir>": the one remembered, or empty
	wchar_t	granted[SB_MAXGRANTED][SB_PATH];	// "granted <dir>": the directories the container is let into
	int		numgranted;
	bool	network_allowed;					// "network allowed": the administrator exempted it
	bool	network_declined;					// "network declined": the player would rather not
} sb_state_t;

static sb_state_t	sb_state;

/*
===============================================================================

BOTH SIDES

===============================================================================
*/

static void SB_Copy (wchar_t *dst, size_t size, const wchar_t *src)
{
	swprintf (dst, size, L"%ls", src);
}

static bool SB_InContainer (void)
{
	HANDLE	token;
	DWORD	in = 0, size;

	if (!OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &token))
		return false;
	if (!GetTokenInformation (token, TokenIsAppContainer, &in, sizeof(in), &size))
		in = 0;
	CloseHandle (token);
	return in != 0;
}

// sandbox.cfg in the folder of the container with that SID
static bool SB_StatePath (PSID sid, wchar_t *path, size_t size)
{
	LPWSTR	sidtext;
	PWSTR	folder;
	HRESULT	hr;

	if (!ConvertSidToStringSidW (sid, &sidtext))
		return false;
	hr = GetAppContainerFolderPath (sidtext, &folder);
	LocalFree (sidtext);
	if (FAILED (hr))
		return false;
	swprintf (path, size, L"%ls\\sandbox.cfg", folder);
	CoTaskMemFree (folder);
	return true;
}

static void SB_LoadState (const wchar_t *path)
{
	FILE	*f;
	char	line[SB_PATH * 3];

	memset (&sb_state, 0, sizeof(sb_state));
	f = _wfopen (path, L"rb");
	if (!f)
		return;
	while (fgets (line, sizeof(line), f))
	{
		line[strcspn (line, "\r\n")] = 0;
		if (!strncmp (line, "basedir ", 8))
			MultiByteToWideChar (CP_UTF8, 0, line + 8, -1, sb_state.basedir, SB_PATH);
		else if (!strncmp (line, "granted ", 8) && sb_state.numgranted < SB_MAXGRANTED)
			MultiByteToWideChar (CP_UTF8, 0, line + 8, -1, sb_state.granted[sb_state.numgranted++], SB_PATH);
		else if (!strcmp (line, "network allowed"))
			sb_state.network_allowed = true;
		else if (!strcmp (line, "network declined"))
			sb_state.network_declined = true;
	}
	fclose (f);
}

static void SB_WriteLine (FILE *f, const char *key, const wchar_t *value)
{
	char	text[SB_PATH * 3];

	if (!WideCharToMultiByte (CP_UTF8, 0, value, -1, text, sizeof(text), NULL, NULL))
		return;
	fprintf (f, "%s %s\n", key, text);
}

static bool SB_SaveState (const wchar_t *path)
{
	FILE	*f;
	int		i;

	f = _wfopen (path, L"wb");
	if (!f)
		return false;
	if (sb_state.basedir[0])
		SB_WriteLine (f, "basedir", sb_state.basedir);
	for (i = 0 ; i < sb_state.numgranted ; i++)
		SB_WriteLine (f, "granted", sb_state.granted[i]);
	if (sb_state.network_allowed)
		fprintf (f, "network allowed\n");
	if (sb_state.network_declined)
		fprintf (f, "network declined\n");
	fclose (f);
	return true;
}

/*
===============================================================================

THE GAME, INSIDE

===============================================================================
*/

static wchar_t	sb_statepath[SB_PATH];

// the container this process runs in: its SID, for its folder
static bool SB_OwnStatePath (void)
{
	HANDLE	token;
	BYTE	info[256];
	DWORD	size;
	bool	found = false;

	if (!OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &token))
		return false;
	if (GetTokenInformation (token, TokenAppContainerSid, info, sizeof(info), &size))
		found = SB_StatePath (((TOKEN_APPCONTAINER_INFORMATION *)info)->TokenAppContainer, sb_statepath, SB_PATH);
	CloseHandle (token);
	return found;
}

// the next start asks for the game directory again (and, if the player said
// no, whether to let the sandbox reach this computer's own addresses); this one
// keeps it open
static void SB_Forget_f (void)
{
	bool	remembered;

	SB_LoadState (sb_statepath);
	remembered = sb_state.basedir[0] != 0;
	sb_state.basedir[0] = 0;
	sb_state.network_declined = false;
	if (!SB_SaveState (sb_statepath))
	{
		Con_Printf ("Couldn't write %ls\n", sb_statepath);
		return;
	}
	if (remembered)
		Con_Printf ("The game directory is forgotten: the next start asks for one.\n");
	else
		Con_Printf ("No game directory is remembered.\n");
}

void Sys_SandboxInit (void)
{
	if (!SB_InContainer ())
		return;

	Con_Printf ("Running in the AppContainer sandbox\n");
	if (!SB_OwnStatePath ())
	{
		Con_Printf ("The sandbox's folder can't be found: sys_forget_sandbox can't forget the game directory\n");
		return;
	}
	Cmd_AddCommand ("sys_forget_sandbox", SB_Forget_f,
		"Forgets the remembered Quake directory, so the next start asks for it again.");

	SB_LoadState (sb_statepath);
	if (sb_state.network_declined)
		Con_Printf ("The sandbox keeps SoftWorld from this computer's own addresses (127.0.0.1), and players "
			"from its server; sys_forget_sandbox asks again at the next start.\n");
}

/*
===============================================================================

THE LAUNCHER, OUTSIDE

===============================================================================
*/

static int SB_Fail (const wchar_t *fmt, ...)
{
	wchar_t	text[2048];
	va_list	args;

	va_start (args, fmt);
	vswprintf (text, sizeof(text) / sizeof(text[0]), fmt, args);
	va_end (args);
	MessageBoxW (NULL, text, L"SoftWorld", MB_OK | MB_ICONERROR);
	return 1;
}

static const wchar_t *SB_ErrorText (DWORD error)
{
	static wchar_t	text[512];

	if (!FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, text,
		sizeof(text) / sizeof(text[0]), NULL))
		swprintf (text, sizeof(text) / sizeof(text[0]), L"error %lu", error);
	text[wcscspn (text, L"\r\n")] = 0;
	return text;
}

// the directory holds the game
static bool SB_IsGameDir (const wchar_t *dir)
{
	wchar_t	path[SB_PATH];

	swprintf (path, SB_PATH, L"%ls\\id1\\pak0.pak", dir);
	return GetFileAttributesW (path) != INVALID_FILE_ATTRIBUTES;
}

// an absolute path, without a trailing separator but at a drive's root
static bool SB_FullPath (const wchar_t *path, wchar_t *full, size_t size)
{
	DWORD	len = GetFullPathNameW (path, (DWORD)size, full, NULL);

	if (!len || len >= size)
		return false;
	if (len > 3 && (full[len - 1] == L'\\' || full[len - 1] == L'/'))
		full[len - 1] = 0;
	return true;
}

/*
===============================================================================

LETTING THE CONTAINER IN

===============================================================================
*/

// a long change of permissions: a window telling how far it is, shown once it
// has taken a moment, and the links met, which the change doesn't follow
typedef struct
{
	const wchar_t	*dir;
	HWND			window;
	DWORD			start, shown;
	unsigned		objects;
	bool			links;		// gather them
} sb_progress_t;

static wchar_t	sb_links[SB_MAXGRANTED][SB_PATH];
static int		sb_numlinks;

static void SB_ShowProgress (sb_progress_t *p)
{
	wchar_t				text[SB_PATH + 64];
	MSG					msg;
	NONCLIENTMETRICSW	metrics = {.cbSize = sizeof(metrics)};
	HFONT				font;
	UINT				dpi;
	RECT				work;
	int					w, h;

	if (!p->window)
	{
		dpi = GetDpiForSystem ();
		w = MulDiv (560, (int)dpi, 96);
		h = MulDiv (64, (int)dpi, 96);
		SystemParametersInfoW (SPI_GETWORKAREA, 0, &work, 0);
		p->window = CreateWindowExW (WS_EX_TOOLWINDOW, L"STATIC", L"",
			WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_PATHELLIPSIS,
			(work.left + work.right - w) / 2, (work.top + work.bottom - h) / 2, w, h,
			NULL, NULL, GetModuleHandleW (NULL), NULL);
		if (!p->window)
			return;
		if (SystemParametersInfoForDpi (SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
		{
			font = CreateFontIndirectW (&metrics.lfMessageFont);
			SendMessageW (p->window, WM_SETFONT, (WPARAM)font, TRUE);
		}
	}
	swprintf (text, sizeof(text) / sizeof(text[0]), L"Letting SoftWorld's sandbox into %ls: %u files", p->dir,
		p->objects);
	SetWindowTextW (p->window, text);
	while (PeekMessageW (&msg, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}
	p->shown = GetTickCount ();
}

static VOID CALLBACK SB_Progress (LPWSTR name, [[maybe_unused]] DWORD status,
	[[maybe_unused]] PPROG_INVOKE_SETTING invoke, PVOID args, [[maybe_unused]] BOOL set)
{
	sb_progress_t	*p = args;
	DWORD			now, attributes;

	if (p->links && sb_numlinks < SB_MAXGRANTED)
	{
		attributes = GetFileAttributesW (name);
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
			SB_Copy (sb_links[sb_numlinks++], SB_PATH, name);
	}

	p->objects++;
	if (p->objects & 63)
		return;
	now = GetTickCount ();
	if (now - p->start > 300 && now - p->shown > 100)
		SB_ShowProgress (p);
}

// the DACL lets the SID (or every app container) in with those rights, and
// what is made in the directory inherits it
static bool SB_HasAccess (PACL dacl, PSID sid, DWORD rights)
{
	ACL_SIZE_INFORMATION	info;
	ACCESS_ALLOWED_ACE		*ace;
	SID_IDENTIFIER_AUTHORITY	package = SECURITY_APP_PACKAGE_AUTHORITY;
	PSID					all;
	DWORD					i;
	bool					found = false;

	if (!dacl || !GetAclInformation (dacl, &info, sizeof(info), AclSizeInformation))
		return false;
	if (!AllocateAndInitializeSid (&package, SECURITY_BUILTIN_APP_PACKAGE_RID_COUNT, SECURITY_APP_PACKAGE_BASE_RID,
		SECURITY_BUILTIN_PACKAGE_ANY_PACKAGE, 0, 0, 0, 0, 0, 0, &all))
		return false;
	for (i = 0 ; i < info.AceCount && !found ; i++)
	{
		if (!GetAce (dacl, i, (LPVOID *)&ace) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
			continue;
		if ((ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
			!= (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) || (ace->Mask & rights) != rights)
			continue;
		found = EqualSid ((PSID)&ace->SidStart, sid) || EqualSid ((PSID)&ace->SidStart, all);
	}
	FreeSid (all);
	return found;
}

// lets the container into the directory and all in it with those rights, or
// takes it out (REVOKE_ACCESS), gathering the links met into sb_links if
// asked; 0 or the error, and whether anything changed
static DWORD SB_SetAccess (const wchar_t *dir, PSID sid, DWORD rights, ACCESS_MODE mode, bool links, bool *changed)
{
	PSECURITY_DESCRIPTOR	sd;
	PACL					dacl, newdacl;
	EXPLICIT_ACCESSW		access = {0};
	sb_progress_t			progress = {.dir = dir, .links = links};
	wchar_t					path[SB_PATH];
	DWORD					error;

	if (changed)
		*changed = false;
	SB_Copy (path, SB_PATH, dir);
	error = GetNamedSecurityInfoW (path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);
	if (error)
		return error;
	if (mode == GRANT_ACCESS && SB_HasAccess (dacl, sid, rights))
	{
		LocalFree (sd);
		return 0;
	}
	if (changed)
		*changed = true;

	access.grfAccessPermissions = rights;
	access.grfAccessMode = mode;
	access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	access.Trustee.ptstrName = sid;
	error = SetEntriesInAclW (1, &access, dacl, &newdacl);
	LocalFree (sd);
	if (error)
		return error;

	// the new DACL on the directory, and what it lets inherit on all in it
	progress.start = GetTickCount ();
	error = TreeSetNamedSecurityInfoW (path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, newdacl, NULL,
		TREE_SEC_INFO_SET, SB_Progress, ProgressInvokeEveryObject, &progress);
	LocalFree (newdacl);
	if (progress.window)
		DestroyWindow (progress.window);
	return error;
}

static void SB_Remember (const wchar_t *dir)
{
	int		i;

	for (i = 0 ; i < sb_state.numgranted ; i++)
		if (!_wcsicmp (sb_state.granted[i], dir))
			return;
	if (sb_state.numgranted < SB_MAXGRANTED)
		SB_Copy (sb_state.granted[sb_state.numgranted++], SB_PATH, dir);
}

// where a link (a junction, a symbolic link) leads; false for a reparse point
// that leads nowhere else (a file in the cloud)
static bool SB_LinkTarget (const wchar_t *link, wchar_t *target, size_t size)
{
	HANDLE	h;
	wchar_t	final[SB_PATH];
	DWORD	len;

	h = CreateFileW (link, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	len = GetFinalPathNameByHandleW (h, final, SB_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle (h);
	if (!len || len >= SB_PATH)
		return false;
	if (!wcsncmp (final, L"\\\\?\\UNC\\", 8))
		swprintf (target, size, L"\\\\%ls", final + 8);
	else if (!wcsncmp (final, L"\\\\?\\", 4))
		SB_Copy (target, size, final + 4);
	else
		SB_Copy (target, size, final);
	return _wcsicmp (target, link) != 0;
}

/*
================
SB_Grant

Lets the container into the game directory and all in it, the places its links
lead to included, and remembers what it let it into
================
*/
static bool SB_Grant (const wchar_t *dir, PSID sid)
{
	static wchar_t	queue[SB_MAXGRANTED][SB_PATH];
	wchar_t			target[SB_PATH];
	int				count = 1, i, j, k;
	DWORD			error;
	bool			changed;

	SB_Copy (queue[0], SB_PATH, dir);
	for (i = 0 ; i < count ; i++)
	{
		sb_numlinks = 0;
		error = SB_SetAccess (queue[i], sid, SB_GAMERIGHTS, GRANT_ACCESS, true, &changed);
		if (error && i == 0)
		{
			SB_Fail (L"SoftWorld can't let its sandbox into %ls: %ls\n\nChoose a directory of your own.", dir,
				SB_ErrorText (error));
			return false;
		}
		// a place a link leads to that can't be let in stays out: the game says it can't open what is there
		if (error)
			continue;
		if (i == 0 || changed)
			SB_Remember (queue[i]);
		for (j = 0 ; j < sb_numlinks && count < SB_MAXGRANTED ; j++)
		{
			if (!SB_LinkTarget (sb_links[j], target, SB_PATH))
				continue;
			for (k = 0 ; k < count && _wcsicmp (queue[k], target) ; k++)
				;
			if (k == count)
				SB_Copy (queue[count++], SB_PATH, target);
		}
	}
	return true;
}

// takes the container out of every directory it was let into
static void SB_RevokeAll (PSID sid)
{
	int		i;

	for (i = 0 ; i < sb_state.numgranted ; i++)
		SB_SetAccess (sb_state.granted[i], sid, SB_GAMERIGHTS, REVOKE_ACCESS, false, NULL);
	sb_state.numgranted = 0;
}

/*
===============================================================================

THE GAME DIRECTORY

===============================================================================
*/

// asks for the game directory until one with the game in it is chosen; false
// if the player would rather not
static bool SB_ChooseDir (wchar_t *dir, size_t size)
{
	IFileOpenDialog	*dialog;
	IShellItem		*item;
	PWSTR			path;
	bool			chosen = false;

	if (FAILED (CoCreateInstance (&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
		(void **)&dialog)))
		return false;
	IFileOpenDialog_SetOptions (dialog, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	IFileOpenDialog_SetTitle (dialog, L"Choose your Quake directory: the one with id1\\pak0.pak in it, and qw and the mods");
	IFileOpenDialog_SetOkButtonLabel (dialog, L"Play");
	while (!chosen && SUCCEEDED (IFileOpenDialog_Show (dialog, NULL)))
	{
		if (FAILED (IFileOpenDialog_GetResult (dialog, &item)))
			break;
		if (SUCCEEDED (IShellItem_GetDisplayName (item, SIGDN_FILESYSPATH, &path)))
		{
			if (SB_IsGameDir (path))
				chosen = SB_FullPath (path, dir, size);
			else
				MessageBoxW (NULL, L"That isn't a Quake directory.\n\nChoose the directory with id1\\pak0.pak in it.",
					L"SoftWorld", MB_OK | MB_ICONWARNING);
			CoTaskMemFree (path);
		}
		IShellItem_Release (item);
	}
	IFileOpenDialog_Release (dialog);
	return chosen;
}

// -basedir's value, as Sys_WinMain splits the command line (on spaces; quotes
// are kept)
static bool SB_ArgValue (const char *cmdline, const char *name, wchar_t *value, size_t size)
{
	char		word[SB_PATH];
	const char	*s = cmdline;
	size_t		len;
	bool		next = false;

	while (*s)
	{
		while (*s && (*s <= 32 || *s > 126))
			s++;
		for (len = 0 ; s[len] > 32 && s[len] <= 126 ; len++)
			;
		if (!len)
			break;
		if (next)
		{
			snprintf (word, sizeof(word), "%.*s", (int)len, s);
			return MultiByteToWideChar (CP_UTF8, 0, word, -1, value, (int)size) != 0;
		}
		next = len == strlen (name) && !strncmp (s, name, len);
		s += len;
	}
	return false;
}

static bool SB_HasArg (const char *cmdline, const char *name)
{
	const char	*s = cmdline;
	size_t		len = strlen (name);

	while ((s = strstr (s, name)))
	{
		if ((s == cmdline || s[-1] <= 32) && (s[len] <= 32 || s[len] > 126))
			return true;
		s += len;
	}
	return false;
}

/*
===============================================================================

THE NETWORK

===============================================================================
*/

// the loopback exemptions, from FirewallAPI.dll: the SDK has no import library for it
typedef DWORD (WINAPI *sb_getconfig_t) (DWORD *count, PSID_AND_ATTRIBUTES *sids);
typedef DWORD (WINAPI *sb_setconfig_t) (DWORD count, PSID_AND_ATTRIBUTES sids);

static void (*SB_FirewallFunction (const char *name)) (void)
{
	static HMODULE	dll;

	if (!dll)
		dll = LoadLibraryExW (L"FirewallAPI.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	return dll ? (void (*)(void))GetProcAddress (dll, name) : NULL;
}

static DWORD SB_GetExemptions (DWORD *count, PSID_AND_ATTRIBUTES *sids)
{
	sb_getconfig_t	get = (sb_getconfig_t)SB_FirewallFunction ("NetworkIsolationGetAppContainerConfig");

	return get ? get (count, sids) : ERROR_PROC_NOT_FOUND;
}

static DWORD SB_SetExemptions (DWORD count, PSID_AND_ATTRIBUTES sids)
{
	sb_setconfig_t	set = (sb_setconfig_t)SB_FirewallFunction ("NetworkIsolationSetAppContainerConfig");

	return set ? set (count, sids) : ERROR_PROC_NOT_FOUND;
}

// the container is exempt from the isolation of this computer's own addresses
static bool SB_LoopbackExempt (PSID sid)
{
	PSID_AND_ATTRIBUTES	list = NULL;
	DWORD				count = 0, i;

	// the documentation names no way to free the list: kept for as long as the launcher runs
	if (SB_GetExemptions (&count, &list))
		return false;
	for (i = 0 ; i < count ; i++)
		if (EqualSid (list[i].Sid, sid))
			return true;
	return false;
}

/*
================
SB_SetUpNetwork

-sandbox-network, as an administrator: the container exempt from loopback
isolation, and a firewall rule letting players in to its server; 0 if done
================
*/
static int SB_SetUpNetwork (PSID sid)
{
	PSID_AND_ATTRIBUTES	list = NULL, all;
	DWORD				count = 0;
	INetFwPolicy2		*policy = NULL;
	INetFwRules			*rules = NULL;
	INetFwRule3			*rule = NULL;
	LPWSTR				sidtext;
	BSTR				name, description, package;
	HRESULT				hr;

	if (!SB_LoopbackExempt (sid))
	{
		if (SB_GetExemptions (&count, &list))
			return 1;
		all = calloc (count + 1, sizeof(*all));
		if (!all)
			return 1;
		if (count)
			memcpy (all, list, count * sizeof(*all));
		all[count].Sid = sid;
		if (SB_SetExemptions (count + 1, all))
			return 1;
		free (all);
	}

	if (!ConvertSidToStringSidW (sid, &sidtext))
		return 1;
	name = SysAllocString (L"SoftWorld");
	description = SysAllocString (L"Players joining the server of SoftWorld, which runs in an AppContainer sandbox");
	package = SysAllocString (sidtext);
	LocalFree (sidtext);

	hr = CoCreateInstance (&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2, (void **)&policy);
	if (SUCCEEDED (hr))
		hr = INetFwPolicy2_get_Rules (policy, &rules);
	if (SUCCEEDED (hr))
	{
		INetFwRules_Remove (rules, name);	// an earlier start's
		hr = CoCreateInstance (&CLSID_NetFwRule, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwRule3, (void **)&rule);
	}
	if (SUCCEEDED (hr))
	{
		INetFwRule3_put_Name (rule, name);
		INetFwRule3_put_Description (rule, description);
		// UDP, and TCP for browsers' clients (the WebSocket port, net_ws.c)
		INetFwRule3_put_Protocol (rule, NET_FW_IP_PROTOCOL_ANY);
		INetFwRule3_put_Direction (rule, NET_FW_RULE_DIR_IN);
		INetFwRule3_put_Action (rule, NET_FW_ACTION_ALLOW);
		INetFwRule3_put_Profiles (rule, NET_FW_PROFILE2_ALL);
		hr = INetFwRule3_put_LocalAppPackageId (rule, package);
	}
	if (SUCCEEDED (hr))
		hr = INetFwRule3_put_Enabled (rule, VARIANT_TRUE);
	if (SUCCEEDED (hr))
		hr = INetFwRules_Add (rules, (INetFwRule *)rule);

	if (rule)
		INetFwRule3_Release (rule);
	if (rules)
		INetFwRules_Release (rules);
	if (policy)
		INetFwPolicy2_Release (policy);
	SysFreeString (name);
	SysFreeString (description);
	SysFreeString (package);
	return SUCCEEDED (hr) ? 0 : 1;
}

// asks once whether an administrator lets the container reach this computer's
// own addresses and players in, and has it done
static void SB_AskNetwork (PSID sid, const wchar_t *exe)
{
	SHELLEXECUTEINFOW	run = {.cbSize = sizeof(run)};
	DWORD				code = 1;

	if (sb_state.network_allowed || sb_state.network_declined || SB_LoopbackExempt (sid))
		return;

	if (MessageBoxW (NULL, L"SoftWorld runs in a sandbox, which keeps it from this computer's own addresses: "
		L"a server or a QTV proxy on 127.0.0.1, or a client here joining SoftWorld's own server. Players can't "
		L"join its server through the firewall either.\n\nWindows lets it, once an administrator allows it. "
		L"Allow it now?", L"SoftWorld", MB_YESNO | MB_ICONQUESTION) == IDYES)
	{
		run.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
		run.lpVerb = L"runas";
		run.lpFile = exe;
		run.lpParameters = L"" SB_NETWORKARG;
		run.nShow = SW_HIDE;
		if (ShellExecuteExW (&run) && run.hProcess)
		{
			WaitForSingleObject (run.hProcess, INFINITE);
			GetExitCodeProcess (run.hProcess, &code);
			CloseHandle (run.hProcess);
		}
	}
	if (code == 0)
		sb_state.network_allowed = true;
	else
		sb_state.network_declined = true;
}

/*
===============================================================================

THE CURSOR

The container may not confine the cursor (ClipCursor needs the window station
written, and is denied), so the launcher does it for the game: the game writes
the rectangle to a block they share and signals an event, both handles the
game inherits and finds in SW_SANDBOX_CURSOR. The launcher confines it only
while the game's window is in front and only within that window, and frees it
when the game ends, however it ends.

===============================================================================
*/

#define SB_CURSORVAR	L"SW_SANDBOX_CURSOR"

typedef struct
{
	RECT			rect;		// in screen coordinates
	volatile LONG	clip;		// confined to rect, else free
} sb_cursor_t;

static sb_cursor_t	*sb_cursor;			// the shared block, the game's or the launcher's view of it
static HANDLE		sb_cursorevent;		// the game signals it after writing the block

/*
================
Sys_ClipCursor

Confines the cursor to rect, in screen coordinates, or frees it (NULL): in the
container through the launcher
================
*/
void Sys_ClipCursor (const RECT *rect)
{
	static bool			opened;
	wchar_t				value[64];
	unsigned long long	block, event;

	if (!opened)
	{
		opened = true;
		if (SB_InContainer () && GetEnvironmentVariableW (SB_CURSORVAR, value, 64)
			&& swscanf (value, L"%llu %llu", &block, &event) == 2)
		{
			sb_cursor = MapViewOfFile ((HANDLE)(uintptr_t)block, FILE_MAP_WRITE, 0, 0, sizeof(*sb_cursor));
			sb_cursorevent = (HANDLE)(uintptr_t)event;
		}
	}
	if (!sb_cursor)
	{
		ClipCursor (rect);
		return;
	}
	if (rect)
		sb_cursor->rect = *rect;
	InterlockedExchange (&sb_cursor->clip, rect != NULL);
	SetEvent (sb_cursorevent);
}

// the launcher: the cursor as the game last asked, if its window is in front;
// freed only from its own confinement, not another program's set since
static void SB_ClipForGame (DWORD game)
{
	static RECT	clipped;		// as the system has it, clamped to the screens
	static bool	clipping;
	HWND		front = GetForegroundWindow ();
	DWORD		owner = 0;
	RECT		window, rect, now;

	if (front)
		GetWindowThreadProcessId (front, &owner);
	if (InterlockedCompareExchange (&sb_cursor->clip, 0, 0) && owner == game && GetWindowRect (front, &window)
		&& IntersectRect (&rect, &sb_cursor->rect, &window) && ClipCursor (&rect))
	{
		clipping = GetClipCursor (&clipped) != FALSE;
		return;
	}
	if (clipping && GetClipCursor (&now) && EqualRect (&now, &clipped))
		ClipCursor (NULL);
	clipping = false;
}

/*
===============================================================================

STARTING THE GAME

===============================================================================
*/

// the program again, inside the container, in a job that ends it with the
// launcher; its exit code
static int SB_Run (const wchar_t *exe, PSID sid, const wchar_t *cwd)
{
	SID_AND_ATTRIBUTES		caps[SB_NUMCAPS];
	SECURITY_CAPABILITIES	security = {.AppContainerSid = sid, .Capabilities = caps};
	STARTUPINFOEXW			si = {.StartupInfo.cb = sizeof(si)};
	STARTUPINFOW			own;
	PROCESS_INFORMATION		pi = {0};
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION	limits = {0};
	SECURITY_ATTRIBUTES		inherited = {.nLength = sizeof(inherited), .bInheritHandle = TRUE};
	SIZE_T					size = 0;
	HANDLE					job, block, handles[2];
	wchar_t					*cmdline, value[64];
	DWORD					code = 1, i;
	BOOL					started;

	for (i = 0 ; i < SB_NUMCAPS ; i++)
	{
		caps[i].Attributes = SE_GROUP_ENABLED;
		if (!ConvertStringSidToSidW (sb_capabilities[i], &caps[i].Sid))
			return SB_Fail (L"SoftWorld can't make its sandbox: %ls", SB_ErrorText (GetLastError ()));
		security.CapabilityCount++;
	}

	// the cursor's block and event, the only handles the game inherits
	block = CreateFileMappingW (INVALID_HANDLE_VALUE, &inherited, PAGE_READWRITE, 0, sizeof(sb_cursor_t), NULL);
	sb_cursor = block ? MapViewOfFile (block, FILE_MAP_WRITE, 0, 0, sizeof(sb_cursor_t)) : NULL;
	sb_cursorevent = CreateEventW (&inherited, FALSE, FALSE, NULL);
	if (!sb_cursor || !sb_cursorevent)
		return SB_Fail (L"SoftWorld can't make its sandbox: %ls", SB_ErrorText (GetLastError ()));
	handles[0] = block;
	handles[1] = sb_cursorevent;
	swprintf (value, 64, L"%llu %llu", (unsigned long long)(uintptr_t)block,
		(unsigned long long)(uintptr_t)sb_cursorevent);
	SetEnvironmentVariableW (SB_CURSORVAR, value);

	InitializeProcThreadAttributeList (NULL, 2, 0, &size);
	si.lpAttributeList = malloc (size);
	if (!si.lpAttributeList || !InitializeProcThreadAttributeList (si.lpAttributeList, 2, 0, &size)
		|| !UpdateProcThreadAttribute (si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
			&security, sizeof(security), NULL, NULL)
		|| !UpdateProcThreadAttribute (si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			handles, sizeof(handles), NULL, NULL))
		return SB_Fail (L"SoftWorld can't make its sandbox: %ls", SB_ErrorText (GetLastError ()));

	// shown as the launcher was asked to be
	GetStartupInfoW (&own);
	si.StartupInfo.dwFlags = own.dwFlags & STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = own.wShowWindow;

	job = CreateJobObjectW (NULL, NULL);
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (job)
		SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

	cmdline = _wcsdup (GetCommandLineW ());
	started = cmdline && CreateProcessW (exe, cmdline, NULL, NULL, TRUE,
		EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, NULL, cwd, &si.StartupInfo, &pi);
	if (!started)
		code = (DWORD)SB_Fail (L"SoftWorld can't start in its sandbox: %ls", SB_ErrorText (GetLastError ()));
	else
	{
		if (job)
			AssignProcessToJobObject (job, pi.hProcess);
		AllowSetForegroundWindow (pi.dwProcessId);
		ResumeThread (pi.hThread);
		CloseHandle (pi.hThread);

		// the cursor as the game asks, until it ends; then free
		handles[0] = pi.hProcess;
		handles[1] = sb_cursorevent;
		while (WaitForMultipleObjects (2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
			SB_ClipForGame (pi.dwProcessId);
		InterlockedExchange (&sb_cursor->clip, 0);
		SB_ClipForGame (pi.dwProcessId);

		GetExitCodeProcess (pi.hProcess, &code);
		CloseHandle (pi.hProcess);
	}

	free (cmdline);
	DeleteProcThreadAttributeList (si.lpAttributeList);
	free (si.lpAttributeList);
	for (i = 0 ; i < security.CapabilityCount ; i++)
		LocalFree (caps[i].Sid);
	if (job)
		CloseHandle (job);
	UnmapViewOfFile (sb_cursor);
	CloseHandle (block);
	CloseHandle (sb_cursorevent);
	return (int)code;
}

/*
================
SB_Launch

The game directory, the container let into it, and the game run inside; its
exit code
================
*/
static int SB_Launch (const char *cmdline)
{
	PSID		sid;
	HRESULT		hr;
	wchar_t		exe[SB_PATH], exedir[SB_PATH], statepath[SB_PATH], arg[SB_PATH];
	static wchar_t	gamedir[SB_PATH];
	const wchar_t	*cwd = NULL;
	wchar_t		*slash;
	DWORD		len, error;
	int			code;

	CoInitializeEx (NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	hr = CreateAppContainerProfile (SB_CONTAINER, L"SoftWorld", L"SoftWorld, a QuakeWorld client", NULL, 0, &sid);
	if (hr == HRESULT_FROM_WIN32 (ERROR_ALREADY_EXISTS))
		hr = DeriveAppContainerSidFromAppContainerName (SB_CONTAINER, &sid);
	if (FAILED (hr))
		return SB_Fail (L"SoftWorld can't make its sandbox: %ls", SB_ErrorText ((DWORD)hr));

	if (SB_HasArg (cmdline, SB_NETWORKARG))
	{
		code = SB_SetUpNetwork (sid);
		FreeSid (sid);
		return code;
	}

	len = GetModuleFileNameW (NULL, exe, SB_PATH);
	if (!len || len >= SB_PATH)
		return SB_Fail (L"SoftWorld can't find its own program");
	SB_Copy (exedir, SB_PATH, exe);
	slash = wcsrchr (exedir, L'\\');
	if (slash)
		*slash = 0;

	if (!SB_StatePath (sid, statepath, SB_PATH))
		return SB_Fail (L"SoftWorld can't find its sandbox's folder");
	SB_LoadState (statepath);

	if (SB_ArgValue (cmdline, "-basedir", arg, SB_PATH))
	{
		// named on the command line: let in, not remembered; the game resolves
		// it from the same working directory
		if (!SB_FullPath (arg, gamedir, SB_PATH) || GetFileAttributesW (gamedir) == INVALID_FILE_ATTRIBUTES)
			return SB_Fail (L"-basedir names %ls, which can't be found", arg);
		if (!SB_Grant (gamedir, sid))
			return 1;
	}
	else
	{
		if (!sb_state.basedir[0] || !SB_IsGameDir (sb_state.basedir))
		{
			// forgotten, or gone: out of every directory, then the one chosen
			SB_RevokeAll (sid);
			sb_state.basedir[0] = 0;
			SB_SaveState (statepath);
			if (!SB_ChooseDir (sb_state.basedir, SB_PATH))
				return 0;
		}
		SB_Copy (gamedir, SB_PATH, sb_state.basedir);
		if (!SB_Grant (gamedir, sid))
			return 1;
		cwd = gamedir;		// Sys_WinMain takes it as the base directory
	}

	// the program's own directory, to run it
	error = SB_SetAccess (exedir, sid, SB_EXERIGHTS, GRANT_ACCESS, false, NULL);
	if (error)
		return SB_Fail (L"SoftWorld can't let its sandbox run it from %ls: %ls\n\nMove SoftWorld to a directory "
			L"of your own.", exedir, SB_ErrorText (error));

	SB_AskNetwork (sid, exe);
	SB_SaveState (statepath);

	code = SB_Run (exe, sid, cwd);
	FreeSid (sid);
	return code;
}

/*
================
Sys_SandboxLaunch

Outside the container this is the launcher: the game's exit code, whatever it
is (a crash's is negative). Inside, this process is the game.
================
*/
bool Sys_SandboxLaunch (const char *cmdline, int *code)
{
	if (SB_InContainer ())
		return false;
	*code = SB_Launch (cmdline);
	return true;
}
