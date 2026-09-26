// vid_d3d11.c -- Windows video backend: the main window and a DXGI flip-model swapchain
// that presents the software renderer's framebuffer through Direct3D 11.
//
// The frame is uploaded as R10G10B10A2, with SDR white at 255, and drawn by
// present.hlsl into the letterboxed viewport. The swapchain is SDR (B8G8R8A8) or, when
// the display the window is on runs in HDR mode, scRGB (linear R16G16B16A16_FLOAT).
// Which one is decided again when the window moves or the display settings change.

#include "args.h"
#include "cmd.h"
#include "cvar.h"
#include "mem.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"
#include "client.h"
#include "keys.h"
#include "render.h"
#include "sound.h"
#include "vid.h"
#include "win_local.h"

#define COBJMACROS
#include <d3d11.h>
#include <math.h>
#include <dxgi1_6.h>

#include "present_vs.h"
#include "present_ps.h"

viddef_t	vid;				// global video state

HWND		mainwindow;

static cvar_t	vid_vsync = {.name = "vid_vsync", .string = "1", .archive = true};
// render pixels per pixel of the 320x200 layout; 0 is the most the window holds
static cvar_t	vid_scale = {.name = "vid_scale", .string = "0", .archive = true};
// 0: whole multiples of the render size, letterboxed; 1: fill the window, sharp bilinear
static cvar_t	vid_scalemode = {.name = "vid_scalemode", .string = "0", .archive = true};
static cvar_t	vid_contrast = {.name = "vid_contrast", .string = "1", .archive = true};
// use HDR output when the display is in HDR mode
static cvar_t	vid_hdr = {.name = "vid_hdr", .string = "1", .archive = true};
// brightness of SDR white on an HDR display, in nits; 0 uses Windows' SDR content brightness
static cvar_t	vid_hdr_paperwhite = {.name = "vid_hdr_paperwhite", .string = "0", .archive = true};

#define VID_BASE_WIDTH	320
#define VID_BASE_HEIGHT	200
#define VID_MAX_SCALE	16

// the constant buffer of present.hlsl
typedef struct
{
	float	blend[4];
	float	texsize[2];
	float	scale[2];
	float	gamma;
	float	contrast;
	float	sharp;
	float	hdr;
	float	paperwhite;
	float	peak;
	float	pad[2];
} present_constants_t;
static_assert (sizeof(present_constants_t) % 16 == 0, "constant buffers are whole float4s");

static IDXGIFactory2			*d3d_factory;
static ID3D11Device				*d3d_device;
static ID3D11DeviceContext		*d3d_context;
static IDXGISwapChain1			*d3d_swapchain;
static ID3D11RenderTargetView	*d3d_rtv;
static ID3D11Texture2D			*d3d_frame;
static ID3D11ShaderResourceView	*d3d_frame_srv;
static ID3D11VertexShader		*d3d_vs;
static ID3D11PixelShader		*d3d_ps;
static ID3D11SamplerState		*d3d_sampler;
static ID3D11Buffer				*d3d_constants;
static HANDLE					d3d_waitable;		// signaled when a frame may be queued
static bool						d3d_allow_tearing;
static UINT						d3d_swapflags;

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
static vid_present_t	vid_present = {.gamma = 1, .contrast = 1};

static LRESULT CALLBACK MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static int VID_WantedScale (void);
static void VID_SetScale (int scale);

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

/*
================
VID_CreateDevice
================
*/
static void VID_CreateDevice (void)
{
	static const D3D_FEATURE_LEVEL	levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	IDXGIFactory5		*factory5;
	IDXGISwapChain2		*swapchain2;
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

	// queue at most one frame, and wait for room before drawing the next
	VID_CheckHR (IDXGISwapChain1_QueryInterface (d3d_swapchain, &IID_IDXGISwapChain2, (void **)&swapchain2),
		"IDXGISwapChain2");
	IDXGISwapChain2_SetMaximumFrameLatency (swapchain2, 1);
	d3d_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject (swapchain2);
	IDXGISwapChain2_Release (swapchain2);

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
		.ByteWidth = sizeof (present_constants_t),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};
	VID_CheckHR (ID3D11Device_CreateBuffer (d3d_device, &constants, NULL, &d3d_constants),
		"ID3D11Device::CreateBuffer");
}

