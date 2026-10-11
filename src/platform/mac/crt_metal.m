// crt_metal.m -- the CRT shader's passes (vid_crt.h) in Metal 4: crt.metal's
// library (crt_metallib.c), a texture each pass draws in, the LUTs, and
// PrePass's mip chain, crt_spd's. A pass's parameters and sizes are in a buffer,
// a region a frame in flight; its textures are bound by its argument table.
// Metal 4 tracks no hazards: each pass waits for the fragment and dispatch work
// before it (a consumer barrier), and crt_spd's dispatches for the one before.
// Nothing here exists until the shader is first turned on.

#include "mem.h"
#include "print.h"
#include "crt_metal.h"

#include <string.h>

// crt.metal compiled (crt_metallib.c)
extern const unsigned char	vid_crt_metallib[];
extern const size_t			vid_crt_metallib_size;

#define CRT_MAX_LEVELS	16						// mip levels of a texture at most
#define CRT_FORMATS		4						// the last pass's pipelines, one a format drawn in
#define CRT_PASS_AT(i)	(1024 + (i) * 256)		// a pass's sizes in a slot, after the parameters
#define CRT_SPD_AT(d)	(CRT_PASS_AT (CRT_PASSES) + (d) * 256)	// a dispatch's constants
#define CRT_STRIDE		CRT_SPD_AT (CRT_SPD_MAX_DISPATCHES)	// from one slot's to the next's

static_assert (VID_CRT_PARAMS_SIZE <= 1024, "the parameters fit before the passes' sizes");
static_assert (CRT_STRIDE % 256 == 0, "a slot's constants start where a buffer's may");

typedef struct
{
	id<MTLTexture>	texture;
	id<MTLTexture>	levels[CRT_MAX_LEVELS];		// PrePass's levels alone, for crt_spd
	int				width, height;
	int				numlevels;
	bool			clear;						// cleared before it's next read
} crt_texture_t;

static id<MTLDevice>				crt_device;
static id<MTLResidencySet>			crt_residency;
static id<MTLLibrary>				crt_library;
static id<MTL4Compiler>				crt_compiler;
static id<MTLRenderPipelineState>	crt_pipelines[CRT_PASSES];	// but the last's
static struct { MTLPixelFormat format; __strong id<MTLRenderPipelineState> pipeline; }	crt_lastpipelines[CRT_FORMATS];
static id<MTLComputePipelineState>	crt_spdpipeline;
static id<MTL4ArgumentTable>		crt_table, crt_lasttable, crt_spdtable;
static id<MTLBuffer>				crt_constants;				// a slot's parameters and sizes each
static id<MTLBuffer>				crt_spdbuffer;				// the counter, then level 6's texels
static crt_texture_t				crt_textures[CRT_TEXTURES];
static int							crt_sizes[4];				// the source's and viewport's the textures are for
static vid_crt_spd_t				crt_spd[CRT_SPD_MAX_DISPATCHES];
static int							crt_spdgroups[CRT_SPD_MAX_DISPATCHES][2];
static int							crt_spddispatches;
static int							crt_lastparity;				// the frame the last pass is of
static uint64_t						crt_lastconstants;			// and its slot's constants

static bool CRT_Metal_Check (id object, NSError *error, const char *what)
{
	if (object)
		return true;
	Con_Printf ("The CRT shader: %s failed%s%s\n", what, error ? ": " : "",
		error ? error.localizedDescription.UTF8String : "");
	return false;
}

