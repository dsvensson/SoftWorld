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
// Without a GPU with Metal 4 it is skipped.
//
// And the CRT shader's passes (crt_metal.m) after the frame, as vid_metal.m
// encodes them: with its own oimage 1 each pixel is the frame's as drawn at its
// own size, within 1 of VID_FrameToRGB, and in extended linear light at paper
// white; at its defaults, three frames of a frame white in its top left quarter
// are finite and brightest there; and PrePass's mip chain, crt_spd's in one
// dispatch or two (a frame 5000 across), is each level's texels the average of
// the four they cover in the level before, within 1.

#import <Metal/Metal.h>

#include "cvar.h"
#include "vid_common.h"
#include "crt_metal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern const unsigned char	vid_present_metallib[];
extern const size_t			vid_present_metallib_size;

#define WIDTH	200
#define HEIGHT	120
#define SKIPPED	77		// ctest's SKIP_RETURN_CODE

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

// the program's wait (posix_local.h), which the platform's Sys_WaitUntil
// calls: nothing here waits
bool Sys_WaitEvents (double until)
{
	(void)until;
	return false;
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

#define CRT_WIDTH	500		// the CRT shader's defaults' target, 2.5 times the frame
#define CRT_HEIGHT	300

static id<MTLRenderPipelineState>	originalpipeline;	// present.metal into CRT_ORIGINAL, RGBA8
static id<MTLTexture>				crttarget;			// float, CRT_WIDTH x CRT_HEIGHT
static id<MTLBuffer>				levelbuf;			// a level of PrePass, read back

// false if there is no GPU to test on
static bool Setup (void)
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
		return false;
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
	desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	originalpipeline = [compiler newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
	Check (originalpipeline, error, "newRenderPipelineStateWithDescriptor");

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
	tdesc.width = CRT_WIDTH;
	tdesc.height = CRT_HEIGHT;
	crttarget = [device newTextureWithDescriptor:tdesc];
	levelbuf = [device newBufferWithLength:5000 * 40 * 4 options:MTLResourceStorageModeShared];

	residency = [device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
	Check (residency, error, "newResidencySetWithDescriptor");
	for (id<MTLAllocation> a in @[framebuf, hudbuf, constants, frametex, hudtex, target, crttarget, levelbuf])
		[residency addAllocation:a];
	[residency commit];
	[queue addResidencySet:residency];

	vid.width = WIDTH;
	vid.height = HEIGHT;
	vid.rowpixels = (unsigned)(pitch / 4);
	shown_frame = framebuf.contents;
	shown_hud = hudbuf.contents;
	return true;
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

/*
===============================================================================

THE CRT SHADER

===============================================================================
*/

static void Commit (void)
{
	id<MTL4CommandBuffer>	list[1] = {commands};

	[commands endCommandBuffer];
	[queue commit:list count:1];
	[queue signalEvent:event value:++serial];
	if (![event waitUntilSignaledValue:serial timeoutMS:5000])
		Sys_Error ("the GPU didn't finish");
}

/*
================
DrawCRT

The frame through the CRT shader's passes into width x height of the target, as
vid_metal.m encodes them: as present.metal shows it at its own size, in SDR,
into CRT_ORIGINAL, then the passes for a frame of sourcewidth x sourceheight,
the last in the target's encoder; a frame of 60 Hz after the last
================
*/
static void DrawCRT (const vid_crt_params_t *params, bool hdr, float paperwhite, int sourcewidth, int sourceheight,
	int width, int height, float *out)
{
	static vid_crt_frame_t			crtframe;
	static double					time = 1;
	vid_fit_t						whole = {0, 0, WIDTH, HEIGHT, 1, 1};
	vid_crt_pass_t					passes[CRT_PASSES];
	MTL4RenderPassDescriptor		*pass = [MTL4RenderPassDescriptor new];
	id<MTL4RenderCommandEncoder>	encoder;

	if (CRT_Metal_SizeChanged (sourcewidth, sourceheight, width, height))
		CRT_Metal_SetSize (sourcewidth, sourceheight, width, height);
	VID_FillConstants (constants.contents, &whole, VID_OUTPUT_SDR, 1, 1);
	time += 1 / 60.0;
	VID_CRT_BeginFrame (&crtframe, time);
	VID_CRT_FillPasses (passes, &crtframe, sourcewidth, sourceheight, width, height,
		hdr ? VID_OUTPUT_LINEAR : VID_OUTPUT_SDR, paperwhite);

	[allocator reset];
	[commands beginCommandBufferWithAllocator:allocator];
	encoder = CRT_Metal_BeginOriginal (commands);
	[encoder setRenderPipelineState:originalpipeline];
	[arguments setTexture:frametex.gpuResourceID atIndex:0];
	[arguments setTexture:hudtex.gpuResourceID atIndex:1];
	[arguments setAddress:constants.gpuAddress atIndex:0];
	[encoder setArgumentTable:arguments atStages:MTLRenderStageFragment];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	CRT_Metal_EncodePasses (commands, 0, params, passes, crtframe.parity);

	pass.colorAttachments[0].texture = crttarget;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	encoder = [commands renderCommandEncoderWithDescriptor:pass];
	CRT_Metal_EncodeLast (encoder, MTLPixelFormatRGBA32Float, (MTLViewport){0, 0, width, height, 0, 1});
	[encoder endEncoding];
	Commit ();

	[crttarget getBytes:out bytesPerRow:(NSUInteger)width * 16 fromRegion:MTLRegionMake2D (0, 0,
		(NSUInteger)width, (NSUInteger)height) mipmapLevel:0];
}

static double SrgbToLinear (double c)
{
	return c <= 0.04045 ? c / 12.92 : pow ((c + 0.055) / 1.055, 2.4);
}

// with oimage 1, the frame as it is: in SDR within 1 of VID_FrameToRGB, in
// extended linear light that at paper white
static void TestCRTOriginal (void)
{
	static float		out[WIDTH * HEIGHT * 4];
	static byte			rgb[WIDTH * HEIGHT * 3];
	vid_crt_params_t	params;
	double				want, worsthdr = 0;
	int					i, c, d, worst = 0, off = 0, offhdr = 0;

	VID_CRT_FillParams (&params);
	params.oimage = 1;
	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});

	DrawCRT (&params, false, 1, WIDTH, HEIGHT, WIDTH, HEIGHT, out);
	VID_FrameToRGB (rgb, true);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			d = abs ((int)lroundf (out[i * 4 + c] * 255) - rgb[i * 3 + c]);
			if (d > worst)
				worst = d;
			if (d > 1 && off++ < 5)
				printf ("CRT original: pixel %d channel %d is %.1f, VID_FrameToRGB %d\n", i, c, out[i * 4 + c] * 255,
					rgb[i * 3 + c]);
		}
	printf ("CRT, the original image: at most %d from VID_FrameToRGB, %d channels over 1\n", worst, off);

	DrawCRT (&params, true, 2, WIDTH, HEIGHT, WIDTH, HEIGHT, out);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			want = 2 * SrgbToLinear (rgb[i * 3 + c] / 255.0);
			if (fabs (out[i * 4 + c] - want) > worsthdr)
				worsthdr = fabs (out[i * 4 + c] - want);
			if (fabs (out[i * 4 + c] - want) > 0.02 && offhdr++ < 5)
				printf ("CRT original, extended linear: pixel %d channel %d is %.4f, not %.4f\n", i, c, out[i * 4 + c],
					want);
		}
	printf ("CRT, the original image in light at paper white 2: at most %.4f off, %d channels over 0.02\n", worsthdr,
		offhdr);
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	if (off || offhdr)
		failures++;
}

