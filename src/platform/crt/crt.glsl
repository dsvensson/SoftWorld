#version 460
// crt.glsl -- guest(r)'s CRT-Guest-Advanced (crt_core.inc) in GLSL, for Vulkan
// (crt_vulkan.c: glslang, which defines VULKAN) and for WebGL 2 (vid_webgl.js:
// GLSL ES 3.00, the #version line replaced by its own and the included files
// put in place). A stage is compiled with VERTEX, FRAGMENT or (Vulkan's alone)
// COMPUTE defined, a fragment stage with its pass's PASS_<NAME> too.
//
// The passes' textures are T0 up, read as each one's sampler says (crt_vulkan.c
// and vid_webgl.js give each its filter), black past the edges: Vulkan's
// border, WebGL's made in the shader. Their parameters are P (crt_params.h) and
// their sizes S
// (vid_crt_pass_t): on Vulkan a uniform buffer and push constants, on WebGL
// uniform buffers at bindings 1 and 2. The last pass takes its output to the
// window: SDR, linear light, or (Vulkan) PQ. The compute stage makes PrePass's
// mip chain (crt_spd).

#ifdef VULKAN
#extension GL_GOOGLE_include_directive : require
#else
precision highp float;
precision highp int;
precision highp sampler2D;
#endif

#ifdef VERTEX

#ifdef VULKAN
layout(location = 0) out vec2 v_uv;
#define VERTEX_ID gl_VertexIndex
#else
out vec2 v_uv;
#define VERTEX_ID gl_VertexID
#endif

// A single triangle that covers the viewport: its top at the top of the image,
// which for WebGL's framebuffers is their first row, and on the canvas (SCREEN)
// at the top as on the other systems
void main ()
{
	vec2 uv = vec2((VERTEX_ID << 1) & 2, VERTEX_ID & 2);
#if defined(SCREEN) && !defined(VULKAN)
	gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
#else
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
#endif
	v_uv = uv;
}

#endif

#ifdef FRAGMENT

#define CRT_GLSL 1
#define V2(x) vec2(x)
#define V3(x) vec3(x)
#define V4(x) vec4(x)
#define MUL(m, v) ((m) * (v))
#define CONST const
#define FUNC
#define LOOP
#define CTXP
#define CTXA
#define TEXP(t)
#define TEXA(t)
#define INOUT(type) inout type
#define TEXLOD(t, uv, level) textureLod(t, uv, level)
#ifdef VULKAN
#define TEXN(t, uv) textureLod(t, uv, 0.0)
#define TEXL(t, uv) textureLod(t, uv, 0.0)
#else
// WebGL has no border to clamp to: what lies past a texture's edges is made
// black as a border would have it, the nearest texel, or a bilinear sample's
// part of the texels inside
float crt_border (vec2 uv, vec2 size)
{
	vec2 inside = clamp(uv * size + 0.5, 0.0, 1.0) * clamp((1.0 - uv) * size + 0.5, 0.0, 1.0);
	return inside.x * inside.y;
}
float crt_inside (vec2 uv)
{
	return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))) ? 1.0 : 0.0;
}
#define TEXN(t, uv) (textureLod(t, uv, 0.0) * crt_inside(uv))
#define TEXL(t, uv) (textureLod(t, uv, 0.0) * crt_border(uv, vec2(textureSize(t, 0))))
#endif

#ifdef VULKAN
layout(std140, set = 0, binding = 0) uniform ParamsBlock
#else
layout(std140) uniform ParamsBlock
#endif
{
#define CRT_PARAM(name) float name;
#include "crt_params.h"
#undef CRT_PARAM
} P;

#ifdef VULKAN
layout(push_constant) uniform PassBlock
#else
layout(std140) uniform PassBlock
#endif
{
	vec4	SourceSize;
	vec4	OriginalSize;
	vec4	OutputSize;
	uint	FrameCount;
	float	TimeScale;		// frames of 60 Hz since the last
	float	Hdr;			// the last pass's output: 0 SDR, 1 linear, 2 PQ
	float	PaperWhite;		// output value of SDR white
} S;

#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D T0;
layout(set = 0, binding = 2) uniform sampler2D T1;
layout(set = 0, binding = 3) uniform sampler2D T2;
layout(set = 0, binding = 4) uniform sampler2D T3;
layout(set = 0, binding = 5) uniform sampler2D T4;
layout(set = 0, binding = 6) uniform sampler2D T5;
layout(set = 0, binding = 7) uniform sampler2D T6;
layout(set = 0, binding = 8) uniform sampler2D T7;
layout(location = 0) in vec2 v_uv;
#else
uniform sampler2D T0;
uniform sampler2D T1;
uniform sampler2D T2;
uniform sampler2D T3;
uniform sampler2D T4;
uniform sampler2D T5;
uniform sampler2D T6;
uniform sampler2D T7;
in vec2 v_uv;
#endif
layout(location = 0) out vec4 o_color;

#include "crt_core.inc"

