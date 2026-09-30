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
// sys_mac_gui.m -- the macOS system interface for the programs with a client: the
// application, its game directory, its event loop and its waits, errors, and the
// clipboard
//
// The applications run in the App Sandbox: the game directory is one the player
// chooses, on the first start, and a security-scoped bookmark of it in the user
// defaults opens it again on the next (sys_forget_sandbox forgets it). The
// defaults, not a cvar: the cvars' config.cfg is in that directory.
//
// The main loop is the game's, as on Windows; events are taken from the queue
// between frames. The wait for the next frame is AppKit's wait for an event,
// with the kqueue (sockets, and the timer that ends the wait) as a run loop
// source that posts an event when it has something.

#import <AppKit/AppKit.h>

#include "args.h"
#include "cmd.h"
#include "host.h"
#include "print.h"
#include "sys.h"
#include "sound.h"
#include "entry.h"
#include "mac_local.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool	ActiveApp, Minimized;

#define SYS_WAKE_EVENT	1	// the subtype of the event the kqueue posts

static int		sys_woken;			// what the kqueue had (SYS_WAIT_*), since the last wait
static bool		sys_wakeposted;		// a wake event is in the queue

@interface SWApplicationDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SWApplicationDelegate

// Command+Q and the Dock's Quit ask the game, which asks the player
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
	(void)sender;
	Cbuf_AddText ("quit\n");
	return NSTerminateCancel;
}

@end

static SWApplicationDelegate	*sys_delegate;

/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];
	static bool	inerror;

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	// the console and its log see the error; an error while shutting down
	// goes straight to the alert
	if (!inerror)
	{
		inerror = true;
		Con_Printf ("Sys_Error: %s\n", text);
		Host_Shutdown ();
	}
	fprintf (stderr, "Error: %s\n", text);

	@autoreleasepool
	{
		NSAlert	*alert = [NSAlert new];

		alert.messageText = @"SoftWorld";
		alert.informativeText = [[NSString alloc] initWithBytes:text length:strlen (text)
			encoding:NSISOLatin1StringEncoding];
		alert.alertStyle = NSAlertStyleCritical;
		[NSApp activate];
		[alert runModal];
	}

	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;

	va_start (argptr, fmt);
	vprintf (fmt, argptr);
	va_end (argptr);
}

void Sys_Quit (void)
{
	Host_Shutdown ();

	exit (0);
}

/*
================
Sys_GetClipboardText
================
*/
char *Sys_GetClipboardText (void)
{
	@autoreleasepool
	{
		NSString	*text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];

		return text ? strdup (text.UTF8String) : NULL;
	}
}

/*
===============================================================================

EVENTS

===============================================================================
*/

void Sys_SendKeyEvents (void)
{
	NSEvent	*event;

	@autoreleasepool
	{
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
				inMode:NSDefaultRunLoopMode dequeue:YES]))
		{
			if (event.type == NSEventTypeApplicationDefined && event.subtype == SYS_WAKE_EVENT)
			{
				sys_wakeposted = false;
				continue;
			}
			if (IN_HandleEvent (event))
				continue;
			[NSApp sendEvent:event];
		}
	}
}

// the kqueue has something: note what, and wake the wait for an event
static void Sys_QueueReady (CFFileDescriptorRef descriptor, CFOptionFlags flags, void *info)
{
	(void)flags;
	(void)info;
	sys_woken |= Sys_ReadWaitQueue (false);
	CFFileDescriptorEnableCallBacks (descriptor, kCFFileDescriptorReadCallBack);

	if (!sys_wakeposted)
	{
		[NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
			modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:SYS_WAKE_EVENT data1:0 data2:0]
			atStart:YES];
		sys_wakeposted = true;
	}
}

static void Sys_WatchWaitQueue (void)
{
	CFFileDescriptorRef	descriptor;
	CFRunLoopSourceRef	source;

	descriptor = CFFileDescriptorCreate (NULL, Sys_WaitQueue (), false, Sys_QueueReady, NULL);
	if (!descriptor)
		Sys_Error ("Couldn't watch the kqueue");
	CFFileDescriptorEnableCallBacks (descriptor, kCFFileDescriptorReadCallBack);
	source = CFFileDescriptorCreateRunLoopSource (NULL, descriptor, 0);
	CFRunLoopAddSource (CFRunLoopGetMain (), source, kCFRunLoopCommonModes);
	CFRelease (source);
}

