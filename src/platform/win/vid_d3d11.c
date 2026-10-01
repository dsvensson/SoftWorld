// vid_d3d11.c -- Windows video backend: the main window and a DXGI flip-model swapchain
// that presents the software renderer's framebuffer through Direct3D 11.
//
// The renderer draws into memory the GPU copies from, as vid_vulkan.c has it on a
// GPU of its own memory: each layer (the 3D view, R10G10B10A2 as vid.h's pixels
// are, and its 2D, RGBA8) is a staging texture, cached system memory the CPU
// reads and writes, mapped while it is drawn in. At present it is unmapped and the
// GPU copies it into the texture present.hlsl reads, which lays the layers into
// the letterboxed viewport (vid_common.c has the fit and the constants); the CPU
// copies nothing.
//
// The GPU copies a layer after VID_Update has returned, so there are two of each,
// as vid_vulkan.c and vid_metal.m have them: the view is drawn into them in turn,
// and the 2D goes to the one not shown when it is drawn again. Mapping a layer
// waits for the GPU's copy of it.
//
// The swapchain is SDR (B8G8R8A8) or, when the display the window is on runs in
// HDR mode, scRGB (linear R16G16B16A16_FLOAT). Which one is decided again when
// the window moves or the display settings change.

#include "args.h"
#include "cmd.h"
#include "mem.h"
#include "print.h"
#include "sys.h"
#include "client.h"
#include "keys.h"
#include "sound.h"
#include "vid_common.h"
#include "win_local.h"

#define COBJMACROS
#include <d3d11.h>
#include <math.h>
#include <dxgi1_6.h>

#include "present_vs.h"
#include "present_ps.h"

HWND		mainwindow;

#define VID_SLOTS		2		// frames in flight at most, and so view layers

// a layer the renderer draws in: a staging texture, mapped while it is drawn
// in or read, which the GPU copies into the texture the shader reads
typedef struct
{
	ID3D11Texture2D	*staging;
	void			*data;		// while mapped
	UINT			pitch;		// bytes from one row to the next
} vid_layer_t;

static IDXGIFactory2			*d3d_factory;
static ID3D11Device				*d3d_device;
static ID3D11DeviceContext		*d3d_context;
static IDXGISwapChain1			*d3d_swapchain;
static IDXGISwapChain2			*d3d_swapchain2;	// the same, for the frame latency
static UINT						d3d_latency;		// frames queued at most
static ID3D11RenderTargetView	*d3d_rtv;
static ID3D11Texture2D			*d3d_frame;			// the view the shader reads
static ID3D11ShaderResourceView	*d3d_frame_srv;
static ID3D11Texture2D			*d3d_hud;			// and the 2D, copied when it changes
static ID3D11ShaderResourceView	*d3d_hud_srv;
static ID3D11VertexShader		*d3d_vs;
static ID3D11PixelShader		*d3d_ps;
static ID3D11SamplerState		*d3d_sampler;
static ID3D11Buffer				*d3d_constants;
static HANDLE					d3d_waitable;		// signaled when a frame may be queued
static char						d3d_gpuname[128];	// the adapter's name
static bool						d3d_allow_tearing;
static UINT						d3d_swapflags;

static vid_layer_t	vid_views[VID_SLOTS];	// the 3D view
static vid_layer_t	vid_huds[2];			// the 2D
static int			vid_slot;				// vid.buffer's view
static int			vid_hudshown;			// the 2D shown; vid.hud is the other
static int			vid_drawnslot;			// the view last drawn, presented or not
static int			vid_drawnhud;			// and its 2D

static bool		vid_initialized;
static bool		vid_fullscreen;
static WINDOWPLACEMENT	vid_windowed_placement = {.length = sizeof (WINDOWPLACEMENT)};
static int		client_width, client_height;

static bool		vid_outputdirty = true;	// recheck the display: moved, or settings changed
static bool		vid_hdroutput;			// the swapchain is scRGB
static bool		vid_outputknown;		// the output mode has been reported
static float	vid_hdrwanted = -1;		// vid_hdr when the display was last checked
static float	vid_peaknits = 1000;	// the display's brightest white
static float	vid_sdrwhitenits = 200;	// Windows' SDR content brightness on that display

static LRESULT CALLBACK MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static void VID_SetScale (void);

