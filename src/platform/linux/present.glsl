// present.glsl -- draws the software-rendered frame into the letterboxed viewport;
// present.metal in GLSL for Vulkan, both stages (VERTEX or FRAGMENT defined).
//
// The layers aren't textures: they are the renderer's own buffers, read through
// their device addresses, a pixel a uint. The 3D view is RGB30, each channel 512 times
// the fourth root of its light: 512 is SDR white, 1023 light 15.9 times as bright. The
// shader applies gamma and contrast to the light and takes it to the display, laying
// the view blend over its sRGB values as Quake did (in light it would wash the view
// out). For SDR, light brighter than white goes toward white keeping its hue, the rest
// is as it is. For HDR it is linear light with SDR white at paper white (hdr 1, as the
// other presenters have it), or that light as PQ in BT.2020 (hdr 2), 1 being reference
// white, 203 cd/m²; only light near and past the display's headroom is compressed, and
// while the headroom isn't above paper white, what is brighter goes toward white as for
// SDR. The 2D (RGBA8, sRGB, premultiplied) is laid over it as it is.

#version 460

#ifdef VERTEX

layout(location = 0) out vec2 o_uv;

// A single triangle that covers the viewport.
void main ()
{
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
	o_uv = uv;
}

#else

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Pixels
{
	uint	p[];
};

// vid_present_t: vid_present_constants_t, then the layers
layout(push_constant, std430) uniform Present
{
	vec4	blend;		// sRGB color, and how much of it covers the view
	vec2	texsize;	// frame size in texels
	vec2	scale;		// screen pixels per texel
	float	gamma;		// exponent applied to the view's light; 1 keeps it
	float	contrast;	// the light as mid gray times (light / mid gray) to this; 1 keeps it
	float	sharp;		// 0: integer scale, nearest texel; 1: sharp bilinear
	float	hdr;		// 0: SDR output; 1: linear output; 2: PQ output
	float	paperwhite;	// output value of SDR white
	float	peak;		// output value of the display's brightest white
	vec2	pad;
	uvec2	view;		// the 3D view's address
	uvec2	hud;		// the 2D's
	uint	rowpixels;	// pixels from one row of a layer to the next
} p;

layout(location = 0) in vec2 i_uv;
layout(location = 0) out vec4 o_color;

// a layer's pixel, the nearest one inside it: an address outside is a fault
uint Fetch (uvec2 layer, ivec2 t)
{
	t = clamp(t, ivec2(0), ivec2(p.texsize) - 1);
	return Pixels(layer).p[uint(t.y) * p.rowpixels + uint(t.x)];
}

vec4 Decode (bool view, uint c)
{
	if (view)
		return vec4(uvec3(c, c >> 10, c >> 20) & 1023u, 0) / 1023.0;
	return unpackUnorm4x8(c);
}

// a layer of the frame, both as big: as a texture's sampler takes it, in the
// channels as they are
vec4 SampleLayer (bool view, vec2 uv)
{
	uvec2 layer = view ? p.view : p.hud;
	vec2 texel = uv * p.texsize;

	if (p.sharp == 0)
		return Decode(view, Fetch(layer, ivec2(min(floor(texel), p.texsize - 1))));

	// sharp bilinear: nearest inside each texel, a one pixel wide blend at the edges
	vec2 base = floor(texel);
	vec2 center_dist = fract(texel) - 0.5;
	vec2 range = 0.5 - 0.5 / p.scale;
	vec2 f = (center_dist - clamp(center_dist, -range, range)) * p.scale + 0.5;

	// the bilinear filter at base + f, in texels
	vec2 at = base + f - 0.5;
	ivec2 t = ivec2(floor(at));
	vec2 w = at - floor(at);
	vec4 a = mix(Decode(view, Fetch(layer, t)), Decode(view, Fetch(layer, t + ivec2(1, 0))), w.x);
	vec4 b = mix(Decode(view, Fetch(layer, t + ivec2(0, 1))), Decode(view, Fetch(layer, t + ivec2(1, 1))), w.x);
	return mix(a, b, w.y);
}

const float MIDGRAY = 0.18;		// linear light that contrast keeps

// the view's linear light from its channels, fourth roots of it scaled to 512
vec3 ViewLight (vec3 texel)
{
	vec3 c = texel * (1023.0 / 512.0);
	c *= c;
	return c * c;
}

vec3 SrgbToLinear (vec3 c)
{
	return mix(pow(max((c + 0.055) / 1.055, 0.0), vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}

vec3 LinearToSrgb (vec3 l)
{
	return mix(1.055 * pow(max(l, 0.0), vec3(1.0 / 2.4)) - 0.055, l * 12.92, lessThanEqual(l, vec3(0.0031308)));
}

// light for SDR: what is brighter than white keeps its hue and goes toward white the
// brighter it is; the rest is as it is
vec3 FitWhite (vec3 c)
{
	float m = max(c.r, max(c.g, c.b));
	return m <= 1 ? c : mix(c / m, vec3(1.0), 1 - 1 / m);
}

// compresses what is brighter than knee toward peak instead of clipping it
vec3 RollOff (vec3 c, float knee, float peak)
{
	vec3 over = max(c - knee, 0.0);
	float span = max(peak - knee, 1e-3);
	return min(c, knee) + span * (1 - exp(-over / span));
}

// linear BT.709 light as PQ in BT.2020, 1 being reference white at 203 cd/m²
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
	return pow((c1 + c2 * ym) / (1 + c3 * ym), vec3(m2));
}

void main ()
{
	vec3 light = ViewLight(SampleLayer(true, i_uv).rgb);
	vec4 h = SampleLayer(false, i_uv);

	light = pow(max(light, 0.0), vec3(p.gamma));
	light = MIDGRAY * pow(max(light / MIDGRAY, 0.0), vec3(p.contrast));

	if (p.hdr == 0)
	{
		vec3 c = mix(LinearToSrgb(FitWhite(light)), p.blend.rgb, p.blend.a);
		o_color = vec4(c * (1 - h.a) + h.rgb, 1);
		return;
	}

	// SDR white at paper white, linear up to near the peak; the 2D over it in linear light
	light = SrgbToLinear(mix(LinearToSrgb(light), p.blend.rgb, p.blend.a));
	if (p.peak <= p.paperwhite * 1.05)
		light = FitWhite(light) * p.paperwhite;
	else
		light = RollOff(light * p.paperwhite, max(p.paperwhite, 0.75 * p.peak), p.peak);
	vec3 hudlin = SrgbToLinear(h.rgb / max(h.a, 1.0 / 255.0)) * p.paperwhite * h.a;
	light = hudlin + light * (1 - h.a);
	o_color = vec4(p.hdr == 2 ? LinearToPQ(light) : light, 1);
}

#endif
