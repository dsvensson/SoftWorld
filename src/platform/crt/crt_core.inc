/*
   CRT - Guest - Advanced

   Copyright (C) 2018-2025 guest(r)
   Pre-shaders afterglow (color altering): Copyright (C) 2019-2025 guest(r) and Dr. Venom

   Incorporates many good ideas and suggestions from Dr. Venom.
   I would also like give thanks to many Libretro forums members for continuous feedback, suggestions and using the shader.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// crt_core.inc -- the passes of crt-guest-advanced.slangp after its two stock
// passes (vid_crt.h), from libretro's slang-shaders, crt/shaders/guest/advanced
// as of 77c52ce (2026-03-25): afterglow0, pre-shaders-afterglow, avg-lum,
// linearize, gaussian_horizontal and _vertical, bloom_horizontal and _vertical,
// crt-guest-advanced and deconvergence. Each pass is a function crt_<name> of the
// texture coordinate, returning what it draws.
//
// The code is in GLSL's words, made HLSL and the Metal shading language by the
// file that includes it (crt.hlsl, crt.glsl, crt.metal), which defines:
//   vec2 to vec4 and mat3       the types, and mix, fract, inversesqrt where they're named otherwise
//   V2(x), V3(x), V4(x)         a vector of x
//   MUL(m, v)                   a matrix built of column vectors times a vector
//   CONST                       a constant of the program's
//   FUNC                        a function of the file's own
//   LOOP                        a loop not to unroll
//   P, S                        the parameters (crt_params.h) and the pass's sizes (vid_crt_pass_t)
//   T0 to T7                    the pass's textures
//   TEXN(t, uv), TEXL(t, uv)    a texture's top level, its nearest texel, or bilinear
//   TEXLOD(t, uv, level)        a mip level, bilinear
//   CTXP, CTXA                  in Metal, a function's parameters for P and S, and its arguments
//   TEXP(t), TEXA(t)            and for a texture
//   INOUT(type)                 a parameter written back
//
// What differs from the slang shaders:
// - Textures are read at their top level: the mip chain is avg-lum's, at the
//   level it names, and no pass reads anything smaller than it is drawn.
// - Afterglow's persistence and raster bloom's smoothing are per frame of 60 Hz,
//   as many as have passed (S.TimeScale): as they were at 60 frames a second, and
//   so at any other.
// - pow is of what isn't negative (GLSL leaves the rest undefined).
// - deconvergence's mask width isn't read before its array for shadowMask -1,
//   which doesn't use it; reversed smoothstep is written out.

// GLSL's mod: the sign of y
#define MOD(x, y) ((x) - (y) * floor((x) / (y)))
#define POW(x, y) pow(max(x, 0.0), y)

FUNC vec3 plant (vec3 tar, float r)
{
	float t = max(max(tar.r, tar.g), tar.b) + 0.00001;
	return tar * r / t;
}

// smoothstep with edge0 over edge1 too, as GPUs compute it
FUNC vec2 crt_smoothstep2 (vec2 edge0, vec2 edge1, vec2 x)
{
	vec2 t = clamp((x - edge0) / (edge1 - edge0), V2(0.0), V2(1.0));
	return t * t * (3.0 - 2.0 * t);
}

FUNC vec2 Warp (CTXP vec2 pos)
{
	pos = pos * 2.0 - 1.0;
	pos = mix(pos, vec2(pos.x * inversesqrt(1.0 - P.c_shape * pos.y * pos.y),
		pos.y * inversesqrt(1.0 - P.c_shape * pos.x * pos.x)), vec2(P.warpX, P.warpY) / P.c_shape);
	return pos * 0.5 + 0.5;
}

FUNC vec2 Overscan (vec2 pos, float dx, float dy)
{
	pos = pos * 2.0 - 1.0;
	pos *= vec2(dx, dy);
	return pos * 0.5 + 0.5;
}

/*
===============================================================================

AFTERGLOW0: the phosphors' afterglow, the last frame's faded with this one

Copyright (C) 2020 - 2025 guest(r)

===============================================================================
*/

#define Source T0
#define OriginalHistory0 T0
#define AfterglowPassFeedback T1

FUNC vec4 crt_afterglow (CTXP TEXP(T0) TEXP(T1) vec2 uv)
{
	vec2 dx = vec2(S.OriginalSize.z, 0.0);
	vec2 dy = vec2(0.0, S.OriginalSize.w);

	vec3 color0 = TEXN(OriginalHistory0, uv).rgb;
	vec3 color1 = TEXN(OriginalHistory0, uv - dx).rgb;
	vec3 color2 = TEXN(OriginalHistory0, uv + dx).rgb;
	vec3 color3 = TEXN(OriginalHistory0, uv - dy).rgb;
	vec3 color4 = TEXN(OriginalHistory0, uv + dy).rgb;

	if (P.esrc > 1.5)
	{
		color0 = TEXN(Source, uv).rgb;
		color1 = TEXN(Source, uv - dx).rgb;
		color2 = TEXN(Source, uv + dx).rgb;
		color3 = TEXN(Source, uv - dy).rgb;
		color4 = TEXN(Source, uv + dy).rgb;
	}

	vec3 color = (2.5 * color0 + color1 + color2 + color3 + color4) / 6.5;

	vec3 accumulate = TEXN(AfterglowPassFeedback, uv).rgb;

	float b = P.bth / 255.0;
	float c = max(max(color0.r, color0.g), color0.b);
	float w = smoothstep(b, 2.0 * b, c);

	// what persists of the last frame, a frame of 60 Hz at a time
	vec3 persist = POW(V3(0.49) + vec3(P.PR, P.PG, P.PB), V3(S.TimeScale));
	vec3 result = mix(max(mix(color, accumulate, persist) - 1.25 / 255.0 * S.TimeScale, 0.0), color, w);

	return vec4(result, w);
}

#undef Source
#undef OriginalHistory0
#undef AfterglowPassFeedback

/*
===============================================================================

PRE-SHADERS-AFTERGLOW: color (LUTs, the CRT's and the display's gamut, the
color temperature, saturation, contrast, brightness), the afterglow added, and
the vignette in alpha

Copyright (C) 2019-2025 guest(r) and Dr. Venom

===============================================================================
*/

// Color profile matrices

CONST mat3 Profile0 = mat3(
	vec3( 0.412391,  0.212639,  0.019331),
	vec3( 0.357584,  0.715169,  0.119195),
	vec3( 0.180481,  0.072192,  0.950532));

CONST mat3 Profile1 = mat3(
	vec3( 0.430554,  0.222004,  0.020182),
	vec3( 0.341550,  0.706655,  0.129553),
	vec3( 0.178352,  0.071341,  0.939322));

CONST mat3 Profile2 = mat3(
	vec3( 0.396686,  0.210299,  0.006131),
	vec3( 0.372504,  0.713766,  0.115356),
	vec3( 0.181266,  0.075936,  0.967571));

CONST mat3 Profile3 = mat3(
	vec3( 0.393521,  0.212376,  0.018739),
	vec3( 0.365258,  0.701060,  0.111934),
	vec3( 0.191677,  0.086564,  0.958385));

CONST mat3 Profile4 = mat3(
	vec3( 0.392258,  0.209410,  0.016061),
	vec3( 0.351135,  0.725680,  0.093636),
	vec3( 0.166603,  0.064910,  0.850324));

CONST mat3 Profile5 = mat3(
	vec3( 0.377923,  0.195679,  0.010514),
	vec3( 0.317366,  0.722319,  0.097826),
	vec3( 0.207738,  0.082002,  1.076960));

CONST mat3 ToSRGB = mat3(
	vec3( 3.240970, -0.969244,  0.055630),
	vec3(-1.537383,  1.875968, -0.203977),
	vec3(-0.498611,  0.041555,  1.056972));

CONST mat3 ToModern = mat3(
	vec3( 2.791723, -0.894766,  0.041678),
	vec3(-1.173165,  1.815586, -0.130886),
	vec3(-0.440973,  0.032000,  1.002034));

CONST mat3 ToDCI = mat3(
	vec3( 2.973422, -0.867605,  0.045031),
	vec3(-1.110433,  1.843757, -0.095697),
	vec3(-0.480247,  0.024743,  1.201215));

CONST mat3 ToAdobe = mat3(
	vec3( 2.041588, -0.969244,  0.013444),
	vec3(-0.565007,  1.875968, -0.11836),
	vec3(-0.344731,  0.041555,  1.015175));

CONST mat3 ToREC = mat3(
	vec3( 1.716651, -0.666684,  0.017640),
	vec3(-0.355671,  1.616481, -0.042771),
	vec3(-0.253366,  0.015769,  0.942103));

CONST mat3 ToP3 = mat3(
	vec3( 2.493509, -0.829473,  0.0358512),
	vec3(-0.931388,  1.762630, -0.0761839),
	vec3(-0.402712,  0.023624,  0.9570296));

// Color temperature matrices

CONST mat3 D65_to_D55 = mat3(
	vec3(0.4850339153, 0.2500956126, 0.0227359648),
	vec3(0.3488957224, 0.6977914447, 0.1162985741),
	vec3(0.1302823568, 0.0521129427, 0.6861537456));

CONST mat3 D65_to_D93 = mat3(
	vec3(0.3412754080, 0.1759701322, 0.0159972847),
	vec3(0.3646170520, 0.7292341040, 0.1215390173),
	vec3(0.2369894093, 0.0947957637, 1.2481442225));

#define LUTLOW 5.0		// "Fix LUT Dark - Range" from 0.0 to 50.0 - RGB singletons
#define LUTBR 1.0		// "Fix LUT Brightness" from 0.0 to 1.0

FUNC vec3 fix_lut (vec3 lutcolor, vec3 ref)
{
	float r = length(ref);
	float l = length(lutcolor);
	float m = max(max(ref.r, ref.g), ref.b);
	ref = normalize(lutcolor + 0.0000001) * mix(r, l, POW(m, 1.25));
	return mix(lutcolor, ref, LUTBR);
}

