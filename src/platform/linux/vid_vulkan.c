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
// vid_vulkan.c -- Linux video backend: Vulkan presenting the software renderer's
// frame in the Wayland window (wl_linux.c).
//
// The renderer draws into memory the GPU reads: each layer (the 3D view, RGB30,
// and its 2D, RGBA8) is a buffer mapped for the CPU, and present.glsl reads it
// through its device address, decoding the pixels itself, as present.metal lays
// the layers into the letterboxed viewport (vid_common.c has the fit and the
// constants). On a GPU that shares the CPU's memory, as an integrated one does,
// the buffer is where the CPU drew, cached as the CPU's memory is; a GPU of its
// own memory copies the frame there first (it would read it over the bus a
// pixel at a time).
//
// The GPU is the compositor's, which the frames go to without a copy between
// GPUs; -gpu n takes another.
//
// The GPU reads a layer after VID_Update has returned, so there are two of each,
// as vid_metal.m has them: the view is drawn into them in turn, and the 2D goes
// to the one not shown when it is drawn again. Each remembers the last frame that
// read it, a value of the timeline semaphore each frame signals, and goes to the
// renderer only once the GPU has finished that frame.
//
// With vsync the swapchain is FIFO and a frame is presented once the one before
// is on the screen (present wait): one shown, one queued, and a frame being drawn.
// Without, it is IMMEDIATE (tearing, if the compositor lets windows tear) or
// MAILBOX, and a frame is presented once the last was shown or passed over; the
// frames between are drawn but not shown.
//
// The swapchain is SDR (10-bit, sRGB), or on an HDR display PQ in BT.2020 (HDR10).
// Which one follows the compositor's preferred image description for the window.

#include "args.h"
#include "cmd.h"
#include "print.h"
#include "sys.h"
#include "sound.h"
#include "present_spirv.h"
#include "wl_local.h"

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VID_SLOTS		2				// frames in flight at most, and so view buffers
#define VID_HUNG_NS		2000000000ull	// a frame the GPU hasn't finished in this long won't be
#define VID_PACE_NS		100000000ull	// the longest the pace waits for the display
#define VID_ACQUIRES	(VID_SLOTS + 1)	// acquire semaphores, used in turn
#define VID_MAX_IMAGES	8
#define VID_PIPELINES	4				// pipelines made, one a swapchain format

typedef struct
{
	VkBuffer		buffer;		// what the renderer draws in, mapped
	VkDeviceMemory	memory;
	void			*data;
	VkDeviceAddress	address;
	VkBuffer		copy;		// on a GPU of its own memory, the frame there
	VkDeviceMemory	copymemory;
	VkDeviceAddress	copyaddress;
	uint64_t		serial;		// the last frame that read it
} vid_layer_t;

static VkInstance			vk_instance;
static VkSurfaceKHR			vk_surface;
static VkPhysicalDevice		vk_gpu;
static VkPhysicalDeviceProperties	vk_gpuprops;
static bool					vk_compositorgpu;	// the one the compositor draws with
static VkDevice				vk_device;
static VkQueue				vk_queue;
static uint32_t				vk_family;
static VkCommandPool		vk_pool;
static VkCommandBuffer		vk_commands[VID_SLOTS];
static VkSemaphore			vk_timeline;		// signaled with each frame's serial when it is done
static uint64_t				vk_serial;			// the last frame submitted
static VkSemaphore			vk_acquired[VID_ACQUIRES];
static uint64_t				vk_acquiredserial[VID_ACQUIRES];	// the frame that waited on it
static VkShaderModule		vk_vs, vk_fs;
static VkPipelineLayout		vk_layout;
static struct { VkFormat format; VkPipeline pipeline; }	vk_pipelines[VID_PIPELINES];

static uint32_t		vk_hostmemory;		// the type of the layers' memory
static bool			vk_inplace;			// the GPU reads the layers where the CPU drew them
static uint32_t		vk_localmemory;		// the GPU's own, for the copies

static bool			vk_colorspaces;		// VK_EXT_swapchain_colorspace
static bool			vk_presentwait;		// VK_KHR_present_id and _wait
static bool			vk_hdrmetadata;		// VK_EXT_hdr_metadata
static PFN_vkWaitForPresentKHR	vk_WaitForPresent;
static PFN_vkSetHdrMetadataEXT	vk_SetHdrMetadata;

static VkSwapchainKHR		vk_swapchain;
static VkFormat				vk_format;
static VkColorSpaceKHR		vk_colorspace;
static VkPresentModeKHR		vk_presentmode;
static uint32_t				vk_numimages;
static VkImage				vk_images[VID_MAX_IMAGES];
static VkImageView			vk_views[VID_MAX_IMAGES];
static VkSemaphore			vk_rendered[VID_MAX_IMAGES];	// an image's frame is drawn
static VkExtent2D			vk_extent;
static int					vk_unitwidth, vk_unitheight;	// the window the swapchain is for, in its units
static bool					vk_swapdirty = true;			// make the swapchain again
static uint64_t				vk_presentid;		// the last frame presented

static vid_layer_t	vid_views[VID_SLOTS];	// the 3D view
static vid_layer_t	vid_huds[2];			// the 2D
static int			vid_slot;				// vid.buffer's view
static int			vid_hudshown;			// the 2D shown; vid.hud is the other
static int			vid_drawnslot;			// the view last drawn, presented or not
static int			vid_drawnhud;			// and its 2D

static bool		vid_initialized;
static int		vid_latency;				// frames in flight: 1 with vsync, 2 without
static double	vid_presenttime;			// when the last frame was presented
static int		client_width, client_height;	// the window, in pixels

static int		vid_output = VID_OUTPUT_SDR;	// what the swapchain is
static bool		vid_outputknown;			// the output mode has been reported
static uint32_t	vid_colorserial = ~0u;		// the preferred description last checked
static float	vid_hdrwanted = -1;			// vid_hdr when the output was last checked
static float	vid_paperwhite = 1, vid_peak = 1;	// PQ's, in reference whites

static void VID_SetScale (void);

static const char *VID_Result (VkResult result)
{
	switch (result)
	{
	case VK_ERROR_OUT_OF_HOST_MEMORY:		return "out of host memory";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:		return "out of device memory";
	case VK_ERROR_INITIALIZATION_FAILED:	return "initialization failed";
	case VK_ERROR_DEVICE_LOST:				return "device lost";
	case VK_ERROR_EXTENSION_NOT_PRESENT:	return "extension not present";
	case VK_ERROR_FEATURE_NOT_PRESENT:		return "feature not present";
	case VK_ERROR_INCOMPATIBLE_DRIVER:		return "incompatible driver";
	case VK_ERROR_SURFACE_LOST_KHR:			return "surface lost";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:	return "window in use";
	case VK_ERROR_OUT_OF_DATE_KHR:			return "out of date";
	case VK_TIMEOUT:						return "timeout";
	default:								return "error";
	}
}

