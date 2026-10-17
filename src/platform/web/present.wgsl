// present.wgsl -- draws the software-rendered frame into the letterboxed viewport;
// the web's present.glsl in WGSL for WebGPU (vid_webgpu.js), whose canvas can
// show HDR, both stages (vs and fs).
//
// The layers are integer textures of the renderer's pixels, a uint each, read by
// texel as the other shaders read the buffers. The 3D view is RGB30, each channel
// 512 times the fourth root of its light: 512 is SDR white, 1023 light 15.9 times
// as bright. The shader applies gamma and contrast to the light and takes it to
// the display, laying the view blend over its sRGB values as Quake did (in light
// it would wash the view out). For SDR (hdr 0), light brighter than white goes
// toward white keeping its hue, the rest is as it is, and the 2D (RGBA8, sRGB,
// premultiplied) is laid over it as it is. For HDR (hdr 3, a browser's extended
// sRGB canvas) it is light with SDR white at paper white, the browser's 1, only
// light near and past the display's peak compressed (and while the peak isn't
// above paper white, what is brighter goes toward white as for SDR), the 2D laid
// over it in light at paper white, and all of it put in sRGB's curve, which the
// canvas takes on past 1.

// vid_present_constants_t
struct Present
{
	blend: vec4f,		// sRGB color, and how much of it covers the view
	texsize: vec2f,		// frame size in texels
	scale: vec2f,		// screen pixels per texel
	gamma: f32,			// exponent applied to the view's light; 1 keeps it
	contrast: f32,		// the light as mid gray times (light / mid gray) to this; 1 keeps it
	sharp: f32,			// 0: integer scale, nearest texel; 1: sharp bilinear
	hdr: f32,			// 0: SDR output; 3: extended sRGB output
	paperwhite: f32,	// SDR white, in the output's light
	peak: f32,			// the display's brightest white, in the output's light
	pad: vec2f,
}

@group(0) @binding(0) var<uniform> p: Present;
@group(0) @binding(1) var u_view: texture_2d<u32>;
@group(0) @binding(2) var u_hud: texture_2d<u32>;

struct Varyings
{
	@builtin(position) position: vec4f,
	@location(0) uv: vec2f,
}

// A single triangle that covers the viewport, its top at the top as on the other systems.
@vertex
fn vs (@builtin(vertex_index) i: u32) -> Varyings
{
	let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
	return Varyings(vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0), uv);
}

// a layer's pixel, the nearest one inside it
fn Fetch (view: bool, t: vec2i) -> u32
{
	let c = clamp(t, vec2i(0), vec2i(p.texsize) - 1);
	if (view) {
		return textureLoad(u_view, c, 0).r;
	}
	return textureLoad(u_hud, c, 0).r;
}

fn Decode (view: bool, c: u32) -> vec4f
{
	if (view) {
		return vec4f(vec3f(vec3u(c, c >> 10u, c >> 20u) & vec3u(1023u)), 0.0) / 1023.0;
	}
	return vec4f(vec4u(c, c >> 8u, c >> 16u, c >> 24u) & vec4u(255u)) / 255.0;
}

// a layer of the frame, both as big: as a texture's sampler takes it, in the
// channels as they are
fn SampleLayer (view: bool, uv: vec2f) -> vec4f
{
	let texel = uv * p.texsize;

	if (p.sharp == 0.0) {
		return Decode(view, Fetch(view, vec2i(min(floor(texel), p.texsize - 1.0))));
	}

	// sharp bilinear: nearest inside each texel, a one pixel wide blend at the edges
	let base = floor(texel);
	let center_dist = fract(texel) - 0.5;
	let range = 0.5 - 0.5 / p.scale;
	let f = (center_dist - clamp(center_dist, -range, range)) * p.scale + 0.5;

	// the bilinear filter at base + f, in texels
	let at = base + f - 0.5;
	let t = vec2i(floor(at));
	let w = at - floor(at);
	let a = mix(Decode(view, Fetch(view, t)), Decode(view, Fetch(view, t + vec2i(1, 0))), w.x);
	let b = mix(Decode(view, Fetch(view, t + vec2i(0, 1))), Decode(view, Fetch(view, t + vec2i(1, 1))), w.x);
	return mix(a, b, w.y);
}

const MIDGRAY = 0.18;		// linear light that contrast keeps

// the view's linear light from its channels, fourth roots of it scaled to 512
fn ViewLight (texel: vec3f) -> vec3f
{
	var c = texel * (1023.0 / 512.0);
	c *= c;
	return c * c;
}

fn SrgbToLinear (c: vec3f) -> vec3f
{
	return select(pow(max((c + 0.055) / 1.055, vec3f(0.0)), vec3f(2.4)), c / 12.92, c <= vec3f(0.04045));
}

// on past 1 as it goes up to it, as an extended sRGB canvas takes it
fn LinearToSrgb (l: vec3f) -> vec3f
{
	return select(1.055 * pow(max(l, vec3f(0.0)), vec3f(1.0 / 2.4)) - 0.055, l * 12.92, l <= vec3f(0.0031308));
}

// light for SDR: what is brighter than white keeps its hue and goes toward white the
// brighter it is; the rest is as it is
fn FitWhite (c: vec3f) -> vec3f
{
	let m = max(c.r, max(c.g, c.b));

	if (m <= 1.0) {
		return c;
	}
	return mix(c / m, vec3f(1.0), 1.0 - 1.0 / m);
}

// compresses what is brighter than knee toward peak instead of clipping it
fn RollOff (c: vec3f, knee: f32, peak: f32) -> vec3f
{
	let over = max(c - knee, vec3f(0.0));
	let span = max(peak - knee, 1e-3);
	return min(c, vec3f(knee)) + span * (1.0 - exp(-over / span));
}

@fragment
fn fs (v: Varyings) -> @location(0) vec4f
{
	var light = ViewLight(SampleLayer(true, v.uv).rgb);
	let h = SampleLayer(false, v.uv);

	light = pow(max(light, vec3f(0.0)), vec3f(p.gamma));
	light = MIDGRAY * pow(max(light / MIDGRAY, vec3f(0.0)), vec3f(p.contrast));

	if (p.hdr == 0.0) {
		let c = mix(LinearToSrgb(FitWhite(light)), p.blend.rgb, p.blend.a);
		return vec4f(c * (1.0 - h.a) + h.rgb, 1.0);
	}

	// SDR white at paper white, linear up to near the peak; the 2D over it in
	// light; all of it in sRGB's curve
	light = SrgbToLinear(mix(LinearToSrgb(light), p.blend.rgb, p.blend.a));
	if (p.peak <= p.paperwhite * 1.05) {
		light = FitWhite(light) * p.paperwhite;
	}
	else {
		light = RollOff(light * p.paperwhite, max(p.paperwhite, 0.75 * p.peak), p.peak);
	}
	let hudlin = SrgbToLinear(h.rgb / max(h.a, 1.0 / 255.0)) * p.paperwhite * h.a;
	return vec4f(LinearToSrgb(hudlin + light * (1.0 - h.a)), 1.0);
}