FUNC float vignette (CTXP vec2 pos)
{
	vec2 b = vec2(P.vigdef, P.vigdef) * vec2(1.0, S.OriginalSize.x / S.OriginalSize.y) * 0.125;
	pos = clamp(pos, V2(0.0), V2(1.0));
	pos = abs(2.0 * (pos - 0.5));
	vec2 res = mix(V2(0.0), V2(1.0), crt_smoothstep2(V2(1.0), V2(1.0) - b, sqrt(pos)));
	res = POW(res, V2(0.70));
	return max(mix(1.0, sqrt(res.x * res.y), P.vigstr), 0.0);
}

FUNC float contrast (CTXP float x)
{
	return max(mix(x, smoothstep(0.0, 1.0, x), P.contr), 0.0);
}

FUNC vec3 pgc (CTXP vec3 c)
{
	float mc = max(max(c.r, c.g), c.b);
	float mg = POW(mc, 1.0 / P.pre_gc);
	return c * mg / (mc + 1e-8);
}

#define StockPass T0
#define AfterglowPass T1
#define SamplerLUT1 T2
#define SamplerLUT2 T3
#define SamplerLUT3 T4
#define SamplerLUT4 T5

FUNC vec4 crt_prepass (CTXP TEXP(T0) TEXP(T1) TEXP(T2) TEXP(T3) TEXP(T4) TEXP(T5) vec2 uv)
{
	vec4 imgColor = TEXN(StockPass, uv);
	vec4 aftglow = TEXN(AfterglowPass, uv);

	float w = 1.0 - aftglow.w;

	float l = length(aftglow.rgb);
	aftglow.rgb = P.AS * w * normalize(POW(aftglow.rgb + 0.01, V3(P.agsat))) * l;
	float bp = w * P.BP / 255.0;

	if (P.sega_fix > 0.5) imgColor.rgb = imgColor.rgb * (255.0 / 239.0);

	imgColor.rgb = min(imgColor.rgb, 1.0);

	vec3 color = imgColor.rgb;

	if (int(P.TNTC) == 0)
	{
		color.rgb = imgColor.rgb;
	}
	else
	{
		float lutlow = LUTLOW / 255.0; float invLS = 1.0 / P.LS;
		vec3 lut_ref = imgColor.rgb + lutlow * (1.0 - POW(imgColor.rgb, V3(0.333)));
		float lutb = lut_ref.b * (1.0 - 0.5 * invLS);
		lut_ref.rg = lut_ref.rg * (1.0 - invLS) + 0.5 * invLS;
		float tile1 = ceil(lutb * (P.LS - 1.0));
		float tile0 = max(tile1 - 1.0, 0.0);
		float f = fract(lutb * (P.LS - 1.0)); if (f == 0.0) f = 1.0;
		vec2 coord0 = vec2(tile0 + lut_ref.r, lut_ref.g) * vec2(invLS, 1.0);
		vec2 coord1 = vec2(tile1 + lut_ref.r, lut_ref.g) * vec2(invLS, 1.0);
		vec4 color1 = imgColor, color2 = imgColor, res = imgColor;

		if (int(P.TNTC) == 1)
		{
			color1 = TEXL(SamplerLUT1, coord0);
			color2 = TEXL(SamplerLUT1, coord1);
			res = mix(color1, color2, f);
		}
		else if (int(P.TNTC) == 2)
		{
			color1 = TEXL(SamplerLUT2, coord0);
			color2 = TEXL(SamplerLUT2, coord1);
			res = mix(color1, color2, f);
		}
		else if (int(P.TNTC) == 3)
		{
			color1 = TEXL(SamplerLUT3, coord0);
			color2 = TEXL(SamplerLUT3, coord1);
			res = mix(color1, color2, f);
		}
		else if (int(P.TNTC) == 4)
		{
			color1 = TEXL(SamplerLUT4, coord0);
			color2 = TEXL(SamplerLUT4, coord1);
			res = mix(color1, color2, f);
		}

		res.rgb = fix_lut(res.rgb, imgColor.rgb);

		color = mix(imgColor.rgb, res.rgb, min(P.TNTC, 1.0));
	}

	vec3 c = clamp(color, V3(0.0), V3(1.0));

	float p = 2.2;
	mat3 m_out = ToSRGB;

	if (P.CS == 0.0) { p = 2.2; m_out = ToSRGB;   } else
	if (P.CS == 1.0) { p = 2.2; m_out = ToModern; } else
	if (P.CS == 2.0) { p = 2.6; m_out = ToDCI;    } else
	if (P.CS == 3.0) { p = 2.2; m_out = ToAdobe;  } else
	if (P.CS == 4.0) { p = 2.4; m_out = ToREC;    } else
	if (P.CS == 5.0) { p = 2.2; m_out = ToP3;     }

	color = POW(c, V3(p));

	mat3 m_in = Profile0;

	if (P.CP == 0.0) { m_in = Profile0; } else
	if (P.CP == 1.0) { m_in = Profile1; } else
	if (P.CP == 2.0) { m_in = Profile2; } else
	if (P.CP == 3.0) { m_in = Profile3; } else
	if (P.CP == 4.0) { m_in = Profile4; } else
	if (P.CP == 5.0) { m_in = Profile5; }

	color = MUL(m_in, color);
	color = MUL(m_out, color);

	color = clamp(color, V3(0.0), V3(1.0));

	color = POW(color, V3(1.0 / p));

	if (P.CP == -1.0) color = c;

	vec3 scolor1 = plant(POW(color, V3(P.wp_saturation)), max(max(color.r, color.g), color.b));
	float luma = dot(color, vec3(0.299, 0.587, 0.114));
	vec3 scolor2 = mix(V3(luma), color, P.wp_saturation);
	color = (P.wp_saturation > 1.0) ? scolor1 : scolor2;

	color = plant(color, contrast(CTXA max(max(color.r, color.g), color.b)));

	p = 2.2;
	color = clamp(color, V3(0.0), V3(1.0));
	color = POW(color, V3(p));

	vec3 warmer = MUL(D65_to_D55, color);
	warmer = MUL(ToSRGB, warmer);

	vec3 cooler = MUL(D65_to_D93, color);
	cooler = MUL(ToSRGB, cooler);

	float m = abs(P.WP) / 100.0;

	vec3 comp = (P.WP < 0.0) ? cooler : warmer;

	color = mix(color, comp, m);
	color = POW(color, V3(1.0 / p));
	color = pgc(CTXA color);

	if (P.BP > -0.5) color = color + aftglow.rgb + bp; else
	{
		color = max(color + P.BP / 255.0, 0.0) / (1.0 + P.BP / 255.0 * step(-P.BP / 255.0, max(max(color.r, color.g), color.b)))
			+ aftglow.rgb;
	}

	color = min(color * P.pre_bb, 1.0);

	return vec4(color, vignette(CTXA uv));
}

#undef StockPass
#undef AfterglowPass
#undef SamplerLUT1
#undef SamplerLUT2
#undef SamplerLUT3
#undef SamplerLUT4

/*
===============================================================================

AVG-LUM: the frame's average brightness for raster bloom, smoothed over the
frames before, in alpha, and the smart edges' color distances

Average Luminance Shader, Smart Edge Interpolation Coefficients Calculation
Copyright (C) 2018-2025 guest(r)
Thanks to HunterK for the mipmap hint. :D

===============================================================================
*/

// Reference: http://www.compuphase.com/cmetric.htm
// Reference: ScaleFX, author Sp00kyFox

FUNC float dist (vec3 A, vec3 B)
{
	float r = 0.5 * (A.r + B.r);
	vec3 d = A - B;
	vec3 c = vec3(2. + r, 4., 3. - r);

	return sqrt(dot(c * d, d)) / 3.;
}

#define Source T0
#define AvgLumPassFeedback T1

FUNC vec4 crt_avglum (CTXP TEXP(T0) TEXP(T1) vec2 uv)
{
	vec2 TEX0 = uv * 1.00001;

	float m = max(log2(S.SourceSize.x), log2(S.SourceSize.y));
	m = floor(max(m, 1.0)) - 1.0;

	vec2 dx = vec2(1.0 / S.SourceSize.x, 0.0);
	vec2 x2 = 2.0 * dx;

	float ltotal = 0.0;

	ltotal += length(TEXLOD(Source, vec2(0.3, 0.3), m).rgb);
	ltotal += length(TEXLOD(Source, vec2(0.3, 0.7), m).rgb);
	ltotal += length(TEXLOD(Source, vec2(0.7, 0.3), m).rgb);
	ltotal += length(TEXLOD(Source, vec2(0.7, 0.7), m).rgb);

	ltotal *= 0.25;

	ltotal = POW(0.577350269 * ltotal, 0.70);

	float lhistory = TEXL(AvgLumPassFeedback, vec2(0.5, 0.5)).a;

	// what's kept of the frames before, a frame of 60 Hz at a time
	ltotal = mix(ltotal, lhistory, POW(min(mix(P.lsmooth, P.lsmooth + P.lsdev, ltotal), 0.99), S.TimeScale));

	vec3 l1 = TEXL(Source, TEX0.xy     ).rgb;
	vec3 r1 = TEXL(Source, TEX0.xy + dx).rgb;
	vec3 l2 = TEXL(Source, TEX0.xy - dx).rgb;
	vec3 r2 = TEXL(Source, TEX0.xy + x2).rgb;

	float c1 = dist(l2, l1);
	float c2 = dist(l1, r1);
	float c3 = dist(r2, r1);

	return vec4(c1, c2, c3, ltotal);
}

#undef Source
#undef AvgLumPassFeedback

/*
===============================================================================

LINEARIZE: the frame in linear light, interlaced or not; alpha tells the
passes after its gamma and the interlacing

Interlacing
Copyright (C) 2020 - 2025 guest(r)

===============================================================================
*/