static void VID_Check (VkResult result, const char *what)
{
	if (result < 0)
		Sys_Error ("%s failed: %s (%d)", what, VID_Result (result), (int)result);
}

/*
===============================================================================

THE WINDOW

===============================================================================
*/

// the compositor's idle blanking and locking are kept off while the game is
// played in fullscreen
static void VID_UpdateIdle (void)
{
	WL_SetIdleInhibit (ActiveApp && !Minimized && WL_IsFullscreen ());
}

void VID_AppActivate (bool active)
{
	static bool	sound_active;		// sound starts blocked (sys_linux_gui.c), until the window is active

	if (active == ActiveApp)
		return;
	ActiveApp = active;

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
	VID_UpdateIdle ();
}

// the compositor shows the window nowhere (another workspace, minimized): there
// is nothing to present, and waiting for the display would hold up the loop
// (and a server in it)
void VID_WindowSuspended (bool suspended)
{
	Minimized = suspended;
	VID_UpdateIdle ();
}

static void VID_SetFullscreen (bool fullscreen)
{
	if (fullscreen != WL_IsFullscreen ())
		WL_SetFullscreen (fullscreen);
}

static void VID_Fullscreen_f (void)
{
	VID_SetFullscreen (!WL_IsFullscreen ());
}

void VID_ToggleFullscreen (void)
{
	VID_Fullscreen_f ();
}

// the frames are the window's size in pixels
static void VID_UpdateClientSize (void)
{
	int		pixelwidth, pixelheight, width, height;

	WL_WindowSize (&pixelwidth, &pixelheight, &width, &height);
	if (pixelwidth != client_width || pixelheight != client_height || width != vk_unitwidth
		|| height != vk_unitheight)
		vk_swapdirty = true;
	client_width = pixelwidth;
	client_height = pixelheight;
	VID_UpdateIdle ();
}

/*
===============================================================================

THE GPU

===============================================================================
*/

static bool VID_HasExtension (const VkExtensionProperties *extensions, uint32_t count, const char *name)
{
	uint32_t	i;

	for (i = 0 ; i < count ; i++)
		if (!strcmp (extensions[i].extensionName, name))
			return true;
	return false;
}

