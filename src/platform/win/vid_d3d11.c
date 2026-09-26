// vid_d3d11.c -- Windows video backend: the main window and a DXGI flip-model swapchain
// that presents the software renderer's framebuffer through Direct3D 11.

#include "quakedef.h"
#include "winquake.h"
#include "d_local.h"

#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_6.h>

#include "present_vs.h"
#include "present_ps.h"

viddef_t	vid;				// global video state

unsigned short	d_8to16table[256];
unsigned		d_8to24table[256];

HWND		mainwindow;
modestate_t	modestate = MS_UNINIT;
qboolean	DDActive;			// never true: there is no exclusive fullscreen mode

int			window_center_x, window_center_y;
RECT		window_rect;

cvar_t		_windowed_mouse = {"_windowed_mouse", "1", true};
cvar_t		vid_vsync = {"vid_vsync", "1", true};

#define VID_BASE_WIDTH	320
#define VID_BASE_HEIGHT	200
#define VID_MAX_SCALE	4		// MAXWIDTH/MAXHEIGHT in r_shared.h cap the render size

static ID3D11Device				*d3d_device;
static ID3D11DeviceContext		*d3d_context;
static IDXGISwapChain1			*d3d_swapchain;
static ID3D11RenderTargetView	*d3d_rtv;
static ID3D11Texture2D			*d3d_frame;
static ID3D11ShaderResourceView	*d3d_frame_srv;
static ID3D11VertexShader		*d3d_vs;
static ID3D11PixelShader		*d3d_ps;
static ID3D11SamplerState		*d3d_sampler;
static bool						d3d_allow_tearing;

static bool		vid_initialized;
static bool		vid_fullscreen;
static WINDOWPLACEMENT	vid_windowed_placement = {sizeof (WINDOWPLACEMENT)};
static int		client_width, client_height;
static uint32_t	vid_palette32[256];		// B8G8R8A8 for each palette index
static byte		*vid_surfcache;
static int		vid_surfcachesize;

static LRESULT CALLBACK MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

static void VID_CheckHR (HRESULT hr, const char *what)
{
	if (FAILED (hr))
		Sys_Error ("%s failed (HRESULT 0x%08lx)", what, (unsigned long)hr);
}

#define VID_RELEASE(obj) do { if (obj) { IUnknown_Release ((IUnknown *)(obj)); (obj) = NULL; } } while (0)

/*
================
VID_UpdateWindowStatus

Keeps the screen-space window rectangle current for the mouse code.
================
*/
static void VID_UpdateWindowStatus (void)
{
	RECT	client;
	POINT	topleft = {0, 0};

	GetClientRect (mainwindow, &client);
	ClientToScreen (mainwindow, &topleft);

	window_rect.left = topleft.x;
	window_rect.top = topleft.y;
	window_rect.right = topleft.x + client.right;
	window_rect.bottom = topleft.y + client.bottom;
	window_center_x = (window_rect.left + window_rect.right) / 2;
	window_center_y = (window_rect.top + window_rect.bottom) / 2;

	IN_UpdateClipCursor ();
}

/*
================
ClearAllStates
================
*/
static void ClearAllStates (void)
{
	int		i;

// send an up event for each key, to make sure the server clears them all
	for (i = 0 ; i < 256 ; i++)
		Key_Event (i, false);

	Key_ClearStates ();
	IN_ClearStates ();
}

/*
================
VID_CreateBackbufferView
================
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
================
*/
static void VID_ResizeSwapchain (int width, int height)
{
	if (!d3d_swapchain || width <= 0 || height <= 0)
		return;
	if (width == client_width && height == client_height)
		return;

	client_width = width;
	client_height = height;

	ID3D11DeviceContext_OMSetRenderTargets (d3d_context, 0, NULL, NULL);
	VID_RELEASE (d3d_rtv);
	VID_CheckHR (IDXGISwapChain1_ResizeBuffers (d3d_swapchain, 0, 0, 0, DXGI_FORMAT_UNKNOWN,
		d3d_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0), "IDXGISwapChain1::ResizeBuffers");
	VID_CreateBackbufferView ();
}