// SDR white on an HDR display: vid_hdr_paperwhite, or else what Windows uses
static float VID_PaperWhiteNits (void)
{
	return vid_hdr_paperwhite.value > 0 ? vid_hdr_paperwhite.value : vid_sdrwhitenits;
}

static void VID_CheckHR (HRESULT hr, const char *what)
{
	if (FAILED (hr))
		Sys_Error ("%s failed (HRESULT 0x%08lx)", what, (unsigned long)hr);
}

#define VID_RELEASE(obj) do { if (obj) { IUnknown_Release ((IUnknown *)(obj)); (obj) = NULL; } } while (0)

/*
===============================================================================

SWAPCHAIN

===============================================================================
*/

static void VID_CreateBackbufferView (void)
{
	ID3D11Texture2D	*backbuffer;

	VID_CheckHR (IDXGISwapChain1_GetBuffer (d3d_swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer),
		"IDXGISwapChain1::GetBuffer");
	VID_CheckHR (ID3D11Device_CreateRenderTargetView (d3d_device, (ID3D11Resource *)backbuffer, NULL, &d3d_rtv),
		"ID3D11Device::CreateRenderTargetView");
	ID3D11Texture2D_Release (backbuffer);
}

/*
================
VID_ResizeSwapchain

Resizes the buffers to the window, and changes their format if format isn't
DXGI_FORMAT_UNKNOWN.
================
*/
static void VID_ResizeSwapchain (int width, int height, DXGI_FORMAT format)
{
	if (!d3d_swapchain || width <= 0 || height <= 0)
		return;
	if (width == client_width && height == client_height && format == DXGI_FORMAT_UNKNOWN)
		return;

	client_width = width;
	client_height = height;

	ID3D11DeviceContext_OMSetRenderTargets (d3d_context, 0, NULL, NULL);
	VID_RELEASE (d3d_rtv);
	ID3D11DeviceContext_Flush (d3d_context);
	VID_CheckHR (IDXGISwapChain1_ResizeBuffers (d3d_swapchain, 0, 0, 0, format, d3d_swapflags),
		"IDXGISwapChain1::ResizeBuffers");
	VID_CreateBackbufferView ();
}

/*
================
VID_WindowOutput

The display that shows most of the window
================
*/
static IDXGIOutput6 *VID_WindowOutput (void)
{
	IDXGIAdapter1	*adapter;
	IDXGIOutput		*output;
	IDXGIOutput6	*best = NULL;
	DXGI_OUTPUT_DESC	desc;
	RECT			window;
	LONG			area, bestarea = -1, w, h;
	UINT			a, o;

	GetWindowRect (mainwindow, &window);
	for (a=0 ; IDXGIFactory2_EnumAdapters1 (d3d_factory, a, &adapter) != DXGI_ERROR_NOT_FOUND ; a++)
	{
		for (o=0 ; IDXGIAdapter1_EnumOutputs (adapter, o, &output) != DXGI_ERROR_NOT_FOUND ; o++)
		{
			IDXGIOutput_GetDesc (output, &desc);
			w = min (window.right, desc.DesktopCoordinates.right) - max (window.left, desc.DesktopCoordinates.left);
			h = min (window.bottom, desc.DesktopCoordinates.bottom) - max (window.top, desc.DesktopCoordinates.top);
			area = (w > 0 && h > 0) ? w * h : 0;
			if (area > bestarea)
			{
				VID_RELEASE (best);
				if (SUCCEEDED (IDXGIOutput_QueryInterface (output, &IID_IDXGIOutput6, (void **)&best)))
					bestarea = area;
			}
			IDXGIOutput_Release (output);
		}
		IDXGIAdapter1_Release (adapter);
	}
	return best;
}

