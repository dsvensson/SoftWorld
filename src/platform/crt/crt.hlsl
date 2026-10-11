// crt.hlsl -- guest(r)'s CRT-Guest-Advanced (crt_core.inc) in HLSL, for
// crt_d3d11.c: a vertex shader for the passes, a pixel shader each (PS_<pass>),
// the last taking its output to the window, SDR or scRGB; crt_spd.hlsl makes
// PrePass's mip chain. The passes' textures are T0 up, each read through
// g_nearest or g_linear (black past the edges, as RetroArch's default wrap mode
// has it); their parameters are P (crt_params.h, b0) and their sizes S
// (vid_crt_pass_t, b1).

#define CRT_HLSL 1

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mat3 float3x3
#define mix lerp
#define fract frac
#define inversesqrt rsqrt
#define V2(x) ((float2)(x))
#define V3(x) ((float3)(x))
#define V4(x) ((float4)(x))
// mat3 is built of rows here, the transpose of GLSL's columns
#define MUL(m, v) mul(v, m)
#define CONST static const
#define FUNC
#define LOOP [loop]
#define CTXP
#define CTXA
#define TEXP(t)
#define TEXA(t)
#define INOUT(type) inout type
#define TEXN(t, uv) t.SampleLevel(g_nearest, uv, 0)
#define TEXL(t, uv) t.SampleLevel(g_linear, uv, 0)
#define TEXLOD(t, uv, level) t.SampleLevel(g_linear, uv, level)

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
	float	Hdr;			// the last pass's output: 0 SDR, 1 scRGB
	float	PaperWhite;		// scRGB value of SDR white
};

cbuffer ParamsBlock : register(b0)
{
	Params	P;
};

cbuffer PassBlock : register(b1)
{
	PassInfo	S;
};

Texture2D		T0 : register(t0);
Texture2D		T1 : register(t1);
Texture2D		T2 : register(t2);
Texture2D		T3 : register(t3);
Texture2D		T4 : register(t4);
Texture2D		T5 : register(t5);
Texture2D		T6 : register(t6);
Texture2D		T7 : register(t7);
SamplerState	g_nearest : register(s0);
SamplerState	g_linear : register(s1);

#include "crt_core.inc"

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

// A single triangle that covers the viewport.
VSOut VS_CRT (uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.uv = uv;
	return o;
}

float4 PS_Afterglow (VSOut i) : SV_Target		{ return crt_afterglow(i.uv); }
float4 PS_Prepass (VSOut i) : SV_Target			{ return crt_prepass(i.uv); }
float4 PS_Avglum (VSOut i) : SV_Target			{ return crt_avglum(i.uv); }
float4 PS_Linearize (VSOut i) : SV_Target		{ return crt_linearize(i.uv); }
float4 PS_Gauss_h (VSOut i) : SV_Target			{ return crt_gauss_h(i.uv); }
float4 PS_Gauss_v (VSOut i) : SV_Target			{ return crt_gauss_v(i.uv); }
float4 PS_Bloom_h (VSOut i) : SV_Target			{ return crt_bloom_h(i.uv); }
float4 PS_Bloom_v (VSOut i) : SV_Target			{ return crt_bloom_v(i.uv); }
float4 PS_Crt (VSOut i) : SV_Target				{ return crt_crt(i.uv); }

float3 SrgbToLinear (float3 c)
{
	return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0), 2.4);
}

// what the shader shows, its values the display's as SDR has them, or in scRGB
// at paper white
float4 PS_Deconvergence (VSOut i) : SV_Target
{
	float3 c = crt_deconvergence(i.uv).rgb;

	if (S.Hdr == 0)
		return float4(c, 1);
	return float4(SrgbToLinear(saturate(c)) * S.PaperWhite, 1);
}
