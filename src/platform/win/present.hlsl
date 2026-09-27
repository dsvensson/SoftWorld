// present.hlsl -- draws the software-rendered frame into the letterboxed viewport.
//
// The 3D view is R10G10B10A2 holding linear light, each channel 512 times the fourth root of
// its light: 512 is SDR white, 1023 light 15.9 times as bright. The shader applies gamma and
// contrast to the light and takes it to the display, laying the view blend over its sRGB
// values as Quake did (in light it would wash the view out). For SDR, light brighter than
// white goes toward white keeping its hue, the rest is as it is. For HDR it is scRGB (linear,
// 1.0 = 80 nits) with SDR white at paper white, and only light near and past the display's
// peak is compressed. The 2D (RGBA8, sRGB, premultiplied) is laid over it as it is.

Texture2D<float4> g_frame : register(t0);
Texture2D<float4> g_hud : register(t1);
SamplerState g_linear : register(s0);

cbuffer Present : register(b0)
{
	float4	g_blend;		// sRGB color, and how much of it covers the view
	float2	g_texsize;		// frame size in texels
	float2	g_scale;		// screen pixels per texel
	float	g_gamma;		// exponent applied to the view's light; 1 keeps it
	float	g_contrast;		// the light as mid gray times (light / mid gray) to this; 1 keeps it
	float	g_sharp;		// 0: integer scale, nearest texel; 1: sharp bilinear
	float	g_hdr;			// 0: SDR output; 1: scRGB output
	float	g_paperwhite;	// scRGB value of SDR white
	float	g_peak;			// scRGB value of the display's brightest white
	float2	g_pad;
};

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

// A single triangle that covers the viewport.
VSOut VSMain (uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.uv = uv;
	return o;
}

// a layer of the frame, both as big
float4 SampleLayer (Texture2D<float4> layer, float2 uv)
{
	float2 texel = uv * g_texsize;
	float4 c;

	[branch] if (g_sharp == 0)
	{
		int2 t = int2(min(floor(texel), g_texsize - 1));
		c = layer.Load(int3(t, 0));
	}
	else
	{
		// sharp bilinear: nearest inside each texel, a one pixel wide blend at the edges
		float2 base = floor(texel);
		float2 center_dist = frac(texel) - 0.5;
		float2 range = 0.5 - 0.5 / g_scale;
		float2 f = (center_dist - clamp(center_dist, -range, range)) * g_scale + 0.5;
		c = layer.SampleLevel(g_linear, (base + f) / g_texsize, 0);
	}
	return c;
}

static const float MIDGRAY = 0.18;		// linear light that contrast keeps

// the view's linear light from its channels, fourth roots of it scaled to 512
float3 ViewLight (float3 texel)
{
	float3 c = texel * (1023.0 / 512.0);
	c *= c;
	return c * c;
}

float3 SrgbToLinear (float3 c)
{
	return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0), 2.4);
}

float3 LinearToSrgb (float3 l)
{
	return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(max(l, 0), 1.0 / 2.4) - 0.055;
}

// light for SDR: what is brighter than white keeps its hue and goes toward white the
// brighter it is; the rest is as it is
float3 FitWhite (float3 c)
{
	float m = max(c.r, max(c.g, c.b));
	return m <= 1 ? c : lerp(c / m, 1, 1 - 1 / m);
}

// compresses what is brighter than knee toward peak instead of clipping it
float3 RollOff (float3 c, float knee, float peak)
{
	float3 over = max(c - knee, 0);
	float span = max(peak - knee, 1e-3);
	return min(c, knee) + span * (1 - exp(-over / span));
}

float4 PSMain (VSOut i) : SV_Target
{
	float3 light = ViewLight(SampleLayer(g_frame, i.uv).rgb);
	float4 hud = SampleLayer(g_hud, i.uv);

	light = pow(max(light, 0), g_gamma);
	light = MIDGRAY * pow(max(light / MIDGRAY, 0), g_contrast);

	if (g_hdr == 0)
	{
		float3 c = lerp(LinearToSrgb(FitWhite(light)), g_blend.rgb, g_blend.a);
		return float4(c * (1 - hud.a) + hud.rgb, 1);
	}

	// SDR white at paper white, linear up to near the peak; the 2D over it in linear light
	light = SrgbToLinear(lerp(LinearToSrgb(light), g_blend.rgb, g_blend.a));
	light = RollOff(light * g_paperwhite, max(g_paperwhite, 0.75 * g_peak), g_peak);
	float3 hudlin = SrgbToLinear(hud.rgb / max(hud.a, 1.0 / 255.0)) * g_paperwhite * hud.a;
	return float4(hudlin + light * (1 - hud.a), 1);
}
