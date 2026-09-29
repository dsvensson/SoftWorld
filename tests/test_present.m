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
// test_present.m -- present.metal drawn with Metal 4 as vid_metal.m draws it, from
// buffers laid out as the renderer fills them, into a float target read back:
// in SDR each pixel within 1 of what VID_FrameToRGB (screenshots) makes of it,
// with and without gamma and a view blend; and in HDR, SDR white at paper white,
// the brightest light rolled off below the peak, and the 2D at paper white.

#import <Metal/Metal.h>

#include "cvar.h"
#include "vid_common.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern const unsigned char	vid_present_metallib[];
extern const size_t			vid_present_metallib_size;

#define WIDTH	200
#define HEIGHT	120

static int	failures;

//
// the engine, as far as vid_common.c needs it
//

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	(void)fmt;
}

void R_SetRenderSize (int width, int height, int scale)
{
	(void)width;
	(void)height;
	(void)scale;
}

static const pixel_t	*shown_frame;
static const hudpixel_t	*shown_hud;

void VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud)
{
	*frame = shown_frame;
	*hud = shown_hud;
}

//
// Metal
//

static id<MTLDevice>				device;
static id<MTL4CommandQueue>			queue;
static id<MTL4CommandAllocator>		allocator;
static id<MTL4CommandBuffer>		commands;
static id<MTL4ArgumentTable>		arguments;
static id<MTLRenderPipelineState>	pipeline;
static id<MTLResidencySet>			residency;
static id<MTLSharedEvent>			event;
static uint64_t						serial;
static id<MTLBuffer>				framebuf, hudbuf, constants;
static id<MTLTexture>				frametex, hudtex, target;
static NSUInteger					pitch;

static void Check (id object, NSError *error, const char *what)
{
	if (!object)
		Sys_Error ("%s failed%s%s", what, error ? ": " : "", error ? error.localizedDescription.UTF8String : "");
}

static void Setup (void)
{
	MTL4ArgumentTableDescriptor		*table = [MTL4ArgumentTableDescriptor new];
	MTL4RenderPipelineDescriptor	*desc = [MTL4RenderPipelineDescriptor new];
	MTL4LibraryFunctionDescriptor	*vs = [MTL4LibraryFunctionDescriptor new];
	MTL4LibraryFunctionDescriptor	*fs = [MTL4LibraryFunctionDescriptor new];
	MTLTextureDescriptor			*tdesc;
	id<MTL4Compiler>				compiler;
	id<MTLLibrary>					library;
	NSError							*error = nil;
	NSUInteger						align;

	device = MTLCreateSystemDefaultDevice ();
	if (!device || ![device supportsFamily:MTLGPUFamilyMetal4])
		Sys_Error ("no Metal 4 GPU");
	queue = [device newMTL4CommandQueue];
	allocator = [device newCommandAllocator];
	commands = [device newCommandBuffer];
	event = [device newSharedEvent];

	library = [device newLibraryWithData:dispatch_data_create (vid_present_metallib, vid_present_metallib_size,
		NULL, ^{}) error:&error];
	Check (library, error, "newLibraryWithData");
	compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
	Check (compiler, error, "newCompilerWithDescriptor");
	vs.name = @"present_vs";
	vs.library = library;
	fs.name = @"present_fs";
	fs.library = library;
	desc.vertexFunctionDescriptor = vs;
	desc.fragmentFunctionDescriptor = fs;
	desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
	pipeline = [compiler newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
	Check (pipeline, error, "newRenderPipelineStateWithDescriptor");

	table.maxBufferBindCount = 1;
	table.maxTextureBindCount = 2;
	arguments = [device newArgumentTableWithDescriptor:table error:&error];
	Check (arguments, error, "newArgumentTableWithDescriptor");

	// the layers as vid_metal.m makes them
	align = MAX ([device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGB10A2Unorm],
		[device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm]);
	pitch = (WIDTH * 4 + align - 1) / align * align;
	framebuf = [device newBufferWithLength:pitch * HEIGHT options:MTLResourceStorageModeShared];
	hudbuf = [device newBufferWithLength:pitch * HEIGHT options:MTLResourceStorageModeShared];
	constants = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
	tdesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:WIDTH
		height:HEIGHT mipmapped:NO];
	tdesc.storageMode = MTLStorageModeShared;
	tdesc.usage = MTLTextureUsageShaderRead;
	frametex = [framebuf newTextureWithDescriptor:tdesc offset:0 bytesPerRow:pitch];
	Check (frametex, nil, "the frame texture");
	tdesc.pixelFormat = MTLPixelFormatRGBA8Unorm;
	hudtex = [hudbuf newTextureWithDescriptor:tdesc offset:0 bytesPerRow:pitch];
	Check (hudtex, nil, "the 2D texture");

	tdesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:WIDTH
		height:HEIGHT mipmapped:NO];
	tdesc.storageMode = MTLStorageModeShared;
	tdesc.usage = MTLTextureUsageRenderTarget;
	target = [device newTextureWithDescriptor:tdesc];

	residency = [device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
	Check (residency, error, "newResidencySetWithDescriptor");
	for (id<MTLAllocation> a in @[framebuf, hudbuf, constants, frametex, hudtex, target])
		[residency addAllocation:a];
	[residency commit];
	[queue addResidencySet:residency];

	vid.width = WIDTH;
	vid.height = HEIGHT;
	vid.rowpixels = (unsigned)(pitch / 4);
	shown_frame = framebuf.contents;
	shown_hud = hudbuf.contents;
}