static void VID_CreateInstance (void)
{
	VkExtensionProperties	extensions[256];
	uint32_t				count = 256, version = 0, numwanted = 0;
	const char				*wanted[3];

	vkEnumerateInstanceVersion (&version);
	if (version < VK_API_VERSION_1_3)
		Sys_Error ("SoftWorld needs Vulkan 1.3; the Vulkan loader is %u.%u", VK_API_VERSION_MAJOR (version),
			VK_API_VERSION_MINOR (version));
	VID_Check (vkEnumerateInstanceExtensionProperties (NULL, &count, extensions), "Listing Vulkan's extensions");
	if (!VID_HasExtension (extensions, count, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME))
		Sys_Error ("Vulkan has no Wayland surfaces (%s)", VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
	wanted[numwanted++] = VK_KHR_SURFACE_EXTENSION_NAME;
	wanted[numwanted++] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	vk_colorspaces = VID_HasExtension (extensions, count, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
	if (vk_colorspaces)
		wanted[numwanted++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;

	VID_Check (vkCreateInstance (&(VkInstanceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &(VkApplicationInfo){
				.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				.pApplicationName = "SoftWorld",
				.pEngineName = "SoftWorld",
				.apiVersion = VK_API_VERSION_1_3,
			},
			.enabledExtensionCount = numwanted,
			.ppEnabledExtensionNames = wanted,
		}, NULL, &vk_instance), "vkCreateInstance");

	VID_Check (vkCreateWaylandSurfaceKHR (vk_instance, &(VkWaylandSurfaceCreateInfoKHR){
			.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
			.display = way.display,
			.surface = way.surface,
		}, NULL, &vk_surface), "vkCreateWaylandSurfaceKHR");
}

typedef struct
{
	VkPhysicalDevice	gpu;
	VkPhysicalDeviceProperties	props;
	uint32_t	family;			// a queue family that draws and presents
	bool		compositors;	// the compositor draws with it
	bool		presentwait;
	bool		hdrmetadata;
} vid_gpu_t;

// whether the GPU can present the frame: Vulkan 1.3, what the presenter
// uses of it, and a queue that draws and presents to the window
static bool VID_CheckGPU (VkPhysicalDevice gpu, vid_gpu_t *out, const char **why)
{
	VkExtensionProperties	extensions[512];
	VkQueueFamilyProperties	families[32];
	uint32_t				count = 512, numfamilies = 32, i;
	VkBool32				present;
	unsigned				major, minor;
	VkPhysicalDeviceDrmPropertiesEXT	drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2			props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	VkPhysicalDevicePresentWaitFeaturesKHR	waitf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
	VkPhysicalDevicePresentIdFeaturesKHR	idf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
	VkPhysicalDeviceVulkan13Features	f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features	f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &f13};
	VkPhysicalDeviceFeatures2			f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};

	memset (out, 0, sizeof(*out));
	out->gpu = gpu;
	vkGetPhysicalDeviceProperties (gpu, &out->props);
	if (out->props.apiVersion < VK_API_VERSION_1_3)
	{
		*why = "no Vulkan 1.3";
		return false;
	}
	if (vkEnumerateDeviceExtensionProperties (gpu, NULL, &count, extensions) < 0)
		count = 0;
	if (!VID_HasExtension (extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
	{
		*why = "no swapchains";
		return false;
	}

	out->presentwait = VID_HasExtension (extensions, count, VK_KHR_PRESENT_ID_EXTENSION_NAME)
		&& VID_HasExtension (extensions, count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
	if (out->presentwait)
	{
		idf.pNext = &waitf;
		f13.pNext = &idf;
	}
	vkGetPhysicalDeviceFeatures2 (gpu, &f2);
	if (!f12.bufferDeviceAddress || !f12.timelineSemaphore || !f13.dynamicRendering || !f13.synchronization2)
	{
		*why = "no buffer device addresses, timeline semaphores, dynamic rendering or synchronization2";
		return false;
	}
	out->presentwait = out->presentwait && idf.presentId && waitf.presentWait;
	out->hdrmetadata = VID_HasExtension (extensions, count, VK_EXT_HDR_METADATA_EXTENSION_NAME);

	vkGetPhysicalDeviceQueueFamilyProperties (gpu, &numfamilies, families);
	for (i = 0 ; i < numfamilies ; i++)
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			&& vkGetPhysicalDeviceSurfaceSupportKHR (gpu, i, vk_surface, &present) == VK_SUCCESS && present)
			break;
	if (i == numfamilies)
	{
		*why = "it can't present to the window";
		return false;
	}
	out->family = i;

	// the compositor's, by the DRM device the compositor named
	if (VID_HasExtension (extensions, count, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)
		&& WL_MainDevice (&major, &minor))
	{
		props2.pNext = &drm;
		vkGetPhysicalDeviceProperties2 (gpu, &props2);
		out->compositors = (drm.hasPrimary && drm.primaryMajor == major && drm.primaryMinor == minor)
			|| (drm.hasRender && drm.renderMajor == major && drm.renderMinor == minor);
	}
	return true;
}

// the compositor's GPU, else the first that can present; -gpu n the n'th
static void VID_ChooseGPU (vid_gpu_t *chosen)
{
	VkPhysicalDevice	gpus[16];
	vid_gpu_t			gpu;
	uint32_t			count = 16, i;
	const char			*why = NULL;
	int					forced = -1, p;
	bool				found = false;

	if ((p = COM_CheckParm ("-gpu")) && p + 1 < com_argc)
		forced = atoi (com_argv[p + 1]);
	if (vkEnumeratePhysicalDevices (vk_instance, &count, gpus) < 0 || !count)
		Sys_Error ("Vulkan found no GPU");

	for (i = 0 ; i < count ; i++)
	{
		if (forced >= 0 && (int)i != forced)
			continue;
		if (!VID_CheckGPU (gpus[i], &gpu, &why))
		{
			Con_Printf ("GPU %u, %s: %s\n", i, gpu.props.deviceName, why);
			continue;
		}
		if (!found || (gpu.compositors && !chosen->compositors))
		{
			*chosen = gpu;
			found = true;
		}
	}
	if (!found && forced >= 0)
		Sys_Error ("-gpu %d: %s", forced, forced < (int)count ? why : "there is no such GPU");
	if (!found)
		Sys_Error ("No GPU can present SoftWorld's frames: it needs Vulkan 1.3");
}

static uint32_t VID_MemoryType (uint32_t types, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer,
	bool *preferred)
{
	VkPhysicalDeviceMemoryProperties	memory;
	uint32_t	i, found = UINT32_MAX;

	vkGetPhysicalDeviceMemoryProperties (vk_gpu, &memory);
	for (i = 0 ; i < memory.memoryTypeCount ; i++)
	{
		if (!(types & (1u << i)) || (memory.memoryTypes[i].propertyFlags & need) != need)
			continue;
		if ((memory.memoryTypes[i].propertyFlags & prefer) == prefer)
		{
			if (preferred)
				*preferred = true;
			return i;
		}
		if (found == UINT32_MAX)
			found = i;
	}
	if (preferred)
		*preferred = false;
	return found;
}

static VkShaderModule VID_ShaderModule (const unsigned char *code, size_t size)
{
	VkShaderModule	module;

	VID_Check (vkCreateShaderModule (vk_device, &(VkShaderModuleCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = size,
			.pCode = (const uint32_t *)code,
		}, NULL, &module), "vkCreateShaderModule");
	return module;
}

static void VID_CreateDevice (void)
{
	vid_gpu_t	gpu;
	const char	*extensions[4];
	uint32_t	numextensions = 0, i;
	VkBuffer	probe;
	VkMemoryRequirements	req;
	VkPhysicalDevicePresentWaitFeaturesKHR	waitf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
		.presentWait = VK_TRUE};
	VkPhysicalDevicePresentIdFeaturesKHR	idf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		.pNext = &waitf, .presentId = VK_TRUE};
	VkPhysicalDeviceVulkan13Features	f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE};
	VkPhysicalDeviceVulkan12Features	f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &f13, .bufferDeviceAddress = VK_TRUE, .timelineSemaphore = VK_TRUE};

	VID_ChooseGPU (&gpu);
	vk_gpu = gpu.gpu;
	vk_gpuprops = gpu.props;
	vk_family = gpu.family;
	vk_compositorgpu = gpu.compositors;
	vk_presentwait = gpu.presentwait;
	vk_hdrmetadata = gpu.hdrmetadata;

	extensions[numextensions++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	if (vk_presentwait)
	{
		extensions[numextensions++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
		extensions[numextensions++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
		f13.pNext = &idf;
	}
	if (vk_hdrmetadata)
		extensions[numextensions++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;

	VID_Check (vkCreateDevice (vk_gpu, &(VkDeviceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &f12,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = vk_family,
				.queueCount = 1,
				.pQueuePriorities = &(float){1.0f},
			},
			.enabledExtensionCount = numextensions,
			.ppEnabledExtensionNames = extensions,
		}, NULL, &vk_device), "vkCreateDevice");
	vkGetDeviceQueue (vk_device, vk_family, 0, &vk_queue);
	if (vk_presentwait)
		vk_WaitForPresent = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr (vk_device, "vkWaitForPresentKHR");
	if (vk_hdrmetadata)
		vk_SetHdrMetadata = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr (vk_device, "vkSetHdrMetadataEXT");
	vk_presentwait = vk_WaitForPresent != NULL;
	vk_hdrmetadata = vk_SetHdrMetadata != NULL;

	VID_Check (vkCreateCommandPool (vk_device, &(VkCommandPoolCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = vk_family,
		}, NULL, &vk_pool), "vkCreateCommandPool");
	VID_Check (vkAllocateCommandBuffers (vk_device, &(VkCommandBufferAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = VID_SLOTS,
		}, vk_commands), "vkAllocateCommandBuffers");

	VID_Check (vkCreateSemaphore (vk_device, &(VkSemaphoreCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &(VkSemaphoreTypeCreateInfo){
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			},
		}, NULL, &vk_timeline), "The timeline semaphore");
	for (i = 0 ; i < VID_ACQUIRES ; i++)
		VID_Check (vkCreateSemaphore (vk_device, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO},
			NULL, &vk_acquired[i]), "An acquire semaphore");

	// the shaders, compiled with the program; the constants and the layers' addresses pushed
	vk_vs = VID_ShaderModule (vid_present_vert, vid_present_vert_size);
	vk_fs = VID_ShaderModule (vid_present_frag, vid_present_frag_size);
	VID_Check (vkCreatePipelineLayout (vk_device, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &(VkPushConstantRange){
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.size = sizeof(vid_present_push_t),
			},
		}, NULL, &vk_layout), "vkCreatePipelineLayout");

	// the layers' memory: the CPU's, cached, as the renderer reads what it
	// blends over; where the GPU reads it too if there is such
	VID_Check (vkCreateBuffer (vk_device, &(VkBufferCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = 65536,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
				| VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		}, NULL, &probe), "vkCreateBuffer");
	vkGetBufferMemoryRequirements (vk_device, probe, &req);
	vkDestroyBuffer (vk_device, probe, NULL);
	vk_hostmemory = VID_MemoryType (req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vk_inplace);
	if (vk_hostmemory == UINT32_MAX)
	{
		Con_Printf ("The GPU has no cached memory the CPU can draw in: drawing is slower\n");
		vk_hostmemory = VID_MemoryType (req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vk_inplace);
		if (vk_hostmemory == UINT32_MAX)
			Sys_Error ("The GPU has no memory the CPU can draw in");
	}
	vk_localmemory = VID_MemoryType (req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL);
}