static id<MTLRenderPipelineState> CRT_Metal_Pipeline (const char *pass, MTLPixelFormat format)
{
	MTL4RenderPipelineDescriptor	*desc = [MTL4RenderPipelineDescriptor new];
	MTL4LibraryFunctionDescriptor	*vs = [MTL4LibraryFunctionDescriptor new];
	MTL4LibraryFunctionDescriptor	*fs = [MTL4LibraryFunctionDescriptor new];
	id<MTLRenderPipelineState>		pipeline;
	NSError							*error = nil;

	vs.name = @"crt_vs";
	vs.library = crt_library;
	fs.name = [NSString stringWithFormat:@"crt_%s_fs", pass];
	fs.library = crt_library;
	desc.vertexFunctionDescriptor = vs;
	desc.fragmentFunctionDescriptor = fs;
	desc.colorAttachments[0].pixelFormat = format;
	pipeline = [crt_compiler newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
	CRT_Metal_Check (pipeline, error, "A pass's pipeline");
	return pipeline;
}

static id<MTL4ArgumentTable> CRT_Metal_Table (NSUInteger buffers, NSUInteger textures)
{
	MTL4ArgumentTableDescriptor	*desc = [MTL4ArgumentTableDescriptor new];
	id<MTL4ArgumentTable>		table;
	NSError						*error = nil;

	desc.maxBufferBindCount = buffers;
	desc.maxTextureBindCount = textures;
	table = [crt_device newArgumentTableWithDescriptor:desc error:&error];
	CRT_Metal_Check (table, error, "newArgumentTableWithDescriptor");
	return table;
}

static void CRT_Metal_FreeTexture (crt_texture_t *t)
{
	int		i;

	if (t->texture)
		[crt_residency removeAllocation:t->texture];
	for (i = 0 ; i < CRT_MAX_LEVELS ; i++)
		t->levels[i] = nil;
	t->texture = nil;
	t->width = t->height = t->numlevels = 0;
	t->clear = false;
}

// the last pass's pipeline for a texture of format, made the first time
static id<MTLRenderPipelineState> CRT_Metal_LastPipeline (MTLPixelFormat format)
{
	int		i;

	for (i = 0 ; i < CRT_FORMATS && crt_lastpipelines[i].pipeline ; i++)
		if (crt_lastpipelines[i].format == format)
			return crt_lastpipelines[i].pipeline;
	if (i == CRT_FORMATS)
		return nil;		// a fifth format
	crt_lastpipelines[i].pipeline = CRT_Metal_Pipeline (vid_crt_passes[CRT_PASSES - 1].name, format);
	crt_lastpipelines[i].format = format;
	return crt_lastpipelines[i].pipeline;
}

// a texture of vid_crt_textures' kind, in the GPU's own memory, or a LUT's
// pixels in memory both have
static bool CRT_Metal_CreateTexture (int index, int width, int height, const byte *pixels)
{
	const vid_crt_texture_t	*kind = &vid_crt_textures[index];
	crt_texture_t			*t = &crt_textures[index];
	MTLTextureDescriptor	*desc;
	int						i;

	CRT_Metal_FreeTexture (t);
	t->numlevels = kind->mips ? VID_CRT_MipLevels (width, height) : 1;
	if (t->numlevels > CRT_MAX_LEVELS)
		t->numlevels = CRT_MAX_LEVELS;
	desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kind->format == CRT_RGBA16F
		? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA8Unorm width:(NSUInteger)width height:(NSUInteger)height
		mipmapped:NO];
	desc.mipmapLevelCount = (NSUInteger)t->numlevels;
	if (pixels)
	{
		desc.storageMode = MTLStorageModeShared;
		desc.usage = MTLTextureUsageShaderRead;
	}
	else
	{
		desc.storageMode = MTLStorageModePrivate;
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead
			| (kind->mips ? MTLTextureUsageShaderWrite : 0);
	}
	t->texture = [crt_device newTextureWithDescriptor:desc];
	if (!CRT_Metal_Check (t->texture, nil, "newTextureWithDescriptor"))
		return false;
	if (pixels)
		[t->texture replaceRegion:MTLRegionMake2D (0, 0, (NSUInteger)width, (NSUInteger)height) mipmapLevel:0
			withBytes:pixels bytesPerRow:(NSUInteger)width * 4];
	if (kind->mips)
		for (i = 0 ; i < t->numlevels ; i++)
		{
			t->levels[i] = [t->texture newTextureViewWithPixelFormat:desc.pixelFormat textureType:MTLTextureType2D
				levels:NSMakeRange ((NSUInteger)i, 1) slices:NSMakeRange (0, 1)];
			if (!CRT_Metal_Check (t->levels[i], nil, "newTextureViewWithPixelFormat"))
				return false;
		}
	[crt_residency addAllocation:t->texture];
	t->width = width;
	t->height = height;
	t->clear = kind->feedback;
	return true;
}