/*
================
VID_CreateDevice
================
*/
static void VID_CreateDevice (void)
{
	static const D3D_FEATURE_LEVEL	levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	IDXGIFactory2	*factory;
	IDXGIFactory5	*factory5;
	HRESULT			hr;
	RECT			client;
	BOOL			tearing = FALSE;

	hr = D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, ARRAYSIZE (levels),
		D3D11_SDK_VERSION, &d3d_device, NULL, &d3d_context);
	if (FAILED (hr))
	{
		Con_Printf ("No usable Direct3D 11 hardware device, falling back to WARP\n");
		VID_CheckHR (D3D11CreateDevice (NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, ARRAYSIZE (levels),
			D3D11_SDK_VERSION, &d3d_device, NULL, &d3d_context), "D3D11CreateDevice");
	}

	VID_CheckHR (CreateDXGIFactory2 (0, &IID_IDXGIFactory2, (void **)&factory), "CreateDXGIFactory2");

	if (SUCCEEDED (IDXGIFactory2_QueryInterface (factory, &IID_IDXGIFactory5, (void **)&factory5)))
	{
		if (FAILED (IDXGIFactory5_CheckFeatureSupport (factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
				&tearing, sizeof (tearing))))
			tearing = FALSE;
		IDXGIFactory5_Release (factory5);
	}
	d3d_allow_tearing = tearing != FALSE;

	GetClientRect (mainwindow, &client);
	client_width = client.right;
	client_height = client.bottom;

	DXGI_SWAP_CHAIN_DESC1 desc = {
		.Width = 0,
		.Height = 0,
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = {.Count = 1},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = 2,
		.Scaling = DXGI_SCALING_NONE,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
		.Flags = d3d_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0,
	};
	VID_CheckHR (IDXGIFactory2_CreateSwapChainForHwnd (factory, (IUnknown *)d3d_device, mainwindow, &desc,
		NULL, NULL, &d3d_swapchain), "IDXGIFactory2::CreateSwapChainForHwnd");

	// fullscreen is a borderless window toggled by us, never DXGI exclusive mode
	IDXGIFactory2_MakeWindowAssociation (factory, mainwindow, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	IDXGIFactory2_Release (factory);

	VID_CreateBackbufferView ();

	VID_CheckHR (ID3D11Device_CreateVertexShader (d3d_device, g_present_vs, sizeof (g_present_vs), NULL, &d3d_vs),
		"ID3D11Device::CreateVertexShader");
	VID_CheckHR (ID3D11Device_CreatePixelShader (d3d_device, g_present_ps, sizeof (g_present_ps), NULL, &d3d_ps),
		"ID3D11Device::CreatePixelShader");

	D3D11_SAMPLER_DESC sampler = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
		.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MaxLOD = D3D11_FLOAT32_MAX,
	};
	VID_CheckHR (ID3D11Device_CreateSamplerState (d3d_device, &sampler, &d3d_sampler),
		"ID3D11Device::CreateSamplerState");
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
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
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
================
VID_AllocBuffers

