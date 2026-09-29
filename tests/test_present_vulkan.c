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
// test_present_vulkan.c -- present.glsl drawn with Vulkan as vid_vulkan.c draws it,
// from buffers laid out as the renderer fills them and read through their device
// addresses, into a float target read back: in SDR each pixel within 1 of what
// VID_FrameToRGB (screenshots) makes of it, with and without gamma and a view blend;
// scaled by 2.5, sharp bilinear, each pixel inside a texel that texel's, and each
// on an edge between its two; in linear HDR, SDR white at paper white, the
// brightest light rolled off below the peak, and the 2D at paper white; and as PQ,
// the same in cd/m² over BT.2020. Without a GPU with Vulkan 1.3 it is skipped.

#include "cvar.h"
#include "sys.h"
#include "vid_common.h"
#include "present_spirv.h"

#include <vulkan/vulkan.h>

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
#define SKIPPED		77

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

static const pixel_t	*shown_frame;
static const hudpixel_t	*shown_hud;

void VID_ShownLayers (const pixel_t **frame, const hudpixel_t **hud)
{
	*frame = shown_frame;
	*hud = shown_hud;
}

//
// Vulkan
//

typedef struct
{
	VkBuffer		buffer;
	VkDeviceMemory	memory;
	void			*data;
	VkDeviceAddress	address;
} buffer_t;

static VkInstance		instance;
static VkPhysicalDevice	gpu;
static VkDevice			device;
static VkQueue			queue;
static VkCommandPool	pool;
static VkCommandBuffer	commands;
static VkFence			fence;
static VkPipelineLayout	layout;
static VkPipeline		pipeline;
static VkImage			target;
static VkImageView		targetview;
static VkDeviceMemory	targetmemory;
static buffer_t			framebuf, hudbuf, readback;

static void Check (VkResult result, const char *what)
{
	if (result != VK_SUCCESS)
		Sys_Error ("%s failed (%d)", what, (int)result);
}

static uint32_t MemoryType (uint32_t types, VkMemoryPropertyFlags need)
{
	VkPhysicalDeviceMemoryProperties	memory;
	uint32_t	i;

	vkGetPhysicalDeviceMemoryProperties (gpu, &memory);
	for (i = 0 ; i < memory.memoryTypeCount ; i++)
		if ((types & (1u << i)) && (memory.memoryTypes[i].propertyFlags & need) == need)
			return i;
	Sys_Error ("no memory type 0x%x", need);
}