bool CRT_Metal_Init (id<MTLDevice> device, id<MTLResidencySet> residency, int slots)
{
	MTL4ComputePipelineDescriptor	*spd = [MTL4ComputePipelineDescriptor new];
	MTL4LibraryFunctionDescriptor	*cs = [MTL4LibraryFunctionDescriptor new];
	NSError							*error = nil;
	byte							*pixels;
	int								i, w, h;
	bool							ok;

	crt_device = device;
	crt_residency = residency;

	// the shaders, compiled with the program; the array stays where it is
	crt_library = [device newLibraryWithData:dispatch_data_create (vid_crt_metallib, vid_crt_metallib_size, NULL,
		^{}) error:&error];
	ok = CRT_Metal_Check (crt_library, error, "Loading the shaders");
	crt_compiler = ok ? [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error] : nil;
	ok = ok && CRT_Metal_Check (crt_compiler, error, "newCompilerWithDescriptor");
	for (i = 0 ; i < CRT_PASSES && ok ; i++)
		if (vid_crt_passes[i].output != CRT_SCREEN)
			ok = (crt_pipelines[i] = CRT_Metal_Pipeline (vid_crt_passes[i].name,
				vid_crt_textures[vid_crt_passes[i].output].format == CRT_RGBA16F ? MTLPixelFormatRGBA16Float
				: MTLPixelFormatRGBA8Unorm)) != nil;
	// the last pass's for the drawables, SDR and extended linear, made now and not as a frame is drawn
	ok = ok && CRT_Metal_LastPipeline (MTLPixelFormatBGR10A2Unorm) && CRT_Metal_LastPipeline (MTLPixelFormatRGBA16Float);
	if (ok)
	{
		cs.name = @"crt_spd";
		cs.library = crt_library;
		spd.computeFunctionDescriptor = cs;
		crt_spdpipeline = [crt_compiler newComputePipelineStateWithDescriptor:spd compilerTaskOptions:nil error:&error];
		ok = CRT_Metal_Check (crt_spdpipeline, error, "crt_spd's pipeline");
	}

	// a pass's parameters and sizes, and up to 7 textures; crt_spd's constants,
	// counter and texels, and a level and the 12 after it
	crt_table = ok ? CRT_Metal_Table (2, 8) : nil;
	crt_lasttable = ok ? CRT_Metal_Table (2, 8) : nil;
	crt_spdtable = ok ? CRT_Metal_Table (3, 1 + CRT_SPD_LEVELS) : nil;
	ok = ok && crt_table && crt_lasttable && crt_spdtable;

	crt_constants = ok ? [device newBufferWithLength:CRT_STRIDE * (NSUInteger)slots
		options:MTLResourceStorageModeShared] : nil;
	crt_spdbuffer = ok ? [device newBufferWithLength:CRT_SPD_BUFFER_SIZE options:MTLResourceStorageModeShared] : nil;
	ok = ok && CRT_Metal_Check (crt_constants, nil, "The constants buffer")
		&& CRT_Metal_Check (crt_spdbuffer, nil, "crt_spd's buffer");
	if (ok)
	{
		memset (crt_spdbuffer.contents, 0, CRT_SPD_BUFFER_SIZE);
		[residency addAllocation:crt_constants];
		[residency addAllocation:crt_spdbuffer];
	}

	for (i = 0 ; i < 4 && ok ; i++)
	{
		pixels = VID_CRT_LoadLUT (i, &w, &h);
		ok = pixels && CRT_Metal_CreateTexture (CRT_LUT1 + i, w, h, pixels);
		if (!pixels)
			Con_Printf ("The CRT shader: LUT %d is broken\n", i + 1);
		Mem_Free (pixels);
	}
	memset (crt_sizes, 0, sizeof(crt_sizes));

	if (!ok)
	{
		CRT_Metal_Shutdown ();
		return false;
	}
	[residency commit];
	return true;
}

void CRT_Metal_Shutdown (void)
{
	int		i;

	if (!crt_device)
		return;
	for (i = 0 ; i < CRT_TEXTURES ; i++)
		CRT_Metal_FreeTexture (&crt_textures[i]);
	if (crt_constants)
		[crt_residency removeAllocation:crt_constants];
	if (crt_spdbuffer)
		[crt_residency removeAllocation:crt_spdbuffer];
	[crt_residency commit];
	for (i = 0 ; i < CRT_PASSES ; i++)
		crt_pipelines[i] = nil;
	for (i = 0 ; i < CRT_FORMATS ; i++)
		crt_lastpipelines[i].pipeline = nil;
	crt_spdpipeline = nil;
	crt_compiler = nil;
	crt_library = nil;
	crt_table = crt_lasttable = crt_spdtable = nil;
	crt_constants = crt_spdbuffer = nil;
	crt_spddispatches = 0;
	crt_residency = nil;
	crt_device = nil;
}

