// crt.metal -- guest(r)'s CRT-Guest-Advanced (crt_core.inc) in the Metal shading
// language, for crt_metal.m: crt_vs for the passes, a fragment function each
// (crt_<pass>_fs), the last taking its output to the window, SDR or extended
// linear (EDR), and crt_spd, which makes PrePass's mip chain. A pass's
// parameters are at buffer 0 (crt_params.h), its sizes at buffer 1
// (vid_crt_pass_t), its textures from texture 0 up, each read nearest or
// bilinear as the preset reads it, black past the edges as RetroArch's default
// wrap mode has it.

#include <metal_stdlib>
using namespace metal;

#define CRT_METAL 1

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mat3 float3x3
#define inversesqrt rsqrt
#define V2(x) float2(x)
#define V3(x) float3(x)
#define V4(x) float4(x)
#define MUL(m, v) ((m) * (v))
#define CONST constant
#define FUNC static
#define LOOP
#define CTXP constant Params &P, constant PassInfo &S,
#define CTXA P, S,
#define TEXP(t) texture2d<float> t,
#define TEXA(t) t,
#define INOUT(type) thread type &

constexpr sampler g_nearest (filter::nearest, address::clamp_to_border, border_color::transparent_black);
constexpr sampler g_linear (filter::linear, mip_filter::linear, address::clamp_to_border,
	border_color::transparent_black);

#define TEXN(t, uv) t.sample(g_nearest, uv, level(0))
#define TEXL(t, uv) t.sample(g_linear, uv, level(0))
#define TEXLOD(t, uv, lod) t.sample(g_linear, uv, level(lod))

struct Params
{
#define CRT_PARAM(name) float name;
#include "crt_params.h"
#undef CRT_PARAM
};

struct PassInfo
{
	float4	SourceSize;
	float4	OriginalSize;
	float4	OutputSize;
	uint	FrameCount;
	float	TimeScale;		// frames of 60 Hz since the last
	float	Hdr;			// the last pass's output: 0 SDR, 1 extended linear
	float	PaperWhite;		// output value of SDR white
};
static_assert (sizeof(PassInfo) == 64, "the constants are vid_crt_pass_t");

#include "crt_core.inc"

struct VSOut
{
	float4 pos [[position]];
	float2 uv;
};

// A single triangle that covers the viewport.
vertex VSOut crt_vs (uint id [[vertex_id]])
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.uv = uv;
	return o;
}

#define CRT_ARGS VSOut i [[stage_in]], constant Params &P [[buffer(0)]], constant PassInfo &S [[buffer(1)]]

fragment float4 crt_afterglow_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]], texture2d<float> T1 [[texture(1)]])
{
	return crt_afterglow(P, S, T0, T1, i.uv);
}

fragment float4 crt_prepass_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]], texture2d<float> T1 [[texture(1)]],
	texture2d<float> T2 [[texture(2)]], texture2d<float> T3 [[texture(3)]], texture2d<float> T4 [[texture(4)]],
	texture2d<float> T5 [[texture(5)]])
{
	return crt_prepass(P, S, T0, T1, T2, T3, T4, T5, i.uv);
}

fragment float4 crt_avglum_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]], texture2d<float> T1 [[texture(1)]])
{
	return crt_avglum(P, S, T0, T1, i.uv);
}

fragment float4 crt_linearize_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]])
{
	return crt_linearize(P, S, T0, i.uv);
}

fragment float4 crt_gauss_h_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]])
{
	return crt_gauss_h(P, S, T0, i.uv);
}

fragment float4 crt_gauss_v_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]])
{
	return crt_gauss_v(P, S, T0, i.uv);
}

fragment float4 crt_bloom_h_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]])
{
	return crt_bloom_h(P, S, T0, i.uv);
}

fragment float4 crt_bloom_v_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]])
{
	return crt_bloom_v(P, S, T0, i.uv);
}

fragment float4 crt_crt_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]], texture2d<float> T1 [[texture(1)]],
	texture2d<float> T2 [[texture(2)]])
{
	return crt_crt(P, S, T0, T1, T2, i.uv);
}

static float3 SrgbToLinear (float3 c)
{
	return select(pow(max((c + 0.055) / 1.055, 0.0), float3(2.4)), c / 12.92, c <= 0.04045);
}

