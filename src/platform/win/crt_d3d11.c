// crt_d3d11.c -- the CRT shader's passes (vid_crt.h) in Direct3D 11: crt.hlsl's
// shaders, a texture each pass draws in (shader resource and render target),
// the LUTs, and PrePass's mip chain, made by crt_spd.hlsl where the device binds
// its 13 unordered access views (feature level 11_1), else by GenerateMips.
// Nothing here exists until the shader is first turned on.

#include "mem.h"
#include "print.h"
#include "crt_d3d11.h"

#include <string.h>

#include "crt_vs.h"
#include "crt_afterglow_ps.h"
#include "crt_prepass_ps.h"
#include "crt_avglum_ps.h"
#include "crt_linearize_ps.h"
#include "crt_gauss_h_ps.h"
#include "crt_gauss_v_ps.h"
#include "crt_bloom_h_ps.h"
#include "crt_bloom_v_ps.h"
#include "crt_crt_ps.h"
#include "crt_deconvergence_ps.h"
#include "crt_spd_cs.h"

#define CRT_MAX_LEVELS	16		// mip levels of a texture at most

#define CRT_RELEASE(obj) do { if (obj) { IUnknown_Release ((IUnknown *)(obj)); (obj) = NULL; } } while (0)

typedef struct
{
	ID3D11Texture2D				*texture;
	ID3D11ShaderResourceView	*srv;		// the passes' view of it, all its levels
	ID3D11RenderTargetView		*rtv;		// its top level, drawn in
	int							width, height;
	int							levels;
	ID3D11ShaderResourceView	*levelsrv[CRT_MAX_LEVELS];	// PrePass's levels alone, which crt_spd reads
	ID3D11UnorderedAccessView	*uav[CRT_MAX_LEVELS];		// and writes
} crt_texture_t;

static const struct
{
	const BYTE	*code;
	SIZE_T		size;
} crt_pixelshaders[CRT_PASSES] = {
	{g_crt_afterglow_ps, sizeof(g_crt_afterglow_ps)},
	{g_crt_prepass_ps, sizeof(g_crt_prepass_ps)},
	{g_crt_avglum_ps, sizeof(g_crt_avglum_ps)},
	{g_crt_linearize_ps, sizeof(g_crt_linearize_ps)},
	{g_crt_gauss_h_ps, sizeof(g_crt_gauss_h_ps)},
	{g_crt_gauss_v_ps, sizeof(g_crt_gauss_v_ps)},
	{g_crt_bloom_h_ps, sizeof(g_crt_bloom_h_ps)},
	{g_crt_bloom_v_ps, sizeof(g_crt_bloom_v_ps)},
	{g_crt_crt_ps, sizeof(g_crt_crt_ps)},
	{g_crt_deconvergence_ps, sizeof(g_crt_deconvergence_ps)},
};

static ID3D11Device				*crt_device;
static ID3D11VertexShader		*crt_vs;
static ID3D11PixelShader		*crt_ps[CRT_PASSES];
static ID3D11ComputeShader		*crt_cs;			// NULL: GenerateMips makes the mip chain
static ID3D11SamplerState		*crt_samplers[2];	// nearest, bilinear
static ID3D11Buffer				*crt_params;
static ID3D11Buffer				*crt_passes[CRT_PASSES];
static ID3D11Buffer				*crt_spdconstants[CRT_SPD_MAX_DISPATCHES];
static ID3D11Buffer				*crt_spdbuffer;		// the counter, then level 6's texels
static ID3D11UnorderedAccessView	*crt_spduav;
static crt_texture_t			crt_textures[CRT_TEXTURES];
static vid_crt_spd_t			crt_spd[CRT_SPD_MAX_DISPATCHES];
static int						crt_spdgroups[CRT_SPD_MAX_DISPATCHES][2];
static int						crt_spddispatches;