/*
================
VID_SDRWhiteNits

The brightness Windows gives SDR content on the display named device (a GDI
device name, as in DXGI_OUTPUT_DESC1), or 0 if it isn't known
================
*/
static float VID_SDRWhiteNits (const WCHAR *device)
{
	DISPLAYCONFIG_PATH_INFO	*paths;
	DISPLAYCONFIG_MODE_INFO	*modes;
	UINT32					numpaths, nummodes, i;
	float					nits = 0;

	if (GetDisplayConfigBufferSizes (QDC_ONLY_ACTIVE_PATHS, &numpaths, &nummodes) != ERROR_SUCCESS)
		return 0;
	paths = Mem_Alloc (numpaths * sizeof(*paths));
	modes = Mem_Alloc (nummodes * sizeof(*modes));
	if (QueryDisplayConfig (QDC_ONLY_ACTIVE_PATHS, &numpaths, paths, &nummodes, modes, NULL) == ERROR_SUCCESS)
	{
		for (i=0 ; i<numpaths ; i++)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {
				.header = {
					.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
					.size = sizeof(source),
					.adapterId = paths[i].sourceInfo.adapterId,
					.id = paths[i].sourceInfo.id,
				},
			};
			DISPLAYCONFIG_SDR_WHITE_LEVEL white = {
				.header = {
					.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
					.size = sizeof(white),
					.adapterId = paths[i].targetInfo.adapterId,
					.id = paths[i].targetInfo.id,
				},
			};

			if (DisplayConfigGetDeviceInfo (&source.header) != ERROR_SUCCESS || wcscmp (source.viewGdiDeviceName, device))
				continue;
			if (DisplayConfigGetDeviceInfo (&white.header) == ERROR_SUCCESS)
				nits = white.SDRWhiteLevel / 1000.0f * 80.0f;		// 1000 is 80 nits
			break;
		}
	}
	Mem_Free (paths);
	Mem_Free (modes);
	return nits;
}

