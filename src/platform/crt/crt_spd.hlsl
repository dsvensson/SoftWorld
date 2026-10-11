// crt_spd.hlsl -- PrePass's mip chain for crt_d3d11.c (vid_crt.h), in a file of
// its own as fxc takes groupshared memory in compute shaders alone

/*
===============================================================================

CS_SPD: PrePass's mip chain in one dispatch, as AMD's FidelityFX Single Pass
Downsampler makes one. Each workgroup makes its 64x64 tile of the base level into
the 6 levels after it, a level from four texels of the one before, and writes
its one texel of the sixth to the buffer too; the last workgroup to be done
makes the levels after that from those. Each texel is the average of the
four it covers in the level before (a level is half the one before's size,
rounded down), what lies past an edge of a level 1 across the texel there.

===============================================================================
*/

cbuffer SpdBlock : register(b0)
{
	uint	g_base;			// unused here: the views are of the levels from it
	uint	g_levels;		// how many levels after it to make
	uint	g_numgroups;
	uint	g_groupsx;
	uint	g_width;		// the base level's size
	uint	g_height;
	uint2	g_pad;
};

Texture2D<float4>					g_spdsource : register(t0);		// the base level
RWTexture2D<unorm float4>			g_mips[12] : register(u0);		// the levels after it
globallycoherent RWByteAddressBuffer	g_spdbuffer : register(u12);	// the counter, then level 6's texels

groupshared float4	g_lds[16][16];
groupshared uint	g_spdlast;

// a level's size, the base 0
uint2 SpdSize (uint level)
{
	return max(uint2(g_width, g_height) >> level, uint2(1, 1));
}

void SpdStore (uint level, uint2 p, float4 v)
{
	if (level <= g_levels && all(p < SpdSize(level)))
		g_mips[level - 1][p] = v;
}

float4 SpdLoadSource (uint2 p, uint2 last)
{
	return g_spdsource.Load(int3(min(p, last), 0));
}

float4 SpdLoadMid (uint2 p, uint2 last)
{
	p = min(p, last);
	return asfloat(g_spdbuffer.Load4(16 + (p.y * 64 + p.x) * 16));
}

// a level made of the one before's texels in registers: each thread's four,
// 2x2 from (2tx, 2ty) of the tile at tile, which that level's size cuts
float4 SpdReduceQuad (float4 v[4], uint2 tile, uint tx, uint ty, uint2 size)
{
	uint2	p = tile + uint2(tx, ty) * 2;
	bool	xok = p.x + 1 < size.x, yok = p.y + 1 < size.y;

	return (v[0] + (xok ? v[1] : v[0]) + (yok ? v[2] : v[0]) + (xok ? (yok ? v[3] : v[1]) : (yok ? v[2] : v[0])))
		* 0.25;
}

// level, n texels across the tile, of the one before in g_lds, whose tile at
// tile (2n across) its size cuts
void SpdReduceLDS (uint level, uint n, uint2 group, uint t, uint2 size)
{
	uint	x = t % n, y = t / n;
	int2	lim = int2(size) - 1 - int2(group * (n * 2));
	uint2	last = uint2(clamp(lim, int2(0, 0), int2(n * 2 - 1, n * 2 - 1)));
	float4	r = 0;

	if (t < n * n)
	{
		uint2	a = uint2(x, y) * 2;
		uint2	b = min(a + 1, last);

		r = (g_lds[a.y][a.x] + g_lds[a.y][b.x] + g_lds[b.y][a.x] + g_lds[b.y][b.x]) * 0.25;
	}
	GroupMemoryBarrierWithGroupSync ();
	if (t < n * n)
	{
		g_lds[y][x] = r;
		SpdStore(level, group * n + uint2(x, y), r);
	}
	GroupMemoryBarrierWithGroupSync ();
}