bool CRT_Metal_SizeChanged (int sourcewidth, int sourceheight, int viewwidth, int viewheight)
{
	return crt_sizes[0] != sourcewidth || crt_sizes[1] != sourceheight || crt_sizes[2] != viewwidth
		|| crt_sizes[3] != viewheight;
}

void CRT_Metal_SetSize (int sourcewidth, int sourceheight, int viewwidth, int viewheight)
{
	crt_texture_t	*t;
	int				i, w, h;
	bool			made = true;

	for (i = 0 ; i < CRT_TEXTURES ; i++)
	{
		if (vid_crt_textures[i].size == CRT_SIZE_LUT)
			continue;
		t = &crt_textures[i];
		VID_CRT_TextureSize (i, sourcewidth, sourceheight, viewwidth, viewheight, &w, &h);
		if (t->texture && t->width == w && t->height == h)
			continue;
		if (!CRT_Metal_CreateTexture (i, w, h, NULL))
		{
			CRT_Metal_FreeTexture (t);
			made = false;
			continue;
		}
		if (vid_crt_textures[i].mips)
			crt_spddispatches = VID_CRT_SpdDispatches (w, h, crt_spd, crt_spdgroups);
	}
	[crt_residency commit];
	if (!made)
		return;		// a size that couldn't be made: nothing is drawn
	crt_sizes[0] = sourcewidth;
	crt_sizes[1] = sourceheight;
	crt_sizes[2] = viewwidth;
	crt_sizes[3] = viewheight;
}

// an encoder drawing in all of t's top level, once the fragment and dispatch work
// before it is done
static id<MTL4RenderCommandEncoder> CRT_Metal_Encoder (id<MTL4CommandBuffer> commands, const crt_texture_t *t,
	MTLLoadAction load)
{
	MTL4RenderPassDescriptor		*pass = [MTL4RenderPassDescriptor new];
	id<MTL4RenderCommandEncoder>	encoder;

	pass.colorAttachments[0].texture = t->texture;
	pass.colorAttachments[0].level = 0;
	pass.colorAttachments[0].loadAction = load;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake (0, 0, 0, 0);
	encoder = [commands renderCommandEncoderWithDescriptor:pass];
	[encoder barrierAfterQueueStages:MTLStageFragment | MTLStageDispatch beforeStages:MTLStageFragment
		visibilityOptions:MTL4VisibilityOptionDevice];
	[encoder setViewport:(MTLViewport){0, 0, (double)t->width, (double)t->height, 0, 1}];
	return encoder;
}

id<MTL4RenderCommandEncoder> CRT_Metal_BeginOriginal (id<MTL4CommandBuffer> commands)
{
	return CRT_Metal_Encoder (commands, &crt_textures[CRT_ORIGINAL], MTLLoadActionDontCare);
}

// PrePass's mip chain from its top level, just drawn
static void CRT_Metal_Mips (id<MTL4CommandBuffer> commands, uint64_t constants)
{
	id<MTL4ComputeCommandEncoder>	encoder = [commands computeCommandEncoder];
	const crt_texture_t				*t = &crt_textures[CRT_PREPASS];
	int								d, k, level;

	[encoder barrierAfterQueueStages:MTLStageFragment | MTLStageDispatch beforeStages:MTLStageDispatch
		visibilityOptions:MTL4VisibilityOptionDevice];
	[encoder setComputePipelineState:crt_spdpipeline];
	for (d = 0 ; d < crt_spddispatches ; d++)
	{
		// the dispatch before's levels, read by this one
		if (d)
			[encoder barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch
				visibilityOptions:MTL4VisibilityOptionDevice];
		[crt_spdtable setAddress:constants + CRT_SPD_AT (d) atIndex:0];
		[crt_spdtable setAddress:crt_spdbuffer.gpuAddress atIndex:1];
		[crt_spdtable setAddress:crt_spdbuffer.gpuAddress + 16 atIndex:2];
		[crt_spdtable setTexture:t->levels[crt_spd[d].base].gpuResourceID atIndex:0];
		for (k = 0 ; k < CRT_SPD_LEVELS ; k++)
		{
			level = (int)crt_spd[d].base + 1 + k;
			if (level >= t->numlevels)
				level = t->numlevels - 1;
			[crt_spdtable setTexture:t->levels[level].gpuResourceID atIndex:(NSUInteger)(1 + k)];
		}
		[encoder setArgumentTable:crt_spdtable];
		[encoder dispatchThreadgroups:MTLSizeMake ((NSUInteger)crt_spdgroups[d][0], (NSUInteger)crt_spdgroups[d][1], 1)
			threadsPerThreadgroup:MTLSizeMake (CRT_SPD_THREADS, 1, 1)];
	}
	[encoder endEncoding];
}

