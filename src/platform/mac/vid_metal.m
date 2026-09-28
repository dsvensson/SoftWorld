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
// vid_metal.m -- macOS video backend: the main window, and Metal 4 presenting the
// software renderer's frame in it.
//
// The renderer draws into memory the GPU reads: on Apple silicon a shared buffer is
// the memory the CPU draws in, so the frame isn't uploaded. The 3D view and its 2D
// layer are each a buffer with a linear texture over it (RGB10A2, as vid.h's pixels
// are, and RGBA8), and present.metal lays them into the letterboxed viewport as
// present.hlsl does (vid_common.c has the fit and the constants).
//
// The GPU reads a layer after VID_Update has returned, so there are two of each: the
// view is drawn into them in turn, and the 2D goes to the one not shown when it is
// drawn again (Draw_Flush redraws all of it). Each remembers the last frame that
// read it, and goes to the renderer only once the GPU has finished that frame.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "cmd.h"
#include "print.h"
#include "sys.h"
#include "sound.h"
#include "vid_common.h"
#include "mac_local.h"

#include <math.h>
#include <stdatomic.h>

// present.metal compiled (present_metallib.c)
extern const unsigned char	vid_present_metallib[];
extern const size_t			vid_present_metallib_size;

#define VID_SLOTS		2		// frames in flight at most, and so view buffers
#define VID_CONSTANTS	256		// bytes from one slot's constants to the next's
#define VID_HUNG_MS		2000	// a frame the GPU hasn't finished in this long won't be

typedef struct
{
	id<MTLBuffer>	buffer;		// what the renderer draws in
	id<MTLTexture>	texture;	// the same memory, for the shader
	uint64_t		serial;		// the last frame that read it
} vid_layer_t;

@interface SWView : NSView
@end

@interface SWWindowDelegate : NSObject <NSWindowDelegate>
@end

NSWindow				*vid_window;
static SWView			*vid_view;
static CAMetalLayer		*vid_metal;
static SWWindowDelegate	*vid_delegate;

static id<MTLDevice>				mtl_device;
static id<MTL4CommandQueue>			mtl_queue;
static id<MTL4CommandAllocator>		mtl_allocators[VID_SLOTS];
static id<MTL4CommandBuffer>		mtl_commands[VID_SLOTS];
static id<MTL4ArgumentTable>		mtl_arguments;
static id<MTLRenderPipelineState>	mtl_sdrpipeline;
static id<MTLResidencySet>			mtl_residency;
static id<MTLSharedEvent>			mtl_event;		// signaled with each frame's serial when it is done
static uint64_t						mtl_serial;		// the last frame committed
static id<MTLBuffer>				mtl_constants;	// vid_present_constants_t, a slot's each

static vid_layer_t	vid_views[VID_SLOTS];	// the 3D view
static vid_layer_t	vid_huds[2];			// the 2D
static int			vid_slot;				// vid.buffer's view
static int			vid_hudshown;			// the 2D shown; vid.hud is the other
static int			vid_drawnslot;			// the view last drawn, presented or not
static int			vid_drawnhud;			// and its 2D

static bool		vid_initialized;
static bool		vid_fullscreen;
static int		vid_latency;				// frames in flight: 1 with vsync, 2 without
static _Atomic uint64_t	vid_onscreen;		// the last frame the display has shown
static double	vid_presenttime;			// when the last frame was presented
static double	vid_acquiretime;			// when the last drawable was had
static double	vid_gatedtime;				// when waiting for a drawable last took long
static int		client_width, client_height;	// the view, in pixels

static void VID_SetScale (void);

/*
===============================================================================

THE WINDOW

===============================================================================
*/