static bool CRT_D3D11_Check (HRESULT hr, const char *what)
{
	if (SUCCEEDED (hr))
		return true;
	Con_Printf ("The CRT shader: %s failed (HRESULT 0x%08lx)\n", what, (unsigned long)hr);
	return false;
}

static ID3D11Buffer *CRT_D3D11_ConstantBuffer (UINT size)
{
	ID3D11Buffer	*buffer = NULL;
	D3D11_BUFFER_DESC desc = {
		.ByteWidth = (size + 15) & ~15u,
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};

	if (!CRT_D3D11_Check (ID3D11Device_CreateBuffer (crt_device, &desc, NULL, &buffer), "CreateBuffer"))
		return NULL;
	return buffer;
}

static void CRT_D3D11_FreeTexture (crt_texture_t *t)
{
	int		i;

	for (i = 0 ; i < CRT_MAX_LEVELS ; i++)
	{
		CRT_RELEASE (t->levelsrv[i]);
		CRT_RELEASE (t->uav[i]);
	}
	CRT_RELEASE (t->rtv);
	CRT_RELEASE (t->srv);
	CRT_RELEASE (t->texture);
	t->width = t->height = t->levels = 0;
}

// a texture of vid_crt_textures' kind, data its pixels for a LUT
static bool CRT_D3D11_CreateTexture (int index, int width, int height, const void *data)
{
	const vid_crt_texture_t	*kind = &vid_crt_textures[index];
	crt_texture_t			*t = &crt_textures[index];
	int						i;
	D3D11_TEXTURE2D_DESC	desc = {
		.Width = (UINT)width,
		.Height = (UINT)height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = kind->format == CRT_RGBA16F ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {.Count = 1},
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
	};
	D3D11_SUBRESOURCE_DATA	initial = {.pSysMem = data, .SysMemPitch = (UINT)width * 4};

	CRT_D3D11_FreeTexture (t);
	t->levels = 1;
	if (data)
	{
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	}
	else if (kind->mips)
	{
		t->levels = VID_CRT_MipLevels (width, height);
		if (t->levels > CRT_MAX_LEVELS)
			t->levels = CRT_MAX_LEVELS;
		desc.MipLevels = (UINT)t->levels;
		if (crt_cs)
			desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
		else
			desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	}

	if (!CRT_D3D11_Check (ID3D11Device_CreateTexture2D (crt_device, &desc, data ? &initial : NULL, &t->texture),
			"CreateTexture2D")
		|| !CRT_D3D11_Check (ID3D11Device_CreateShaderResourceView (crt_device, (ID3D11Resource *)t->texture, NULL,
			&t->srv), "CreateShaderResourceView"))
		return false;
	if (!data)
	{
		D3D11_RENDER_TARGET_VIEW_DESC rtv = {
			.Format = desc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
		};
		if (!CRT_D3D11_Check (ID3D11Device_CreateRenderTargetView (crt_device, (ID3D11Resource *)t->texture, &rtv,
				&t->rtv), "CreateRenderTargetView"))
			return false;
	}
	if (kind->mips && crt_cs)
		for (i = 0 ; i < t->levels ; i++)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC level = {
				.Format = desc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {.MostDetailedMip = (UINT)i, .MipLevels = 1},
			};
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {
				.Format = desc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = {.MipSlice = (UINT)i},
			};
			if (!CRT_D3D11_Check (ID3D11Device_CreateShaderResourceView (crt_device, (ID3D11Resource *)t->texture,
					&level, &t->levelsrv[i]), "CreateShaderResourceView")
				|| !CRT_D3D11_Check (ID3D11Device_CreateUnorderedAccessView (crt_device, (ID3D11Resource *)t->texture,
					&uav, &t->uav[i]), "CreateUnorderedAccessView"))
				return false;
		}
	t->width = width;
	t->height = height;
	return true;
}