// until the GPU has finished frame serial; a GPU that doesn't is hung
static void VID_WaitFrame (uint64_t serial)
{
	uint64_t	done;

	if (!serial || (vkGetSemaphoreCounterValue (vk_device, vk_timeline, &done) == VK_SUCCESS && done >= serial))
		return;
	if (vkWaitSemaphores (vk_device, &(VkSemaphoreWaitInfo){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &vk_timeline,
			.pValues = &serial,
		}, VID_HUNG_NS) != VK_SUCCESS)
		Sys_Error ("The GPU didn't finish a frame in %llu ms", VID_HUNG_NS / 1000000);
}

static VkPipeline VID_Pipeline (VkFormat format)
{
	static const VkDynamicState	dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipeline	pipeline;
	int			i;

	for (i = 0 ; i < VID_PIPELINES && vk_pipelines[i].pipeline ; i++)
		if (vk_pipelines[i].format == format)
			return vk_pipelines[i].pipeline;
	if (i == VID_PIPELINES)
		Sys_Error ("VID_Pipeline: too many swapchain formats");

	VID_Check (vkCreateGraphicsPipelines (vk_device, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &(VkPipelineRenderingCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.colorAttachmentCount = 1,
				.pColorAttachmentFormats = &format,
			},
			.stageCount = 2,
			.pStages = (VkPipelineShaderStageCreateInfo[]){
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
					.module = vk_vs, .pName = "main"},
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
					.module = vk_fs, .pName = "main"},
			},
			.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
			.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
				.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
			.pViewportState = &(VkPipelineViewportStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
				.viewportCount = 1,
				.scissorCount = 1},
			.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
				.polygonMode = VK_POLYGON_MODE_FILL,
				.cullMode = VK_CULL_MODE_NONE,
				.lineWidth = 1.0f},
			.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
				.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
			.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
				.attachmentCount = 1,
				.pAttachments = &(VkPipelineColorBlendAttachmentState){
					.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
						| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT}},
			.pDynamicState = &(VkPipelineDynamicStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
				.dynamicStateCount = 2,
				.pDynamicStates = dynamic},
			.layout = vk_layout,
		}, NULL, &pipeline), "vkCreateGraphicsPipelines");
	vk_pipelines[i].format = format;
	vk_pipelines[i].pipeline = pipeline;
	return pipeline;
}

/*
===============================================================================

SDR AND HDR

===============================================================================
*/

static const char *VID_FormatName (VkFormat format, VkColorSpaceKHR space)
{
	static char	name[64];
	const char	*f, *s;

	switch (format)
	{
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:	f = "10-bit (XRGB2101010)";	break;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:	f = "10-bit (XBGR2101010)";	break;
	case VK_FORMAT_B8G8R8A8_UNORM:				f = "8-bit (XRGB8888)";		break;
	case VK_FORMAT_R8G8B8A8_UNORM:				f = "8-bit (XBGR8888)";		break;
	default:									f = "another format";		break;
	}
	s = space == VK_COLOR_SPACE_HDR10_ST2084_EXT ? "PQ, BT.2020" : "sRGB";
	snprintf (name, sizeof(name), "%s %s", f, s);
	return name;
}