/*
================
Sys_WaitEvents

An event for the window, a packet, or the kqueue's timer at until; a packet
that came since the last wait ends this one at once, as a Windows socket
event stays set until it is waited on
================
*/
bool Sys_WaitEvents (double until)
{
	NSEvent	*event;
	bool	early;

	sys_woken |= Sys_ReadWaitQueue (false);
	if (sys_woken & (SYS_WAIT_FD | SYS_WAIT_SIGNAL))
	{
		sys_woken = 0;
		return true;
	}
	sys_woken = 0;

	Sys_SetWaitTimer (until);
	for (;;)
	{
		@autoreleasepool
		{
			// the timer ends the wait; the date only if the kqueue couldn't
			event = [NSApp nextEventMatchingMask:NSEventMaskAny
				untilDate:[NSDate dateWithTimeIntervalSinceNow:until - Sys_DoubleTime () + 0.05]
				inMode:NSDefaultRunLoopMode dequeue:NO];
			if (!event)
			{
				early = false;
				break;
			}
			if (event.type != NSEventTypeApplicationDefined || event.subtype != SYS_WAKE_EVENT)
			{
				early = true;		// input for the window
				break;
			}
			[NSApp nextEventMatchingMask:NSEventMaskApplicationDefined untilDate:[NSDate distantPast]
				inMode:NSDefaultRunLoopMode dequeue:YES];
			sys_wakeposted = false;
		}
		if (sys_woken & (SYS_WAIT_FD | SYS_WAIT_SIGNAL))
		{
			early = true;
			break;
		}
		if (sys_woken & SYS_WAIT_TIMER)
		{
			early = false;
			break;
		}
		// the kqueue had nothing new: wait on
	}
	if (!(sys_woken & SYS_WAIT_TIMER))
		Sys_ClearWaitTimer ();
	sys_woken = 0;
	return early;
}

/*
===============================================================================

THE GAME DIRECTORY

===============================================================================
*/

#define SYS_BASEDIR_KEY		@"basedir"	// the user default holding the directory's bookmark

static void Sys_Alert (NSString *title, NSString *text)
{
	NSAlert	*alert = [NSAlert new];

	alert.messageText = title;
	alert.informativeText = text;
	[NSApp activate];
	[alert runModal];
}

// the directory holds the game
static bool Sys_IsGameDir (NSURL *dir)
{
	return [NSFileManager.defaultManager fileExistsAtPath:[dir URLByAppendingPathComponent:@"id1/pak0.pak"].path];
}

static bool Sys_SaveBookmark (NSURL *dir)
{
	NSError	*error = nil;
	NSData	*bookmark = [dir bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
		includingResourceValuesForKeys:nil relativeToURL:nil error:&error];

	if (!bookmark)
	{
		Sys_Alert (@"The game directory can't be remembered", error.localizedDescription);
		return false;
	}
	[NSUserDefaults.standardUserDefaults setObject:bookmark forKey:SYS_BASEDIR_KEY];
	return true;
}

// the directory chosen before, opened for as long as the program runs; nil if
// none was, or it can't be opened
static NSURL *Sys_BookmarkedDir (void)
{
	NSData	*bookmark = [NSUserDefaults.standardUserDefaults dataForKey:SYS_BASEDIR_KEY];
	NSError	*error = nil;
	BOOL	stale = NO;
	NSURL	*dir;

	if (!bookmark)
		return nil;
	dir = [NSURL URLByResolvingBookmarkData:bookmark options:NSURLBookmarkResolutionWithSecurityScope
		relativeToURL:nil bookmarkDataIsStale:&stale error:&error];
	if (!dir)
	{
		Sys_Printf ("The game directory remembered can't be found: %s\n", error.localizedDescription.UTF8String);
		return nil;
	}
	if (![dir startAccessingSecurityScopedResource])
	{
		Sys_Printf ("The game directory remembered, %s, can't be opened\n", dir.fileSystemRepresentation);
		return nil;
	}
	// moved or renamed: the bookmark is made again from where it is now
	if (stale)
		Sys_SaveBookmark (dir);
	return dir;
}

// asks for the game directory until one with the game in it is chosen, and
// remembers it; nil if the player would rather not
static NSURL *Sys_ChooseDir (void)
{
	NSOpenPanel	*panel = [NSOpenPanel openPanel];
	NSURL		*dir;

	Sys_Printf ("Asking for the game directory\n");
	panel.canChooseFiles = NO;
	panel.canChooseDirectories = YES;
	panel.allowsMultipleSelection = NO;
	panel.message = @"Choose your Quake directory: the one with id1/pak0.pak in it, and qw and the mods.";
	panel.prompt = @"Play";
	[NSApp activate];
	while ([panel runModal] == NSModalResponseOK)
	{
		dir = panel.URL;
		if (Sys_IsGameDir (dir))
		{
			Sys_SaveBookmark (dir);
			return dir;
		}
		Sys_Alert (@"That isn't a Quake directory", @"Choose the directory with id1/pak0.pak in it.");
	}
	return nil;
}