static bool CRT_D3D11_CreateSampler (D3D11_FILTER filter, ID3D11SamplerState **sampler)
{
	// black past the edges, as RetroArch's default wrap mode is
	D3D11_SAMPLER_DESC desc = {
		.Filter = filter,
		.AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
		.AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
		.AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MaxLOD = D3D11_FLOAT32_MAX,
	};

	return CRT_D3D11_Check (ID3D11Device_CreateSamplerState (crt_device, &desc, sampler), "CreateSamplerState");
}

bool CRT_D3D11_Init (ID3D11Device *device)
{
	byte	*pixels;
	int		i, w, h;
	bool	ok = true;

	crt_device = device;
	ID3D11Device_AddRef (device);

	ok = ok && CRT_D3D11_Check (ID3D11Device_CreateVertexShader (device, g_crt_vs, sizeof(g_crt_vs), NULL, &crt_vs),
		"CreateVertexShader");
	for (i = 0 ; i < CRT_PASSES && ok ; i++)
		ok = CRT_D3D11_Check (ID3D11Device_CreatePixelShader (device, crt_pixelshaders[i].code,
			crt_pixelshaders[i].size, NULL, &crt_ps[i]), "CreatePixelShader");
	ok = ok && CRT_D3D11_CreateSampler (D3D11_FILTER_MIN_MAG_MIP_POINT, &crt_samplers[0])
		&& CRT_D3D11_CreateSampler (D3D11_FILTER_MIN_MAG_MIP_LINEAR, &crt_samplers[1]);
	ok = ok && (crt_params = CRT_D3D11_ConstantBuffer ((UINT)sizeof(vid_crt_params_t))) != NULL;
	for (i = 0 ; i < CRT_PASSES && ok ; i++)
		ok = (crt_passes[i] = CRT_D3D11_ConstantBuffer ((UINT)sizeof(vid_crt_pass_t))) != NULL;

	// the single pass downsampler binds 13 unordered access views: 11_1's 64,
	// not 11_0's 8
	if (ok && ID3D11Device_GetFeatureLevel (device) >= D3D_FEATURE_LEVEL_11_1)
	{
		static const UINT zero[CRT_SPD_BUFFER_SIZE / 4] = {0};
		D3D11_BUFFER_DESC desc = {
			.ByteWidth = CRT_SPD_BUFFER_SIZE,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
		};
		D3D11_SUBRESOURCE_DATA initial = {.pSysMem = zero};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {.NumElements = CRT_SPD_BUFFER_SIZE / 4, .Flags = D3D11_BUFFER_UAV_FLAG_RAW},
		};

		ok = CRT_D3D11_Check (ID3D11Device_CreateComputeShader (device, g_crt_spd_cs, sizeof(g_crt_spd_cs), NULL,
			&crt_cs), "CreateComputeShader")
			&& CRT_D3D11_Check (ID3D11Device_CreateBuffer (device, &desc, &initial, &crt_spdbuffer), "CreateBuffer")
			&& CRT_D3D11_Check (ID3D11Device_CreateUnorderedAccessView (device, (ID3D11Resource *)crt_spdbuffer, &uav,
				&crt_spduav), "CreateUnorderedAccessView");
		for (i = 0 ; i < CRT_SPD_MAX_DISPATCHES && ok ; i++)
			ok = (crt_spdconstants[i] = CRT_D3D11_ConstantBuffer ((UINT)sizeof(vid_crt_spd_t))) != NULL;
	}

	for (i = 0 ; i < 4 && ok ; i++)
	{
		pixels = VID_CRT_LoadLUT (i, &w, &h);
		if (!pixels)
		{
			Con_Printf ("The CRT shader: LUT %d is broken\n", i + 1);
			ok = false;
			break;
		}
		ok = CRT_D3D11_CreateTexture (CRT_LUT1 + i, w, h, pixels);
		Mem_Free (pixels);
	}

	if (!ok)
		CRT_D3D11_Shutdown ();
	return ok;
}