// the swapchain format for an output, VK_FORMAT_UNDEFINED where the surface has none
static VkFormat VID_ChooseFormat (int output, VkColorSpaceKHR *space)
{
	static const VkFormat	sdr[] = {VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
	static const VkFormat	hdr[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
	VkSurfaceFormatKHR	formats[64];
	uint32_t			count = 64, i, j;
	const VkFormat		*wanted = output == VID_OUTPUT_PQ ? hdr : sdr;
	uint32_t			numwanted = output == VID_OUTPUT_PQ ? 2 : 4;

	*space = output == VID_OUTPUT_PQ ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR (vk_gpu, vk_surface, &count, formats) < 0)
		return VK_FORMAT_UNDEFINED;
	for (j = 0 ; j < numwanted ; j++)
		for (i = 0 ; i < count ; i++)
			if (formats[i].format == wanted[j] && formats[i].colorSpace == *space)
				return wanted[j];
	return VK_FORMAT_UNDEFINED;
}

/*
================
VID_CheckOutput

SDR, or PQ where the compositor would like HDR for the window (its preferred
image description is PQ or HLG, or shows brighter than its reference white)
and the swapchain can be HDR10
================
*/
static void VID_CheckOutput (void)
{
	const wl_colors_t	*colors = WL_PreferredColors ();
	VkColorSpaceKHR		space;
	float				reference, peak;
	bool				displayhdr, wasknown;
	int					output;
	const char			*why = NULL;

	vid_colorserial = colors->serial;
	vid_hdrwanted = vid_hdr.value;

	reference = colors->reference > 0 ? colors->reference : 203;
	peak = colors->targetmax > 0 ? colors->targetmax : colors->maxlum;
	displayhdr = colors->known && (colors->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
		|| colors->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG || peak >= reference * 1.5f);

	output = VID_OUTPUT_SDR;
	if (!way.color)
		why = "the compositor has no color management";
	else if (!colors->known)
		why = "the compositor didn't describe the display";
	else if (displayhdr && !vid_hdr.value)
		why = "vid_hdr is 0";
	else if (displayhdr && (!vk_colorspaces || VID_ChooseFormat (VID_OUTPUT_PQ, &space) == VK_FORMAT_UNDEFINED))
		why = "the Vulkan driver presents no HDR10 to this compositor";
	else if (displayhdr)
		output = VID_OUTPUT_PQ;

	// in reference whites: SDR white where the compositor puts it, or at
	// vid_hdr_paperwhite nits; the peak is the display's
	vid_paperwhite = vid_hdr_paperwhite.value > 0 ? vid_hdr_paperwhite.value / reference : 1;
	vid_peak = fmaxf (peak / reference, vid_paperwhite);

	if (output != vid_output)
		vk_swapdirty = true;
	wasknown = vid_outputknown;
	vid_outputknown = true;
	if (wasknown && output == vid_output)
		return;
	vid_output = output;
	if (output == VID_OUTPUT_PQ)
		Con_Printf ("HDR output (PQ): SDR white at %.0f nits%s, up to %.0f nits\n", vid_paperwhite * reference,
			vid_hdr_paperwhite.value > 0 ? "" : " (the compositor's)", vid_peak * reference);
	else if (why && (displayhdr || !colors->known))
		Con_Printf ("SDR output: %s\n", why);
	else
		Con_Printf ("SDR output\n");
}

// what the frames are mastered for: the display's range, so the compositor
// maps nothing
static void VID_SetHdrMetadata (void)
{
	const wl_colors_t	*colors = WL_PreferredColors ();
	const float			*p = colors->targetprimaries;
	float				reference = colors->reference > 0 ? colors->reference : 203;

	if (!vk_hdrmetadata || vid_output != VID_OUTPUT_PQ)
		return;
	VkHdrMetadataEXT	metadata = {
		.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
		.displayPrimaryRed = {0.708f, 0.292f},
		.displayPrimaryGreen = {0.170f, 0.797f},
		.displayPrimaryBlue = {0.131f, 0.046f},
		.whitePoint = {0.3127f, 0.3290f},
		.maxLuminance = 203 * vid_peak,
		.minLuminance = colors->targetmin * 203 / reference,
		.maxContentLightLevel = 203 * vid_peak,
		.maxFrameAverageLightLevel = 203 * vid_paperwhite,
	};
	if (colors->hastargetprimaries)
	{
		metadata.displayPrimaryRed = (VkXYColorEXT){p[0], p[1]};
		metadata.displayPrimaryGreen = (VkXYColorEXT){p[2], p[3]};
		metadata.displayPrimaryBlue = (VkXYColorEXT){p[4], p[5]};
		metadata.whitePoint = (VkXYColorEXT){p[6], p[7]};
	}
	vk_SetHdrMetadata (vk_device, 1, &vk_swapchain, &metadata);
}

/*
===============================================================================

THE SWAPCHAIN

===============================================================================
*/

static const char *VID_PresentModeName (VkPresentModeKHR mode)
{
	switch (mode)
	{
	case VK_PRESENT_MODE_FIFO_KHR:		return "vsync (FIFO)";
	case VK_PRESENT_MODE_IMMEDIATE_KHR:	return "no vsync, tearing where the compositor lets it (IMMEDIATE)";
	case VK_PRESENT_MODE_MAILBOX_KHR:	return "no vsync, the newest frame at each refresh (MAILBOX)";
	default:							return "another mode";
	}
}

// FIFO with vsync; without, IMMEDIATE (tearing), else MAILBOX
static VkPresentModeKHR VID_ChoosePresentMode (void)
{
	VkPresentModeKHR	modes[16];
	uint32_t			count = 16, i;
	bool				immediate = false, mailbox = false;

	if (vid_latency == 1 || vkGetPhysicalDeviceSurfacePresentModesKHR (vk_gpu, vk_surface, &count, modes) < 0)
		return VK_PRESENT_MODE_FIFO_KHR;
	for (i = 0 ; i < count ; i++)
	{
		immediate |= modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
		mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
	}
	return immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
		: VK_PRESENT_MODE_FIFO_KHR;
}

static void VID_DestroySwapchainImages (void)
{
	uint32_t	i;

	for (i = 0 ; i < vk_numimages ; i++)
	{
		vkDestroyImageView (vk_device, vk_views[i], NULL);
		vkDestroySemaphore (vk_device, vk_rendered[i], NULL);
	}
	vk_numimages = 0;
}

/*
================
VID_CreateSwapchain

For the window's size, the output and the vsync; from the one before, which
Vulkan may take the images of
================
*/
static void VID_CreateSwapchain (void)
{
	VkSurfaceCapabilitiesKHR	caps;
	VkSwapchainKHR				old = vk_swapchain;
	VkPresentModeKHR			mode;
	VkCompositeAlphaFlagBitsKHR	alpha;
	VkFormat					format;
	VkColorSpaceKHR				space;
	uint32_t					count, i;
	int							pixelwidth, pixelheight;

	vk_swapdirty = false;
	WL_WindowSize (&pixelwidth, &pixelheight, &vk_unitwidth, &vk_unitheight);
	client_width = pixelwidth;
	client_height = pixelheight;
	if (pixelwidth <= 0 || pixelheight <= 0)
		return;

	VID_Check (vkGetPhysicalDeviceSurfaceCapabilitiesKHR (vk_gpu, vk_surface, &caps), "The surface's capabilities");
	vk_extent = caps.currentExtent;
	if (vk_extent.width == UINT32_MAX)
	{
		vk_extent.width = (uint32_t)pixelwidth;
		vk_extent.height = (uint32_t)pixelheight;
	}
	vk_extent.width = vk_extent.width < caps.minImageExtent.width ? caps.minImageExtent.width : vk_extent.width;
	vk_extent.height = vk_extent.height < caps.minImageExtent.height ? caps.minImageExtent.height : vk_extent.height;
	if (caps.maxImageExtent.width && vk_extent.width > caps.maxImageExtent.width)
		vk_extent.width = caps.maxImageExtent.width;
	if (caps.maxImageExtent.height && vk_extent.height > caps.maxImageExtent.height)
		vk_extent.height = caps.maxImageExtent.height;

	format = VID_ChooseFormat (vid_output, &space);
	if (format == VK_FORMAT_UNDEFINED && vid_output != VID_OUTPUT_SDR)
	{
		vid_output = VID_OUTPUT_SDR;
		format = VID_ChooseFormat (vid_output, &space);
	}
	if (format == VK_FORMAT_UNDEFINED)
		Sys_Error ("The window takes no SDR format SoftWorld draws (10-bit or 8-bit UNORM, sRGB)");
	mode = VID_ChoosePresentMode ();
	for (alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ; alpha <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ; alpha <<= 1)
		if (caps.supportedCompositeAlpha & alpha)
			break;

	// nothing is destroyed that a frame in flight uses, or a present waits on
	VID_WaitFrame (vk_serial);
	vkQueueWaitIdle (vk_queue);
	VID_DestroySwapchainImages ();

	VID_Check (vkCreateSwapchainKHR (vk_device, &(VkSwapchainCreateInfoKHR){
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = vk_surface,
			// the fewest the mode takes: each more is a frame more that may be queued
			.minImageCount = caps.minImageCount,
			.imageFormat = format,
			.imageColorSpace = space,
			.imageExtent = vk_extent,
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = caps.currentTransform,
			.compositeAlpha = alpha,
			.presentMode = mode,
			.clipped = VK_TRUE,
			.oldSwapchain = old,
		}, NULL, &vk_swapchain), "vkCreateSwapchainKHR");
	if (old)
		vkDestroySwapchainKHR (vk_device, old, NULL);
	vk_presentid = 0;

	count = VID_MAX_IMAGES;
	VID_Check (vkGetSwapchainImagesKHR (vk_device, vk_swapchain, &count, vk_images), "vkGetSwapchainImagesKHR");
	for (i = 0 ; i < count ; i++)
	{
		VID_Check (vkCreateImageView (vk_device, &(VkImageViewCreateInfo){
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = vk_images[i],
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = format,
				.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			}, NULL, &vk_views[i]), "vkCreateImageView");
		VID_Check (vkCreateSemaphore (vk_device, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO},
			NULL, &vk_rendered[i]), "A present semaphore");
	}
	vk_numimages = count;

	if (mode != vk_presentmode || format != vk_format || space != vk_colorspace)
		Con_Printf ("Presenting with %s, %u images, %s\n", VID_PresentModeName (mode), count,
			VID_FormatName (format, space));
	vk_presentmode = mode;
	vk_format = format;
	vk_colorspace = space;
	VID_SetHdrMetadata ();
}