#define PrePass T0

FUNC vec3 linearize_fetch (CTXP TEXP(T0) vec2 coord)
{
	vec2 dx = vec2(S.SourceSize.z, 0.0) * P.downsample_levelx;
	vec2 dy = vec2(0.0, S.SourceSize.w) * P.downsample_levely;
	vec2 d1 = dx + dy;
	vec2 d2 = dx - dy;

	float sum = 15.0;
	vec3 result = 3.0 * TEXL(PrePass, coord     ).rgb +
	              2.0 * TEXL(PrePass, coord + dx).rgb +
	              2.0 * TEXL(PrePass, coord - dx).rgb +
	              2.0 * TEXL(PrePass, coord + dy).rgb +
	              2.0 * TEXL(PrePass, coord - dy).rgb +
	              TEXL(PrePass, coord + d1).rgb +
	              TEXL(PrePass, coord - d1).rgb +
	              TEXL(PrePass, coord + d2).rgb +
	              TEXL(PrePass, coord - d2).rgb;

	return result / sum;
}

FUNC vec4 crt_linearize (CTXP TEXP(T0) vec2 uv)
{
	vec2 vTexCoord = uv * 1.00001;

	vec3 c1 = TEXL(PrePass, vTexCoord).rgb;
	vec3 c2 = TEXL(PrePass, vTexCoord + vec2(0.0, S.OriginalSize.w)).rgb;

	if ((P.downsample_levelx + P.downsample_levely) > 0.025)
	{
		c1 = linearize_fetch(CTXA TEXA(PrePass) vTexCoord);
		c2 = linearize_fetch(CTXA TEXA(PrePass) vTexCoord + vec2(0.0, S.OriginalSize.w));
	}

	vec3 c = c1;

	float intera = 1.0;
	float gamma_in = P.GAMMA_INPUT;

	float m1 = max(max(c1.r, c1.g), c1.b);
	float m2 = max(max(c2.r, c2.g), c2.b);
	vec3 df = abs(c1 - c2);

	float d = max(max(df.r, df.g), df.b);
	if (P.interm == 2.0) d = mix(0.1 * d, 10.0 * d, step(m1 / (m2 + 0.0001), m2 / (m1 + 0.0001)));

	float r = m1;

	float yres_div = 1.0; if (P.intres > 1.25) yres_div = P.intres;

	bool hscan = (P.hiscan > 0.5);

	if ((P.inter <= S.OriginalSize.y / yres_div && (P.interm > 0.5) && P.intres != 1.0 && P.intres != 0.5
		&& P.vga_mode < 0.5) || hscan)
	{
		intera = 0.25;
		float line_no  = floor(MOD(S.OriginalSize.y * vTexCoord.y, 2.0));
		float frame_no = floor(MOD(float(S.FrameCount), 2.0));
		float ii = abs(line_no - frame_no);

		if (P.interm < 3.5 || P.interm > 5.5)
		{
			if (P.interm == 6.0) c = mix(c2, c1, ii); else
			{
				c2 = plant(mix(c2, c2 * c2, P.iscans), max(max(c2.r, c2.g), c2.b));
				r = max(m1 * ii, (1.0 - P.iscan) * min(m1, m2));
				c = plant(mix(mix(c1, c2, min(mix(m1, 1.0 - m2, min(m1, 1.0 - m1)) / (d + 0.00001), 1.0)), c1, ii), r);
				if (P.interm == 3.0) c = (1.0 - 0.5 * P.iscan) * mix(c2, c1, ii);
			}
		}
		if (P.interm == 4.0) { c = plant(mix(c, c * c, 0.5 * P.iscans), max(max(c.r, c.g), c.b)) * (1.0 - 0.5 * P.iscan); }
		if (P.interm == 5.0) { c = mix(c2, c1, 0.5); c = plant(mix(c, c * c, 0.5 * P.iscans), max(max(c.r, c.g), c.b)) * (1.0 - 0.5 * P.iscan); }

		if (hscan) c = c1;
	}

	if (P.vga_mode > 0.5)
	{
		c = c1;
		if (P.inter <= S.OriginalSize.y) intera = 0.75; else intera = 0.5;
	}

	c = POW(c, V3(gamma_in));

	if (vTexCoord.x > 0.5) gamma_in = intera; else gamma_in = 1.0 / gamma_in;

	return vec4(c, gamma_in);
}

#undef PrePass

/*
===============================================================================

GAUSSIAN_HORIZONTAL, GAUSSIAN_VERTICAL: the glow, 800x600

Gaussian blur - horizontal and vertical passes, dynamic range, resizable
Copyright (C) 2020 - 2025 guest(r)

===============================================================================
*/

FUNC float gaussian (float x, float invsqrsigma)
{
	return exp(-x * x * invsqrsigma);
}

#define LinearizePass T0

FUNC vec4 crt_gauss_h (CTXP TEXP(T0) vec2 uv)
{
	float FINE_GLOW = (P.FINE_GLOW > 0.5) ? P.FINE_GLOW : mix(0.75, 0.5, -P.FINE_GLOW);
	float invsqrsigma = 1.0 / (2.0 * P.SIGMA_H * P.SIGMA_H);

	vec4 SourceSize1 = S.OriginalSize * vec4(FINE_GLOW, FINE_GLOW, 1.0 / FINE_GLOW, 1.0 / FINE_GLOW);
	float f = fract(SourceSize1.x * uv.x);
	f = 0.5 - f;
	vec2 tex = floor(SourceSize1.xy * uv) * SourceSize1.zw + 0.5 * SourceSize1.zw;
	vec3 color = V3(0.0);
	vec2 dx = vec2(SourceSize1.z, 0.0);

	float w = 0.0;
	float wsum = 0.0;
	vec3 pixel = V3(0.0);
	float n = -P.SIZEH;

	LOOP for ( ; n <= P.SIZEH ; n = n + 1.0)
	{
		pixel = TEXL(LinearizePass, tex + n * dx).rgb;
		if (P.m_glow > 0.5)
		{
			pixel = max(pixel - P.m_glow_cutoff, 0.0);
			pixel = plant(pixel, max(max(max(pixel.r, pixel.g), pixel.b) - P.m_glow_cutoff, 0.0));
		}
		w = gaussian(n + f, invsqrsigma);
		color = color + w * pixel;
		wsum = wsum + w;
	}

	color = color / wsum;

	return vec4(color, 1.0);
}

#undef LinearizePass
#define Source T0

FUNC vec4 crt_gauss_v (CTXP TEXP(T0) vec2 uv)
{
	float FINE_GLOW = (P.FINE_GLOW > 0.5) ? P.FINE_GLOW : mix(0.75, 0.5, -P.FINE_GLOW);
	float invsqrsigma = 1.0 / (2.0 * P.SIGMA_V * P.SIGMA_V);

	vec4 SourceSize1 = vec4(S.SourceSize.x, S.OriginalSize.y, S.SourceSize.z, S.OriginalSize.w)
		* vec4(FINE_GLOW, FINE_GLOW, 1.0 / FINE_GLOW, 1.0 / FINE_GLOW);
	float f = fract(SourceSize1.y * uv.y);
	f = 0.5 - f;
	vec2 tex = floor(SourceSize1.xy * uv) * SourceSize1.zw + 0.5 * SourceSize1.zw;
	vec3 color = V3(0.0);
	vec2 dy = vec2(0.0, SourceSize1.w);

	float w = 0.0;
	float wsum = 0.0;
	vec3 pixel = V3(0.0);
	float n = -P.SIZEV;

	LOOP for ( ; n <= P.SIZEV ; n = n + 1.0)
	{
		pixel = TEXL(Source, tex + n * dy).rgb;
		w = gaussian(n + f, invsqrsigma);
		color = color + w * pixel;
		wsum = wsum + w;
	}

	color = color / wsum;

	return vec4(color, 1.0);
}

#undef Source

/*
===============================================================================

BLOOM_HORIZONTAL, BLOOM_VERTICAL: bloom and halation, 800x600

Gaussian blur - horizontal and vertical passes, dynamic range, resizable
Copyright (C) 2020 - 2023 guest(r) - guest.r@gmail.com

===============================================================================
*/

#define LinearizePass T0

FUNC vec4 crt_bloom_h (CTXP TEXP(T0) vec2 uv)
{
	float FINE_BLOOM = (P.FINE_BLOOM > 0.5) ? P.FINE_BLOOM : mix(0.75, 0.5, -P.FINE_BLOOM);
	float invsqrsigma = 1.0 / (2.0 * P.SIGMA_HB * P.SIGMA_HB);

	vec4 SourceSize1 = S.OriginalSize * vec4(FINE_BLOOM, FINE_BLOOM, 1.0 / FINE_BLOOM, 1.0 / FINE_BLOOM);
	float f = fract(SourceSize1.x * uv.x);
	f = 0.5 - f;
	vec2 tex = floor(SourceSize1.xy * uv) * SourceSize1.zw + 0.5 * SourceSize1.zw;
	vec4 color = V4(0.0);
	vec2 dx = vec2(SourceSize1.z, 0.0);

	float w = 0.0;
	float wsum = 0.0;
	vec4 pixel = V4(0.0);
	float n = -P.SIZEHB;

	LOOP for ( ; n <= P.SIZEHB ; n = n + 1.0)
	{
		pixel = TEXL(LinearizePass, tex + n * dx);
		w = gaussian(n + f, invsqrsigma);
		pixel.a = max(max(pixel.r, pixel.g), pixel.b);
		pixel.a *= pixel.a * pixel.a;
		color = color + w * pixel;
		wsum = wsum + w;
	}

	color = color / wsum;

	return vec4(color.rgb, POW(color.a, 0.333333));
}

#undef LinearizePass
#define Source T0