// what the shader shows, its values the display's as SDR has them, or in
// extended linear light at paper white
fragment float4 crt_deconvergence_fs (CRT_ARGS, texture2d<float> T0 [[texture(0)]], texture2d<float> T1 [[texture(1)]],
	texture2d<float> T2 [[texture(2)]], texture2d<float> T3 [[texture(3)]], texture2d<float> T4 [[texture(4)]],
	texture2d<float> T5 [[texture(5)]], texture2d<float> T6 [[texture(6)]])
{
	float3 c = crt_deconvergence(P, S, T0, T1, T2, T3, T4, T5, T6, i.uv).rgb;

	if (S.Hdr == 0)
		return float4(c, 1);
	return float4(SrgbToLinear(saturate(c)) * S.PaperWhite, 1);
}

/*
===============================================================================

crt_spd: PrePass's mip chain in one dispatch, as AMD's FidelityFX Single Pass
Downsampler makes one, as crt_spd.hlsl. Each workgroup makes its 64x64 tile of the
base level into the 6 levels after it, a level from four texels of the one
before, and writes its one texel of the sixth to the buffer too; the last
workgroup to be done makes the levels after that from those. Each texel is the
average of the four it covers in the level before (a level is half the one
before's size, rounded down), what lies past an edge of a level 1 across the
texel there.

===============================================================================
*/

// vid_crt_spd_t
struct SpdInfo
{
	uint	base;		// unused here: the views are of the levels from it
	uint	levels;		// how many levels after it to make
	uint	numgroups;
	uint	groupsx;
	uint	width;		// the base level's size
	uint	height;
	uint2	pad;
};

typedef array<texture2d<float, access::write>, 12>	spd_mips_t;

// a level's size, the base 0
static uint2 SpdSize (constant SpdInfo &g, uint level)
{
	return max(uint2(g.width, g.height) >> level, uint2(1));
}

static void SpdStore (constant SpdInfo &g, spd_mips_t mips, uint level, uint2 p, float4 v)
{
	if (level <= g.levels && all(p < SpdSize(g, level)))
		mips[level - 1].write(v, p);
}

// a level made of the one before's texels in registers: each thread's four,
// 2x2 from (2tx, 2ty) of the tile at tile, which that level's size cuts
static float4 SpdReduceQuad (thread float4 *v, uint2 tile, uint tx, uint ty, uint2 size)
{
	uint2	p = tile + uint2(tx, ty) * 2;
	bool	xok = p.x + 1 < size.x, yok = p.y + 1 < size.y;

	return (v[0] + (xok ? v[1] : v[0]) + (yok ? v[2] : v[0]) + (xok ? (yok ? v[3] : v[1]) : (yok ? v[2] : v[0])))
		* 0.25;
}

// level, n texels across the tile, of the one before in lds (16 across), whose
// tile at group (2n across) its size cuts
static void SpdReduceLDS (constant SpdInfo &g, spd_mips_t mips, threadgroup float4 *lds, uint level, uint n,
	uint2 group, uint t, uint2 size)
{
	uint	x = t % n, y = t / n;
	int2	lim = int2(size) - 1 - int2(group * (n * 2));
	uint2	last = uint2(clamp(lim, int2(0), int2(int(n * 2) - 1)));
	float4	r = float4(0);

	if (t < n * n)
	{
		uint2	a = uint2(x, y) * 2;
		uint2	b = min(a + 1, last);

		r = (lds[a.y * 16 + a.x] + lds[a.y * 16 + b.x] + lds[b.y * 16 + a.x] + lds[b.y * 16 + b.x]) * 0.25;
	}
	threadgroup_barrier (mem_flags::mem_threadgroup);
	if (t < n * n)
	{
		lds[y * 16 + x] = r;
		SpdStore(g, mips, level, group * n + uint2(x, y), r);
	}
	threadgroup_barrier (mem_flags::mem_threadgroup);
}

