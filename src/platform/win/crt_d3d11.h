// crt_d3d11.h -- the CRT shader's passes (vid_crt.h) in Direct3D 11, which
// vid_d3d11.c draws the frame through and test_present_d3d11.c tests
#pragma once

#define COBJMACROS
#include <d3d11.h>

#include "vid_crt.h"

// the shaders, samplers, constant buffers and LUTs; false (and why printed) if
// the device can't have them
bool	CRT_D3D11_Init (ID3D11Device *device);
void	CRT_D3D11_Shutdown (void);

// the textures for a frame of source pixels shown in a viewport of view pixels,
// made again where a size changed (AFTERGLOW's and AVGLUM's black)
void	CRT_D3D11_SetSize (ID3D11DeviceContext *context, int sourcewidth, int sourceheight, int viewwidth,
			int viewheight);

// CRT_ORIGINAL, for the frame to be drawn into as present.hlsl shows it, at its
// own size, before CRT_D3D11_Draw
ID3D11RenderTargetView	*CRT_D3D11_Original (void);

// a texture as it is, for the tests
ID3D11Texture2D	*CRT_D3D11_Texture (int texture);

// the passes, the last into target in viewport; the frame of parity's
void	CRT_D3D11_Draw (ID3D11DeviceContext *context, ID3D11RenderTargetView *target, const D3D11_VIEWPORT *viewport,
			const vid_crt_params_t *params, const vid_crt_pass_t passes[CRT_PASSES], int parity);
