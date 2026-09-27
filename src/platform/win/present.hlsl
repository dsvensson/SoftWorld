// present.hlsl -- draws the software-rendered frame into the letterboxed viewport.
//
// The 3D view is R10G10B10A2 with SDR white at 255 of 1023, which leaves headroom for
// overbright light. The shader applies the view blend, gamma and contrast to it, then lays
// the 2D over it (RGBA8, premultiplied, SDR), and writes either SDR (display gamma,
// clipped) or scRGB (linear, 1.0 = 80 nits) for HDR displays, the 2D at paper white.

Texture2D<float4> g_frame : register(t0);
Texture2D<float4> g_hud : register(t1);
SamplerState g_linear : register(s0);

cbuffer Present : register(b0)
{
	float4	g_blend;		// rgb, and how much of it covers the view
	float2	g_texsize;		// frame size in texels
	float2	g_scale;		// screen pixels per texel
	float	g_gamma;		// exponent applied to the view; 1 keeps it
	float	g_contrast;		// multiplier; 1 keeps it
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

// compresses what is brighter than knee toward peak instead of clipping it
float3 RollOff (float3 c, float knee, float peak)
{
	float3 over = max(c - knee, 0);
	float span = max(peak - knee, 1e-3);
	return min(c, knee) + span * (1 - exp(-over / span));
}

float4 PSMain (VSOut i) : SV_Target
{
	float3 c = SampleLayer(g_frame, i.uv).rgb * (1023.0 / 255.0);
	float4 hud = SampleLayer(g_hud, i.uv);

	c = lerp(c, g_blend.rgb, g_blend.a);
	c = pow(max(c, 0), g_gamma) * max(g_contrast, 0);

	if (g_hdr == 0)
		return float4(saturate(c) * (1 - hud.a) + hud.rgb, 1);

	// display gamma to linear light, SDR white at paper white, highlights up to the peak;
	// the 2D over it in linear light
	float3 lin = RollOff(pow(max(c, 0), 2.2) * g_paperwhite, g_paperwhite, g_peak);
	float3 hudlin = pow(max(hud.rgb / max(hud.a, 1.0 / 255.0), 0), 2.2) * g_paperwhite * hud.a;
	return float4(hudlin + lin * (1 - hud.a), 1);
}