/*
===============================================================================

THE FRAME

===============================================================================
*/

static VkBuffer VID_CreateBuffer (VkDeviceSize size, uint32_t type, VkDeviceMemory *memory, VkDeviceAddress *address)
{
	VkBuffer				buffer;
	VkMemoryRequirements	req;

	VID_Check (vkCreateBuffer (vk_device, &(VkBufferCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
				| VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		}, NULL, &buffer), "A frame buffer");
	vkGetBufferMemoryRequirements (vk_device, buffer, &req);
	VID_Check (vkAllocateMemory (vk_device, &(VkMemoryAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.pNext = &(VkMemoryAllocateFlagsInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
				.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
			},
			.allocationSize = req.size,
			.memoryTypeIndex = type,
		}, NULL, memory), "A frame buffer's memory");
	VID_Check (vkBindBufferMemory (vk_device, buffer, *memory, 0), "vkBindBufferMemory");
	*address = vkGetBufferDeviceAddress (vk_device, &(VkBufferDeviceAddressInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer});
	return buffer;
}

static void VID_FreeLayer (vid_layer_t *layer)
{
	if (layer->buffer)
	{
		vkDestroyBuffer (vk_device, layer->buffer, NULL);
		vkFreeMemory (vk_device, layer->memory, NULL);
	}
	if (layer->copy)
	{
		vkDestroyBuffer (vk_device, layer->copy, NULL);
		vkFreeMemory (vk_device, layer->copymemory, NULL);
	}
	memset (layer, 0, sizeof(*layer));
}

static void VID_AllocLayer (vid_layer_t *layer, VkDeviceSize size)
{
	layer->buffer = VID_CreateBuffer (size, vk_hostmemory, &layer->memory, &layer->address);
	VID_Check (vkMapMemory (vk_device, layer->memory, 0, VK_WHOLE_SIZE, 0, &layer->data), "vkMapMemory");
	memset (layer->data, 0, (size_t)size);
	if (!vk_inplace)
		layer->copy = VID_CreateBuffer (size, vk_localmemory, &layer->copymemory, &layer->copyaddress);
	layer->serial = 0;
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
	VkDeviceSize	size = (VkDeviceSize)frame.width * (VkDeviceSize)frame.height * sizeof(pixel_t);
	int			i;

	// nothing is freed that a frame in flight reads
	VID_WaitFrame (vk_serial);
	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_FreeLayer (&vid_views[i]);
	for (i = 0 ; i < 2 ; i++)
		VID_FreeLayer (&vid_huds[i]);

	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_AllocLayer (&vid_views[i], size);
	for (i = 0 ; i < 2 ; i++)
		VID_AllocLayer (&vid_huds[i], size);

	vid_slot = vid_drawnslot = 0;
	vid_hudshown = vid_drawnhud = 0;
	vid.buffer = vid_views[vid_slot].data;
	vid.hud = vid_huds[vid_hudshown ^ 1].data;
	VID_SetFrame (&frame, (unsigned)frame.width);
}

// the frame last drawn, whether it was presented or not (screenshots, dumps)
void VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud)
{
	*frame = vid_views[vid_drawnslot].data;
	*hud = vid_huds[vid_drawnhud].data;
}

// frames in flight: one with vsync, where the display sets the pace and a second
// would be a refresh more of input lag; two without, so a frame is drawn while
// the last is presented
static void VID_SetLatency (void)
{
	int		latency = vid_vsync.value ? 1 : 2;

	if (latency == vid_latency)
		return;
	vid_latency = latency;
	vk_swapdirty = true;
}

/*
================
VID_Presentable

With vsync, every frame is presented. Without, the game doesn't wait for the
display: a frame is presented once the last was shown or passed over, as
present wait or the compositor's feedback tells; the frames between are
drawn but not shown.
================
*/
static bool VID_Presentable (void)
{
	if (vid_latency == 1 || !vk_presentid)
		return true;
	if (Sys_DoubleTime () - vid_presenttime > 0.1)
		return true;		// a frame the display never told of
	if (vk_presentwait)
		return vk_WaitForPresent (vk_device, vk_swapchain, vk_presentid, 0) == VK_SUCCESS;
	if (way.presentation)
		return WL_PresentStats ()->presented >= vk_presentid;
	return true;
}