// at the defaults, a frame white in its top left quarter: three frames, every
// value finite, and the quarter four times as bright as the one across from it
static void TestCRTDefaults (void)
{
	static float		out[CRT_WIDTH * CRT_HEIGHT * 4];
	pixel_t				*frame = framebuf.contents;
	vid_crt_params_t	params;
	double				topleft = 0, bottomright = 0;
	float				v;
	int					x, y, i, bad = 0;

	VID_CRT_FillParams (&params);
	memset (hudbuf.contents, 0, hudbuf.length);
	for (y = 0 ; y < HEIGHT ; y++)
		for (x = 0 ; x < WIDTH ; x++)
			frame[y * vid.rowpixels + x] = x < WIDTH / 2 && y < HEIGHT / 2
				? RGB30 (RGB30_WHITE, RGB30_WHITE, RGB30_WHITE) : RGB30 (0, 0, 0);
	for (i = 0 ; i < 3 ; i++)
		DrawCRT (&params, false, 1, WIDTH, HEIGHT, CRT_WIDTH, CRT_HEIGHT, out);
	for (y = 0 ; y < CRT_HEIGHT ; y++)
		for (x = 0 ; x < CRT_WIDTH ; x++)
			for (i = 0 ; i < 3 ; i++)
			{
				v = out[(y * CRT_WIDTH + x) * 4 + i];
				if (!isfinite (v) || v < 0 || v > 1.001f)
				{
					if (bad++ < 5)
						printf ("CRT defaults: pixel %d,%d channel %d is %f\n", x, y, i, v);
					continue;
				}
				if (x < CRT_WIDTH / 2 && y < CRT_HEIGHT / 2)
					topleft += v;
				else if (x >= CRT_WIDTH / 2 && y >= CRT_HEIGHT / 2)
					bottomright += v;
			}
	topleft /= CRT_WIDTH / 2 * CRT_HEIGHT / 2 * 3;
	bottomright /= CRT_WIDTH / 2 * CRT_HEIGHT / 2 * 3;
	printf ("CRT, defaults, 3 frames: %d values not in 0 to 1, the white quarter %.3f, the black one %.3f\n", bad,
		topleft, bottomright);
	// the glow and bloom light the black quarter a little
	if (bad || topleft < 0.3 || bottomright > topleft / 4)
		failures++;
}