/*
================
VID_CheckOutput

Switches between SDR and scRGB output to match the display the window is on
================
*/
static void VID_CheckOutput (void)
{
	IDXGIOutput6		*output;
	IDXGISwapChain3		*swapchain3;
	DXGI_OUTPUT_DESC1	desc;
	UINT				support = 0;
	bool				displayhdr = false, hdr, wasknown;
	const char			*why = NULL;

	vid_outputdirty = false;
	vid_hdrwanted = vid_hdr.value;

	// the factory goes stale when displays or their HDR mode change
	if (!IDXGIFactory2_IsCurrent (d3d_factory))
	{
		VID_RELEASE (d3d_factory);
		VID_CheckHR (CreateDXGIFactory2 (0, &IID_IDXGIFactory2, (void **)&d3d_factory), "CreateDXGIFactory2");
	}

	output = VID_WindowOutput ();
	if (output)
	{
		if (SUCCEEDED (IDXGIOutput6_GetDesc1 (output, &desc)))
		{
			displayhdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
			if (desc.MaxLuminance > 0)
				vid_peaknits = desc.MaxLuminance;
			vid_sdrwhitenits = VID_SDRWhiteNits (desc.DeviceName);
			if (vid_sdrwhitenits <= 0)
				vid_sdrwhitenits = 200;
		}
		IDXGIOutput6_Release (output);
	}
	else
		why = "the display wasn't found";

	hdr = displayhdr && vid_hdr.value;
	if (displayhdr && !vid_hdr.value)
		why = "vid_hdr is 0";

	if (FAILED (IDXGISwapChain1_QueryInterface (d3d_swapchain, &IID_IDXGISwapChain3, (void **)&swapchain3)))
		return;

	// the color space a swapchain can present depends on its format, so switch that first
	if (hdr != vid_hdroutput)
		VID_ResizeSwapchain (client_width, client_height, hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM);
	if (hdr && (FAILED (IDXGISwapChain3_CheckColorSpaceSupport (swapchain3, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support))
		|| !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
		|| FAILED (IDXGISwapChain3_SetColorSpace1 (swapchain3, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))))
	{
		hdr = false;
		why = "scRGB can't be presented";
		VID_ResizeSwapchain (client_width, client_height, DXGI_FORMAT_B8G8R8A8_UNORM);
	}
	if (!hdr)
		IDXGISwapChain3_SetColorSpace1 (swapchain3, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	IDXGISwapChain3_Release (swapchain3);

	wasknown = vid_outputknown;
	vid_outputknown = true;
	if (wasknown && hdr == vid_hdroutput)
		return;
	vid_hdroutput = hdr;
	if (hdr)
		Con_Printf ("HDR output: SDR white at %.0f nits%s, %.0f nits peak\n", VID_PaperWhiteNits (),
			vid_hdr_paperwhite.value > 0 ? "" : " (Windows setting)", vid_peaknits);
	else if (why)
		Con_Printf ("SDR output: %s\n", why);
	else
		Con_Printf ("SDR output\n");
}

// the adapter the device is on, by name
static void VID_GetGPUName (void)
{
	IDXGIDevice			*device;
	IDXGIAdapter		*adapter;
	DXGI_ADAPTER_DESC	desc;

	if (FAILED (ID3D11Device_QueryInterface (d3d_device, &IID_IDXGIDevice, (void **)&device)))
		return;
	if (SUCCEEDED (IDXGIDevice_GetAdapter (device, &adapter)))
	{
		if (SUCCEEDED (IDXGIAdapter_GetDesc (adapter, &desc))
			&& !WideCharToMultiByte (CP_UTF8, 0, desc.Description, -1, d3d_gpuname, (int)sizeof(d3d_gpuname),
				NULL, NULL))
			d3d_gpuname[0] = 0;
		IDXGIAdapter_Release (adapter);
	}
	IDXGIDevice_Release (device);
}

/*
================
VID_CreateDevice
================
*/
static void VID_CreateDevice (void)
{
	static const D3D_FEATURE_LEVEL	levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	IDXGIFactory5		*factory5;
	HRESULT				hr;
	RECT				client;
	BOOL				tearing = FALSE;

	hr = D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, ARRAYSIZE (levels),
		D3D11_SDK_VERSION, &d3d_device, NULL, &d3d_context);
	if (FAILED (hr))
	{
		Con_Printf ("No usable Direct3D 11 hardware device, falling back to WARP\n");
		VID_CheckHR (D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, ARRAYSIZE (levels),
			D3D11_SDK_VERSION, &d3d_device, NULL, &d3d_context), "D3D11CreateDevice");
	}

	VID_GetGPUName ();
	VID_CheckHR (CreateDXGIFactory2 (0, &IID_IDXGIFactory2, (void **)&d3d_factory), "CreateDXGIFactory2");

	if (SUCCEEDED (IDXGIFactory2_QueryInterface (d3d_factory, &IID_IDXGIFactory5, (void **)&factory5)))
	{
		if (FAILED (IDXGIFactory5_CheckFeatureSupport (factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
				&tearing, sizeof (tearing))))
			tearing = FALSE;
		IDXGIFactory5_Release (factory5);
	}
	d3d_allow_tearing = tearing != FALSE;
	d3d_swapflags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
		| (d3d_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

	GetClientRect (mainwindow, &client);
	client_width = client.right;
	client_height = client.bottom;

	DXGI_SWAP_CHAIN_DESC1 desc = {
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = {.Count = 1},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = 2,
		.Scaling = DXGI_SCALING_NONE,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
		.Flags = d3d_swapflags,
	};
	VID_CheckHR (IDXGIFactory2_CreateSwapChainForHwnd (d3d_factory, (IUnknown *)d3d_device, mainwindow, &desc,
		NULL, NULL, &d3d_swapchain), "IDXGIFactory2::CreateSwapChainForHwnd");

	// fullscreen is a borderless window toggled by us, never DXGI exclusive mode
	IDXGIFactory2_MakeWindowAssociation (d3d_factory, mainwindow, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

	// queue few frames (VID_Update), and wait for room before drawing the next
	VID_CheckHR (IDXGISwapChain1_QueryInterface (d3d_swapchain, &IID_IDXGISwapChain2, (void **)&d3d_swapchain2),
		"IDXGISwapChain2");
	d3d_latency = 1;
	IDXGISwapChain2_SetMaximumFrameLatency (d3d_swapchain2, d3d_latency);
	d3d_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject (d3d_swapchain2);

	VID_CreateBackbufferView ();

	VID_CheckHR (ID3D11Device_CreateVertexShader (d3d_device, g_present_vs, sizeof (g_present_vs), NULL, &d3d_vs),
		"ID3D11Device::CreateVertexShader");
	VID_CheckHR (ID3D11Device_CreatePixelShader (d3d_device, g_present_ps, sizeof (g_present_ps), NULL, &d3d_ps),
		"ID3D11Device::CreatePixelShader");

	D3D11_SAMPLER_DESC sampler = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MaxLOD = D3D11_FLOAT32_MAX,
	};
	VID_CheckHR (ID3D11Device_CreateSamplerState (d3d_device, &sampler, &d3d_sampler),
		"ID3D11Device::CreateSamplerState");

	D3D11_BUFFER_DESC constants = {
		.ByteWidth = sizeof (vid_present_constants_t),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};
	VID_CheckHR (ID3D11Device_CreateBuffer (d3d_device, &constants, NULL, &d3d_constants),
		"ID3D11Device::CreateBuffer");
}