kernel void crt_spd (uint2 group [[threadgroup_position_in_grid]], uint t [[thread_index_in_threadgroup]],
	constant SpdInfo &g [[buffer(0)]],
	device atomic_uint *counter [[buffer(1)]],					// workgroups done
	coherent(device) device float4 *mid [[buffer(2)]],			// level 6's texels
	texture2d<float, access::read> source [[texture(0)]],		// the base level
	spd_mips_t mips [[texture(1)]])								// the levels after it
{
	threadgroup float4	lds[16 * 16];
	threadgroup uint	islast;
	uint	tx = t % 16, ty = t / 16;
	uint2	last = SpdSize(g, 0) - 1;
	float4	v[4];
	float4	m;

	// level 1: each thread 2x2 of the tile's 32x32
	for (uint j = 0 ; j < 2 ; j++)
		for (uint i = 0 ; i < 2 ; i++)
		{
			uint2	p = group * 32 + uint2(tx * 2 + i, ty * 2 + j);
			uint2	q = p * 2;

			v[j * 2 + i] = (source.read(min(q, last)) + source.read(min(q + uint2(1, 0), last))
				+ source.read(min(q + uint2(0, 1), last)) + source.read(min(q + 1, last))) * 0.25;
			SpdStore(g, mips, 1, p, v[j * 2 + i]);
		}
	if (g.levels < 2)
		return;

	// level 2: a texel each, 16x16
	m = SpdReduceQuad(v, group * 32, tx, ty, SpdSize(g, 1));
	SpdStore(g, mips, 2, group * 16 + uint2(tx, ty), m);
	lds[ty * 16 + tx] = m;
	threadgroup_barrier (mem_flags::mem_threadgroup);

	// levels 3 to 6, 8x8 to the one texel
	if (g.levels < 3)
		return;
	SpdReduceLDS(g, mips, lds, 3, 8, group, t, SpdSize(g, 2));
	if (g.levels < 4)
		return;
	SpdReduceLDS(g, mips, lds, 4, 4, group, t, SpdSize(g, 3));
	if (g.levels < 5)
		return;
	SpdReduceLDS(g, mips, lds, 5, 2, group, t, SpdSize(g, 4));
	if (g.levels < 6)
		return;
	SpdReduceLDS(g, mips, lds, 6, 1, group, t, SpdSize(g, 5));
	if (g.levels < 7)
		return;

	// the workgroup's texel of level 6 for the last one, which makes the rest
	if (t == 0)
	{
		mid[group.y * 64 + group.x] = lds[0];
		atomic_thread_fence (mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
		islast = atomic_fetch_add_explicit(counter, 1, memory_order_relaxed) == g.numgroups - 1 ? 1 : 0;
		atomic_thread_fence (mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
	}
	threadgroup_barrier (mem_flags::mem_threadgroup);
	if (!islast)
		return;
	if (t == 0)
		atomic_store_explicit(counter, 0, memory_order_relaxed);		// for the next dispatch

	// level 7 of level 6, as level 1 of the base
	last = SpdSize(g, 6) - 1;
	for (uint j = 0 ; j < 2 ; j++)
		for (uint i = 0 ; i < 2 ; i++)
		{
			uint2	p = uint2(tx * 2 + i, ty * 2 + j);
			uint2	q = p * 2;
			uint2	a = min(q, last), b = min(q + 1, last);

			v[j * 2 + i] = (mid[a.y * 64 + a.x] + mid[a.y * 64 + b.x] + mid[b.y * 64 + a.x] + mid[b.y * 64 + b.x])
				* 0.25;
			SpdStore(g, mips, 7, p, v[j * 2 + i]);
		}
	if (g.levels < 8)
		return;

	m = SpdReduceQuad(v, uint2(0), tx, ty, SpdSize(g, 7));
	SpdStore(g, mips, 8, uint2(tx, ty), m);
	lds[ty * 16 + tx] = m;
	threadgroup_barrier (mem_flags::mem_threadgroup);

	if (g.levels < 9)
		return;
	SpdReduceLDS(g, mips, lds, 9, 8, uint2(0), t, SpdSize(g, 8));
	if (g.levels < 10)
		return;
	SpdReduceLDS(g, mips, lds, 10, 4, uint2(0), t, SpdSize(g, 9));
	if (g.levels < 11)
		return;
	SpdReduceLDS(g, mips, lds, 11, 2, uint2(0), t, SpdSize(g, 10));
	if (g.levels < 12)
		return;
	SpdReduceLDS(g, mips, lds, 12, 1, uint2(0), t, SpdSize(g, 11));
}