/*
================
VID_CreateFrameTexture

The texture the software framebuffer is uploaded into every frame.
================
*/
static void VID_CreateFrameTexture (int width, int height)
{
	VID_RELEASE (d3d_frame_srv);
	VID_RELEASE (d3d_frame);

	D3D11_TEXTURE2D_DESC desc = {
		.Width = (UINT)width,
		.Height = (UINT)height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R10G10B10A2_UNORM,
		.SampleDesc = {.Count = 1},
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
	};
	VID_CheckHR (ID3D11Device_CreateTexture2D (d3d_device, &desc, NULL, &d3d_frame),
		"ID3D11Device::CreateTexture2D");
	VID_CheckHR (ID3D11Device_CreateShaderResourceView (d3d_device, (ID3D11Resource *)d3d_frame, NULL,
		&d3d_frame_srv), "ID3D11Device::CreateShaderResourceView");
}

/*
===============================================================================

WINDOW

===============================================================================
*/

/*
================
VID_AllocBuffers

The framebuffer, z-buffer and surface cache for the current render size.
================
*/
static void VID_AllocBuffers (int width, int height, int scale)
{
	Mem_Free (vid.buffer);
	vid.buffer = Mem_Alloc ((size_t)width * height * sizeof(pixel_t));

	vid.rowpixels = width;
	vid.width = width;
	vid.height = height;
	vid.conwidth = width / scale;
	vid.conheight = height / scale;
	vid.aspect = ((float)height / (float)width) * (320.0f / 240.0f);
	vid.recalc_refdef = 1;

	vid.scale = (unsigned)scale;
	R_SetRenderSize (width, height, scale);
}

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
	int		scale = 2;
	int		i;

	Cvar_RegisterVariable (&vid_vsync);
	Cvar_RegisterVariable (&vid_scale);
	Cvar_RegisterVariable (&vid_scalemode);
	Cvar_RegisterVariable (&vid_contrast);
	Cvar_RegisterVariable (&vid_hdr);
	Cvar_RegisterVariable (&vid_hdr_paperwhite);
	Cmd_AddCommand ("vid_fullscreen", VID_Fullscreen_f);

	// -scale forces the render scale; the window starts that size either way
	i = COM_CheckParm ("-scale");
	if (i && i + 1 < com_argc)
	{
		scale = Q_atoi (com_argv[i + 1]);
		if (scale < 1)
			scale = 1;
		if (scale > VID_MAX_SCALE)
			scale = VID_MAX_SCALE;
		Cvar_SetValue ("vid_scale", (float)scale);
	}

	VID_CreateWindow (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
	VID_CreateDevice ();
	VID_SetScale (VID_WantedScale ());

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
	VID_RELEASE (d3d_frame_srv);
	VID_RELEASE (d3d_frame);
	VID_RELEASE (d3d_rtv);
	VID_RELEASE (d3d_swapchain);
	VID_RELEASE (d3d_context);
	VID_RELEASE (d3d_device);
	VID_RELEASE (d3d_factory);

	if (mainwindow)
		DestroyWindow (mainwindow);
	mainwindow = NULL;
}

void VID_SetPresent (const vid_present_t *present)
{
	vid_present = *present;
}

/*
================
VID_WantedScale

vid_scale, or the largest whole multiple of 320x200 the window holds
================
*/
static int VID_WantedScale (void)
{
	int		scale;

	if (vid_scale.value >= 1)
		scale = (int)vid_scale.value;
	else
	{
		scale = client_width / VID_BASE_WIDTH;
		if (client_height / VID_BASE_HEIGHT < scale)
			scale = client_height / VID_BASE_HEIGHT;
	}
	if (scale < 1)
		scale = 1;
	if (scale > VID_MAX_SCALE)
		scale = VID_MAX_SCALE;
	return scale;
}