static void CreateBuffer (buffer_t *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkMemoryRequirements	req;

	Check (vkCreateBuffer (device, &(VkBufferCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		}, NULL, &b->buffer), "vkCreateBuffer");
	vkGetBufferMemoryRequirements (device, b->buffer, &req);
	Check (vkAllocateMemory (device, &(VkMemoryAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.pNext = &(VkMemoryAllocateFlagsInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
				.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
			},
			.allocationSize = req.size,
			.memoryTypeIndex = MemoryType (req.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
		}, NULL, &b->memory), "vkAllocateMemory");
	Check (vkBindBufferMemory (device, b->buffer, b->memory, 0), "vkBindBufferMemory");
	Check (vkMapMemory (device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->data), "vkMapMemory");
	memset (b->data, 0, (size_t)size);
	b->address = vkGetBufferDeviceAddress (device, &(VkBufferDeviceAddressInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b->buffer});
}

static VkShaderModule ShaderModule (const unsigned char *code, size_t size)
{
	VkShaderModule	module;

	Check (vkCreateShaderModule (device, &(VkShaderModuleCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = size,
			.pCode = (const uint32_t *)code,
		}, NULL, &module), "vkCreateShaderModule");
	return module;
}

// false if there is no GPU to test on
static bool Setup (void)
{
	static const VkDynamicState	dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	static const VkFormat		format = VK_FORMAT_R32G32B32A32_SFLOAT;
	VkPhysicalDevice			gpus[16];
	VkPhysicalDeviceProperties	props;
	VkQueueFamilyProperties		families[16];
	VkMemoryRequirements		req;
	uint32_t					count = 16, numfamilies = 16, family = 0, i;
	VkShaderModule				vs, fs;
	VkPhysicalDeviceVulkan13Features	f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features	f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &f13};
	VkPhysicalDeviceFeatures2			f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};

	if (vkCreateInstance (&(VkInstanceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &(VkApplicationInfo){
				.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				.apiVersion = VK_API_VERSION_1_3,
			},
		}, NULL, &instance) != VK_SUCCESS)
		return false;
	if (vkEnumeratePhysicalDevices (instance, &count, gpus) < 0)
		return false;
	for (i = 0 ; i < count && !gpu ; i++)
	{
		vkGetPhysicalDeviceProperties (gpus[i], &props);
		vkGetPhysicalDeviceFeatures2 (gpus[i], &f2);
		if (props.apiVersion >= VK_API_VERSION_1_3 && f12.bufferDeviceAddress && f13.dynamicRendering
			&& f13.synchronization2)
			gpu = gpus[i];
	}
	if (!gpu)
		return false;
	printf ("present: on %s\n", props.deviceName);

	vkGetPhysicalDeviceQueueFamilyProperties (gpu, &numfamilies, families);
	while (family < numfamilies && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		family++;
	Check (vkCreateDevice (gpu, &(VkDeviceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &(VkPhysicalDeviceVulkan12Features){
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
				.pNext = &(VkPhysicalDeviceVulkan13Features){
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
					.dynamicRendering = VK_TRUE,
					.synchronization2 = VK_TRUE,
				},
				.bufferDeviceAddress = VK_TRUE,
			},
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = family,
				.queueCount = 1,
				.pQueuePriorities = &(float){1.0f},
			},
		}, NULL, &device), "vkCreateDevice");
	vkGetDeviceQueue (device, family, 0, &queue);
	Check (vkCreateCommandPool (device, &(VkCommandPoolCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = family,
		}, NULL, &pool), "vkCreateCommandPool");
	Check (vkAllocateCommandBuffers (device, &(VkCommandBufferAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		}, &commands), "vkAllocateCommandBuffers");
	Check (vkCreateFence (device, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence),
		"vkCreateFence");

	// the pipeline as vid_vulkan.c makes it, for a float target
	vs = ShaderModule (vid_present_vert, vid_present_vert_size);
	fs = ShaderModule (vid_present_frag, vid_present_frag_size);
	Check (vkCreatePipelineLayout (device, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &(VkPushConstantRange){
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.size = sizeof(vid_present_push_t),
			},
		}, NULL, &layout), "vkCreatePipelineLayout");
	Check (vkCreateGraphicsPipelines (device, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &(VkPipelineRenderingCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.colorAttachmentCount = 1,
				.pColorAttachmentFormats = &format,
			},
			.stageCount = 2,
			.pStages = (VkPipelineShaderStageCreateInfo[]){
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
					.module = vs, .pName = "main"},
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
					.module = fs, .pName = "main"},
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
			.layout = layout,
		}, NULL, &pipeline), "vkCreateGraphicsPipelines");

	// the layers as vid_vulkan.c makes them, rows the frame's width apart
	CreateBuffer (&framebuf, WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	CreateBuffer (&hudbuf, WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	CreateBuffer (&readback, MAX_WIDTH * MAX_HEIGHT * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

	Check (vkCreateImage (device, &(VkImageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = format,
			.extent = {MAX_WIDTH, MAX_HEIGHT, 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		}, NULL, &target), "vkCreateImage");
	vkGetImageMemoryRequirements (device, target, &req);
	Check (vkAllocateMemory (device, &(VkMemoryAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = MemoryType (req.memoryTypeBits, 0),
		}, NULL, &targetmemory), "vkAllocateMemory");
	Check (vkBindImageMemory (device, target, targetmemory, 0), "vkBindImageMemory");
	Check (vkCreateImageView (device, &(VkImageViewCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = target,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		}, NULL, &targetview), "vkCreateImageView");

	vid.width = WIDTH;
	vid.height = HEIGHT;
	vid.rowpixels = WIDTH;
	shown_frame = framebuf.data;
	shown_hud = hudbuf.data;
	return true;
}

// the frame as the shader shows it into width x height, one float RGBA a pixel
static void Draw (int output, float paperwhite, float peak, int width, int height, float *out)
{
	vid_fit_t			fit = {0, 0, (float)width, (float)height, (float)width / WIDTH, 1};
	vid_present_push_t	push;

	VID_FillConstants (&push.constants, &fit, output, paperwhite, peak);
	push.view[0] = (uint32_t)framebuf.address;
	push.view[1] = (uint32_t)(framebuf.address >> 32);
	push.hud[0] = (uint32_t)hudbuf.address;
	push.hud[1] = (uint32_t)(hudbuf.address >> 32);
	push.rowpixels = vid.rowpixels;

	vkResetCommandBuffer (commands, 0);
	vkBeginCommandBuffer (commands, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
	vkCmdPipelineBarrier2 (commands, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &(VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.image = target,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
	});
	vkCmdBeginRendering (commands, &(VkRenderingInfo){
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {{0, 0}, {(uint32_t)width, (uint32_t)height}},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &(VkRenderingAttachmentInfo){
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = targetview,
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		},
	});
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdSetViewport (commands, 0, 1, &(VkViewport){0, 0, (float)width, (float)height, 0, 1});
	vkCmdSetScissor (commands, 0, 1, &(VkRect2D){{0, 0}, {(uint32_t)width, (uint32_t)height}});
	vkCmdPushConstants (commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw (commands, 3, 1, 0, 0);
	vkCmdEndRendering (commands);
	vkCmdPipelineBarrier2 (commands, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &(VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.image = target,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
	});
	vkCmdCopyImageToBuffer (commands, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
		&(VkBufferImageCopy){
			.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			.imageExtent = {(uint32_t)width, (uint32_t)height, 1},
		});
	vkCmdPipelineBarrier2 (commands, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &(VkMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
			.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		},
	});
	Check (vkEndCommandBuffer (commands), "vkEndCommandBuffer");
	Check (vkQueueSubmit (queue, 1, &(VkSubmitInfo){
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &commands,
		}, fence), "vkQueueSubmit");
	Check (vkWaitForFences (device, 1, &fence, VK_TRUE, 5000000000ull), "the GPU didn't finish");
	vkResetFences (device, 1, &fence);
	memcpy (out, readback.data, (size_t)width * (size_t)height * 16);
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
	pixel_t		*frame = framebuf.data;
	hudpixel_t	*hud = hudbuf.data;
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

static void TestSDR (const char *what)
{
	static float	out[WIDTH * HEIGHT * 4];
	static byte		rgb[WIDTH * HEIGHT * 3];
	int				i, c, d, worst = 0, off = 0;

	Draw (VID_OUTPUT_SDR, 1, 1, WIDTH, HEIGHT, out);
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
static void TestSharp (void)
{
	static float	one[WIDTH * HEIGHT * 4], scaled[MAX_WIDTH * MAX_HEIGHT * 4];
	pixel_t			*frame = framebuf.data;
	int				x, y, c, tx, ty, off = 0, inside = 0, edges = 0;
	float			fx, fy, got, a, b, lo, hi;

	for (x = 0 ; x < WIDTH * HEIGHT ; x++)
		frame[x] = RGB30 (Rand () % 513, Rand () % 513, Rand () % 513);
	memset (hudbuf.data, 0, WIDTH * HEIGHT * 4);

	Draw (VID_OUTPUT_SDR, 1, 1, WIDTH, HEIGHT, one);
	Draw (VID_OUTPUT_SDR, 1, 1, MAX_WIDTH, MAX_HEIGHT, scaled);
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

// PQ's code to cd/m² (SMPTE ST 2084)
static float PQToNits (float e)
{
	const double	m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double	c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	double			p = pow (e, 1 / m2);

	return (float)(10000 * pow (fmax (p - c1, 0) / (c2 - c3 * p), 1 / m1));
}

static void TestHDR (void)
{
	static float	out[WIDTH * HEIGHT * 4];
	pixel_t			*frame = framebuf.data;
	hudpixel_t		*hud = hudbuf.data;

	memset (hudbuf.data, 0, WIDTH * HEIGHT * 4);
	frame[0] = RGB30 (RGB30_WHITE, RGB30_WHITE, RGB30_WHITE);
	frame[1] = RGB30 (1023, 1023, 1023);
	frame[2] = RGB30 (1023, 0, 0);
	frame[3] = RGB30 (0, 0, 0);
	frame[4] = RGB30 (0, 0, 0);
	hud[3] = HUD_RGBA (255, 255, 255, 255);

	// paper white 2, peak 6: white is 2, the brightest light (15.9) is under 6, the 2D's white is 2
	Draw (VID_OUTPUT_LINEAR, 2, 6, WIDTH, HEIGHT, out);
	Expect ("HDR white", out[0], 1.99f, 2.01f);
	Expect ("HDR brightest", out[4], 4.5f, 6.0f);
	Expect ("HDR red keeps its hue: green", out[9], 0, 0.001f);
	Expect ("HDR 2D white", out[12], 1.99f, 2.01f);

	// no headroom yet: what is brighter than white goes toward white
	Draw (VID_OUTPUT_LINEAR, 1, 1, WIDTH, HEIGHT, out);
	Expect ("HDR without headroom, white", out[0], 0.99f, 1.01f);
	Expect ("HDR without headroom, brightest", out[4], 0.99f, 1.01f);
	Expect ("HDR without headroom, bright red: green", out[9], 0.5f, 1.0f);

	// PQ, 1 being reference white at 203 cd/m²: the same, in cd/m², with BT.709's
	// red as BT.2020 has it (green 0.0691 of the light, red 0.6274)
	Draw (VID_OUTPUT_PQ, 2, 6, WIDTH, HEIGHT, out);
	Expect ("PQ white", PQToNits (out[0]), 2 * 203 * 0.99f, 2 * 203 * 1.01f);
	Expect ("PQ white, blue", PQToNits (out[2]), 2 * 203 * 0.99f, 2 * 203 * 1.01f);
	Expect ("PQ brightest", PQToNits (out[4]), 4.5f * 203, 6.0f * 203 * 1.01f);
	Expect ("PQ red in BT.2020: green over red", PQToNits (out[9]) / PQToNits (out[8]), 0.105f, 0.115f);
	Expect ("PQ black", PQToNits (out[16]), 0, 0.01f);
	Expect ("PQ 2D white", PQToNits (out[12]), 2 * 203 * 0.99f, 2 * 203 * 1.01f);
}

int main (void)
{
	if (!Setup ())
	{
		printf ("present: no GPU with Vulkan 1.3, skipped\n");
		return SKIPPED;
	}

	Fill ();
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestSDR ("SDR");
	TestSharp ();

	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	TestSDR ("SDR, gamma 0.8 and a blend");

	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestHDR ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present: the shader shows what screenshots do\n");
	return 0;
}