static void VID_Barrier (VkCommandBuffer commands, const VkMemoryBarrier2 *memory, const VkImageMemoryBarrier2 *image)
{
	vkCmdPipelineBarrier2 (commands, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = memory ? 1 : 0,
		.pMemoryBarriers = memory,
		.imageMemoryBarrierCount = image ? 1 : 0,
		.pImageMemoryBarriers = image,
	});
}

// the layers the GPU reads: where the CPU drew them, or copied to the GPU's memory
static void VID_RecordCopies (VkCommandBuffer commands, vid_layer_t *view, vid_layer_t *hud, VkDeviceSize size)
{
	VkBufferCopy	region = {.size = size};

	if (vk_inplace)
		return;
	vkCmdCopyBuffer (commands, view->buffer, view->copy, 1, &region);
	vkCmdCopyBuffer (commands, hud->buffer, hud->copy, 1, &region);
	VID_Barrier (commands, &(VkMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
	}, NULL);
}

/*
================
VID_Update

Presents the frame the renderer drew, letterboxed into the window, and hands
it the buffers of the next once the GPU is done with them
================
*/
void VID_Update (void)
{
	VkCommandBuffer		commands;
	vid_present_push_t	push;
	vid_fit_t			fit;
	VkSemaphore			acquired;
	VkResult			result;
	uint32_t			index;
	int					slot = vid_slot, hud, a;
	float				paperwhite, peak;
	uint64_t			pace;
	vid_layer_t			*view, *layer2d;

	if (!vid_initialized || Minimized)
		return;

	// the 2D drawn this frame is shown from now on
	hud = vid.huddirty ? vid_hudshown ^ 1 : vid_hudshown;
	vid_drawnslot = slot;
	vid_drawnhud = hud;

	VID_UpdateClientSize ();
	if (client_width <= 0 || client_height <= 0)
		return;
	if (WL_PreferredColors ()->serial != vid_colorserial || vid_hdr.value != vid_hdrwanted)
		VID_CheckOutput ();
	VID_SetLatency ();
	if (vk_swapdirty)
		VID_CreateSwapchain ();
	if (!vk_numimages || !VID_Presentable ())
		return;

	// with vsync, the frame before is on the screen before this one is queued
	if (vid_latency == 1 && vk_presentwait && vk_presentid)
		vk_WaitForPresent (vk_device, vk_swapchain, vk_presentid, VID_PACE_NS);

	// an image to draw in; with none free, the frame is drawn again
	a = (int)(vk_serial % VID_ACQUIRES);
	acquired = vk_acquired[a];
	VID_WaitFrame (vk_acquiredserial[a]);
	result = vkAcquireNextImageKHR (vk_device, vk_swapchain, vid_latency == 1 ? VID_PACE_NS : 0, acquired,
		VK_NULL_HANDLE, &index);
	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		vk_swapdirty = true;
		return;
	}
	if (result == VK_TIMEOUT || result == VK_NOT_READY)
		return;
	VID_Check (result, "vkAcquireNextImageKHR");
	if (result == VK_SUBOPTIMAL_KHR)
		vk_swapdirty = true;

	view = &vid_views[slot];
	layer2d = &vid_huds[hud];
	fit = VID_Fit (client_width, client_height);
	paperwhite = vid_output == VID_OUTPUT_PQ ? vid_paperwhite : 1;
	peak = vid_output == VID_OUTPUT_PQ ? vid_peak : 1;
	VID_FillConstants (&push.constants, &fit, vid_output, paperwhite, peak);
	push.view[0] = (uint32_t)(vk_inplace ? view->address : view->copyaddress);
	push.view[1] = (uint32_t)((vk_inplace ? view->address : view->copyaddress) >> 32);
	push.hud[0] = (uint32_t)(vk_inplace ? layer2d->address : layer2d->copyaddress);
	push.hud[1] = (uint32_t)((vk_inplace ? layer2d->address : layer2d->copyaddress) >> 32);
	push.rowpixels = vid.rowpixels;

	// the slot's last frame is done: its buffer wasn't handed out before
	commands = vk_commands[slot];
	vkResetCommandBuffer (commands, 0);
	vkBeginCommandBuffer (commands, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	});
	VID_RecordCopies (commands, view, layer2d, (VkDeviceSize)vid.rowpixels * vid.height * sizeof(pixel_t));
	VID_Barrier (commands, NULL, &(VkImageMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.image = vk_images[index],
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	});
	vkCmdBeginRendering (commands, &(VkRenderingInfo){
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {{0, 0}, vk_extent},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &(VkRenderingAttachmentInfo){
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = vk_views[index],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = {.color = {{0, 0, 0, 1}}},
		},
	});
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_GRAPHICS, VID_Pipeline (vk_format));
	vkCmdSetViewport (commands, 0, 1, &(VkViewport){fit.x, fit.y, fit.width, fit.height, 0, 1});
	vkCmdSetScissor (commands, 0, 1, &(VkRect2D){{0, 0}, vk_extent});
	vkCmdPushConstants (commands, vk_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw (commands, 3, 1, 0, 0);
	vkCmdEndRendering (commands);
	VID_Barrier (commands, NULL, &(VkImageMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.image = vk_images[index],
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	});
	VID_Check (vkEndCommandBuffer (commands), "vkEndCommandBuffer");

	// drawn once the image is had; the image's semaphore and the frame's serial when done
	vk_serial++;
	VID_Check (vkQueueSubmit2 (vk_queue, 1, &(VkSubmitInfo2){
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1,
			.pWaitSemaphoreInfos = &(VkSemaphoreSubmitInfo){
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = acquired,
				.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			},
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &(VkCommandBufferSubmitInfo){
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = commands,
			},
			.signalSemaphoreInfoCount = 2,
			.pSignalSemaphoreInfos = (VkSemaphoreSubmitInfo[]){
				{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = vk_rendered[index],
					.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
				{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = vk_timeline, .value = vk_serial,
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
			},
		}, VK_NULL_HANDLE), "vkQueueSubmit2");
	vk_acquiredserial[a] = vk_serial;

	// the viewport's size and the compositor's feedback go with Vulkan's commit
	WL_SetFrameSize ((int)vk_extent.width, (int)vk_extent.height, vk_unitwidth, vk_unitheight);
	vid_presenttime = Sys_DoubleTime ();
	WL_BeforePresent (vk_serial, vid_presenttime);
	result = vkQueuePresentKHR (vk_queue, &(VkPresentInfoKHR){
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext = vk_presentwait ? &(VkPresentIdKHR){
			.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
			.swapchainCount = 1,
			.pPresentIds = &vk_serial,
		} : NULL,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vk_rendered[index],
		.swapchainCount = 1,
		.pSwapchains = &vk_swapchain,
		.pImageIndices = &index,
	});
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		vk_swapdirty = true;
	else
		VID_Check (result, "vkQueuePresentKHR");
	vk_presentid = vk_serial;

	view->serial = vk_serial;
	layer2d->serial = vk_serial;
	vid_hudshown = hud;
	vid.huddirty = false;

	// the pace: no more frames in flight than the latency (the wait may end
	// early; it is the one below that keeps the buffers safe)
	if (vk_serial >= (uint64_t)vid_latency)
	{
		pace = vk_serial - (uint64_t)vid_latency + 1;
		vkWaitSemaphores (vk_device, &(VkSemaphoreWaitInfo){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &vk_timeline,
			.pValues = &pace,
		}, VID_PACE_NS);
	}

	// the next frame is drawn in the other view, and a new 2D in the one not
	// shown, once the GPU is done with them
	vid_slot = slot ^ 1;
	VID_WaitFrame (vid_views[vid_slot].serial);
	VID_WaitFrame (vid_huds[vid_hudshown ^ 1].serial);
	vid.buffer = vid_views[vid_slot].data;
	vid.hud = vid_huds[vid_hudshown ^ 1].data;

	// the window was resized, or the video settings changed
	if (VID_NeedsResize (client_width, client_height))
		VID_SetScale ();
}

/*
===============================================================================

THE REPORT

===============================================================================
*/

static const char *VID_GPUType (VkPhysicalDeviceType type)
{
	switch (type)
	{
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:	return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:		return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:				return "software";
	default:										return "other";
	}
}

static void VID_PrintGPU (void)
{
	Con_Printf ("Vulkan: %s (%s, %s)\n", vk_gpuprops.deviceName, VID_GPUType (vk_gpuprops.deviceType),
		vk_compositorgpu ? "the compositor's" : "not the compositor's: its frames are copied between GPUs");
	if (vk_inplace)
		Con_Printf ("The GPU reads the frame where the CPU draws it\n");
	else
		Con_Printf ("The frame is copied to the GPU's own memory each frame\n");
}

static void VID_Info_f (void)
{
	const wl_present_stats_t	*stats = WL_PresentStats ();

	VID_PrintGPU ();
	Con_Printf ("Presenting with %s, %u images, %s, %ux%u\n", VID_PresentModeName (vk_presentmode), vk_numimages,
		VID_FormatName (vk_format, vk_colorspace), vk_extent.width, vk_extent.height);
	Con_Printf ("%s, %s\n", vk_presentwait ? "present wait" : "no present wait",
		vk_hdrmetadata ? "HDR metadata" : "no HDR metadata");
	Con_Printf ("Wayland:\n");
	WL_PrintProtocols (true);
	WL_PrintScanout ();
	if (!stats->count)
	{
		Con_Printf ("No frame shown has been told of (presentation-time)\n");
		return;
	}
	Con_Printf ("Frames shown %llu, passed over %llu, scanned out directly %llu\n", (unsigned long long)stats->count,
		(unsigned long long)stats->discarded, (unsigned long long)stats->zerocopy);
	Con_Printf ("The last: %s%s%s%s\n", stats->flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY
		? "scanned out directly" : "composited",
		stats->flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC ? ", at a refresh" : ", torn or unsynchronized",
		stats->flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK ? ", the display's clock" : "",
		stats->flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION ? ", the display's completion" : "");
	if (stats->refresh > 0)
		Con_Printf ("Refresh %.2f Hz\n", 1 / stats->refresh);
	Con_Printf ("From present to shown: %.2f ms on average, at most %.2f, over the last frames\n",
		stats->latency * 1000, stats->worst * 1000);
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
	Cmd_AddCommand ("vid_fullscreen", VID_Fullscreen_f);
	Cmd_AddCommand ("vid_info", VID_Info_f);

	if (!WL_Init (VID_BASE_WIDTH * scale, VID_BASE_HEIGHT * scale))
		Sys_Error ("SoftWorld runs on Wayland, and found no Wayland compositor (WAYLAND_DISPLAY)");
	VID_CreateInstance ();
	VID_CreateDevice ();
	VID_PrintGPU ();
	WL_PrintProtocols (false);
	WL_PrintScanout ();

	VID_CheckOutput ();
	VID_SetLatency ();
	VID_CreateSwapchain ();
	VID_SetScale ();
	vid_initialized = true;

	// the window's first frame is up before it is made fullscreen
	VID_Update ();
	if (COM_CheckParm ("-fullscreen"))
		VID_SetFullscreen (true);
}

void VID_Shutdown (void)
{
	int		i;

	if (!vid_initialized)
		return;
	vid_initialized = false;

	vkDeviceWaitIdle (vk_device);
	for (i = 0 ; i < VID_SLOTS ; i++)
		VID_FreeLayer (&vid_views[i]);
	for (i = 0 ; i < 2 ; i++)
		VID_FreeLayer (&vid_huds[i]);
	vid.buffer = NULL;
	vid.hud = NULL;

	VID_DestroySwapchainImages ();
	vkDestroySwapchainKHR (vk_device, vk_swapchain, NULL);
	for (i = 0 ; i < VID_PIPELINES ; i++)
		if (vk_pipelines[i].pipeline)
			vkDestroyPipeline (vk_device, vk_pipelines[i].pipeline, NULL);
	vkDestroyPipelineLayout (vk_device, vk_layout, NULL);
	vkDestroyShaderModule (vk_device, vk_vs, NULL);
	vkDestroyShaderModule (vk_device, vk_fs, NULL);
	for (i = 0 ; i < VID_ACQUIRES ; i++)
		vkDestroySemaphore (vk_device, vk_acquired[i], NULL);
	vkDestroySemaphore (vk_device, vk_timeline, NULL);
	vkDestroyCommandPool (vk_device, vk_pool, NULL);
	vkDestroyDevice (vk_device, NULL);
	vkDestroySurfaceKHR (vk_instance, vk_surface, NULL);
	vkDestroyInstance (vk_instance, NULL);
	WL_Shutdown ();
}

void VID_SetCaption (const char *text)
{
	WL_SetTitle (text);
}

void VID_BringToFront (void)
{
	WL_Activate ();
}

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
	return WL_IsFullscreen ();
}

const char *VID_GPUName (void)
{
	return vk_gpuprops.deviceName;
}