/*
================
VID_SetScale

A new frame size; the next frame is drawn at it
================
*/
static void VID_SetScale (int scale)
{
	VID_CreateFrameTexture (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
	VID_AllocBuffers (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale, scale);
	Con_DPrintf ("Render size %dx%d\n", VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
}

/*
================
VID_FrameToRGB

What present.hlsl does for SDR (blend, gamma, contrast, clip), through a
table per channel
================
*/
void VID_FrameToRGB (byte *rgb)
{
	static byte	lut[3][1024];
	float		c, contrast;
	unsigned	x, y;
	int			ch, v;

	contrast = fmaxf (vid_present.contrast * vid_contrast.value, 0);
	for (ch = 0 ; ch < 3 ; ch++)
		for (v = 0 ; v < 1024 ; v++)
		{
			c = v / 255.0f;
			c += (vid_present.blend[ch] - c) * vid_present.blend[3];
			c = powf (fmaxf (c, 0), vid_present.gamma) * contrast;
			lut[ch][v] = (byte)(fminf (c, 1) * 255 + 0.5f);
		}

	for (y = 0 ; y < vid.height ; y++)
		for (x = 0 ; x < vid.width ; x++, rgb += 3)
		{
			pixel_t	p = vid.buffer[y * vid.rowpixels + x];

			rgb[0] = lut[0][RGB30_R (p)];
			rgb[1] = lut[1][RGB30_G (p)];
			rgb[2] = lut[2][RGB30_B (p)];
		}
}

/*
================
VID_Update

Uploads the whole frame, whose RGB30 pixels are the texture's format, and
presents it letterboxed into the window.
================
*/
void VID_Update (void)
{
	D3D11_MAPPED_SUBRESOURCE	mapped;
	present_constants_t			constants;
	unsigned					y;
	float						scale, sx, sy;
	UINT						flags = 0, interval;
	int							i;

	if (!vid_initialized || Minimized)
		return;

	if (vid_outputdirty || vid_hdr.value != vid_hdrwanted || !IDXGIFactory2_IsCurrent (d3d_factory))
		VID_CheckOutput ();

	// room for this frame in the queue
	WaitForSingleObjectEx (d3d_waitable, 100, TRUE);

	if (FAILED (ID3D11DeviceContext_Map (d3d_context, (ID3D11Resource *)d3d_frame, 0, D3D11_MAP_WRITE_DISCARD, 0,
			&mapped)))
		return;
	for (y = 0 ; y < vid.height ; y++)
		memcpy ((byte *)mapped.pData + y * mapped.RowPitch, vid.buffer + y * vid.rowpixels,
			vid.width * sizeof(pixel_t));
	ID3D11DeviceContext_Unmap (d3d_context, (ID3D11Resource *)d3d_frame, 0);

	// aspect-preserving fit; whole multiples of the render size unless filling the window
	sx = (float)client_width / (float)vid.width;
	sy = (float)client_height / (float)vid.height;
	scale = sx < sy ? sx : sy;
	if (scale >= 1.0f && !vid_scalemode.value)
		scale = floorf (scale);

	D3D11_VIEWPORT viewport = {
		.Width = floorf (vid.width * scale),
		.Height = floorf (vid.height * scale),
		.MaxDepth = 1.0f,
	};
	viewport.TopLeftX = floorf ((client_width - viewport.Width) * 0.5f);
	viewport.TopLeftY = floorf ((client_height - viewport.Height) * 0.5f);

	for (i = 0 ; i < 4 ; i++)
		constants.blend[i] = vid_present.blend[i];
	constants.texsize[0] = (float)vid.width;
	constants.texsize[1] = (float)vid.height;
	constants.scale[0] = constants.scale[1] = scale > 1.0f ? scale : 1.0f;
	constants.gamma = vid_present.gamma;
	constants.contrast = vid_present.contrast * vid_contrast.value;
	constants.sharp = scale == floorf (scale) ? 0.0f : 1.0f;
	constants.hdr = vid_hdroutput ? 1.0f : 0.0f;
	constants.paperwhite = VID_PaperWhiteNits () / 80.0f;		// scRGB 1.0 is 80 nits
	constants.peak = fmaxf (vid_peaknits, VID_PaperWhiteNits ()) / 80.0f;
	constants.pad[0] = constants.pad[1] = 0;
	ID3D11DeviceContext_UpdateSubresource (d3d_context, (ID3D11Resource *)d3d_constants, 0, NULL, &constants, 0, 0);

	static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	ID3D11DeviceContext_ClearRenderTargetView (d3d_context, d3d_rtv, black);
	ID3D11DeviceContext_OMSetRenderTargets (d3d_context, 1, &d3d_rtv, NULL);
	ID3D11DeviceContext_RSSetViewports (d3d_context, 1, &viewport);
	ID3D11DeviceContext_IASetPrimitiveTopology (d3d_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout (d3d_context, NULL);
	ID3D11DeviceContext_VSSetShader (d3d_context, d3d_vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader (d3d_context, d3d_ps, NULL, 0);
	ID3D11DeviceContext_PSSetShaderResources (d3d_context, 0, 1, &d3d_frame_srv);
	ID3D11DeviceContext_PSSetSamplers (d3d_context, 0, 1, &d3d_sampler);
	ID3D11DeviceContext_PSSetConstantBuffers (d3d_context, 0, 1, &d3d_constants);
	ID3D11DeviceContext_Draw (d3d_context, 3, 0);

	interval = vid_vsync.value ? 1 : 0;
	if (!interval && d3d_allow_tearing)
		flags |= DXGI_PRESENT_ALLOW_TEARING;
	IDXGISwapChain1_Present (d3d_swapchain, interval, flags);

	// the window was resized, or vid_scale changed
	if (client_width > 0 && client_height > 0 && VID_WantedScale () != (int)vid.scale)
		VID_SetScale (VID_WantedScale ());
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