#if defined(PASS_AFTERGLOW)
void main () { o_color = crt_afterglow(v_uv); }
#elif defined(PASS_PREPASS)
void main () { o_color = crt_prepass(v_uv); }
#elif defined(PASS_AVGLUM)
void main () { o_color = crt_avglum(v_uv); }
#elif defined(PASS_LINEARIZE)
void main () { o_color = crt_linearize(v_uv); }
#elif defined(PASS_GAUSS_H)
void main () { o_color = crt_gauss_h(v_uv); }
#elif defined(PASS_GAUSS_V)
void main () { o_color = crt_gauss_v(v_uv); }
#elif defined(PASS_BLOOM_H)
void main () { o_color = crt_bloom_h(v_uv); }
#elif defined(PASS_BLOOM_V)
void main () { o_color = crt_bloom_v(v_uv); }
#elif defined(PASS_CRT)
void main () { o_color = crt_crt(v_uv); }
#elif defined(PASS_DECONVERGENCE)

vec3 SrgbToLinear (vec3 c)
{
	return mix(pow(max((c + 0.055) / 1.055, 0.0), vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}

// linear BT.709 light as PQ in BT.2020, 1 being reference white at 203 cd/m2
vec3 LinearToPQ (vec3 l)
{
	const mat3 BT709_TO_BT2020 = mat3(
		0.627404, 0.069097, 0.016391,
		0.329283, 0.919541, 0.088013,
		0.043313, 0.011362, 0.895595);
	const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;

	vec3 y = clamp(BT709_TO_BT2020 * l * (203.0 / 10000.0), 0.0, 1.0);
	vec3 ym = pow(y, vec3(m1));
	return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}

// what the shader shows, its values the display's as SDR has them, or in light
// at paper white
void main ()
{
	vec3 c = crt_deconvergence(v_uv).rgb;

	if (S.Hdr == 0.0)
		o_color = vec4(c, 1.0);
	else
	{
		vec3 l = SrgbToLinear(clamp(c, 0.0, 1.0)) * S.PaperWhite;
		o_color = vec4(S.Hdr == 2.0 ? LinearToPQ(l) : l, 1.0);
	}
}
#endif

#endif

#if defined(COMPUTE) && defined(VULKAN)

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

layout(local_size_x = 256) in;

// vid_crt_spd_t
layout(push_constant) uniform SpdBlock
{
	uint	base;		// unused here: the views are of the levels from it
	uint	levels;		// how many levels after it to make
	uint	numgroups;
	uint	groupsx;
	uint	width;		// the base level's size
	uint	height;
	uvec2	pad;
} g;

layout(set = 0, binding = 0, rgba8) uniform readonly image2D g_source;		// the base level
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D g_mips[12];	// the levels after it
layout(set = 0, binding = 2, std430) coherent buffer SpdBuffer				// the counter, then level 6's texels
{
	uint	counter;
	uint	pad[3];
	vec4	mid[];
} g_buffer;

shared vec4	g_lds[16][16];
shared uint	g_last;

// a level's size, the base 0
uvec2 SpdSize (uint level)
{
	return max(uvec2(g.width, g.height) >> level, uvec2(1));
}

// the array of images is read by constant indices alone: a level is a literal
#define SPD_STORE(level, p, v) \
	if ((level) <= g.levels && all(lessThan(p, SpdSize(level)))) \
		imageStore(g_mips[(level) - 1u], ivec2(p), v)

vec4 SpdLoadSource (uvec2 p, uvec2 last)
{
	return imageLoad(g_source, ivec2(min(p, last)));
}

vec4 SpdLoadMid (uvec2 p, uvec2 last)
{
	p = min(p, last);
	return g_buffer.mid[p.y * 64u + p.x];
}

// a level made of the one before's texels in registers: each thread's four,
// 2x2 from (2tx, 2ty) of the tile at tile, which that level's size cuts
vec4 SpdReduceQuad (vec4 v[4], uvec2 tile, uint tx, uint ty, uvec2 size)
{
	uvec2	p = tile + uvec2(tx, ty) * 2u;
	bool	xok = p.x + 1u < size.x, yok = p.y + 1u < size.y;

	return (v[0] + (xok ? v[1] : v[0]) + (yok ? v[2] : v[0]) + (xok ? (yok ? v[3] : v[1]) : (yok ? v[2] : v[0])))
		* 0.25;
}

// a level n texels across the tile of the one before in g_lds, whose tile at
// group (2n across) its size cuts; for the threads up to n*n, which store it
vec4 SpdReduceLDS (uint n, uvec2 group, uint t, uvec2 size)
{
	uint	x = t % n, y = t / n;
	ivec2	lim = ivec2(size) - 1 - ivec2(group * (n * 2u));
	uvec2	last = uvec2(clamp(lim, ivec2(0), ivec2(int(n * 2u) - 1)));
	vec4	r = vec4(0.0);

	if (t < n * n)
	{
		uvec2	a = uvec2(x, y) * 2u;
		uvec2	b = min(a + 1u, last);

		r = (g_lds[a.y][a.x] + g_lds[a.y][b.x] + g_lds[b.y][a.x] + g_lds[b.y][b.x]) * 0.25;
	}
	barrier ();
	if (t < n * n)
		g_lds[y][x] = r;
	barrier ();
	return r;
}

void main ()
{
	uvec2	group = gl_WorkGroupID.xy;
	uint	t = gl_LocalInvocationIndex;
	uint	tx = t % 16u, ty = t / 16u;
	uvec2	last = SpdSize(0u) - 1u;
	vec4	v[4];
	vec4	m;
	uint	i, j;

	// level 1: each thread 2x2 of the tile's 32x32
	for (j = 0u ; j < 2u ; j++)
		for (i = 0u ; i < 2u ; i++)
		{
			uvec2	p = group * 32u + uvec2(tx * 2u + i, ty * 2u + j);
			uvec2	q = p * 2u;

			v[j * 2u + i] = (SpdLoadSource(q, last) + SpdLoadSource(q + uvec2(1, 0), last)
				+ SpdLoadSource(q + uvec2(0, 1), last) + SpdLoadSource(q + uvec2(1, 1), last)) * 0.25;
			SPD_STORE(1u, p, v[j * 2u + i]);
		}
	if (g.levels < 2u)
		return;

	// level 2: a texel each, 16x16
	m = SpdReduceQuad(v, group * 32u, tx, ty, SpdSize(1u));
	SPD_STORE(2u, group * 16u + uvec2(tx, ty), m);
	g_lds[ty][tx] = m;
	barrier ();

	// levels 3 to 6, 8x8 to the one texel
	if (g.levels < 3u)
		return;
	m = SpdReduceLDS(8u, group, t, SpdSize(2u));
	if (t < 64u) SPD_STORE(3u, group * 8u + uvec2(t % 8u, t / 8u), m);
	if (g.levels < 4u)
		return;
	m = SpdReduceLDS(4u, group, t, SpdSize(3u));
	if (t < 16u) SPD_STORE(4u, group * 4u + uvec2(t % 4u, t / 4u), m);
	if (g.levels < 5u)
		return;
	m = SpdReduceLDS(2u, group, t, SpdSize(4u));
	if (t < 4u) SPD_STORE(5u, group * 2u + uvec2(t % 2u, t / 2u), m);
	if (g.levels < 6u)
		return;
	m = SpdReduceLDS(1u, group, t, SpdSize(5u));
	if (t < 1u) SPD_STORE(6u, group, m);
	if (g.levels < 7u)
		return;

	// the workgroup's texel of level 6 for the last one, which makes the rest
	if (t == 0u)
		g_buffer.mid[group.y * 64u + group.x] = g_lds[0][0];
	memoryBarrierBuffer ();
	barrier ();
	if (t == 0u)
		g_last = atomicAdd(g_buffer.counter, 1u) == g.numgroups - 1u ? 1u : 0u;
	barrier ();
	if (g_last == 0u)
		return;
	if (t == 0u)
		g_buffer.counter = 0u;		// for the next dispatch
	memoryBarrierBuffer ();

	// level 7 of level 6, as level 1 of the base
	last = SpdSize(6u) - 1u;
	for (j = 0u ; j < 2u ; j++)
		for (i = 0u ; i < 2u ; i++)
		{
			uvec2	p = uvec2(tx * 2u + i, ty * 2u + j);
			uvec2	q = p * 2u;

			v[j * 2u + i] = (SpdLoadMid(q, last) + SpdLoadMid(q + uvec2(1, 0), last)
				+ SpdLoadMid(q + uvec2(0, 1), last) + SpdLoadMid(q + uvec2(1, 1), last)) * 0.25;
			SPD_STORE(7u, p, v[j * 2u + i]);
		}
	if (g.levels < 8u)
		return;

	m = SpdReduceQuad(v, uvec2(0), tx, ty, SpdSize(7u));
	SPD_STORE(8u, uvec2(tx, ty), m);
	g_lds[ty][tx] = m;
	barrier ();

	if (g.levels < 9u)
		return;
	m = SpdReduceLDS(8u, uvec2(0), t, SpdSize(8u));
	if (t < 64u) SPD_STORE(9u, uvec2(t % 8u, t / 8u), m);
	if (g.levels < 10u)
		return;
	m = SpdReduceLDS(4u, uvec2(0), t, SpdSize(9u));
	if (t < 16u) SPD_STORE(10u, uvec2(t % 4u, t / 4u), m);
	if (g.levels < 11u)
		return;
	m = SpdReduceLDS(2u, uvec2(0), t, SpdSize(10u));
	if (t < 4u) SPD_STORE(11u, uvec2(t % 2u, t / 2u), m);
	if (g.levels < 12u)
		return;
	m = SpdReduceLDS(1u, uvec2(0), t, SpdSize(11u));
	if (t < 1u) SPD_STORE(12u, uvec2(0), m);
}

#endif
