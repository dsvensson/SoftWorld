/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// test_present_d3d11.c -- present.hlsl drawn with Direct3D 11 as vid_d3d11.c draws it,
// from buffers laid out as the renderer fills them and read raw, into a float target
// read back: in SDR each pixel within 1 of what VID_FrameToRGB (screenshots) makes of
// it, with and without gamma and a view blend; scaled by 2.5, sharp bilinear, each
// pixel inside a texel that texel's, and each on an edge between its two; and in
// linear HDR (scRGB), SDR white at paper white, the brightest light rolled off below
// the peak, and the 2D at paper white, each pixel as VID_FrameToPQ (HDR screenshots)
// makes it. All of it read where the CPU wrote the layers,
// if the driver lets a shader read them there (MapOnDefaultBuffers), and from copies
// of them in the GPU's memory, as vid_d3d11.c has the two. On the GPU, else (or with
// -warp) WARP.

#define COBJMACROS

#include "cvar.h"
#include "sys.h"
#include "vid_common.h"
#include "vid_d3d11.h"

#include <d3d11.h>

#include "present_vs.h"
#include "present_ps.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH		200
#define HEIGHT		120
#define SCALED		5				// WIDTH and HEIGHT times this, halved: 2.5
#define MAX_WIDTH	(WIDTH * SCALED / 2)
#define MAX_HEIGHT	(HEIGHT * SCALED / 2)
#define LAYER_SIZE	(WIDTH * HEIGHT * 4)

static int	failures;

//
// the engine, as far as vid_common.c needs it
//

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	(void)fmt;
}

void R_SetRenderSize (int width, int height, int scale)
{
	(void)width;
	(void)height;
	(void)scale;
}

static pixel_t		*frame;			// the layers, mapped as the renderer has them
static hudpixel_t	*hud;

void VID_ShownLayers (const pixel_t **shownframe, const hudpixel_t **shownhud)
{
	*shownframe = frame;
	*shownhud = hud;
}

//
// Direct3D 11
//

typedef struct
{
	ID3D11Buffer				*buffer;
	ID3D11ShaderResourceView	*srv;
} layer_t;

static ID3D11Device				*device;
static ID3D11DeviceContext		*context;
static ID3D11VertexShader		*vs;
static ID3D11PixelShader		*ps;
static ID3D11Buffer				*constants;
static ID3D11Texture2D			*target, *readback;
static ID3D11RenderTargetView	*rtv;
static bool						mapdefault;			// the CPU's layers can be read by a shader
static layer_t					framecpu, hudcpu;	// the CPU's, mapped but while drawn
static layer_t					framegpu, hudgpu;	// copied into the GPU's memory

static void Check (HRESULT hr, const char *what)
{
	if (FAILED (hr))
		Sys_Error ("%s failed (HRESULT 0x%08lx)", what, (unsigned long)hr);
}