The framebuffer, z-buffer and surface cache for the current render size.
================
*/
static void VID_AllocBuffers (int width, int height)
{
	int		zbuffersize;

	vid_surfcachesize = D_SurfaceCacheForRes (width, height);
	zbuffersize = width * height * (int)sizeof (*d_pzbuffer);

	d_pzbuffer = Hunk_HighAllocName (zbuffersize + vid_surfcachesize, "video");
	vid_surfcache = (byte *)d_pzbuffer + zbuffersize;

	vid.buffer = vid.conbuffer = vid.direct = malloc ((size_t)width * height);
	if (!vid.buffer)
		Sys_Error ("Not enough memory for the framebuffer");

	vid.rowbytes = vid.conrowbytes = width;
	vid.width = vid.conwidth = width;
	vid.height = vid.conheight = height;
	vid.numpages = 1;
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.aspect = ((float)height / (float)width) * (320.0f / 240.0f);
	vid.recalc_refdef = 1;

	D_InitCaches (vid_surfcache, vid_surfcachesize);
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
		MONITORINFO	mi = {sizeof (mi)};

		GetWindowPlacement (mainwindow, &vid_windowed_placement);
		GetMonitorInfo (MonitorFromWindow (mainwindow, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLongPtr (mainwindow, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos (mainwindow, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
		modestate = MS_FULLSCREEN;
	}
	else
	{
		SetWindowLongPtr (mainwindow, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPlacement (mainwindow, &vid_windowed_placement);
		SetWindowPos (mainwindow, NULL, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
		modestate = MS_WINDOWED;
	}

	vid_fullscreen = fullscreen;
	VID_UpdateWindowStatus ();
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

	modestate = MS_WINDOWED;
}

/*
================
VID_Init
================
*/
void VID_Init (unsigned char *palette)
{
	int		scale = 2;
	int		i;

	Cvar_RegisterVariable (&_windowed_mouse);
	Cvar_RegisterVariable (&vid_vsync);
	Cmd_AddCommand ("vid_fullscreen", VID_Fullscreen_f);

	i = COM_CheckParm ("-scale");
	if (i && i + 1 < com_argc)
		scale = Q_atoi (com_argv[i + 1]);
	if (scale < 1)
		scale = 1;
	if (scale > VID_MAX_SCALE)
		scale = VID_MAX_SCALE;

	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	VID_CreateWindow (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
	VID_CreateDevice ();
	VID_CreateFrameTexture (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
	VID_AllocBuffers (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale);
	VID_SetPalette (palette);

	ShowWindow (mainwindow, SW_SHOWDEFAULT);
	UpdateWindow (mainwindow);
	SetForegroundWindow (mainwindow);
	VID_UpdateWindowStatus ();

	if (COM_CheckParm ("-fullscreen"))
		VID_SetFullscreen (true);

	vid_initialized = true;
	vid_menudrawfn = NULL;
	vid_menukeyfn = NULL;
}

/*
================
VID_Shutdown
================
*/
void VID_Shutdown (void)
{
	if (!vid_initialized)
		return;

	vid_initialized = false;

	if (d3d_context)
		ID3D11DeviceContext_ClearState (d3d_context);
	VID_RELEASE (d3d_sampler);
	VID_RELEASE (d3d_ps);
	VID_RELEASE (d3d_vs);
	VID_RELEASE (d3d_frame_srv);
	VID_RELEASE (d3d_frame);
	VID_RELEASE (d3d_rtv);
	VID_RELEASE (d3d_swapchain);
	VID_RELEASE (d3d_context);
	VID_RELEASE (d3d_device);

	if (mainwindow)
		DestroyWindow (mainwindow);
	mainwindow = NULL;
}

/*
================
VID_SetPalette
================
*/
void VID_SetPalette (unsigned char *palette)
{
	int		i;

	for (i = 0 ; i < 256 ; i++)
	{
		vid_palette32[i] = 0xFF000000u | ((uint32_t)palette[i * 3] << 16) |
			((uint32_t)palette[i * 3 + 1] << 8) | (uint32_t)palette[i * 3 + 2];
		d_8to24table[i] = vid_palette32[i];
	}
}

/*
================
VID_ShiftPalette

Called for bonus and pain flashes, and for underwater color changes.
================
*/
void VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}

/*
================
VID_Update

Uploads the whole framebuffer, expanded through the palette, and presents it
letterboxed into the window.
================
*/
void VID_Update ([[maybe_unused]] vrect_t *rects)
{
	D3D11_MAPPED_SUBRESOURCE	mapped;
	unsigned					x, y;
	float						scale, sx, sy;
	UINT						flags = 0, interval;

	if (!vid_initialized || Minimized)
		return;

	if (FAILED (ID3D11DeviceContext_Map (d3d_context, (ID3D11Resource *)d3d_frame, 0, D3D11_MAP_WRITE_DISCARD, 0,
			&mapped)))
		return;

	for (y = 0 ; y < vid.height ; y++)
	{
		const byte	*src = vid.buffer + y * vid.rowbytes;
		uint32_t	*dst = (uint32_t *)((byte *)mapped.pData + y * mapped.RowPitch);

		for (x = 0 ; x < vid.width ; x++)
			dst[x] = vid_palette32[src[x]];
	}
	ID3D11DeviceContext_Unmap (d3d_context, (ID3D11Resource *)d3d_frame, 0);

	// aspect-preserving fit, using whole multiples of the render size when possible
	sx = (float)client_width / (float)vid.width;
	sy = (float)client_height / (float)vid.height;
	scale = sx < sy ? sx : sy;
	if (scale >= 1.0f)
		scale = floorf (scale);

	D3D11_VIEWPORT viewport = {
		.Width = vid.width * scale,
		.Height = vid.height * scale,
		.MaxDepth = 1.0f,
	};
	viewport.TopLeftX = floorf ((client_width - viewport.Width) * 0.5f);
	viewport.TopLeftY = floorf ((client_height - viewport.Height) * 0.5f);

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
	ID3D11DeviceContext_Draw (d3d_context, 3, 0);

	interval = vid_vsync.value ? 1 : 0;
	if (!interval && d3d_allow_tearing)
		flags |= DXGI_PRESENT_ALLOW_TEARING;
	IDXGISwapChain1_Present (d3d_swapchain, interval, flags);
}

/*
================
VID_LockBuffer / VID_UnlockBuffer

The framebuffer is ordinary memory, so there is nothing to lock.
================
*/
void VID_LockBuffer (void)
{
}

void VID_UnlockBuffer (void)
{
}

int VID_ForceUnlockedAndReturnState (void)
{
	return 0;
}

void VID_ForceLockState ([[maybe_unused]] int lk)
{
}

int VID_SetMode ([[maybe_unused]] int modenum, [[maybe_unused]] unsigned char *palette)
{
	return true;
}

void VID_HandlePause ([[maybe_unused]] qboolean pause)
{
}

/*
================
D_BeginDirectRect / D_EndDirectRect

Used for the loading disc; the next full present shows it, so nothing is needed.
================
*/
void D_BeginDirectRect ([[maybe_unused]] int x, [[maybe_unused]] int y, [[maybe_unused]] byte *pbitmap,
	[[maybe_unused]] int width, [[maybe_unused]] int height)
{
}

void D_EndDirectRect ([[maybe_unused]] int x, [[maybe_unused]] int y, [[maybe_unused]] int width,
	[[maybe_unused]] int height)
{
}

//==========================================================================

static const byte scantokey[128] =
{
//  0           1       2       3       4       5       6       7
//  8           9       A       B       C       D       E       F
	0  ,    27,     '1',    '2',    '3',    '4',    '5',    '6',
	'7',    '8',    '9',    '0',    '-',    '=',    K_BACKSPACE, 9, // 0
	'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
	'o',    'p',    '[',    ']',    13 ,    K_CTRL,'a',  's',      // 1
	'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
	'\'' ,    '`',    K_SHIFT,'\\',  'z',    'x',    'c',    'v',      // 2
	'b',    'n',    'm',    ',',    '.',    '/',    K_SHIFT,'*',
	K_ALT,' ',   0  ,    K_F1, K_F2, K_F3, K_F4, K_F5,   // 3
	K_F6, K_F7, K_F8, K_F9, K_F10,  K_PAUSE,    0  , K_HOME,
	K_UPARROW,K_PGUP,'-',K_LEFTARROW,'5',K_RIGHTARROW,'+',K_END, //4
	K_DOWNARROW,K_PGDN,K_INS,K_DEL,0,0,             0,              K_F11,
	K_F12,0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 5
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 6
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
	0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0         // 7
};

/*
=======
MapKey

Map from windows scancodes to quake keynums
=======
*/
static int MapKey (LPARAM lParam)
{
	int		key = (int)(lParam >> 16) & 255;

	if (key > 127)
		return 0;

	return scantokey[key];
}

/*
================
AppActivate
================
*/
static void AppActivate (bool active, bool minimize)
{
	static bool	sound_active = true;

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

	if (ActiveApp)
	{
		if (vid_fullscreen || (_windowed_mouse.value && key_dest == key_game))
		{
			IN_ActivateMouse ();
			IN_HideMouse ();
		}
	}
	else
	{
		IN_DeactivateMouse ();
		IN_ShowMouse ();
	}
}

/*
================
MainWndProc
================
*/
static LRESULT CALLBACK MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	int		buttons;

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
		VID_UpdateWindowStatus ();
		return 0;

	case WM_SIZE:
		Minimized = wParam == SIZE_MINIMIZED;
		if (!Minimized)
		{
			VID_ResizeSwapchain (LOWORD (lParam), HIWORD (lParam));
			VID_UpdateWindowStatus ();
		}
		return 0;

	case WM_SYSCHAR:
		// keep Alt-Space from happening
		return 0;

	case WM_ACTIVATE:
		AppActivate (LOWORD (wParam) != WA_INACTIVE, HIWORD (wParam) != 0);
		// fix the leftover Alt from any Alt-Tab or the like that switched us away
		ClearAllStates ();
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
		Key_Event (MapKey (lParam), true);
		return 0;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		Key_Event (MapKey (lParam), false);
		return 0;

	// Windows may pack multiple mouse events into one update, so always check all
	// button states
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEMOVE:
		buttons = 0;
		if (wParam & MK_LBUTTON)
			buttons |= 1;
		if (wParam & MK_RBUTTON)
			buttons |= 2;
		if (wParam & MK_MBUTTON)
			buttons |= 4;
		IN_MouseEvent (buttons);
		return 0;

	case WM_MOUSEWHEEL:
		if ((short)HIWORD (wParam) > 0)
		{
			Key_Event (K_MWHEELUP, true);
			Key_Event (K_MWHEELUP, false);
		}
		else
		{
			Key_Event (K_MWHEELDOWN, true);
			Key_Event (K_MWHEELDOWN, false);
		}
		return 0;

	case WM_CLOSE:
		// quit from the main loop, not from inside the window procedure
		Cbuf_AddText ("quit\n");
		return 0;

	default:
		break;
	}

	return DefWindowProc (hWnd, uMsg, wParam, lParam);
}