[numthreads(256, 1, 1)]
void CS_SPD (uint3 groupid : SV_GroupID, uint t : SV_GroupIndex)
{
	uint2	group = groupid.xy;
	uint	tx = t % 16, ty = t / 16;
	uint2	last = SpdSize(0) - 1;
	float4	v[4];
	uint	i, j;

	// level 1: each thread 2x2 of the tile's 32x32
	[unroll] for (j = 0 ; j < 2 ; j++)
		[unroll] for (i = 0 ; i < 2 ; i++)
		{
			uint2	p = group * 32 + uint2(tx * 2 + i, ty * 2 + j);
			uint2	q = p * 2;

			v[j * 2 + i] = (SpdLoadSource(q, last) + SpdLoadSource(q + uint2(1, 0), last)
				+ SpdLoadSource(q + uint2(0, 1), last) + SpdLoadSource(q + uint2(1, 1), last)) * 0.25;
			SpdStore(1, p, v[j * 2 + i]);
		}
	if (g_levels < 2)
		return;

	// level 2: a texel each, 16x16
	float4 m = SpdReduceQuad(v, group * 32, tx, ty, SpdSize(1));
	SpdStore(2, group * 16 + uint2(tx, ty), m);
	g_lds[ty][tx] = m;
	GroupMemoryBarrierWithGroupSync ();

	// levels 3 to 6, 8x8 to the one texel
	if (g_levels < 3)
		return;
	SpdReduceLDS(3, 8, group, t, SpdSize(2));
	if (g_levels < 4)
		return;
	SpdReduceLDS(4, 4, group, t, SpdSize(3));
	if (g_levels < 5)
		return;
	SpdReduceLDS(5, 2, group, t, SpdSize(4));
	if (g_levels < 6)
		return;
	SpdReduceLDS(6, 1, group, t, SpdSize(5));
	if (g_levels < 7)
		return;

	// the workgroup's texel of level 6 for the last one, which makes the rest
	if (t == 0)
		g_spdbuffer.Store4(16 + (group.y * 64 + group.x) * 16, asuint(g_lds[0][0]));
	DeviceMemoryBarrierWithGroupSync ();
	if (t == 0)
	{
		uint	done;

		g_spdbuffer.InterlockedAdd(0, 1, done);
		g_spdlast = done == g_numgroups - 1 ? 1 : 0;
	}
	GroupMemoryBarrierWithGroupSync ();
	if (!g_spdlast)
		return;
	if (t == 0)
		g_spdbuffer.Store(0, 0);		// for the next dispatch

	// level 7 of level 6, as level 1 of the base
	last = SpdSize(6) - 1;
	[unroll] for (j = 0 ; j < 2 ; j++)
		[unroll] for (i = 0 ; i < 2 ; i++)
		{
			uint2	p = uint2(tx * 2 + i, ty * 2 + j);
			uint2	q = p * 2;

			v[j * 2 + i] = (SpdLoadMid(q, last) + SpdLoadMid(q + uint2(1, 0), last)
				+ SpdLoadMid(q + uint2(0, 1), last) + SpdLoadMid(q + uint2(1, 1), last)) * 0.25;
			SpdStore(7, p, v[j * 2 + i]);
		}
	if (g_levels < 8)
		return;

	m = SpdReduceQuad(v, uint2(0, 0), tx, ty, SpdSize(7));
	SpdStore(8, uint2(tx, ty), m);
	g_lds[ty][tx] = m;
	GroupMemoryBarrierWithGroupSync ();

	if (g_levels < 9)
		return;
	SpdReduceLDS(9, 8, uint2(0, 0), t, SpdSize(8));
	if (g_levels < 10)
		return;
	SpdReduceLDS(10, 4, uint2(0, 0), t, SpdSize(9));
	if (g_levels < 11)
		return;
	SpdReduceLDS(11, 2, uint2(0, 0), t, SpdSize(10));
	if (g_levels < 12)
		return;
	SpdReduceLDS(12, 1, uint2(0, 0), t, SpdSize(11));
}