FUNC vec4 crt_bloom_v (CTXP TEXP(T0) vec2 uv)
{
	float FINE_BLOOM = (P.FINE_BLOOM > 0.5) ? P.FINE_BLOOM : mix(0.75, 0.5, -P.FINE_BLOOM);
	float invsqrsigma = 1.0 / (2.0 * P.SIGMA_VB * P.SIGMA_VB);

	vec4 SourceSize1 = S.SourceSize;
	SourceSize1.yw = S.OriginalSize.yw;
	SourceSize1 = SourceSize1 * vec4(FINE_BLOOM, FINE_BLOOM, 1.0 / FINE_BLOOM, 1.0 / FINE_BLOOM);

	float f = fract(SourceSize1.y * uv.y);
	f = 0.5 - f;
	vec2 tex = floor(SourceSize1.xy * uv) * SourceSize1.zw + 0.5 * SourceSize1.zw;
	vec4 color = V4(0.0);
	vec2 dy = vec2(0.0, SourceSize1.w);

	float w = 0.0;
	float wsum = 0.0;
	vec4 pixel = V4(0.0);
	float n = -P.SIZEVB;

	LOOP for ( ; n <= P.SIZEVB ; n = n + 1.0)
	{
		pixel = TEXL(Source, tex + n * dy);
		w = gaussian(n + f, invsqrsigma);
		pixel.a *= pixel.a * pixel.a;
		color = color + w * pixel;
		wsum = wsum + w;
	}

	color = color / wsum;

	return vec4(color.rgb, POW(color.a, 0.175));
}

#undef Source

/*
===============================================================================

CRT-GUEST-ADVANCED: the scanlines, the frame filtered across and drawn as
beams, curved, at the viewport's size

===============================================================================
*/

#define eps 1e-10

FUNC float st (float x)
{
	return exp2(-10.0 * x * x);
}

FUNC vec3 sw0 (CTXP float x, float color, float scanline, vec3 c)
{
	float tmp = mix(P.beam_min, P.beam_max, color);
	vec3 sat = mix(V3(1.0 + 1.5 * P.scans), V3(1.0), c);
	float ex = x * tmp;
	ex = (P.gsl > -0.5) ? ex * ex : mix(ex * ex, ex * ex * ex, 0.4);
	return exp2(-scanline * ex * sat);
}

FUNC vec3 sw1 (CTXP float x, float color, float scanline, vec3 c)
{
	x = mix(x, P.beam_min * x, max(x - 0.4 * color, 0.0));
	vec3 sat = mix(V3(1.0 + 1.5 * P.scans), V3(1.0), c);
	float tmp = mix(1.2 * P.beam_min, P.beam_max, color);
	float ex = x * tmp;
	return exp2(-scanline * ex * ex * sat);
}

FUNC vec3 sw2 (CTXP float x, float color, float scanline, vec3 c)
{
	float tmp = mix((2.5 - 0.5 * color) * P.beam_min, P.beam_max, color);
	vec3 sat = mix(V3(1.0 + 1.5 * P.scans), V3(1.0), c);
	tmp = mix(P.beam_max, tmp, POW(x, color + 0.3));
	float ex = x * tmp;
	return exp2(-scanline * ex * ex * sat);
}

FUNC vec3 gc (CTXP vec3 c)
{
	float mc = max(max(c.r, c.g), c.b);
	float mg = POW(mc, 1.0 / P.gamma_c);
	return c * mg / (mc + eps);
}

#define LinearizePass T0
#define AvgLumPass T1
#define PrePass T2