static void VID_AppActivate (bool active)
{
	static bool	sound_active;		// sound starts blocked (sys_mac_gui.m), until the window is active

	if (active == ActiveApp)
		return;
	ActiveApp = active;

// enable/disable sound on focus gain/loss
	if (!ActiveApp && sound_active)
	{
		S_BlockSound ();
		S_ClearBuffer ();
		sound_active = false;
	}
	else if (ActiveApp && !sound_active)
	{
		S_UnblockSound ();
		S_ClearBuffer ();
		sound_active = true;
	}

	IN_WindowActivated (ActiveApp);
}

// minimized, or wholly covered or on another space: nothing to draw into, and
// waiting for a drawable would hold up the loop (and a server in it)
static void VID_CheckMinimized (void)
{
	Minimized = vid_window.miniaturized || !(vid_window.occlusionState & NSWindowOcclusionStateVisible);
}

@implementation SWView

- (instancetype)initWithFrame:(NSRect)frame
{
	self = [super initWithFrame:frame];
	if (self)
		self.wantsLayer = YES;
	return self;
}

- (CALayer *)makeBackingLayer
{
	return [CAMetalLayer layer];
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (void)updateLayer
{
}

- (BOOL)acceptsFirstResponder
{
	return YES;
}

@end

@implementation SWWindowDelegate

// the close button asks the game, which asks the player
- (BOOL)windowShouldClose:(NSWindow *)sender
{
	(void)sender;
	Cbuf_AddText ("quit\n");
	return NO;
}

- (void)windowDidBecomeKey:(NSNotification *)notification
{
	(void)notification;
	VID_AppActivate (true);
}

- (void)windowDidResignKey:(NSNotification *)notification
{
	(void)notification;
	VID_AppActivate (false);
}

- (void)windowDidMiniaturize:(NSNotification *)notification
{
	(void)notification;
	VID_CheckMinimized ();
}

- (void)windowDidDeminiaturize:(NSNotification *)notification
{
	(void)notification;
	VID_CheckMinimized ();
}

- (void)windowDidChangeOcclusionState:(NSNotification *)notification
{
	(void)notification;
	VID_CheckMinimized ();
}

- (void)windowDidResize:(NSNotification *)notification
{
	(void)notification;
	IN_WindowChanged ();
}

- (void)windowDidMove:(NSNotification *)notification
{
	(void)notification;
	IN_WindowChanged ();
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification
{
	(void)notification;
	vid_metal.contentsScale = vid_window.backingScaleFactor;
}

@end

/*
================
VID_CreateWindow

The largest whole multiple of the render size, in pixels, that fits the screen
================
*/
static void VID_CreateWindow (int width, int height)
{
	NSWindowStyleMask	style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
		| NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
	NSScreen	*screen = NSScreen.mainScreen;
	CGFloat		backing = screen ? screen.backingScaleFactor : 1;
	NSRect		visible = screen ? screen.visibleFrame : NSMakeRect (0, 0, 1280, 800);
	NSRect		content, frame;
	CGColorSpaceRef	srgb;
	int			scale;

	for (scale = 4 ; scale > 1 ; scale--)
	{
		content = NSMakeRect (0, 0, width * scale / backing, height * scale / backing);
		frame = [NSWindow frameRectForContentRect:content styleMask:style];
		if (frame.size.width <= visible.size.width && frame.size.height <= visible.size.height)
			break;
	}
	content = NSMakeRect (0, 0, width * scale / backing, height * scale / backing);

	vid_window = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered
		defer:NO];
	if (!vid_window)
		Sys_Error ("Couldn't create the main window");
	vid_window.releasedWhenClosed = NO;
	vid_window.title = @"SoftWorld";
	vid_window.backgroundColor = NSColor.blackColor;
	vid_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
	vid_delegate = [SWWindowDelegate new];
	vid_window.delegate = vid_delegate;

	vid_view = [[SWView alloc] initWithFrame:content];
	vid_window.contentView = vid_view;
	[vid_window makeFirstResponder:vid_view];
	[vid_window center];

	vid_metal = (CAMetalLayer *)vid_view.layer;
	vid_metal.device = mtl_device;
	vid_metal.pixelFormat = MTLPixelFormatBGR10A2Unorm;
	vid_metal.framebufferOnly = YES;
	vid_metal.opaque = YES;
	vid_metal.contentsScale = vid_window.backingScaleFactor;
	srgb = CGColorSpaceCreateWithName (kCGColorSpaceSRGB);
	vid_metal.colorspace = srgb;
	CGColorSpaceRelease (srgb);
	[mtl_queue addResidencySet:vid_metal.residencySet];
}

// the view's size in pixels; the drawables are made that size
static void VID_UpdateClientSize (void)
{
	NSSize	size = [vid_view convertSizeToBacking:vid_view.bounds.size];

	client_width = (int)lround (size.width);
	client_height = (int)lround (size.height);
	if (client_width > 0 && client_height > 0
		&& (vid_metal.drawableSize.width != client_width || vid_metal.drawableSize.height != client_height))
		vid_metal.drawableSize = CGSizeMake (client_width, client_height);
}

/*
===============================================================================

METAL

===============================================================================
*/

static void VID_Check (id object, NSError *error, const char *what)
{
	if (!object)
		Sys_Error ("%s failed%s%s", what, error ? ": " : "", error ? error.localizedDescription.UTF8String : "");
}

static id<MTLRenderPipelineState> VID_CreatePipeline (id<MTL4Compiler> compiler, id<MTLLibrary> library,
	MTLPixelFormat format)
{
	MTL4RenderPipelineDescriptor	*desc = [MTL4RenderPipelineDescriptor new];
	MTL4LibraryFunctionDescriptor	*vs = [MTL4LibraryFunctionDescriptor new];
	MTL4LibraryFunctionDescriptor	*fs = [MTL4LibraryFunctionDescriptor new];
	id<MTLRenderPipelineState>		pipeline;
	NSError							*error = nil;

	vs.name = @"present_vs";
	vs.library = library;
	fs.name = @"present_fs";
	fs.library = library;
	desc.vertexFunctionDescriptor = vs;
	desc.fragmentFunctionDescriptor = fs;
	desc.colorAttachments[0].pixelFormat = format;
	pipeline = [compiler newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
	VID_Check (pipeline, error, "The present pipeline");
	return pipeline;
}

static void VID_CreateDevice (void)
{
	MTL4ArgumentTableDescriptor	*table = [MTL4ArgumentTableDescriptor new];
	MTLResidencySetDescriptor	*residency = [MTLResidencySetDescriptor new];
	id<MTL4Compiler>			compiler;
	id<MTLLibrary>				library;
	dispatch_data_t				data;
	NSError						*error = nil;
	int							i;

	mtl_device = MTLCreateSystemDefaultDevice ();
	if (!mtl_device || ![mtl_device supportsFamily:MTLGPUFamilyMetal4])
		Sys_Error ("SoftWorld needs Metal 4: a Mac with Apple silicon, on macOS 26 or later");

	mtl_queue = [mtl_device newMTL4CommandQueue];
	VID_Check (mtl_queue, nil, "newMTL4CommandQueue");
	for (i = 0 ; i < VID_SLOTS ; i++)
	{
		mtl_allocators[i] = [mtl_device newCommandAllocator];
		VID_Check (mtl_allocators[i], nil, "newCommandAllocator");
		mtl_commands[i] = [mtl_device newCommandBuffer];
		VID_Check (mtl_commands[i], nil, "newCommandBuffer");
	}
	mtl_event = [mtl_device newSharedEvent];
	VID_Check (mtl_event, nil, "newSharedEvent");

	// the shaders, compiled with the program; the array stays where it is
	data = dispatch_data_create (vid_present_metallib, vid_present_metallib_size, NULL, ^{});
	library = [mtl_device newLibraryWithData:data error:&error];
	VID_Check (library, error, "Loading the shaders");
	compiler = [mtl_device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
	VID_Check (compiler, error, "newCompilerWithDescriptor");
	mtl_sdrpipeline = VID_CreatePipeline (compiler, library, MTLPixelFormatBGR10A2Unorm);

	table.maxBufferBindCount = 1;
	table.maxTextureBindCount = 2;
	mtl_arguments = [mtl_device newArgumentTableWithDescriptor:table error:&error];
	VID_Check (mtl_arguments, error, "newArgumentTableWithDescriptor");

	mtl_constants = [mtl_device newBufferWithLength:VID_CONSTANTS * VID_SLOTS options:MTLResourceStorageModeShared];
	VID_Check (mtl_constants, nil, "The constants buffer");

	residency.initialCapacity = 16;
	mtl_residency = [mtl_device newResidencySetWithDescriptor:residency error:&error];
	VID_Check (mtl_residency, error, "newResidencySetWithDescriptor");
	[mtl_residency addAllocation:mtl_constants];
	[mtl_residency commit];
	[mtl_queue addResidencySet:mtl_residency];
}

// until the GPU has finished frame serial; a GPU that doesn't is hung
static void VID_WaitFrame (uint64_t serial)
{
	if (mtl_event.signaledValue >= serial)
		return;
	if (![mtl_event waitUntilSignaledValue:serial timeoutMS:VID_HUNG_MS])
		Sys_Error ("The GPU didn't finish a frame in %d ms", VID_HUNG_MS);
}

/*
===============================================================================

THE FRAME

===============================================================================
*/

static void VID_FreeLayer (vid_layer_t *layer)
{
	if (layer->buffer)
	{
		[mtl_residency removeAllocation:layer->texture];
		[mtl_residency removeAllocation:layer->buffer];
	}
	layer->texture = nil;
	layer->buffer = nil;
	layer->serial = 0;
}

static void VID_AllocLayer (vid_layer_t *layer, MTLPixelFormat format, const vid_frame_t *frame, NSUInteger pitch)
{
	MTLTextureDescriptor	*desc;

	// cached: the renderer reads what it blends over
	layer->buffer = [mtl_device newBufferWithLength:pitch * (NSUInteger)frame->height
		options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache];
	VID_Check (layer->buffer, nil, "A frame buffer");
	memset (layer->buffer.contents, 0, layer->buffer.length);

	desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:(NSUInteger)frame->width
		height:(NSUInteger)frame->height mipmapped:NO];
	desc.storageMode = MTLStorageModeShared;
	desc.cpuCacheMode = MTLCPUCacheModeDefaultCache;
	desc.usage = MTLTextureUsageShaderRead;
	layer->texture = [layer->buffer newTextureWithDescriptor:desc offset:0 bytesPerRow:pitch];
	VID_Check (layer->texture, nil, "A frame texture");

	[mtl_residency addAllocation:layer->buffer];
	[mtl_residency addAllocation:layer->texture];
	layer->serial = 0;
}

/*
================
VID_SetScale

A new frame for the window; the next frame is drawn at its size
================
*/
static void VID_SetScale (void)
{
	vid_frame_t	frame = VID_WantedFrame (client_width, client_height);
	NSUInteger	align, pitch;
	int			i;

	// nothing is freed that a frame in flight reads
	VID_WaitFrame (mtl_serial);
	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_FreeLayer (&vid_views[i]);
	for (i = 0 ; i < 2 ; i++)
		VID_FreeLayer (&vid_huds[i]);

	// the rows as far apart as a texture over a buffer needs them
	align = MAX ([mtl_device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGB10A2Unorm],
		[mtl_device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm]);
	pitch = ((NSUInteger)frame.width * 4 + align - 1) / align * align;

	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_AllocLayer (&vid_views[i], MTLPixelFormatRGB10A2Unorm, &frame, pitch);
	for (i = 0 ; i < 2 ; i++)
		VID_AllocLayer (&vid_huds[i], MTLPixelFormatRGBA8Unorm, &frame, pitch);
	[mtl_residency commit];

	vid_slot = vid_drawnslot = 0;
	vid_hudshown = vid_drawnhud = 0;
	vid.buffer = vid_views[vid_slot].buffer.contents;
	vid.hud = vid_huds[vid_hudshown ^ 1].buffer.contents;
	VID_SetFrame (&frame, (unsigned)(pitch / 4));
}

// the frame last drawn, whether it was presented or not (screenshots, dumps)
void VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud)
{
	*frame = vid_views[vid_drawnslot].buffer.contents;
	*hud = vid_huds[vid_drawnhud].buffer.contents;
}

// frames in flight: one with vsync, where the display sets the pace and a second
// would be a refresh more of input lag; two without, so a frame is drawn while
// the last is presented
static void VID_SetLatency (void)
{
	int		latency = vid_vsync.value ? 1 : 2;

	if (latency == vid_latency)
		return;
	vid_latency = latency;
	vid_metal.displaySyncEnabled = latency == 1;
	vid_metal.maximumDrawableCount = latency == 1 ? 2 : 3;
}

/*
================
VID_Presentable

With vsync, every frame is presented, waiting for a drawable. Without, the
game doesn't wait for the display: a frame is presented once the last is on
the screen, and when drawables come free only at the display's refresh (in a
window, where the compositor has them), once a refresh after the last was had;
the frames between are drawn but not shown.
================
*/
static bool VID_Presentable (void)
{
	NSInteger	fps;
	double		now;

	if (vid_latency == 1)
		return true;
	now = Sys_DoubleTime ();
	if (now - vid_presenttime > 0.1)
		return true;		// a frame the display never told of
	if (atomic_load (&vid_onscreen) < mtl_serial)
		return false;
	if (now - vid_gatedtime > 1.0)
		return true;
	fps = vid_window.screen ? vid_window.screen.maximumFramesPerSecond : 60;
	return now - vid_acquiretime > 0.9 / (fps > 0 ? fps : 60);
}

/*
================
VID_Update

Presents the frame the renderer drew, letterboxed into the window, and hands
it the buffers of the next once the GPU is done with them
================
*/
void VID_Update (void)
{
	id<CAMetalDrawable>				drawable;
	id<MTL4RenderCommandEncoder>	encoder;
	id<MTL4CommandBuffer>			commands;
	MTL4RenderPassDescriptor		*pass;
	vid_present_constants_t			*constants;
	vid_fit_t						fit;
	int								slot = vid_slot, hud;

	if (!vid_initialized || Minimized)
		return;

	// the 2D drawn this frame is shown from now on
	hud = vid.huddirty ? vid_hudshown ^ 1 : vid_hudshown;
	vid_drawnslot = slot;
	vid_drawnhud = hud;

	@autoreleasepool
	{
		VID_UpdateClientSize ();
		if (client_width <= 0 || client_height <= 0)
			return;
		VID_SetLatency ();
		if (!VID_Presentable ())
			return;

		// waits for a free drawable; with none, the frame is drawn again
		vid_acquiretime = Sys_DoubleTime ();
		drawable = [vid_metal nextDrawable];
		if (Sys_DoubleTime () - vid_acquiretime > 0.001)
			vid_gatedtime = vid_acquiretime;
		vid_acquiretime = Sys_DoubleTime ();
		if (!drawable)
			return;

		fit = VID_Fit (client_width, client_height);
		constants = (vid_present_constants_t *)((byte *)mtl_constants.contents + VID_CONSTANTS * slot);
		VID_FillConstants (constants, &fit, false, 1, 1);

		// the slot's last frame is done: its buffer wasn't handed out before
		commands = mtl_commands[slot];
		[mtl_allocators[slot] reset];
		[commands beginCommandBufferWithAllocator:mtl_allocators[slot]];

		pass = [MTL4RenderPassDescriptor new];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake (0, 0, 0, 1);
		encoder = [commands renderCommandEncoderWithDescriptor:pass];
		[encoder setRenderPipelineState:mtl_sdrpipeline];
		[encoder setViewport:(MTLViewport){fit.x, fit.y, fit.width, fit.height, 0, 1}];
		[mtl_arguments setTexture:vid_views[slot].texture.gpuResourceID atIndex:0];
		[mtl_arguments setTexture:vid_huds[hud].texture.gpuResourceID atIndex:1];
		[mtl_arguments setAddress:mtl_constants.gpuAddress + VID_CONSTANTS * slot atIndex:0];
		[encoder setArgumentTable:mtl_arguments atStages:MTLRenderStageFragment];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding];
		[commands endCommandBuffer];

		[mtl_queue waitForDrawable:drawable];
		[mtl_queue commit:&commands count:1];
		[mtl_queue signalDrawable:drawable];
		[mtl_queue signalEvent:mtl_event value:++mtl_serial];
		{
			uint64_t	serial = mtl_serial;

			// shown, or passed over for a newer one
			[drawable addPresentedHandler:^(id<MTLDrawable> shown) {
				(void)shown;
				if (atomic_load (&vid_onscreen) < serial)
					atomic_store (&vid_onscreen, serial);
			}];
		}
		[drawable present];
		vid_presenttime = Sys_DoubleTime ();

		vid_views[slot].serial = mtl_serial;
		vid_huds[hud].serial = mtl_serial;
		vid_hudshown = hud;
		vid.huddirty = false;

		// the pace: no more frames in flight than the latency (the wait may end
		// early; it is the one below that keeps the buffers safe)
		if (mtl_serial >= (uint64_t)vid_latency)
			[mtl_event waitUntilSignaledValue:mtl_serial - (uint64_t)vid_latency + 1 timeoutMS:100];

		// the next frame is drawn in the other view, and a new 2D in the one not
		// shown, once the GPU is done with them
		vid_slot = slot ^ 1;
		VID_WaitFrame (vid_views[vid_slot].serial);
		VID_WaitFrame (vid_huds[vid_hudshown ^ 1].serial);
		vid.buffer = vid_views[vid_slot].buffer.contents;
		vid.hud = vid_huds[vid_hudshown ^ 1].buffer.contents;
	}

	// the window was resized, or the video settings changed
	if (VID_NeedsResize (client_width, client_height))
		VID_SetScale ();
}