void CRT_D3D11_Shutdown (void)
{
	int		i;

	for (i = 0 ; i < CRT_TEXTURES ; i++)
		CRT_D3D11_FreeTexture (&crt_textures[i]);
	for (i = 0 ; i < CRT_SPD_MAX_DISPATCHES ; i++)
		CRT_RELEASE (crt_spdconstants[i]);
	CRT_RELEASE (crt_spduav);
	CRT_RELEASE (crt_spdbuffer);
	CRT_RELEASE (crt_cs);
	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		CRT_RELEASE (crt_passes[i]);
		CRT_RELEASE (crt_ps[i]);
	}
	CRT_RELEASE (crt_params);
	CRT_RELEASE (crt_samplers[0]);
	CRT_RELEASE (crt_samplers[1]);
	CRT_RELEASE (crt_vs);
	CRT_RELEASE (crt_device);
	crt_spddispatches = 0;
}

void CRT_D3D11_SetSize (ID3D11DeviceContext *context, int sourcewidth, int sourceheight, int viewwidth,
	int viewheight)
{
	static const float	black[4] = {0, 0, 0, 0};
	crt_texture_t		*t;
	int					i, w, h;

	for (i = 0 ; i < CRT_TEXTURES ; i++)
	{
		if (vid_crt_textures[i].size == CRT_SIZE_LUT)
			continue;
		t = &crt_textures[i];
		VID_CRT_TextureSize (i, sourcewidth, sourceheight, viewwidth, viewheight, &w, &h);
		if (t->texture && t->width == w && t->height == h)
			continue;
		if (!CRT_D3D11_CreateTexture (i, w, h, NULL))
		{
			CRT_D3D11_FreeTexture (t);
			continue;
		}
		if (vid_crt_textures[i].feedback)
			ID3D11DeviceContext_ClearRenderTargetView (context, t->rtv, black);
		if (vid_crt_textures[i].mips)
			crt_spddispatches = VID_CRT_SpdDispatches (w, h, crt_spd, crt_spdgroups);
	}
}

ID3D11RenderTargetView *CRT_D3D11_Original (void)
{
	return crt_textures[CRT_ORIGINAL].rtv;
}

ID3D11Texture2D *CRT_D3D11_Texture (int texture)
{
	return crt_textures[texture].texture;
}

// PrePass's mip chain from its top level, just drawn
static void CRT_D3D11_Mips (ID3D11DeviceContext *context)
{
	static ID3D11UnorderedAccessView	*nouavs[CRT_SPD_LEVELS + 1];
	static ID3D11ShaderResourceView		*nosrv;
	ID3D11UnorderedAccessView			*uavs[CRT_SPD_LEVELS + 1];
	crt_texture_t						*t = &crt_textures[CRT_PREPASS];
	int									d, i, level;

	ID3D11DeviceContext_OMSetRenderTargets (context, 0, NULL, NULL);
	if (!crt_cs)
	{
		ID3D11DeviceContext_GenerateMips (context, t->srv);
		return;
	}

	ID3D11DeviceContext_CSSetShader (context, crt_cs, NULL, 0);
	for (d = 0 ; d < crt_spddispatches ; d++)
	{
		for (i = 0 ; i < CRT_SPD_LEVELS ; i++)
		{
			level = (int)crt_spd[d].base + 1 + i;
			uavs[i] = level < t->levels ? t->uav[level] : NULL;
		}
		uavs[CRT_SPD_LEVELS] = crt_spduav;
		ID3D11DeviceContext_UpdateSubresource (context, (ID3D11Resource *)crt_spdconstants[d], 0, NULL, &crt_spd[d],
			0, 0);
		ID3D11DeviceContext_CSSetConstantBuffers (context, 0, 1, &crt_spdconstants[d]);
		ID3D11DeviceContext_CSSetShaderResources (context, 0, 1, &t->levelsrv[crt_spd[d].base]);
		ID3D11DeviceContext_CSSetUnorderedAccessViews (context, 0, CRT_SPD_LEVELS + 1, uavs, NULL);
		ID3D11DeviceContext_Dispatch (context, (UINT)crt_spdgroups[d][0], (UINT)crt_spdgroups[d][1], 1);
		// unbound, for the next dispatch to read what this one wrote, and the passes after
		ID3D11DeviceContext_CSSetUnorderedAccessViews (context, 0, CRT_SPD_LEVELS + 1, nouavs, NULL);
		ID3D11DeviceContext_CSSetShaderResources (context, 0, 1, &nosrv);
	}
	ID3D11DeviceContext_CSSetShader (context, NULL, NULL, 0);
}