FUNC vec4 crt_crt (CTXP TEXP(T0) TEXP(T1) TEXP(T2) vec2 uv)
{
	vec2 TEX0 = uv * 1.00001;

	// LinearizePass is the source's size
	vec2 prescalex = V2(1.0);

	vec4 SourceSize = S.OriginalSize * mix(vec4(prescalex.x, 1.0, 1.0 / prescalex.x, 1.0),
		vec4(1.0, prescalex.y, 1.0, 1.0 / prescalex.y), P.TATE);

	float lum = TEXL(AvgLumPass, vec2(0.5, 0.5)).a;

	float gamma_in = 1.0 / TEXL(LinearizePass, vec2(0.25, 0.25)).a;
	float intera = TEXL(LinearizePass, vec2(0.75, 0.25)).a;
	bool hscan = (P.hiscan > 0.5);
	bool inter6 = (intera < 0.35 && P.interm == 6.0);
	bool interb = (((intera < 0.35 && !inter6) || (P.no_scanlines > 0.025)) && !hscan);
	bool notate = (P.TATE < 0.5);
	bool vgascan = ((abs(intera - 0.5) < 0.05) && (P.no_scanlines == 0.0));

	float SourceY = mix(SourceSize.y, SourceSize.x, P.TATE);

	float sy = 1.0;
	if (P.intres == 1.0) sy = max(round(SourceY / 224.0), 1.0);
	if (P.intres > 0.25 && P.intres != 1.0) sy = P.intres;
	if (inter6) sy *= 2.0;

	// VGA Scan Mode
	if (vgascan) sy = 0.5; else if (abs(intera - 0.75) < 0.05) sy = 1.0;

	if (notate) SourceSize *= vec4(1.0, 1.0 / sy, 1.0, sy); else SourceSize *= vec4(1.0 / sy, 1.0, sy, 1.0);

	// Calculating texel coordinates

	vec2 texcoord = TEX0.xy;
	if (P.IOS > 0.0 && !interb)
	{
		vec2 ofactor = S.OutputSize.xy / S.OriginalSize.xy;
		vec2 intfactor = (P.IOS < 2.5) ? floor(ofactor) : ceil(ofactor);
		vec2 diff = ofactor / intfactor;
		float scan = mix(diff.y, diff.x, P.TATE);
		texcoord = Overscan(texcoord, scan, scan);
		if (P.IOS == 1.0 || P.IOS == 3.0) texcoord = mix(vec2(TEX0.x, texcoord.y), vec2(texcoord.x, TEX0.y), P.TATE);
	}

	float factor = 1.00 + (1.0 - 0.5 * P.OS) * P.BLOOM / 100.0 - lum * P.BLOOM / 100.0;
	texcoord = Overscan(texcoord, factor, factor);

	texcoord.y = texcoord.y - P.VShift * S.OriginalSize.w;
	texcoord = Overscan(texcoord, (S.OriginalSize.x - P.overscanX) / S.OriginalSize.x,
		(S.OriginalSize.y - P.overscanY) / S.OriginalSize.y);

	vec2 pos = Warp(CTXA texcoord);

	bool smarte = (P.smart_ei > 0.01 && notate); // smart edge interpolation on / off

	// odd / even field line calcs for mode 6

	float ii = float(inter6) * (floor(MOD(float(S.FrameCount), 2.0)));

	vec2 coffset = vec2(0.5, 0.5 + 0.5 * ii);

	vec2 ps = SourceSize.zw;
	vec2 OGL2Pos = pos * SourceSize.xy - coffset;
	vec2 fp = fract(OGL2Pos);

	vec2 dx = vec2(ps.x, 0.0);
	vec2 dy = vec2(0.0, ps.y);

	// Reading the texels
	vec2 x2 = 2.0 * dx;
	vec2 y2 = 2.0 * dy;

	vec2 offx = dx;
	vec2 off2 = x2;
	vec2 offy = dy;
	float fpx = fp.x;
	if (!notate)
	{
		offx = dy;
		off2 = y2;
		offy = dx;
		fpx = fp.y;
	}
	float f = (notate) ? fp.y : fp.x;

	vec2 pC4 = floor(OGL2Pos) * ps + 0.5 * ps; pC4.y = pC4.y + float(inter6) * 0.25 * dy.y;

	if ((P.intres == 0.5 && notate && prescalex.y < 1.5) || vgascan)
		pC4.y = floor(pC4.y * S.OriginalSize.y) * S.OriginalSize.w + 0.5 * S.OriginalSize.w;
	if ((P.intres == 0.5 && !notate && prescalex.x < 1.5) || (vgascan && !notate))
		pC4.x = floor(pC4.x * S.OriginalSize.x) * S.OriginalSize.z + 0.5 * S.OriginalSize.z;

	if (interb && P.no_scanlines < 0.025 && !hscan) pC4.y = pos.y;
	else if (interb) pC4.y = pC4.y + smoothstep(0.40 - 0.5 * P.no_scanlines, 0.60 + 0.5 * P.no_scanlines, f)
		* mix(SourceSize.w, SourceSize.z, P.TATE);
	if (hscan) pC4 = mix(vec2(pC4.x, pos.y), vec2(pos.x, pC4.y), P.TATE);

	float zero = exp2(-P.h_sharp);
	float sharp1 = P.s_sharp * zero;

	float idiv = clamp(mix(SourceSize.x, SourceSize.y, P.TATE) / 400.0, 1.0, 2.0);
	float fdivider = max(min(mix(prescalex.x, prescalex.y, P.TATE), 2.0), idiv * float(interb));
	fdivider = 1.0 / max(fdivider, 1.0);

	float wl3 = (2.0 + fpx) * fdivider;
	float wl2 = (1.0 + fpx) * fdivider;
	float wl1 = (      fpx) * fdivider;
	float wr1 = (1.0 - fpx) * fdivider;
	float wr2 = (2.0 - fpx) * fdivider;
	float wr3 = (3.0 - fpx) * fdivider;

	wl3 *= wl3; wl3 = exp2(-P.h_sharp * wl3);
	wl2 *= wl2; wl2 = exp2(-P.h_sharp * wl2);
	wl1 *= wl1; wl1 = exp2(-P.h_sharp * wl1);
	wr1 *= wr1; wr1 = exp2(-P.h_sharp * wr1);
	wr2 *= wr2; wr2 = exp2(-P.h_sharp * wr2);
	wr3 *= wr3; wr3 = exp2(-P.h_sharp * wr3);

	float fp1 = 1. - fpx;

	float twl3 = max(wl3 - sharp1, 0.0);
	float twl2 = max(wl2 - sharp1, mix(-0.12, 0.0, 1.0 - fp1 * fp1));
	float twl1 = max(wl1 - sharp1, -0.12);
	float twr1 = max(wr1 - sharp1, -0.12);
	float twr2 = max(wr2 - sharp1, mix(-0.12, 0.0, 1.0 - fpx * fpx));
	float twr3 = max(wr3 - sharp1, 0.0);

	bool sharp = (sharp1 > 0.0);

	vec3 c1 = V3(0.0), c2 = V3(0.0);

	if (smarte)
	{
		twl3 = 0.0; twr3 = 0.0;
		c1 = TEXL(AvgLumPass, pC4       ).xyz;
		c2 = TEXL(AvgLumPass, pC4 + offy).xyz;
		c1 = max(c1 - P.sth, 0.0);
		c2 = max(c2 - P.sth, 0.0);
	}

	vec3 l3, l2, l1, r1, r2, r3, color1, color2, colmin, colmax;

	l3 = TEXL(LinearizePass, pC4 - off2).rgb;
	l2 = TEXL(LinearizePass, pC4 - offx).rgb;
	l1 = TEXL(LinearizePass, pC4       ).rgb;
	r1 = TEXL(LinearizePass, pC4 + offx).rgb;
	r2 = TEXL(LinearizePass, pC4 + off2).rgb;
	r3 = TEXL(LinearizePass, pC4 + offx + off2).rgb;

	colmin = min(min(l1, r1), min(l2, r2));
	colmax = max(max(l1, r1), max(l2, r2));

	if (smarte)
	{
		float pc = min(P.smart_ei * c1.y, P.ei_limit);
		float pl = min(P.smart_ei * max(c1.y, c1.x), P.ei_limit);
		float pr = min(P.smart_ei * max(c1.y, c1.z), P.ei_limit);
		twl1 = max(wl1 - pc, 0.01 * wl1); twr1 = max(wr1 - pc, 0.01 * wr1);
		twl2 = max(wl2 - pl, 0.01 * wl2); twr2 = max(wr2 - pr, 0.01 * wr2);
	}

	color1 = (l3 * twl3 + l2 * twl2 + l1 * twl1 + r1 * twr1 + r2 * twr2 + r3 * twr3) / (twl3 + twl2 + twl1 + twr1 + twr2 + twr3);

	if (sharp) color1 = clamp(mix(clamp(color1, colmin, colmax), color1, P.ring), V3(0.0), V3(1.0));
	float ts = 0.025;
	vec3 luma = vec3(0.2126, 0.7152, 0.0722);

	float lm2 = max(max(l2.r, l2.g), l2.b);
	float lm1 = max(max(l1.r, l1.g), l1.b);
	float rm1 = max(max(r1.r, r1.g), r1.b);
	float rm2 = max(max(r2.r, r2.g), r2.b);

	float swl2 = max(twl2, 0.0) * (dot(l2, luma) + ts);
	float swl1 = max(twl1, 0.0) * (dot(l1, luma) + ts);
	float swr1 = max(twr1, 0.0) * (dot(r1, luma) + ts);
	float swr2 = max(twr2, 0.0) * (dot(r2, luma) + ts);

	float fscolor1 = (lm2 * swl2 + lm1 * swl1 + rm1 * swr1 + rm2 * swr2) / (swl2 + swl1 + swr1 + swr2);
	vec3 scolor1 = V3(clamp(mix(max(max(color1.r, color1.g), color1.b), fscolor1, P.spike), 0.0, 1.0));

	if (!interb) color1 = POW(color1, V3(P.scangamma / gamma_in));

	vec3 scolor2 = V3(0.0);
	color2 = V3(0.0);

	if (!interb && !hscan)
	{
		pC4 += offy;

		if ((P.intres == 0.5 && notate && prescalex.y < 1.5) || vgascan)
			pC4.y = floor((pos.y + 0.33 * offy.y) * S.OriginalSize.y) * S.OriginalSize.w + 0.5 * S.OriginalSize.w;
		if ((P.intres == 0.5 && !notate && prescalex.x < 1.5) || (vgascan && !notate))
			pC4.x = floor((pos.x + 0.33 * offy.x) * S.OriginalSize.x) * S.OriginalSize.z + 0.5 * S.OriginalSize.z;

		l3 = TEXL(LinearizePass, pC4 - off2).rgb;
		l2 = TEXL(LinearizePass, pC4 - offx).rgb;
		l1 = TEXL(LinearizePass, pC4       ).rgb;
		r1 = TEXL(LinearizePass, pC4 + offx).rgb;
		r2 = TEXL(LinearizePass, pC4 + off2).rgb;
		r3 = TEXL(LinearizePass, pC4 + offx + off2).rgb;

		colmin = min(min(l1, r1), min(l2, r2));
		colmax = max(max(l1, r1), max(l2, r2));

		if (smarte)
		{
			float pc = min(P.smart_ei * c2.y, P.ei_limit);
			float pl = min(P.smart_ei * max(c2.y, c2.x), P.ei_limit);
			float pr = min(P.smart_ei * max(c2.y, c2.z), P.ei_limit);
			twl1 = max(wl1 - pc, 0.01 * wl1); twr1 = max(wr1 - pc, 0.01 * wr1);
			twl2 = max(wl2 - pl, 0.01 * wl2); twr2 = max(wr2 - pr, 0.01 * wr2);
		}

		color2 = (l3 * twl3 + l2 * twl2 + l1 * twl1 + r1 * twr1 + r2 * twr2 + r3 * twr3) / (twl3 + twl2 + twl1 + twr1 + twr2 + twr3);

		if (sharp) color2 = clamp(mix(clamp(color2, colmin, colmax), color2, P.ring), V3(0.0), V3(1.0));

		lm2 = max(max(l2.r, l2.g), l2.b);
		lm1 = max(max(l1.r, l1.g), l1.b);
		rm1 = max(max(r1.r, r1.g), r1.b);
		rm2 = max(max(r2.r, r2.g), r2.b);

		swl2 = max(twl2, 0.0) * (dot(l2, luma) + ts);
		swl1 = max(twl1, 0.0) * (dot(l1, luma) + ts);
		swr1 = max(twr1, 0.0) * (dot(r1, luma) + ts);
		swr2 = max(twr2, 0.0) * (dot(r2, luma) + ts);

		float fscolor2 = (lm2 * swl2 + lm1 * swl1 + rm1 * swr1 + rm2 * swr2) / (swl2 + swl1 + swr1 + swr2);
		scolor2 = V3(clamp(mix(max(max(color2.r, color2.g), color2.b), fscolor2, P.spike), 0.0, 1.0));

		color2 = POW(color2, V3(P.scangamma / gamma_in));
	}

	vec3 ctmp = color1; vec3 sctmp = color1; vec3 color = color1;
	vec3 one = V3(1.0);

	if (hscan) { color2 = color1; scolor2 = scolor1; }

	if (!interb)
	{
		// calculating scanlines

		float shape1 = mix(P.scanline1, P.scanline2, f);
		float shape2 = mix(P.scanline1, P.scanline2, 1.0 - f);

		float wt1 = st(f);
		float wt2 = st(1.0 - f);

		vec3 color00 = color1 * wt1 + color2 * wt2;
		vec3 scolor0 = scolor1 * wt1 + scolor2 * wt2;

		ctmp = color00 / (wt1 + wt2);
		sctmp = scolor0 / (wt1 + wt2);

		if (abs(P.rolling_scan) > 0.005)
		{
			color1 = ctmp; color2 = ctmp;
			scolor1 = sctmp; scolor2 = sctmp;
		}

		vec3 w1 = V3(0.0), w2 = V3(0.0);

		vec3 cref1 = mix(sctmp, scolor1, P.beam_size); float creff1 = POW(max(max(cref1.r, cref1.g), cref1.b), P.scan_falloff);
		vec3 cref2 = mix(sctmp, scolor2, P.beam_size); float creff2 = POW(max(max(cref2.r, cref2.g), cref2.b), P.scan_falloff);

		if (P.tds > 0.5) { shape1 = mix(P.scanline2, shape1, creff1); shape2 = mix(P.scanline2, shape2, creff2); }

		float scanpix = mix(S.OriginalSize.x / S.OutputSize.x, S.OriginalSize.y / S.OutputSize.y, float(notate));
		float f1 = fract(f - P.rolling_scan * float(S.FrameCount) * scanpix);
		float f2 = 1.0 - f1;

		float mc1 = max(max(color1.r, color1.g), color1.b) + eps;
		float mc2 = max(max(color2.r, color2.g), color2.b) + eps;

		cref1 = color1 / mc1;
		cref2 = color2 / mc2;

		if (P.gsl < 0.5) { w1 = sw0(CTXA f1, creff1, shape1, cref1); w2 = sw0(CTXA f2, creff2, shape2, cref2); } else
		if (P.gsl == 1.0) { w1 = sw1(CTXA f1, creff1, shape1, cref1); w2 = sw1(CTXA f2, creff2, shape2, cref2); } else
		                  { w1 = sw2(CTXA f1, creff1, shape1, cref1); w2 = sw2(CTXA f2, creff2, shape2, cref2); }

		vec3 w3 = w1 + w2;
		float wf1 = max(max(w3.r, w3.g), w3.b);
		if (wf1 > 1.0) { wf1 = 1.0 / wf1; w1 *= wf1; w2 *= wf1; }

		if (abs(P.clips) > 0.005)
		{
			sy = mc1; one = (P.clips > 0.0) ? w1 : V3(1.0);
			float sat = 1.0001 - min(min(cref1.r, cref1.g), cref1.b);
			color1 = mix(color1, plant(POW(color1, V3(0.70 - 0.325 * sat)), sy), POW(sat, 0.3333) * one * abs(P.clips));
			sy = mc2; one = (P.clips > 0.0) ? w2 : V3(1.0);
			sat = 1.0001 - min(min(cref2.r, cref2.g), cref2.b);
			color2 = mix(color2, plant(POW(color2, V3(0.70 - 0.325 * sat)), sy), POW(sat, 0.3333) * one * abs(P.clips));
		}

		color = (gc(CTXA color1) * w1 + gc(CTXA color2) * w2);

		color = min(color, 1.0);
	}

	if (interb)
	{
		color = gc(CTXA color1);
	}

	float colmx = max(max(ctmp.r, ctmp.g), ctmp.b);

	if (!interb) color = POW(color, V3(gamma_in / P.scangamma));

	return vec4(color, colmx);
}