// a layer as vid_d3d11.c makes it: the CPU's, or the GPU's alone
static void CreateLayer (layer_t *layer, bool cpu)
{
	D3D11_BUFFER_DESC desc = {
		.ByteWidth = LAYER_SIZE,
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE,
		.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC view = {
		.Format = DXGI_FORMAT_R32_TYPELESS,
		.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX,
		.BufferEx = {.NumElements = LAYER_SIZE / 4, .Flags = D3D11_BUFFEREX_SRV_FLAG_RAW},
	};

	if (cpu)
	{
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
		if (!mapdefault)
		{
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
		}
	}
	Check (ID3D11Device_CreateBuffer (device, &desc, NULL, &layer->buffer), "CreateBuffer");
	if (desc.BindFlags)
		Check (ID3D11Device_CreateShaderResourceView (device, (ID3D11Resource *)layer->buffer, &view, &layer->srv),
			"CreateShaderResourceView");
}

static void *Map (layer_t *layer)
{
	D3D11_MAPPED_SUBRESOURCE	mapped;

	Check (ID3D11DeviceContext_Map (context, (ID3D11Resource *)layer->buffer, 0, D3D11_MAP_READ_WRITE, 0, &mapped),
		"Map");
	return mapped.pData;
}

// warp: on WARP, the GPU's memory the CPU's
static void Setup (bool warp)
{
	static const D3D_FEATURE_LEVEL		levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D11_FEATURE_DATA_D3D11_OPTIONS1	options1 = {0};
	IDXGIDevice							*dxgidevice;
	IDXGIAdapter						*adapter;
	DXGI_ADAPTER_DESC					desc;

	if (warp || FAILED (D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2, D3D11_SDK_VERSION,
			&device, NULL, &context)))
		Check (D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 2, D3D11_SDK_VERSION, &device, NULL,
			&context), "D3D11CreateDevice");
	if (SUCCEEDED (ID3D11Device_QueryInterface (device, &IID_IDXGIDevice, (void **)&dxgidevice)))
	{
		if (SUCCEEDED (IDXGIDevice_GetAdapter (dxgidevice, &adapter)) && SUCCEEDED (IDXGIAdapter_GetDesc (adapter, &desc)))
			printf ("present: on %ls\n", desc.Description);
		IDXGIDevice_Release (dxgidevice);
	}
	if (SUCCEEDED (ID3D11Device_CheckFeatureSupport (device, D3D11_FEATURE_D3D11_OPTIONS1, &options1,
			sizeof (options1))))
		mapdefault = options1.MapOnDefaultBuffers != FALSE;

	Check (ID3D11Device_CreateVertexShader (device, g_present_vs, sizeof (g_present_vs), NULL, &vs),
		"CreateVertexShader");
	Check (ID3D11Device_CreatePixelShader (device, g_present_ps, sizeof (g_present_ps), NULL, &ps),
		"CreatePixelShader");
	D3D11_BUFFER_DESC cb = {.ByteWidth = sizeof (d3d_present_t), .Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER};
	Check (ID3D11Device_CreateBuffer (device, &cb, NULL, &constants), "CreateBuffer");

	// a float target, and where it is read back
	D3D11_TEXTURE2D_DESC td = {
		.Width = MAX_WIDTH,
		.Height = MAX_HEIGHT,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
		.SampleDesc = {.Count = 1},
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_RENDER_TARGET,
	};
	Check (ID3D11Device_CreateTexture2D (device, &td, NULL, &target), "CreateTexture2D");
	Check (ID3D11Device_CreateRenderTargetView (device, (ID3D11Resource *)target, NULL, &rtv),
		"CreateRenderTargetView");
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	Check (ID3D11Device_CreateTexture2D (device, &td, NULL, &readback), "CreateTexture2D");

	// the layers as vid_d3d11.c makes them, rows the frame's width apart
	CreateLayer (&framecpu, true);
	CreateLayer (&hudcpu, true);
	CreateLayer (&framegpu, false);
	CreateLayer (&hudgpu, false);
	frame = Map (&framecpu);
	hud = Map (&hudcpu);
	memset (frame, 0, LAYER_SIZE);
	memset (hud, 0, LAYER_SIZE);

	vid.width = WIDTH;
	vid.height = HEIGHT;
	vid.rowpixels = WIDTH;
}