/*
================
Sys_GameDir

The game directory: the one remembered, or else one the player chooses. With
-basedir that is the directory (inside the one remembered: the sandbox lets
the program into nothing else), and nothing is asked.
================
*/
static const char *Sys_GameDir (void)
{
	static char	path[MAX_OSPATH];
	NSURL		*dir;
	FILE		*f;
	int			i;

	@autoreleasepool
	{
		dir = Sys_BookmarkedDir ();
		if ((i = COM_CheckParm ("-basedir")) && i + 1 < com_argc)
		{
			// fs.c takes -basedir's; a directory the sandbox keeps the program out of is said so
			snprintf (path, sizeof(path), "%s/id1/pak0.pak", com_argv[i + 1]);
			f = fopen (path, "rb");
			if (!f && (errno == EPERM || errno == EACCES))
			{
				Sys_Printf ("The sandbox keeps SoftWorld out of %s\n", com_argv[i + 1]);
				Sys_Alert (@"SoftWorld can't open that directory",
					[NSString stringWithFormat:@"The sandbox keeps it out of %s: -basedir must be inside the game "
						"directory chosen. Start SoftWorld without -basedir to choose one (sys_forget_sandbox "
						"forgets the one chosen).", com_argv[i + 1]]);
				exit (1);
			}
			if (f)
				fclose (f);
			return ".";
		}

		if (dir && !Sys_IsGameDir (dir))
		{
			Sys_Printf ("The game directory remembered, %s, has no id1/pak0.pak\n", dir.fileSystemRepresentation);
			dir = nil;
		}
		if (!dir)
			dir = Sys_ChooseDir ();
		if (!dir)
			exit (0);

		// relative paths go there too, as they do on Windows
		snprintf (path, sizeof(path), "%s", dir.fileSystemRepresentation);
		chdir (path);
		return path;
	}
}

// the next start asks for the game directory again; this one keeps it open
static void Sys_ForgetSandbox_f (void)
{
	if (![NSUserDefaults.standardUserDefaults dataForKey:SYS_BASEDIR_KEY])
	{
		Con_Printf ("No game directory is remembered.\n");
		return;
	}
	[NSUserDefaults.standardUserDefaults removeObjectForKey:SYS_BASEDIR_KEY];
	Con_Printf ("The game directory is forgotten: the next start asks for one.\n");
}

/*
===============================================================================

MAIN

===============================================================================
*/

static void Sys_CreateMenu (void)
{
	NSMenu		*bar = [NSMenu new], *app = [NSMenu new];
	NSMenuItem	*item = [NSMenuItem new];

	[app addItemWithTitle:@"Hide SoftWorld" action:@selector(hide:) keyEquivalent:@"h"];
	[app addItem:[NSMenuItem separatorItem]];
	[app addItemWithTitle:@"Quit SoftWorld" action:@selector(terminate:) keyEquivalent:@"q"];
	item.submenu = app;
	[bar addItem:item];
	NSApp.mainMenu = bar;
}

int Sys_MacMain (int argc, char **argv)
{
	quakeparms_t	parms;
	double			time, oldtime, newtime;

	// the console's echo a line at a time, also into a pipe or a file
	setvbuf (stdout, NULL, _IOLBF, 0);

	@autoreleasepool
	{
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		sys_delegate = [SWApplicationDelegate new];
		NSApp.delegate = sys_delegate;
		Sys_CreateMenu ();
		[NSApp finishLaunching];
	}

	COM_InitArgv (argc, argv);
	parms.argc = com_argc;
	parms.argv = com_argv;
	parms.basedir = (char *)Sys_GameDir ();
	parms.cachedir = NULL;
	Cmd_AddCommand ("sys_forget_sandbox", Sys_ForgetSandbox_f,
		"Forgets the remembered Quake directory, so the next start asks for it again.");

	Sys_WatchWaitQueue ();

// because sound is off until we become active
	S_BlockSound ();

	Sys_Printf ("Host_Init\n");
	@autoreleasepool
	{
		Host_Init (&parms);
	}

	oldtime = Sys_DoubleTime ();

	/* main loop */
	while (1)
	{
		@autoreleasepool
		{
			newtime = Sys_DoubleTime ();
			time = newtime - oldtime;
			Host_Frame (time);
			oldtime = newtime;

			// handle what arrived meanwhile, then sleep until the next frame is due,
			// unless input or a packet comes first
			Sys_SendKeyEvents ();
			Sys_WaitUntil (Sys_DoubleTime () + Host_FrameWait ());
		}
	}
}