#undef LinearizePass
#undef AvgLumPass
#undef PrePass

/*
===============================================================================

DECONVERGENCE: the phosphor mask, glow, bloom and halation, deconvergence,
noise, the hum bar, the corners and border, at the viewport's size

===============================================================================
*/

// Shadow mask (1-4 from PD CRT Lottes shader).

FUNC vec3 Mask (CTXP vec2 pos, float mx, float mb)
{
	vec3 mask = vec3(P.maskDark, P.maskDark, P.maskDark);
	vec3 one = V3(1.0);

	if (P.shadowMask == 0.0)
	{
		float mc = 1.0 - max(P.maskstr, 0.0);
		pos.x = fract(pos.x * 0.5);
		if (pos.x < 0.49) { mask.r = 1.0; mask.g = mc; mask.b = 1.0; }
		else { mask.r = mc; mask.g = 1.0; mask.b = mc; }
	}

	// Very compressed TV style shadow mask.
	else if (P.shadowMask == 1.0)
	{
		float lineb = P.maskLight;
		float odd = 0.0;

		if (fract(pos.x / 6.0) < 0.49)
			odd = 1.0;
		if (fract((pos.y + odd) / 2.0) < 0.49)
			lineb = P.maskDark;

		pos.x = floor(MOD(pos.x, 3.0));

		if      (pos.x < 0.5) mask.r = P.maskLight;
		else if (pos.x < 1.5) mask.g = P.maskLight;
		else                  mask.b = P.maskLight;

		mask *= lineb;
	}

	// Aperture-grille.
	else if (P.shadowMask == 2.0)
	{
		pos.x = floor(MOD(pos.x, 3.0));

		if      (pos.x < 0.5) mask.r = P.maskLight;
		else if (pos.x < 1.5) mask.g = P.maskLight;
		else                  mask.b = P.maskLight;
	}

	// Stretched VGA style shadow mask (same as prior shaders).
	else if (P.shadowMask == 3.0)
	{
		pos.x += pos.y * 3.0;
		pos.x  = fract(pos.x / 6.0);

		if      (pos.x < 0.3) mask.r = P.maskLight;
		else if (pos.x < 0.6) mask.g = P.maskLight;
		else                  mask.b = P.maskLight;
	}

	// VGA style shadow mask.
	else if (P.shadowMask == 4.0)
	{
		pos.xy = floor(pos.xy * vec2(1.0, 0.5));
		pos.x += pos.y * 3.0;
		pos.x  = fract(pos.x / 6.0);

		if      (pos.x < 0.3) mask.r = P.maskLight;
		else if (pos.x < 0.6) mask.g = P.maskLight;
		else                  mask.b = P.maskLight;
	}

	// Trinitron mask 5
	else if (P.shadowMask == 5.0)
	{
		mask = V3(0.0);
		pos.x = fract(pos.x / 2.0);
		if (pos.x < 0.49)
		{	mask.r = 1.0;
			mask.b = 1.0;
		}
		else mask.g = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// Trinitron mask 6
	else if (P.shadowMask == 6.0)
	{
		mask = V3(0.0);
		pos.x = floor(MOD(pos.x, 3.0));
		if      (pos.x < 0.5) mask.r = 1.0;
		else if (pos.x < 1.5) mask.g = 1.0;
		else                  mask.b = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// BW Trinitron mask 7
	else if (P.shadowMask == 7.0)
	{
		mask = V3(0.0);
		pos.x = fract(pos.x / 2.0);
		if (pos.x < 0.49)
		{	mask = V3(0.0);
		}
		else mask = V3(1.0);
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// BW Trinitron mask 8
	else if (P.shadowMask == 8.0)
	{
		mask = V3(0.0);
		pos.x = fract(pos.x / 3.0);
		if      (pos.x < 0.3) mask = V3(0.0);
		else if (pos.x < 0.6) mask = V3(1.0);
		else                  mask = V3(1.0);
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// Magenta - Green - Black mask
	else if (P.shadowMask == 9.0)
	{
		mask = V3(0.0);
		pos.x = fract(pos.x / 3.0);
		if      (pos.x < 0.3) mask    = V3(0.0);
		else if (pos.x < 0.6) mask.rb = V2(1.0);
		else                  mask.g  = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// RGBX
	else if (P.shadowMask == 10.0)
	{
		mask = V3(0.0);
		pos.x = fract(pos.x * 0.25);
		if      (pos.x < 0.2) mask   = V3(0.0);
		else if (pos.x < 0.4) mask.r = 1.0;
		else if (pos.x < 0.7) mask.g = 1.0;
		else                  mask.b = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// 4k mask
	else if (P.shadowMask == 11.0)
	{
		mask = V3(0.0);
		pos.x = fract(pos.x * 0.25);
		if      (pos.x < 0.2) mask.r  = 1.0;
		else if (pos.x < 0.4) mask.rg = V2(1.0);
		else if (pos.x < 0.7) mask.gb = V2(1.0);
		else                  mask.b  = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// RRGGBBX mask
	else if (P.shadowMask == 12.0)
	{
		mask = V3(0.0);
		pos.x = floor(MOD(pos.x, 7.0));
		if      (pos.x < 0.5) mask   = V3(0.0);
		else if (pos.x < 2.5) mask.r = 1.0;
		else if (pos.x < 4.5) mask.g = 1.0;
		else                  mask.b = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// 4k mask
	else if (P.shadowMask == 13.0)
	{
		mask = V3(0.0);
		pos.x = floor(MOD(pos.x, 6.0));
		if      (pos.x < 0.5) mask    = V3(0.0);
		else if (pos.x < 1.5) mask.r  = 1.0;
		else if (pos.x < 2.5) mask.rg = V2(1.0);
		else if (pos.x < 3.5) mask    = V3(1.0);
		else if (pos.x < 4.5) mask.gb = V2(1.0);
		else                  mask.b  = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	// 4k mask
	else
	{
		mask = V3(0.0);
		pos.x = floor(MOD(pos.x, 5.0));
		if      (pos.x < 0.5) mask    = V3(0.0);
		else if (pos.x < 1.5) mask.r  = 1.0;
		else if (pos.x < 2.5) mask.rg = V2(1.0);
		else if (pos.x < 3.5) mask.gb = V2(1.0);
		else                  mask.b  = 1.0;
		mask = clamp(mix(mix(one, mask, P.mcut), mix(one, mask, P.maskstr), mx), V3(0.0), V3(1.0));
	}

	if (P.mask_layout > 0.5) mask = mask.rbg;
	float maskmin = min(min(mask.r, mask.g), mask.b);
	return (mask - maskmin) * (1.0 + (P.maskboost - 1.0) * mb) + maskmin;
}

FUNC float SlotMask (CTXP vec2 pos, float m, float swidth)
{
	if ((P.slotmask + P.slotmask1) == 0.0) return 1.0;

	pos.y = floor(pos.y / P.slotms);
	float mlen = swidth * 2.0;
	float px = floor(MOD(pos.x, 0.99999 * mlen));
	float py = floor(fract(pos.y / (2.0 * P.double_slot)) * 2.0 * P.double_slot);
	float slot_dark = mix(1.0 - P.slotmask1, 1.0 - P.slotmask, m);
	float slot = 1.0;
	if (py == 0.0 && px < swidth) slot = slot_dark; else
	if (py == P.double_slot && px >= swidth) slot = slot_dark;

	return slot;
}

FUNC float humbar (CTXP float pos)
{
	if (P.barintensity == 0.0) return 1.0;

	pos = (P.barintensity >= 0.0) ? pos : (1.0 - pos);
	pos = fract(pos + MOD(float(S.FrameCount), P.barspeed) / (P.barspeed - 1.0));
	pos = (P.barintensity < 0.0) ? pos : (1.0 - pos);
	return (1.0 - P.barintensity) + P.barintensity * pos;
}

FUNC float corner (CTXP vec2 pos)
{
	pos = abs(2.0 * (pos - 0.5));
	vec2 aspect = vec2(1.0, S.OutputSize.x / S.OutputSize.y);
	float b = P.bsize1 * 0.05 + 0.0005; pos.y = pos.y + b * (aspect.y - 1.0);
	vec2 crn = max(V2(P.csize), V2(2.0 * b + 0.0015));
	vec2 cp = max(pos - (1.0 - crn * aspect), 0.0) / aspect; float cd = sqrt(dot(cp, cp));
	pos = max(pos, 1.0 - crn + cd);
	float res = mix(1.0, 0.0, smoothstep(1.0 - b, 1.0, sqrt(max(pos.x, pos.y))));
	return POW(res, P.sborder);
}

FUNC float igc (CTXP float mc)
{
	return POW(mc, P.gamma_c);
}

FUNC vec3 gc2 (CTXP vec3 c, float w3)
{
	float mc = max(max(c.r, c.g), c.b);
	float gp = 1.0 / (1.0 + (P.gamma_c2 - 1.0) * mix(0.375, 1.0, w3));
	float mg = POW(mc, gp);
	return c * mg / (mc + eps);
}

// noise function:
// Dedicated to the public domain.
// If you want a real license, you may consider this MIT/BSD/CC0/WTFPL-licensed (take your pick).
// Adapted from ChuckNorris - shadertoy: https://www.shadertoy.com/view/XtK3Dz

FUNC vec3 crt_noise (CTXP vec3 v)
{
	if (P.addnoised < 0.0) v.z = -P.addnoised; else v.z = MOD(v.z, 6001.0) / 1753.0;
	// ensure reasonable range
	v = fract(v) + fract(v * 1e4) + fract(v * 1e-4);
	// seed
	v += vec3(0.12345, 0.6789, 0.314159);
	// more iterations => more random
	v = fract(v * dot(v, v) * 123.456);
	v = fract(v * dot(v, v) * 123.456);
	v = fract(v * dot(v, v) * 123.456);
	v = fract(v * dot(v, v) * 123.456);
	return v;
}

#define LinearizePass T0
#define AvgLumPass T1
#define GlowPass T2
#define BloomPass T3
#define PrePass T4
#define Source T5
#define StockPass T6

FUNC void decon_fetch (CTXP TEXP(T5) TEXP(T3) TEXP(T2) INOUT(vec3) c, INOUT(vec3) b, INOUT(vec3) g, vec2 coord, vec2 bcoord)
{
	float stepx = S.OutputSize.z;
	float stepy = S.OutputSize.w;

	float ds = P.decons;

	vec2 dx = vec2(stepx, 0.0);
	vec2 dy = vec2(0.0, stepy);

	float posx = 2.0 * coord.x - 1.0;
	float posy = 2.0 * coord.y - 1.0;

	if (P.dctypex > 0.025)
	{
		posx = sign(posx) * POW(abs(posx), 1.05 - P.dctypex);
		dx = posx * dx;
	}

	if (P.dctypey > 0.025)
	{
		posy = sign(posy) * POW(abs(posy), 1.05 - P.dctypey);
		dy = posy * dy;
	}

	vec2 rc = P.deconrr * dx + P.deconrry * dy;
	vec2 gcoff = P.deconrg * dx + P.deconrgy * dy;
	vec2 bc = P.deconrb * dx + P.deconrby * dy;

	float r1 = TEXL(Source, coord + rc).r;
	float g1 = TEXL(Source, coord + gcoff).g;
	float b1 = TEXL(Source, coord + bc).b;

	vec3 d = vec3(r1, g1, b1);
	c = clamp(mix(c, d, ds), V3(0.0), V3(1.0));

	r1 = TEXL(BloomPass, bcoord + rc).r;
	g1 = TEXL(BloomPass, bcoord + gcoff).g;
	b1 = TEXL(BloomPass, bcoord + bc).b;

	d = vec3(r1, g1, b1);
	g = mix(b, d, min(ds, 1.0));
	b = g;

	r1 = TEXL(GlowPass, bcoord + rc).r;
	g1 = TEXL(GlowPass, bcoord + gcoff).g;
	b1 = TEXL(GlowPass, bcoord + bc).b;

	d = vec3(r1, g1, b1);
	g = mix(g, d, min(ds, 1.0));
}

// mask widths and mask dark compensate (fractional part) values
#ifdef CRT_GLSL
const float mwidths[15] = float[15](2.0, 3.0, 3.0, 6.0, 6.0, 2.4, 3.4, 2.4, 3.25, 3.4, 4.4, 4.25, 7.4, 6.25, 5.25);
#else
CONST float mwidths[15] = {2.0, 3.0, 3.0, 6.0, 6.0, 2.4, 3.4, 2.4, 3.25, 3.4, 4.4, 4.25, 7.4, 6.25, 5.25};
#endif

FUNC vec4 crt_deconvergence (CTXP TEXP(T0) TEXP(T1) TEXP(T2) TEXP(T3) TEXP(T4) TEXP(T5) TEXP(T6) vec2 uv)
{
	vec2 TEX0 = uv * 1.00001;
	vec2 FragPos = TEX0 * S.OutputSize.xy;	// the slang shader's gl_FragCoord

	float lum = TEXL(AvgLumPass, vec2(0.5, 0.5)).a;

	float gamma_in = 1.0 / TEXL(LinearizePass, vec2(0.25, 0.25)).a;
	float intera = TEXL(LinearizePass, vec2(0.75, 0.25)).a;
	bool inter6 = (intera < 0.35 && P.interm == 6.0);
	bool interb = (((intera < 0.35 && !inter6) || P.no_scanlines > 0.025) && P.hiscan < 0.5);
	bool notate = (P.TATE < 0.5);

	// Calculating texel coordinates

	vec2 texcoord = TEX0.xy;
	if (P.IOS > 0.0 && !interb)
	{
		vec2 ofactor = S.OutputSize.xy / S.OriginalSize.xy;
		vec2 intfactor = (P.IOS < 2.5) ? floor(ofactor) : ceil(ofactor);
		vec2 diff = ofactor / intfactor;
		float scan = mix(diff.y, diff.x, P.TATE);
		texcoord = Overscan(texcoord, scan, scan);
		if (P.IOS == 1.0 || P.IOS == 3.0) texcoord = mix(vec2(TEX0.x, texcoord.y), vec2(texcoord.x, TEX0.y), P.TATE);
	}

	float factor = 1.00 + (1.0 - 0.5 * P.OS) * P.BLOOM / 100.0 - lum * P.BLOOM / 100.0;
	texcoord = Overscan(texcoord, factor, factor);

	texcoord.y = texcoord.y - P.VShift * S.OriginalSize.w;
	texcoord = Overscan(texcoord, (S.OriginalSize.x - P.overscanX) / S.OriginalSize.x,
		(S.OriginalSize.y - P.overscanY) / S.OriginalSize.y);

	vec2 pos1 = TEX0.xy;
	vec2 pos = Warp(CTXA texcoord);
	vec2 pos0 = Warp(CTXA TEX0.xy);
	vec2 posb = 2.0 * (pos0 - 0.5);
	if (P.BLOOM < 0.5) posb = max(abs(posb), abs(2.0 * (pos - 0.5))) * sign(posb);
	posb = 0.5 * posb + 0.5;

	// color and bloom fetching
	vec3 color = TEXL(Source, pos1).rgb;
	vec3 Bloom = TEXL(BloomPass, pos).rgb;
	vec3 Glow = TEXL(GlowPass, pos).rgb;

	if ((abs(P.deconrr) + abs(P.deconrry) + abs(P.deconrg) + abs(P.deconrgy) + abs(P.deconrb) + abs(P.deconrby)) > 0.2)
		decon_fetch(CTXA TEXA(Source) TEXA(BloomPass) TEXA(GlowPass) color, Bloom, Glow, pos1, pos); // deconvergence

	float cm = igc(CTXA max(max(color.r, color.g), color.b));
	float mx1 = TEXL(Source, pos1).a;
	float colmx = max(mx1, cm);
	float w3 = min((max((cm - 0.0005) * 1.0005, 0.0) + 0.0001) / (colmx + 0.0005), 1.0); if (interb) w3 = 1.0;

	vec2 dx = mix(vec2(0.001, 0.0), vec2(0.0, 0.001), P.TATE);
	float mx0 = TEXL(Source, pos1 - dx).a;
	float mx2 = TEXL(Source, pos1 + dx).a;
	float mxg = max(max(mx0, mx1), max(mx2, cm));
	float mx = POW(mxg, 1.40 / gamma_in);
	float cx = POW(colmx, 1.4 / gamma_in);

	// mask boost tweak

	dx = mix(vec2(S.OriginalSize.z, 0.0), vec2(0.0, S.OriginalSize.w), P.TATE) * 0.25;
	mx0 = TEXL(Source, pos1 - dx).a;
	mx2 = TEXL(Source, pos1 + dx).a;
	float mb = (1.0 - min(abs(mx0 - mx2) / (0.5 + mx1), 1.0));

	vec3 one = V3(1.0);

	// Apply Mask

	vec3 orig1 = color;
	vec3 cmask = one;
	vec3 cmask1 = one;
	vec3 cmask2 = one;

	float mwidth = mwidths[clamp(int(P.shadowMask), 0, 14)];
	float mask_compensate = fract(mwidth);
	float mwidth1 = mwidth;

	if (P.shadowMask > -0.5)
	{
		vec2 maskcoord = FragPos.yx;
		if (notate) maskcoord = maskcoord.yx;
		vec2 scoord = maskcoord;

		mwidth = floor(mwidth) * P.masksize;
		float swidth = mwidth;
		bool zoomed = (abs(P.mask_zoom) > 0.75);
		float mscale = 1.0;
		vec2 maskcoord0 = maskcoord;
		maskcoord.y = floor(maskcoord.y / P.masksize);
		mwidth1 = max(mwidth + P.mask_zoom, 2.0);

		if (P.mshift > 0.25)
		{
			float stagg_lvl = 1.0; if (fract(P.mshift) > 0.25) stagg_lvl = 2.0;
			float next_line = float(floor(MOD(maskcoord.y, 2.0 * stagg_lvl)) < stagg_lvl);
			maskcoord0.x = maskcoord0.x + next_line * 0.5 * mwidth1;
		}
		maskcoord = maskcoord0 / P.masksize;

		if (!zoomed)
			cmask = Mask(CTXA floor(maskcoord), mx, mb);
		else
		{
			mscale = mwidth1 / mwidth;
			float mlerp = fract(maskcoord.x / mscale); if (P.mzoom_sh > 0.025) mlerp = clamp((1.0 + P.mzoom_sh) * mlerp - 0.5 * P.mzoom_sh, 0.0, 1.0);
			float mcoord = floor(maskcoord.x / mscale);
			if (P.shadowMask == 12.0 && P.mask_zoom == -2.0) mcoord = ceil(maskcoord.x / mscale);
			cmask = mix(Mask(CTXA vec2(mcoord, maskcoord.y), mx, mb), Mask(CTXA vec2(mcoord + 1.0, maskcoord.y), mx, mb), mlerp);
		}

		float sm_offset = 0.0; bool bsm_offset = (P.shadowMask == 0.0 || P.shadowMask == 2.0 || P.shadowMask == 5.0
			|| P.shadowMask == 6.0 || P.shadowMask == 8.0 || P.shadowMask == 11.0);
		if (zoomed) { if (P.mask_layout < 0.5 && bsm_offset) sm_offset = 1.0; else if (bsm_offset) sm_offset = -1.0; }

		swidth = round(mwidth1); if (P.slotwidth > 0.5) swidth = P.slotwidth; float smask = 1.0;
		smask = SlotMask(CTXA scoord + vec2(sm_offset, 0.0), mx, swidth);

		smask = clamp(smask + mix(P.smask_mit, 0.0, w3 * POW(colmx, 0.3)), 0.0, 1.0);

		cmask2 = cmask;
		cmask *= smask;
		cmask1 = cmask;

		if (abs(P.mask_bloom) > 0.025)
		{
			float maxbl = max(max(max(Bloom.r, Bloom.g), Bloom.b), mxg);
			maxbl = maxbl * max(mix(1.0, 2.0 - colmx, P.bloom_dist), 0.0);
			if (P.mask_bloom > 0.025) cmask = max(min(cmask + maxbl * P.mask_bloom, 1.0), cmask);
			else cmask = max(mix(cmask, cmask * (1.0 - 0.5 * maxbl) + plant(POW(Bloom, V3(0.35)), maxbl), -P.mask_bloom), cmask);
		}

		color = POW(color, V3(P.mask_gamma / gamma_in));
		color = color * cmask;
		color = min(color, 1.0);
		color = POW(color, V3(gamma_in / P.mask_gamma));

		cmask = min(cmask, 1.0);
		cmask1 = min(cmask1, 1.0);
	}

	float dark_compensate = mix(max(clamp(mix(P.mcut, P.maskstr, mx), 0.0, 1.0) - 1.0 + mask_compensate, 0.0) + 1.0, 1.0, mx);
	if (P.shadowMask < -0.5) dark_compensate = 1.0;
	float bb = mix(P.brightboost, P.brightboost1, mx) * dark_compensate;
	color *= bb;

	vec3 Ref = TEXL(LinearizePass, pos).rgb;
	float maxb = TEXL(BloomPass, pos).a;
	float vig = TEXL(PrePass, clamp(pos, 0.0 + 0.5 * S.OriginalSize.zw, 1.0 - 0.5 * S.OriginalSize.zw)).a;

	if (P.pr_scan > 0.025)
	{
		float mbl = max(max(Bloom.r, Bloom.g), Bloom.b);
		Bloom = mix(Bloom, mix(Bloom, plant(orig1, mbl), min(2.5 * (1.0 - w3), 1.0)), min(2.0 * P.pr_scan, 1.0));
	}

	vec3 Bloom1 = Bloom;
	vec3 bcmask = mix(one, cmask1, P.bmask1);
	vec3 hcmask = mix(one, cmask1, P.hmask1);

	if (abs(P.bloom) > 0.025)
	{
		if (P.bloom < -0.01) Bloom1 = plant(Bloom, maxb);
		Bloom1 = min(Bloom1 * (orig1 + color), max(0.5 * (colmx + orig1 - color), 0.001 * Bloom1));
		Bloom1 = 0.5 * (Bloom1 + mix(Bloom1, mix(colmx * orig1, Bloom1, 0.5), 1.0 - color));
		Bloom1 = bcmask * Bloom1 * max(mix(1.0, 2.0 - colmx, P.bloom_dist), 0.0);
		color = POW(POW(color, V3(P.mask_gamma / gamma_in)) + abs(P.bloom) * POW(Bloom1, V3(P.mask_gamma / gamma_in)),
			V3(gamma_in / P.mask_gamma));
	}

	if (P.halation > 0.01)
	{
		Bloom = 0.5 * (Bloom + Bloom * Bloom);
		float mbl = max(max(Bloom.r, Bloom.g), Bloom.b); float cmxh = 0.5 * (colmx + colmx * colmx);
		mbl = mix(mix(cmxh, mix(cmxh, mbl, mbl), colmx), mbl, mb);
		Bloom = plant(Bloom, mix(sqrt(mbl * cmxh), max((mbl - 0.15 * (1.0 - colmx)), 0.4 * cmxh), POW(colmx, 0.25)))
			* mix(0.425, 1.0, colmx);
		Bloom = (3.0 - colmx - color) * plant(0.325 + orig1 / w3, 0.5 * (1.0 + w3)) * hcmask * Bloom;
		color = POW(POW(color, V3(P.mask_gamma / gamma_in)) + P.halation * POW(Bloom, V3(P.mask_gamma / gamma_in)),
			V3(gamma_in / P.mask_gamma));
	}
	else
	if (P.halation < -0.01)
	{
		float mbl = max(max(Bloom.r, Bloom.g), Bloom.b);
		Bloom = plant(Bloom + Ref + orig1 + Bloom * Bloom * Bloom, min(mbl * mbl, 0.75));
		Bloom = 2.0 * mix(1.0, w3, 0.5 * colmx) * hcmask * Bloom;
		color = color - P.halation * Bloom;
	}

	color = min(color, 1.0);
	color = gc2(CTXA color, w3);

	if (P.smoothmask > 0.125)
	{
		float w4 = POW(w3, 0.425 + 0.3 * P.smoothmask);
		w4 = max(w4 - 0.175 * colmx * P.smoothmask, 0.2);
		color = mix(min(color / w4, plant(orig1, 1.0 + 0.175 * colmx * P.smoothmask)) * w4, color, w4);
	}

	if (P.m_glow < 0.5) Glow = mix(Glow, 0.25 * color, colmx);
	else
	{
		maxb = max(max(Glow.r, Glow.g), Glow.b);
		vec3 orig2 = plant(orig1 + 0.001 * Ref, 1.0);
		Bloom = plant(Glow, 1.0);
		Ref = abs(orig2 - Bloom);
		mx0 = max(max(orig2.r, orig2.g), orig2.b) - min(min(orig2.r, orig2.g), orig2.b);
		mx2 = max(max(Bloom.r, Bloom.g), Bloom.b) - min(min(Bloom.r, Bloom.g), Bloom.b);
		Bloom = mix(maxb * min(Bloom, orig2), mix(mix(Glow, max(max(Ref.r, Ref.g), Ref.b) * Glow, max(mx, mx0)),
			mix(color, Glow, mx2), max(mx0, mx2) * Ref), min(sqrt((1.10 - mx0) * (0.10 + mx2)), 1.0));
		if (P.m_glow > 1.5) Glow = mix(0.5 * Glow * Glow, Bloom, Bloom);
		Glow = mix(P.m_glow_low * Glow, P.m_glow_high * Bloom, POW(colmx, P.m_glow_dist / gamma_in));
	}

	if (P.m_glow < 0.5)
	{
		if (P.glow >= 0.0) color = color + 0.5 * Glow * P.glow; else color = color + abs(P.glow) * min(cmask2 * cmask2, 1.0) * Glow;
	}
	else
	{
		vec3 cmaskg = clamp(mix(one, cmask1, P.m_glow_mask), V3(0.0), V3(1.0));
		color = color + abs(P.glow) * cmaskg * Glow;
	}

	color = min(color, 1.0);

	if (P.edgemask > 0.05)
	{
		mx0 = TEXL(Source, pos1 - dx).a; mx0 = TEXL(Source, pos1 - dx * (1.0 - 0.75 * sqrt(mx0))).a;
		mx2 = TEXL(Source, pos1 + dx).a; mx2 = TEXL(Source, pos1 + dx * (1.0 - 0.75 * sqrt(mx2))).a;
		float mx3 = TEXL(Source, pos1 - 4.0 * dx).a;
		float mx4 = TEXL(Source, pos1 + 4.0 * dx).a;
		mx4 = max(POW(abs(mx3 - mx4), 0.55 - 0.40 * cx), min(max(mx3, mx4) / min(0.1 + cx, 1.0), 1.0));
		mb = (1.0 - abs(POW(mx0, 1.0 - 0.65 * mx2) - POW(mx2, 1.0 - 0.65 * mx0)));
		mb = mx4 * P.edgemask * (1.0001 - mb * mb);
		vec3 temp = mix(color, orig1, mb);
		color = max(temp + mix(3.5 * mb * mix(1.625 * temp, temp, cx), V3(0.0), POW(color, V3(0.75 - 0.5 * colmx))), color);
	}

	color = color * mix(1.0, mix(0.5 * (1.0 + w3), w3, mx), P.pr_scan);  // preserve scanlines

	color = min(color, max(orig1, color) * mix(one, cmask1, P.mclip));  // preserve mask

	color = POW(color, V3(1.0 / P.gamma_out));

	float rc = 0.6 * sqrt(max(max(color.r, color.g), color.b)) + 0.4;

	if (abs(P.addnoised) > 0.01)
	{
		vec3 noise0 = crt_noise(CTXA vec3(floor(S.OutputSize.xy * TEX0 / P.noiseresd), float(S.FrameCount)));
		if (P.noisetype < 0.5) color = mix(color, noise0, 0.25 * abs(P.addnoised) * rc);
		else color = min(color * mix(1.0, 1.5 * noise0.x, 0.5 * abs(P.addnoised)), 1.0);
	}

	colmx = max(max(orig1.r, orig1.g), orig1.b);
	color = color + P.bmask * mix(cmask2, 0.125 * (1.0 - colmx) * color, min(20.0 * colmx, 1.0));

	return mix(vec4(color * vig * humbar(CTXA mix(pos.y, pos.x, P.bardir)) * P.post_br * corner(CTXA posb), 1.0),
		TEXN(StockPass, (floor(pos1 * S.OriginalSize.xy) + 0.5) * S.OriginalSize.zw), step(pos1.x, P.oimage - 0.00025));
}

#undef LinearizePass
#undef AvgLumPass
#undef GlowPass
#undef BloomPass
#undef PrePass
#undef Source
#undef StockPass
