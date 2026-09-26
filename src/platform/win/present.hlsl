// present.hlsl -- draws the software-rendered frame into the letterboxed viewport.

Texture2D<float4> g_frame : register(t0);
SamplerState g_sampler : register(s0);

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

float4 PSMain (VSOut i) : SV_Target
{
	return float4(g_frame.Sample(g_sampler, i.uv).rgb, 1.0);
}
