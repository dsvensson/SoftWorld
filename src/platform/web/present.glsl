// present.glsl -- draws the software-rendered frame into the letterboxed viewport;
// present.glsl of Linux in GLSL ES 3.00 for WebGL 2, both stages (VERTEX or
// FRAGMENT defined, after the #version line vid_webgl.js puts first). SDR only:
// WebGL's canvas is.
//
// The layers are integer textures of the renderer's pixels, a uint each, fetched
// by texel as the other shaders read the buffers. The 3D view is RGB30, each
// channel 512 times the fourth root of its light: 512 is SDR white, 1023 light
// 15.9 times as bright. The shader applies gamma and contrast to the light,
// takes what is brighter than white toward white keeping its hue, and lays the
// view blend over its sRGB values as Quake did (in light it would wash the view
// out). The 2D (RGBA8, sRGB, premultiplied) is laid over it as it is.

precision highp float;
precision highp int;
precision highp usampler2D;

// vid_present_constants_t
layout(std140) uniform Present
{
	vec4	blend;		// sRGB color, and how much of it covers the view
	vec2	texsize;	// frame size in texels
	vec2	scale;		// screen pixels per texel
	float	gamma;		// exponent applied to the view's light; 1 keeps it
	float	contrast;	// the light as mid gray times (light / mid gray) to this; 1 keeps it
	float	sharp;		// 0: integer scale, nearest texel; 1: sharp bilinear
	float	hdr;		// 0 here: SDR output
	float	paperwhite;
	float	peak;
	vec2	pad;
} p;

#ifdef VERTEX

out vec2 v_uv;

// A single triangle that covers the viewport, its top at the top as on the other systems.
void main ()
{
	vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
	v_uv = uv;
}

#else

uniform usampler2D u_view;
uniform usampler2D u_hud;

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// a layer's pixel, the nearest one inside it
uint Fetch (bool view, ivec2 t)
{
	t = clamp(t, ivec2(0), ivec2(p.texsize) - 1);
	return view ? texelFetch(u_view, t, 0).r : texelFetch(u_hud, t, 0).r;
}

vec4 Decode (bool view, uint c)
{
	if (view)
		return vec4(vec3(uvec3(c, c >> 10, c >> 20) & 1023u), 0.0) / 1023.0;
	return vec4(uvec4(c, c >> 8, c >> 16, c >> 24) & 255u) / 255.0;
}

// a layer of the frame, both as big: as a texture's sampler takes it, in the
// channels as they are
vec4 SampleLayer (bool view, vec2 uv)
{
	vec2 texel = uv * p.texsize;

	if (p.sharp == 0.0)
		return Decode(view, Fetch(view, ivec2(min(floor(texel), p.texsize - 1.0))));

	// sharp bilinear: nearest inside each texel, a one pixel wide blend at the edges
	vec2 base = floor(texel);
	vec2 center_dist = fract(texel) - 0.5;
	vec2 range = 0.5 - 0.5 / p.scale;
	vec2 f = (center_dist - clamp(center_dist, -range, range)) * p.scale + 0.5;

	// the bilinear filter at base + f, in texels
	vec2 at = base + f - 0.5;
	ivec2 t = ivec2(floor(at));
	vec2 w = at - floor(at);
	vec4 a = mix(Decode(view, Fetch(view, t)), Decode(view, Fetch(view, t + ivec2(1, 0))), w.x);
	vec4 b = mix(Decode(view, Fetch(view, t + ivec2(0, 1))), Decode(view, Fetch(view, t + ivec2(1, 1))), w.x);
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

vec3 LinearToSrgb (vec3 l)
{
	return mix(1.055 * pow(max(l, 0.0), vec3(1.0 / 2.4)) - 0.055, l * 12.92, lessThanEqual(l, vec3(0.0031308)));
}

// light for SDR: what is brighter than white keeps its hue and goes toward white the
// brighter it is; the rest is as it is
vec3 FitWhite (vec3 c)
{
	float m = max(c.r, max(c.g, c.b));
	return m <= 1.0 ? c : mix(c / m, vec3(1.0), 1.0 - 1.0 / m);
}

void main ()
{
	vec3 light = ViewLight(SampleLayer(true, v_uv).rgb);
	vec4 h = SampleLayer(false, v_uv);

	light = pow(max(light, 0.0), vec3(p.gamma));
	light = MIDGRAY * pow(max(light / MIDGRAY, 0.0), vec3(p.contrast));

	vec3 c = mix(LinearToSrgb(FitWhite(light)), p.blend.rgb, p.blend.a);
	o_color = vec4(c * (1.0 - h.a) + h.rgb, 1.0);
}

#endif