// a pass's textures and constants in table
static void CRT_Metal_Bind (id<MTL4ArgumentTable> table, int i, int parity, uint64_t constants)
{
	const vid_crt_pass_info_t	*pass = &vid_crt_passes[i];
	int							k;

	for (k = 0 ; k < pass->numinputs ; k++)
		[table setTexture:crt_textures[VID_CRT_Physical (pass->inputs[k], parity)].texture.gpuResourceID
			atIndex:(NSUInteger)k];
	[table setAddress:constants atIndex:0];
	[table setAddress:constants + CRT_PASS_AT (i) atIndex:1];
}

void CRT_Metal_EncodePasses (id<MTL4CommandBuffer> commands, int slot, const vid_crt_params_t *params,
	const vid_crt_pass_t passes[CRT_PASSES], int parity)
{
	id<MTL4RenderCommandEncoder>	encoder;
	byte							*at = (byte *)crt_constants.contents + CRT_STRIDE * slot;
	uint64_t						constants = crt_constants.gpuAddress + (uint64_t)CRT_STRIDE * (uint64_t)slot;
	int								i, d;

	memcpy (at, params, sizeof(*params));
	for (i = 0 ; i < CRT_PASSES ; i++)
		memcpy (at + CRT_PASS_AT (i), &passes[i], sizeof(passes[i]));
	for (d = 0 ; d < crt_spddispatches ; d++)
		memcpy (at + CRT_SPD_AT (d), &crt_spd[d], sizeof(crt_spd[d]));

	// the feedback textures made since: black, as the frame before had them
	for (i = 0 ; i < CRT_TEXTURES ; i++)
		if (crt_textures[i].clear)
		{
			[CRT_Metal_Encoder (commands, &crt_textures[i], MTLLoadActionClear) endEncoding];
			crt_textures[i].clear = false;
		}

	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		if (vid_crt_passes[i].output == CRT_SCREEN)
			break;		// CRT_Metal_EncodeLast's, in the caller's encoder
		encoder = CRT_Metal_Encoder (commands, &crt_textures[VID_CRT_Physical (vid_crt_passes[i].output, parity)],
			MTLLoadActionDontCare);
		[encoder setRenderPipelineState:crt_pipelines[i]];
		CRT_Metal_Bind (crt_table, i, parity, constants);
		[encoder setArgumentTable:crt_table atStages:MTLRenderStageFragment];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding];
		if (i == CRT_PASS_PREPASS)
			CRT_Metal_Mips (commands, constants);
	}
	crt_lastparity = parity;
	crt_lastconstants = constants;
}

void CRT_Metal_EncodeLast (id<MTL4RenderCommandEncoder> encoder, MTLPixelFormat format, MTLViewport viewport)
{
	id<MTLRenderPipelineState>	pipeline = CRT_Metal_LastPipeline (format);

	if (!pipeline)
		return;
	[encoder barrierAfterQueueStages:MTLStageFragment | MTLStageDispatch beforeStages:MTLStageFragment
		visibilityOptions:MTL4VisibilityOptionDevice];
	[encoder setRenderPipelineState:pipeline];
	[encoder setViewport:viewport];
	CRT_Metal_Bind (crt_lasttable, CRT_PASSES - 1, crt_lastparity, crt_lastconstants);
	[encoder setArgumentTable:crt_lasttable atStages:MTLRenderStageFragment];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

id<MTLTexture> CRT_Metal_Texture (int texture)
{
	return crt_textures[texture].texture;
}