// the frame as the shader shows it into width x height, one float RGBA a pixel:
// read where the CPU wrote it, or copied to the GPU's memory first
static void Draw (bool inplace, int output, float paperwhite, float peak, int width, int height, float *out)
{
	static const float			black[4] = {0, 0, 0, 1};
	vid_fit_t					fit = {0, 0, (float)width, (float)height, (float)width / WIDTH, 1};
	d3d_present_t				p = {0};
	ID3D11ShaderResourceView	*layers[2], *none[2] = {NULL, NULL};
	D3D11_MAPPED_SUBRESOURCE	mapped;
	D3D11_BOX					box = {0, 0, 0, (UINT)width, (UINT)height, 1};
	int							y;

	VID_FillConstants (&p.c, &fit, output, paperwhite, peak);
	p.rowpixels = vid.rowpixels;

	// the layers as VID_Update hands them over
	ID3D11DeviceContext_Unmap (context, (ID3D11Resource *)framecpu.buffer, 0);
	ID3D11DeviceContext_Unmap (context, (ID3D11Resource *)hudcpu.buffer, 0);
	if (inplace)
	{
		layers[0] = framecpu.srv;
		layers[1] = hudcpu.srv;
	}
	else
	{
		ID3D11DeviceContext_CopyResource (context, (ID3D11Resource *)framegpu.buffer, (ID3D11Resource *)framecpu.buffer);
		ID3D11DeviceContext_CopyResource (context, (ID3D11Resource *)hudgpu.buffer, (ID3D11Resource *)hudcpu.buffer);
		layers[0] = framegpu.srv;
		layers[1] = hudgpu.srv;
	}

	D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
	ID3D11DeviceContext_UpdateSubresource (context, (ID3D11Resource *)constants, 0, NULL, &p, 0, 0);
	ID3D11DeviceContext_ClearRenderTargetView (context, rtv, black);
	ID3D11DeviceContext_OMSetRenderTargets (context, 1, &rtv, NULL);
	ID3D11DeviceContext_RSSetViewports (context, 1, &viewport);
	ID3D11DeviceContext_IASetPrimitiveTopology (context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout (context, NULL);
	ID3D11DeviceContext_VSSetShader (context, vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader (context, ps, NULL, 0);
	ID3D11DeviceContext_PSSetShaderResources (context, 0, 2, layers);
	ID3D11DeviceContext_PSSetConstantBuffers (context, 0, 1, &constants);
	ID3D11DeviceContext_Draw (context, 3, 0);
	ID3D11DeviceContext_PSSetShaderResources (context, 0, 2, none);

	ID3D11DeviceContext_CopySubresourceRegion (context, (ID3D11Resource *)readback, 0, 0, 0, 0,
		(ID3D11Resource *)target, 0, &box);
	Check (ID3D11DeviceContext_Map (context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &mapped), "Map");
	for (y = 0 ; y < height ; y++)
		memcpy (out + (size_t)y * width * 4, (const byte *)mapped.pData + (size_t)y * mapped.RowPitch,
			(size_t)width * 16);
	ID3D11DeviceContext_Unmap (context, (ID3D11Resource *)readback, 0);

	// mapped again, as the renderer gets them back, what was written still there
	frame = Map (&framecpu);
	hud = Map (&hudcpu);
}

static uint32_t	rng = 0x2545F491;

static uint32_t Rand (void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// random light, often SDR white and brighter; and random 2D, often none or opaque
static void Fill (void)
{
	unsigned	x, y, a;

	for (y = 0 ; y < HEIGHT ; y++)
		for (x = 0 ; x < WIDTH ; x++)
		{
			frame[y * vid.rowpixels + x] = RGB30 (Rand () % 1024, Rand () % 1024, Rand () % 1024);
			switch (Rand () % 4)
			{
			case 0:		a = 0;					break;
			case 1:		a = 255;				break;
			default:	a = Rand () % 256;		break;
			}
			hud[y * vid.rowpixels + x] = HUD_RGBA (Rand () % (a + 1), Rand () % (a + 1), Rand () % (a + 1), a);
		}
	// the channel order: a red, a green and a blue pixel of SDR white
	frame[0] = RGB30 (RGB30_WHITE, 0, 0);
	frame[1] = RGB30 (0, RGB30_WHITE, 0);
	frame[2] = RGB30 (0, 0, RGB30_WHITE);
	hud[0] = hud[1] = hud[2] = 0;
}

static void TestSDR (bool inplace, const char *what)
{
	static float	out[WIDTH * HEIGHT * 4];
	static byte		rgb[WIDTH * HEIGHT * 3];
	int				i, c, d, worst = 0, off = 0;

	Draw (inplace, VID_OUTPUT_SDR, 1, 1, WIDTH, HEIGHT, out);
	VID_FrameToRGB (rgb, true);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			d = abs ((int)lroundf (out[i * 4 + c] * 255) - rgb[i * 3 + c]);
			if (d > worst)
				worst = d;
			if (d > 1 && off++ < 5)
				printf ("%s: pixel %d channel %d is %.1f, VID_FrameToRGB %d\n", what, i, c, out[i * 4 + c] * 255,
					rgb[i * 3 + c]);
		}
	if (off)
		failures++;
	printf ("%s: at most %d from VID_FrameToRGB, %d channels over 1\n", what, worst, off);
}

/*
================
TestSharp

Scaled by 2.5, sharp bilinear: a pixel whose center is inside a texel's
middle (the texel's all but the one pixel wide blend at its edges) is that
texel as drawn unscaled, and one on an edge is between the two texels. The
light is no brighter than SDR white and there is no 2D, so that each channel
shown only grows with the channel drawn.
================
*/
static void TestSharp (bool inplace)
{
	static float	one[WIDTH * HEIGHT * 4], scaled[MAX_WIDTH * MAX_HEIGHT * 4];
	int				x, y, c, tx, ty, off = 0, inside = 0, edges = 0;
	float			fx, fy, got, a, b, lo, hi;

	for (x = 0 ; x < WIDTH * HEIGHT ; x++)
		frame[x] = RGB30 (Rand () % 513, Rand () % 513, Rand () % 513);
	memset (hud, 0, LAYER_SIZE);

	Draw (inplace, VID_OUTPUT_SDR, 1, 1, WIDTH, HEIGHT, one);
	Draw (inplace, VID_OUTPUT_SDR, 1, 1, MAX_WIDTH, MAX_HEIGHT, scaled);
	for (y = 0 ; y < MAX_HEIGHT ; y++)
		for (x = 0 ; x < MAX_WIDTH ; x++)
		{
			fx = (x + 0.5f) * WIDTH / MAX_WIDTH;
			fy = (y + 0.5f) * HEIGHT / MAX_HEIGHT;
			tx = (int)fx;
			ty = (int)fy;
			// the blend is 1/2.5 texel wide around each edge: 0.3 of a texel on either side
			if (fabsf (fy - ty - 0.5f) > 0.25f)
				continue;
			for (c = 0 ; c < 3 ; c++)
			{
				got = scaled[(y * MAX_WIDTH + x) * 4 + c];
				a = one[(ty * WIDTH + tx) * 4 + c];
				if (fabsf (fx - tx - 0.5f) <= 0.25f)
				{
					inside += c == 0;
					if (fabsf (got - a) * 255 <= 1)
						continue;
				}
				else
				{
					// between this texel and the one across the nearer edge
					b = one[(ty * WIDTH + (fx - tx < 0.5f ? (tx ? tx - 1 : 0) : (tx < WIDTH - 1 ? tx + 1 : tx))) * 4 + c];
					lo = fminf (a, b) - 1 / 255.0f;
					hi = fmaxf (a, b) + 1 / 255.0f;
					edges += c == 0;
					if (got >= lo && got <= hi)
						continue;
				}
				if (off++ < 5)
					printf ("sharp: pixel %d,%d channel %d is %.1f, texel %d,%d %.1f\n", x, y, c, got * 255, tx, ty,
						a * 255);
			}
		}
	if (off)
		failures++;
	printf ("sharp bilinear: %d pixels inside texels, %d on edges, %d channels off\n", inside, edges, off);
}

static void Expect (const char *what, float got, float lo, float hi)
{
	if (got >= lo && got <= hi)
		return;
	printf ("%s: %f, not in %f .. %f\n", what, got, lo, hi);
	failures++;
}

static void TestHDR (bool inplace)
{
	static float	out[WIDTH * HEIGHT * 4];

	memset (hud, 0, LAYER_SIZE);
	frame[0] = RGB30 (RGB30_WHITE, RGB30_WHITE, RGB30_WHITE);
	frame[1] = RGB30 (1023, 1023, 1023);
	frame[2] = RGB30 (1023, 0, 0);
	frame[3] = RGB30 (0, 0, 0);
	hud[3] = HUD_RGBA (255, 255, 255, 255);

	// scRGB, paper white 2, peak 6: white is 2, the brightest light (15.9) is under 6, the 2D's white is 2
	Draw (inplace, VID_OUTPUT_LINEAR, 2, 6, WIDTH, HEIGHT, out);
	Expect ("HDR white", out[0], 1.99f, 2.01f);
	Expect ("HDR brightest", out[4], 4.5f, 6.0f);
	Expect ("HDR red keeps its hue: green", out[9], 0, 0.001f);
	Expect ("HDR 2D white", out[12], 1.99f, 2.01f);
}

// PQ's code to cd/m² (SMPTE ST 2084)
static double PQToNits (double e)
{
	const double	m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double	c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	double			p = pow (e, 1 / m2);

	return 10000 * pow (fmax (p - c1, 0) / (c2 - c3 * p), 1 / m1);
}

/*
================
TestShotHDR

The shader's scRGB, as light over paper white at 203 cd/m² in BT.2020, against
what VID_FrameToPQ (HDR screenshots) makes of the same frame, read back from PQ:
within half a percent, or 0.05 cd/m² under 10
================
*/
static void TestShotHDR (bool inplace, float paperwhite, float peak, const char *what)
{
	static const double	bt709to2020[3][3] = {
		{0.627404, 0.329283, 0.043313},
		{0.069097, 0.919541, 0.011362},
		{0.016391, 0.088013, 0.895595}};
	static float	out[WIDTH * HEIGHT * 4];
	static uint16_t	pq[WIDTH * HEIGHT * 3];
	const float		*o;
	double			want, got, d, worst = 0;
	int				i, c, off = 0;

	Draw (inplace, VID_OUTPUT_LINEAR, paperwhite, peak, WIDTH, HEIGHT, out);
	VID_FrameToPQ (pq);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			o = out + i * 4;
			want = 203 * (bt709to2020[c][0] * o[0] + bt709to2020[c][1] * o[1] + bt709to2020[c][2] * o[2]) / paperwhite;
			got = PQToNits (pq[i * 3 + c] / 65535.0);
			d = fabs (got - want) / fmax (want, 10);
			if (d > worst)
				worst = d;
			if (d > 0.005 && off++ < 5)
				printf ("%s: pixel %d channel %d is %.2f cd/m2, VID_FrameToPQ %.2f\n", what, i, c, want, got);
		}
	if (off)
		failures++;
	printf ("%s: at most %.3f %% from VID_FrameToPQ, %d channels over 0.5 %%\n", what, worst * 100, off);
}

static void TestAll (bool inplace)
{
	printf ("present: the layers %s\n", inplace ? "read where the CPU wrote them" : "copied to the GPU's memory");
	Fill ();
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestSDR (inplace, "SDR");
	TestSharp (inplace);

	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	TestSDR (inplace, "SDR, gamma 0.8 and a blend");

	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestHDR (inplace);

	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	TestShotHDR (inplace, 2.5f, 8, "HDR screenshot, rolled off, gamma 0.8 and a blend");
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestShotHDR (inplace, 2, 2, "HDR screenshot, white fitted under a low peak");
}

int main (int argc, char **argv)
{
	Setup (argc > 1 && !strcmp (argv[1], "-warp"));
	TestAll (false);
	if (mapdefault)
		TestAll (true);
	else
		printf ("present: the driver lets no shader read the CPU's layers; read where they are not tested\n");

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present: the shader shows what screenshots do\n");
	return 0;
}