// a level of PrePass, RGBA as floats, and its size
static float *ReadLevel (int level, int *width, int *height)
{
	id<MTLTexture>					texture = CRT_Metal_Texture (CRT_PREPASS);
	id<MTL4ComputeCommandEncoder>	encoder;
	float							*pixels;
	int								i;

	*width = (int)texture.width >> level > 1 ? (int)texture.width >> level : 1;
	*height = (int)texture.height >> level > 1 ? (int)texture.height >> level : 1;
	[allocator reset];
	[commands beginCommandBufferWithAllocator:allocator];
	encoder = [commands computeCommandEncoder];
	[encoder barrierAfterQueueStages:MTLStageFragment | MTLStageDispatch beforeStages:MTLStageBlit
		visibilityOptions:MTL4VisibilityOptionDevice];
	[encoder copyFromTexture:texture sourceSlice:0 sourceLevel:(NSUInteger)level sourceOrigin:MTLOriginMake (0, 0, 0)
		sourceSize:MTLSizeMake ((NSUInteger)*width, (NSUInteger)*height, 1) toBuffer:levelbuf destinationOffset:0
		destinationBytesPerRow:(NSUInteger)*width * 4 destinationBytesPerImage:(NSUInteger)*width * (NSUInteger)*height * 4];
	[encoder endEncoding];
	Commit ();
	pixels = malloc ((size_t)*width * (size_t)*height * 16);
	for (i = 0 ; i < *width * *height * 4 ; i++)
		pixels[i] = ((const byte *)levelbuf.contents)[i] / 255.0f;
	return pixels;
}

/*
================
TestMips

PrePass's mip chain against the average of the level before's four texels a
texel covers (what lies past an edge of a level 1 across, the texel there),
reduced from its top level as read back, unrounded
================
*/
static void TestMips (const char *what)
{
	float	*ref, *next, *level, want, got;
	int		levels = (int)CRT_Metal_Texture (CRT_PREPASS).mipmapLevelCount;
	int		w, h, nw, nh, l, x, y, c, x1, y1, d, worst = 0, off = 0;

	ref = ReadLevel (0, &w, &h);
	for (l = 1 ; l < levels ; l++)
	{
		level = ReadLevel (l, &nw, &nh);
		next = malloc ((size_t)nw * (size_t)nh * 16);
		for (y = 0 ; y < nh ; y++)
			for (x = 0 ; x < nw ; x++)
				for (c = 0 ; c < 4 ; c++)
				{
					x1 = 2 * x + 1 < w ? 2 * x + 1 : w - 1;
					y1 = 2 * y + 1 < h ? 2 * y + 1 : h - 1;
					want = (ref[(2 * y * w + 2 * x) * 4 + c] + ref[(2 * y * w + x1) * 4 + c]
						+ ref[(y1 * w + 2 * x) * 4 + c] + ref[(y1 * w + x1) * 4 + c]) * 0.25f;
					got = level[(y * nw + x) * 4 + c];
					next[(y * nw + x) * 4 + c] = want;
					d = abs ((int)lroundf (want * 255) - (int)lroundf (got * 255));
					if (d > worst)
						worst = d;
					if (d > 1 && off++ < 5)
						printf ("%s: level %d texel %d,%d channel %d is %.1f, not %.1f\n", what, l, x, y, c, got * 255,
							want * 255);
				}
		free (level);
		free (ref);
		ref = next;
		w = nw;
		h = nh;
	}
	free (ref);
	printf ("%s: %d levels, at most %d from the average of the texels they cover, %d channels over 1\n", what, levels,
		worst, off);
	if (off)
		failures++;
}

static void TestCRT (void)
{
	static float		out[64 * 64 * 4];
	vid_crt_params_t	params;

	TestCRTOriginal ();
	TestCRTDefaults ();
	TestMips ("CRT mip chain, one dispatch");
	// a frame 5000 across: 6 levels from its top, then the rest from the sixth
	VID_CRT_FillParams (&params);
	DrawCRT (&params, false, 1, 5000, 40, 64, 64, out);
	TestMips ("CRT mip chain of 5000x40, two dispatches");
}

int main (void)
{
	@autoreleasepool
	{
		if (!Setup ())
		{
			printf ("present: no GPU with Metal 4, skipped\n");
			return SKIPPED;
		}

		Fill ();
		VID_SetPresent (&(vid_present_t){.gamma = 1});
		TestSDR ("SDR");

		VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
		TestSDR ("SDR, gamma 0.8 and a blend");

		VID_SetPresent (&(vid_present_t){.gamma = 1});
		TestHDR ();

		VID_CRT_Register ();
		if (!CRT_Metal_Init (device, residency, 1))
		{
			printf ("CRT: the shader's passes couldn't be made\n");
			return 1;
		}
		TestCRT ();
		CRT_Metal_Shutdown ();
	}

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present: the shader shows what screenshots do\n");
	return 0;
}