void CRT_D3D11_Draw (ID3D11DeviceContext *context, ID3D11RenderTargetView *target, const D3D11_VIEWPORT *viewport,
	const vid_crt_params_t *params, const vid_crt_pass_t passes[CRT_PASSES], int parity)
{
	static ID3D11ShaderResourceView	*none[CRT_MAX_INPUTS];
	static byte						block[VID_CRT_PARAMS_SIZE];
	ID3D11ShaderResourceView		*inputs[CRT_MAX_INPUTS];
	ID3D11RenderTargetView			*rtv;
	const vid_crt_pass_info_t		*pass;
	const crt_texture_t				*out;
	D3D11_VIEWPORT					vp;
	int								i, j;

	for (i = 0 ; i < CRT_TEXTURES ; i++)
		if (!crt_textures[i].texture)
			return;		// a size that couldn't be made

	memcpy (block, params, sizeof(*params));
	ID3D11DeviceContext_UpdateSubresource (context, (ID3D11Resource *)crt_params, 0, NULL, block, 0, 0);
	ID3D11DeviceContext_IASetPrimitiveTopology (context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout (context, NULL);
	ID3D11DeviceContext_VSSetShader (context, crt_vs, NULL, 0);
	ID3D11DeviceContext_PSSetSamplers (context, 0, 2, crt_samplers);
	ID3D11DeviceContext_PSSetConstantBuffers (context, 0, 1, &crt_params);

	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		pass = &vid_crt_passes[i];
		ID3D11DeviceContext_UpdateSubresource (context, (ID3D11Resource *)crt_passes[i], 0, NULL, &passes[i], 0, 0);

		// what the pass before read may be drawn in now
		ID3D11DeviceContext_PSSetShaderResources (context, 0, CRT_MAX_INPUTS, none);
		if (pass->output == CRT_SCREEN)
		{
			rtv = target;
			vp = *viewport;
		}
		else
		{
			out = &crt_textures[VID_CRT_Physical (pass->output, parity)];
			rtv = out->rtv;
			vp = (D3D11_VIEWPORT){0, 0, (float)out->width, (float)out->height, 0, 1};
		}
		ID3D11DeviceContext_OMSetRenderTargets (context, 1, &rtv, NULL);
		ID3D11DeviceContext_RSSetViewports (context, 1, &vp);
		for (j = 0 ; j < pass->numinputs ; j++)
			inputs[j] = crt_textures[VID_CRT_Physical (pass->inputs[j], parity)].srv;
		ID3D11DeviceContext_PSSetShader (context, crt_ps[i], NULL, 0);
		ID3D11DeviceContext_PSSetConstantBuffers (context, 1, 1, &crt_passes[i]);
		ID3D11DeviceContext_PSSetShaderResources (context, 0, (UINT)pass->numinputs, inputs);
		ID3D11DeviceContext_Draw (context, 3, 0);

		if (i == CRT_PASS_PREPASS)
		{
			ID3D11DeviceContext_PSSetShaderResources (context, 0, CRT_MAX_INPUTS, none);
			CRT_D3D11_Mips (context);
		}
	}
	ID3D11DeviceContext_PSSetShaderResources (context, 0, CRT_MAX_INPUTS, none);
}