/*
================
VID_MapLayer

The layer for the CPU to draw in or read, once the GPU has copied it
================
*/
static void *VID_MapLayer (vid_layer_t *layer)
{
	D3D11_MAPPED_SUBRESOURCE	mapped;

	if (!layer->data)
	{
		VID_CheckHR (ID3D11DeviceContext_Map (d3d_context, (ID3D11Resource *)layer->staging, 0, D3D11_MAP_READ_WRITE,
			0, &mapped), "ID3D11DeviceContext::Map");
		layer->data = mapped.pData;
		layer->pitch = mapped.RowPitch;
	}
	return layer->data;
}

// the layer into the texture the shader reads, copied by the GPU
static void VID_CopyLayer (vid_layer_t *layer, ID3D11Texture2D *texture)
{
	if (layer->data)
	{
		ID3D11DeviceContext_Unmap (d3d_context, (ID3D11Resource *)layer->staging, 0);
		layer->data = NULL;
	}
	ID3D11DeviceContext_CopyResource (d3d_context, (ID3D11Resource *)texture, (ID3D11Resource *)layer->staging);
}

static void VID_FreeLayer (vid_layer_t *layer)
{
	if (layer->data)
		ID3D11DeviceContext_Unmap (d3d_context, (ID3D11Resource *)layer->staging, 0);
	VID_RELEASE (layer->staging);
	layer->data = NULL;
}

static void VID_FreeLayers (void)
{
	int		i;

	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_FreeLayer (&vid_views[i]);
	for (i = 0 ; i < 2 ; i++)
		VID_FreeLayer (&vid_huds[i]);
	VID_RELEASE (d3d_frame_srv);
	VID_RELEASE (d3d_frame);
	VID_RELEASE (d3d_hud_srv);
	VID_RELEASE (d3d_hud);
}

/*
================
VID_CreateLayers

The layers of a frame size: the staging textures the renderer draws in, and
the textures the shader reads, which the GPU copies them into
================
*/
static void VID_CreateLayers (int width, int height)
{
	int		i;

	VID_FreeLayers ();

	D3D11_TEXTURE2D_DESC desc = {
		.Width = (UINT)width,
		.Height = (UINT)height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R10G10B10A2_UNORM,
		.SampleDesc = {.Count = 1},
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE,
	};
	VID_CheckHR (ID3D11Device_CreateTexture2D (d3d_device, &desc, NULL, &d3d_frame), "ID3D11Device::CreateTexture2D");
	VID_CheckHR (ID3D11Device_CreateShaderResourceView (d3d_device, (ID3D11Resource *)d3d_frame, NULL,
		&d3d_frame_srv), "ID3D11Device::CreateShaderResourceView");
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	VID_CheckHR (ID3D11Device_CreateTexture2D (d3d_device, &desc, NULL, &d3d_hud), "ID3D11Device::CreateTexture2D");
	VID_CheckHR (ID3D11Device_CreateShaderResourceView (d3d_device, (ID3D11Resource *)d3d_hud, NULL,
		&d3d_hud_srv), "ID3D11Device::CreateShaderResourceView");

	// read as well as written, so the driver keeps them in memory the CPU caches:
	// the renderer reads what it drew (blending, the underwater warp, screenshots)
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
	desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_CheckHR (ID3D11Device_CreateTexture2D (d3d_device, &desc, NULL, &vid_views[i].staging),
			"ID3D11Device::CreateTexture2D");
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	for (i = 0 ; i < 2 ; i++)
		VID_CheckHR (ID3D11Device_CreateTexture2D (d3d_device, &desc, NULL, &vid_huds[i].staging),
			"ID3D11Device::CreateTexture2D");
}

/*
===============================================================================

WINDOW

===============================================================================
*/

