// crt_metal.h -- the CRT shader's passes (vid_crt.h) in Metal 4, which
// vid_metal.m draws the frame through and test_present.m tests
#pragma once

#import <Metal/Metal.h>

#include "vid_crt.h"

// the pipelines, buffers and LUTs on device, its resources in residency, which
// the queue has; slots frames in flight, each with its own constants. False
// (and why printed) if they couldn't be made
bool	CRT_Metal_Init (id<MTLDevice> device, id<MTLResidencySet> residency, int slots);
// all of it freed, no frame in flight
void	CRT_Metal_Shutdown (void);

// the textures for a frame of source pixels shown in a viewport of view pixels
// aren't the ones made: the frames in flight are to be finished before
// CRT_Metal_SetSize makes them (AFTERGLOW's and AVGLUM's black)
bool	CRT_Metal_SizeChanged (int sourcewidth, int sourceheight, int viewwidth, int viewheight);
void	CRT_Metal_SetSize (int sourcewidth, int sourceheight, int viewwidth, int viewheight);

// an encoder drawing in CRT_ORIGINAL, RGBA8Unorm, at its size, for present.metal's
// pipeline to draw the frame at its own size; the caller ends it
id<MTL4RenderCommandEncoder>	CRT_Metal_BeginOriginal (id<MTL4CommandBuffer> commands);

// the passes but the last, after the frame is in CRT_ORIGINAL: slot's
// constants, the frame of parity's
void	CRT_Metal_EncodePasses (id<MTL4CommandBuffer> commands, int slot, const vid_crt_params_t *params,
			const vid_crt_pass_t passes[CRT_PASSES], int parity);
// the last, in the caller's encoder drawing in a texture of format, at viewport
void	CRT_Metal_EncodeLast (id<MTL4RenderCommandEncoder> encoder, MTLPixelFormat format, MTLViewport viewport);

// a texture, for the tests
id<MTLTexture>	CRT_Metal_Texture (int texture);