/*
===============================================================================

VIDEO CONTRACT

===============================================================================
*/

void VID_Init (void)
{
	int		scale;

	@autoreleasepool
	{
		scale = VID_RegisterCommon ();

		VID_CreateDevice ();
		VID_CreateWindow (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
		VID_UpdateClientSize ();
		VID_SetLatency ();
		VID_SetScale ();

		[vid_window makeKeyAndOrderFront:nil];
		[NSApp activate];
		IN_WindowChanged ();
	}

	vid_initialized = true;
}

void VID_Shutdown (void)
{
	int		i;

	if (!vid_initialized)
		return;
	vid_initialized = false;

	VID_WaitFrame (mtl_serial);
	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_FreeLayer (&vid_views[i]);
	for (i = 0 ; i < 2 ; i++)
		VID_FreeLayer (&vid_huds[i]);
	vid.buffer = NULL;
	vid.hud = NULL;

	[vid_window orderOut:nil];
	vid_window.delegate = nil;
	vid_window = nil;
	vid_view = nil;
	vid_metal = nil;
}

void VID_SetCaption (const char *text)
{
	@autoreleasepool
	{
		vid_window.title = [[NSString alloc] initWithBytes:text length:strlen (text)
			encoding:NSISOLatin1StringEncoding];
	}
}

void VID_BringToFront (void)
{
	if (vid_window.miniaturized)
		[vid_window deminiaturize:nil];
	[NSApp activate];
	[vid_window makeKeyAndOrderFront:nil];
}

bool VID_IsActive (void)
{
	return ActiveApp;
}

bool VID_IsMinimized (void)
{
	return Minimized;
}

bool VID_IsFullscreen (void)
{
	return vid_fullscreen;
}