/*
================
VID_SetFullscreen

Fullscreen is a borderless window covering the monitor the window is on.
================
*/
static void VID_SetFullscreen (bool fullscreen)
{
	if (fullscreen == vid_fullscreen)
		return;

	if (fullscreen)
	{
		MONITORINFO	mi = {.cbSize = sizeof (mi)};

		GetWindowPlacement (mainwindow, &vid_windowed_placement);
		GetMonitorInfo (MonitorFromWindow (mainwindow, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLongPtr (mainwindow, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos (mainwindow, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	}
	else
	{
		SetWindowLongPtr (mainwindow, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPlacement (mainwindow, &vid_windowed_placement);
		SetWindowPos (mainwindow, NULL, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
	}

	vid_fullscreen = fullscreen;
	vid_outputdirty = true;
	IN_WindowChanged ();
}

static void VID_Fullscreen_f (void)
{
	VID_SetFullscreen (!vid_fullscreen);
}

/*
================
VID_CreateWindow
================
*/
static void VID_CreateWindow (int width, int height)
{
	WNDCLASSEX	wc = {
		.cbSize = sizeof (wc),
		.style = CS_OWNDC,
		.lpfnWndProc = MainWndProc,
		.hInstance = global_hInstance,
		.hIcon = LoadIcon (global_hInstance, MAKEINTRESOURCE (1)),
		.hCursor = LoadCursor (NULL, IDC_ARROW),
		.lpszClassName = "SoftWorld",
	};
	RECT		rect, work;
	int			scale;
	DWORD		style = WS_OVERLAPPEDWINDOW;

	if (!RegisterClassEx (&wc))
		Sys_Error ("Couldn't register window class");

	// the largest integer multiple of the render size that fits the desktop work area
	SystemParametersInfo (SPI_GETWORKAREA, 0, &work, 0);
	for (scale = 4 ; scale > 1 ; scale--)
	{
		rect = (RECT){0, 0, width * scale, height * scale};
		AdjustWindowRectEx (&rect, style, FALSE, 0);
		if (rect.right - rect.left <= work.right - work.left && rect.bottom - rect.top <= work.bottom - work.top)
			break;
	}
	rect = (RECT){0, 0, width * scale, height * scale};
	AdjustWindowRectEx (&rect, style, FALSE, 0);

	mainwindow = CreateWindowEx (0, "SoftWorld", "SoftWorld", style, CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, global_hInstance, NULL);
	if (!mainwindow)
		Sys_Error ("Couldn't create the main window");
}

/*
===============================================================================

VIDEO CONTRACT

===============================================================================
*/

void VID_Init (void)
{
	int		scale;

	scale = VID_RegisterCommon ();
	Cmd_AddCommand ("vid_fullscreen", VID_Fullscreen_f,
		"Toggles fullscreen, a borderless window covering the monitor, as Alt+Enter does.");

	VID_CreateWindow (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
	VID_CreateDevice ();
	VID_SetScale ();

	ShowWindow (mainwindow, SW_SHOWDEFAULT);
	UpdateWindow (mainwindow);
	SetForegroundWindow (mainwindow);
	IN_WindowChanged ();

	if (COM_CheckParm ("-fullscreen"))
		VID_SetFullscreen (true);

	vid_initialized = true;
}

void VID_Shutdown (void)
{
	if (!vid_initialized)
		return;

	vid_initialized = false;

	if (d3d_context)
		ID3D11DeviceContext_ClearState (d3d_context);
	if (d3d_waitable)
		CloseHandle (d3d_waitable);
	d3d_waitable = NULL;
	VID_RELEASE (d3d_constants);
	VID_RELEASE (d3d_sampler);
	VID_RELEASE (d3d_ps);
	VID_RELEASE (d3d_vs);
	VID_FreeLayers ();
	vid.buffer = NULL;
	vid.hud = NULL;
	VID_RELEASE (d3d_rtv);
	VID_RELEASE (d3d_swapchain2);
	VID_RELEASE (d3d_swapchain);
	VID_RELEASE (d3d_context);
	VID_RELEASE (d3d_device);
	VID_RELEASE (d3d_factory);

	if (mainwindow)
		DestroyWindow (mainwindow);
	mainwindow = NULL;
}

// the frame last drawn, whether it was presented or not (screenshots, dumps)
void VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud)
{
	*frame = VID_MapLayer (&vid_views[vid_drawnslot]);
	*hud = VID_MapLayer (&vid_huds[vid_drawnhud]);
}

/*
================
VID_SetScale

A new frame for the window; the next frame is drawn at its size
================
*/
static void VID_SetScale (void)
{
	vid_frame_t	frame = VID_WantedFrame (client_width, client_height);
	int			i;

	VID_CreateLayers (frame.width, frame.height);
	for (i = 0 ; i < VID_SLOTS ; i++)
		memset (VID_MapLayer (&vid_views[i]), 0, (size_t)vid_views[i].pitch * (size_t)frame.height);
	for (i = 0 ; i < 2 ; i++)
		memset (VID_MapLayer (&vid_huds[i]), 0, (size_t)vid_huds[i].pitch * (size_t)frame.height);
	vid_slot = vid_drawnslot = 0;
	vid_hudshown = vid_drawnhud = 0;
	vid.buffer = vid_views[vid_slot].data;
	vid.hud = vid_huds[vid_hudshown ^ 1].data;

	// both layers are 32 bits a pixel, and the renderer takes one pitch for both
	if (vid_views[vid_slot].pitch != vid_huds[vid_hudshown ^ 1].pitch || vid_views[vid_slot].pitch % sizeof(pixel_t))
		Sys_Error ("The frame's layers are laid out differently (%u and %u bytes a row)",
			vid_views[vid_slot].pitch, vid_huds[vid_hudshown ^ 1].pitch);
	VID_SetFrame (&frame, vid_views[vid_slot].pitch / (unsigned)sizeof(pixel_t));
}

/*
================
VID_Update

Presents the frame the renderer drew, letterboxed into the window, and hands
it the layers of the next once the GPU has copied what they held
================
*/
void VID_Update (void)
{
	vid_present_constants_t		constants;
	vid_fit_t					fit;
	UINT						flags = 0, interval, latency;
	int							hud;

	if (!vid_initialized || Minimized)
		return;

	if (vid_outputdirty || vid_hdr.value != vid_hdrwanted || !IDXGIFactory2_IsCurrent (d3d_factory))
		VID_CheckOutput ();

	// frames in flight: one with vsync, where the display sets the pace and a
	// second would be a refresh more of input lag; two without, so a frame is
	// drawn while the last is presented (at 1300 fps a sixth more frames, for
	// under a millisecond)
	interval = VID_Vsync () ? 1 : 0;
	latency = interval ? 1 : 2;
	if (latency != d3d_latency)
	{
		IDXGISwapChain2_SetMaximumFrameLatency (d3d_swapchain2, latency);
		d3d_latency = latency;
	}

	// room for this frame in the queue
	WaitForSingleObjectEx (d3d_waitable, 100, TRUE);

	// the frame drawn, and the 2D if it was drawn again, to the textures the shader reads
	hud = vid.huddirty ? vid_hudshown ^ 1 : vid_hudshown;
	VID_CopyLayer (&vid_views[vid_slot], d3d_frame);
	if (vid.huddirty)
		VID_CopyLayer (&vid_huds[hud], d3d_hud);
	vid_drawnslot = vid_slot;
	vid_drawnhud = vid_hudshown = hud;
	vid.huddirty = false;

	fit = VID_Fit (client_width, client_height);
	D3D11_VIEWPORT viewport = {
		.TopLeftX = fit.x,
		.TopLeftY = fit.y,
		.Width = fit.width,
		.Height = fit.height,
		.MaxDepth = 1.0f,
	};

	// scRGB 1.0 is 80 nits
	VID_FillConstants (&constants, &fit, vid_hdroutput ? VID_OUTPUT_LINEAR : VID_OUTPUT_SDR,
		VID_PaperWhiteNits () / 80.0f, fmaxf (vid_peaknits, VID_PaperWhiteNits ()) / 80.0f);
	ID3D11DeviceContext_UpdateSubresource (d3d_context, (ID3D11Resource *)d3d_constants, 0, NULL, &constants, 0, 0);

	static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	ID3D11DeviceContext_ClearRenderTargetView (d3d_context, d3d_rtv, black);
	ID3D11DeviceContext_OMSetRenderTargets (d3d_context, 1, &d3d_rtv, NULL);
	ID3D11DeviceContext_RSSetViewports (d3d_context, 1, &viewport);
	ID3D11DeviceContext_IASetPrimitiveTopology (d3d_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout (d3d_context, NULL);
	ID3D11DeviceContext_VSSetShader (d3d_context, d3d_vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader (d3d_context, d3d_ps, NULL, 0);
	ID3D11ShaderResourceView	*layers[2] = {d3d_frame_srv, d3d_hud_srv};
	ID3D11DeviceContext_PSSetShaderResources (d3d_context, 0, 2, layers);
	ID3D11DeviceContext_PSSetSamplers (d3d_context, 0, 1, &d3d_sampler);
	ID3D11DeviceContext_PSSetConstantBuffers (d3d_context, 0, 1, &d3d_constants);
	ID3D11DeviceContext_Draw (d3d_context, 3, 0);

	if (!interval && d3d_allow_tearing)
		flags |= DXGI_PRESENT_ALLOW_TEARING;
	IDXGISwapChain1_Present (d3d_swapchain, interval, flags);

	// the next frame is drawn in the other view, and a new 2D in the one not
	// shown, once the GPU has copied what they held
	vid_slot ^= 1;
	vid.buffer = VID_MapLayer (&vid_views[vid_slot]);
	vid.hud = VID_MapLayer (&vid_huds[vid_hudshown ^ 1]);

	// the window was resized, or the video settings changed
	if (VID_NeedsResize (client_width, client_height))
		VID_SetScale ();
}

/*
===============================================================================

WINDOW MESSAGES

===============================================================================
*/

/*
================
AppActivate
================
*/
static void AppActivate (bool active, bool minimize)
{
	static bool	sound_active;		// sound starts blocked (sys_win_gui.c), until the window is active

	ActiveApp = active;
	Minimized = minimize;

// enable/disable sound on focus gain/loss
	if (!ActiveApp && sound_active)
	{
		S_BlockSound ();
		S_ClearBuffer ();
		sound_active = false;
	}
	else if (ActiveApp && !sound_active)
	{
		S_UnblockSound ();
		S_ClearBuffer ();
		sound_active = true;
	}

	IN_WindowActivated (ActiveApp);
}

/*
================
MainWndProc
================
*/
static LRESULT CALLBACK MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	RECT	*suggested;

	switch (uMsg)
	{
	case WM_SYSCOMMAND:
		switch (wParam & ~0x0F)
		{
		case SC_KEYMENU:		// don't open the window menu on Alt
			return 0;
		case SC_SCREENSAVE:
		case SC_MONITORPOWER:
			if (vid_fullscreen)
				return 0;
			break;
		default:
			break;
		}
		break;

	case WM_MOVE:
		vid_outputdirty = true;		// maybe onto another display
		IN_WindowChanged ();
		return 0;

	case WM_DISPLAYCHANGE:
		vid_outputdirty = true;
		break;

	case WM_DPICHANGED:
		suggested = (RECT *)lParam;
		if (!vid_fullscreen)
			SetWindowPos (hWnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
				suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;

	case WM_SIZE:
		Minimized = wParam == SIZE_MINIMIZED;
		if (!Minimized)
		{
			VID_ResizeSwapchain (LOWORD (lParam), HIWORD (lParam), DXGI_FORMAT_UNKNOWN);
			IN_WindowChanged ();
		}
		return 0;

	case WM_ACTIVATE:
		AppActivate (LOWORD (wParam) != WA_INACTIVE, HIWORD (wParam) != 0);
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wParam == VK_RETURN && (lParam & (1 << 29)))
		{
			// Alt+Enter toggles borderless fullscreen
			if (!(lParam & (1 << 30)))
				VID_SetFullscreen (!vid_fullscreen);
			return 0;
		}
		break;

	case WM_CLOSE:
		// quit from the main loop, not from inside the window procedure
		Cbuf_AddText ("quit\n");
		return 0;

	default:
		break;
	}

	if (IN_HandleMessage (uMsg, wParam, lParam))
		return 0;
	return DefWindowProc (hWnd, uMsg, wParam, lParam);
}

void VID_SetCaption (const char *text)
{
	SetWindowTextA (mainwindow, text);
}

void VID_BringToFront (void)
{
	ShowWindow (mainwindow, SW_RESTORE);
	SetForegroundWindow (mainwindow);
}

/*
================
VID_IsActive / VID_IsMinimized
================
*/
bool VID_IsActive (void)
{
	return ActiveApp;
}

bool VID_IsMinimized (void)
{
	return Minimized;
}

bool VID_IsFullscreen (void)
{
	return vid_fullscreen;
}

const char *VID_GPUName (void)
{
	return d3d_gpuname;
}
