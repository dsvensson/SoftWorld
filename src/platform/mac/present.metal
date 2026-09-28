// present.metal -- draws the software-rendered frame into the letterboxed viewport;
// present.hlsl in the Metal shading language.
//
// The 3D view is RGB10A2 holding linear light, each channel 512 times the fourth root of
// its light: 512 is SDR white, 1023 light 15.9 times as bright. The shader applies gamma and
// contrast to the light and takes it to the display, laying the view blend over its sRGB
// values as Quake did (in light it would wash the view out). For SDR, light brighter than
// white goes toward white keeping its hue, the rest is as it is. For HDR it is extended
// linear sRGB, 1.0 being SDR white as the system shows it (EDR), with SDR white at paper
// white, and only light near and past the display's headroom is compressed; while the
// headroom isn't above paper white (EDR coming on), what is brighter goes toward white as
// for SDR. The 2D (RGBA8, sRGB, premultiplied) is laid over it as it is.

#include <metal_stdlib>
using namespace metal;

struct Present
{
	float4	blend;		// sRGB color, and how much of it covers the view
	float2	texsize;	// frame size in texels
	float2	scale;		// screen pixels per texel
	float	gamma;		// exponent applied to the view's light; 1 keeps it
	float	contrast;	// the light as mid gray times (light / mid gray) to this; 1 keeps it
	float	sharp;		// 0: integer scale, nearest texel; 1: sharp bilinear
	float	hdr;		// 0: SDR output; 1: extended linear output
	float	paperwhite;	// output value of SDR white
	float	peak;		// output value of the display's brightest white
	float2	pad;
};
static_assert (sizeof(Present) == 64, "the constants are vid_present_constants_t");

struct VSOut
{
	float4 pos [[position]];
	float2 uv;
};

// A single triangle that covers the viewport.
vertex VSOut present_vs (uint id [[vertex_id]])
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.uv = uv;
	return o;
}

constexpr sampler linear_clamp (filter::linear, address::clamp_to_edge);

// a layer of the frame, both as big
static float4 SampleLayer (texture2d<float> layer, float2 uv, constant Present &p)
{
	float2 texel = uv * p.texsize;

	if (p.sharp == 0)
	{
		uint2 t = uint2(min(floor(texel), p.texsize - 1));
		return layer.read(t);
	}

	// sharp bilinear: nearest inside each texel, a one pixel wide blend at the edges
	float2 base = floor(texel);
	float2 center_dist = fract(texel) - 0.5;
	float2 range = 0.5 - 0.5 / p.scale;
	float2 f = (center_dist - clamp(center_dist, -range, range)) * p.scale + 0.5;
	return layer.sample(linear_clamp, (base + f) / p.texsize, level(0));
}

constant float MIDGRAY = 0.18;		// linear light that contrast keeps

// the view's linear light from its channels, fourth roots of it scaled to 512
static float3 ViewLight (float3 texel)
{
	float3 c = texel * (1023.0 / 512.0);
	c *= c;
	return c * c;
}

static float3 SrgbToLinear (float3 c)
{
	return select(pow(max((c + 0.055) / 1.055, 0.0), float3(2.4)), c / 12.92, c <= 0.04045);
}

static float3 LinearToSrgb (float3 l)
{
	return select(1.055 * pow(max(l, 0.0), float3(1.0 / 2.4)) - 0.055, l * 12.92, l <= 0.0031308);
}

// light for SDR: what is brighter than white keeps its hue and goes toward white the
// brighter it is; the rest is as it is
static float3 FitWhite (float3 c)
{
	float m = max(c.r, max(c.g, c.b));
	return m <= 1 ? c : mix(c / m, float3(1.0), 1 - 1 / m);
}

// compresses what is brighter than knee toward peak instead of clipping it
static float3 RollOff (float3 c, float knee, float peak)
{
	float3 over = max(c - knee, 0.0);
	float span = max(peak - knee, 1e-3);
	return min(c, knee) + span * (1 - exp(-over / span));
}

fragment float4 present_fs (VSOut i [[stage_in]],
	texture2d<float> frame [[texture(0)]],
	texture2d<float> hud [[texture(1)]],
	constant Present &p [[buffer(0)]])
{
	float3 light = ViewLight(SampleLayer(frame, i.uv, p).rgb);
	float4 h = SampleLayer(hud, i.uv, p);

	light = pow(max(light, 0.0), float3(p.gamma));
	light = MIDGRAY * pow(max(light / MIDGRAY, 0.0), float3(p.contrast));

	if (p.hdr == 0)
	{
		float3 c = mix(LinearToSrgb(FitWhite(light)), p.blend.rgb, p.blend.a);
		return float4(c * (1 - h.a) + h.rgb, 1);
	}

	// SDR white at paper white, linear up to near the peak; the 2D over it in linear light
	light = SrgbToLinear(mix(LinearToSrgb(light), p.blend.rgb, p.blend.a));
	if (p.peak <= p.paperwhite * 1.05)
		light = FitWhite(light) * p.paperwhite;
	else
		light = RollOff(light * p.paperwhite, max(p.paperwhite, 0.75 * p.peak), p.peak);
	float3 hudlin = SrgbToLinear(h.rgb / max(h.a, 1.0 / 255.0)) * p.paperwhite * h.a;
	return float4(hudlin + light * (1 - h.a), 1);
}