// the frame as the shader shows it, one float RGBA a pixel
static void Draw (bool hdr, float paperwhite, float peak, float *out)
{
	vid_fit_t					fit = {0, 0, WIDTH, HEIGHT, 1, 1};
	MTL4RenderPassDescriptor	*pass = [MTL4RenderPassDescriptor new];
	id<MTL4RenderCommandEncoder>	encoder;
	id<MTL4CommandBuffer>		list[1] = {commands};

	VID_FillConstants (constants.contents, &fit, hdr ? VID_OUTPUT_LINEAR : VID_OUTPUT_SDR, paperwhite, peak);

	[allocator reset];
	[commands beginCommandBufferWithAllocator:allocator];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	encoder = [commands renderCommandEncoderWithDescriptor:pass];
	[encoder setRenderPipelineState:pipeline];
	[encoder setViewport:(MTLViewport){0, 0, WIDTH, HEIGHT, 0, 1}];
	[arguments setTexture:frametex.gpuResourceID atIndex:0];
	[arguments setTexture:hudtex.gpuResourceID atIndex:1];
	[arguments setAddress:constants.gpuAddress atIndex:0];
	[encoder setArgumentTable:arguments atStages:MTLRenderStageFragment];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	[commands endCommandBuffer];
	[queue commit:list count:1];
	[queue signalEvent:event value:++serial];
	if (![event waitUntilSignaledValue:serial timeoutMS:5000])
		Sys_Error ("the GPU didn't finish");

	[target getBytes:out bytesPerRow:WIDTH * 16 fromRegion:MTLRegionMake2D (0, 0, WIDTH, HEIGHT) mipmapLevel:0];
}

static uint32_t	rng = 0x2545F491;

static uint32_t Rand (void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// random light, often SDR white and brighter; and random 2D, often none or opaque
static void Fill (void)
{
	pixel_t		*frame = framebuf.contents;
	hudpixel_t	*hud = hudbuf.contents;
	unsigned	x, y, a;

	for (y = 0 ; y < HEIGHT ; y++)
		for (x = 0 ; x < WIDTH ; x++)
		{
			frame[y * vid.rowpixels + x] = RGB30 (Rand () % 1024, Rand () % 1024, Rand () % 1024);
			switch (Rand () % 4)
			{
			case 0:		a = 0;					break;
			case 1:		a = 255;				break;
			default:	a = Rand () % 256;		break;
			}
			hud[y * vid.rowpixels + x] = HUD_RGBA (Rand () % (a + 1), Rand () % (a + 1), Rand () % (a + 1), a);
		}
	// the channel order: a red, a green and a blue pixel of SDR white
	frame[0] = RGB30 (RGB30_WHITE, 0, 0);
	frame[1] = RGB30 (0, RGB30_WHITE, 0);
	frame[2] = RGB30 (0, 0, RGB30_WHITE);
	hud[0] = hud[1] = hud[2] = 0;
}

static void TestSDR (const char *what)
{
	static float	out[WIDTH * HEIGHT * 4];
	static byte		rgb[WIDTH * HEIGHT * 3];
	int				i, c, d, worst = 0, off = 0;

	Draw (false, 1, 1, out);
	VID_FrameToRGB (rgb, true);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			d = abs ((int)lroundf (out[i * 4 + c] * 255) - rgb[i * 3 + c]);
			if (d > worst)
				worst = d;
			if (d > 1 && off++ < 5)
				printf ("%s: pixel %d channel %d is %.1f, VID_FrameToRGB %d\n", what, i, c, out[i * 4 + c] * 255,
					rgb[i * 3 + c]);
		}
	if (off)
		failures++;
	printf ("%s: at most %d from VID_FrameToRGB, %d channels over 1\n", what, worst, off);
}

static void Expect (const char *what, float got, float lo, float hi)
{
	if (got >= lo && got <= hi)
		return;
	printf ("%s: %f, not in %f .. %f\n", what, got, lo, hi);
	failures++;
}

static void TestHDR (void)
{
	static float	out[WIDTH * HEIGHT * 4];
	pixel_t			*frame = framebuf.contents;
	hudpixel_t		*hud = hudbuf.contents;

	memset (hudbuf.contents, 0, hudbuf.length);
	frame[0] = RGB30 (RGB30_WHITE, RGB30_WHITE, RGB30_WHITE);
	frame[1] = RGB30 (1023, 1023, 1023);
	frame[2] = RGB30 (1023, 0, 0);
	frame[3] = RGB30 (0, 0, 0);
	hud[3] = HUD_RGBA (255, 255, 255, 255);

	// paper white 2, peak 6: white is 2, the brightest light (15.9) is under 6, the 2D's white is 2
	Draw (true, 2, 6, out);
	Expect ("HDR white", out[0], 1.99f, 2.01f);
	Expect ("HDR brightest", out[4], 4.5f, 6.0f);
	Expect ("HDR red keeps its hue: green", out[9], 0, 0.001f);
	Expect ("HDR 2D white", out[12], 1.99f, 2.01f);

	// no headroom yet: what is brighter than white goes toward white
	Draw (true, 1, 1, out);
	Expect ("HDR without headroom, white", out[0], 0.99f, 1.01f);
	Expect ("HDR without headroom, brightest", out[4], 0.99f, 1.01f);
	Expect ("HDR without headroom, bright red: green", out[9], 0.5f, 1.0f);
}

int main (void)
{
	@autoreleasepool
	{
		Setup ();

		Fill ();
		VID_SetPresent (&(vid_present_t){.gamma = 1});
		TestSDR ("SDR");

		VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
		TestSDR ("SDR, gamma 0.8 and a blend");

		VID_SetPresent (&(vid_present_t){.gamma = 1});
		TestHDR ();
	}

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present: the shader shows what screenshots do\n");
	return 0;
}
