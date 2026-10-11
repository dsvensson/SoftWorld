// crt_vulkan.c -- the CRT shader's passes (vid_crt.h) in Vulkan: crt.glsl's
// SPIR-V (crt_spirv.c), an image each pass draws in, the LUTs, and PrePass's
// mip chain, crt_spd's. A pass's parameters are a uniform buffer, a region a
// frame in flight (a dynamic offset), its sizes push constants, its textures
// combined image samplers, a descriptor set for each pass and each of the two
// ways the feedback textures stand. Each image's layout is followed as the
// commands are recorded, all of them submitted in the order recorded. Nothing
// here exists until the shader is first turned on.

#include "mem.h"
#include "print.h"
#include "crt_spirv.h"
#include "crt_vulkan.h"

#include <string.h>

#define CRT_MAX_LEVELS	16		// mip levels of a texture at most
#define CRT_FORMATS		4		// the last pass's pipelines, one a swapchain format

typedef struct
{
	VkImage			image;
	VkDeviceMemory	memory;
	VkImageView		view;					// all its levels, sampled
	VkImageView		levels[CRT_MAX_LEVELS];	// each alone: the first drawn in, PrePass's for crt_spd
	int				width, height;
	int				numlevels;
	VkImageLayout	layout;					// all its levels', as the commands recorded leave it
	bool			clear;					// cleared before it's next read
} crt_image_t;

typedef struct
{
	VkBuffer		buffer;
	VkDeviceMemory	memory;
	void			*data;
} crt_buffer_t;

static VkPhysicalDevice			crt_gpu;
static VkDevice					crt_device;
static VkSampler				crt_samplers[2];		// nearest, bilinear; a black border
static VkDescriptorSetLayout	crt_setlayout, crt_spdsetlayout;
static VkPipelineLayout			crt_layout, crt_spdlayout;
static VkDescriptorPool			crt_pool;
static VkDescriptorSet			crt_sets[CRT_PASSES][2];	// a pass's, by the frame's parity
static VkDescriptorSet			crt_spdsets[CRT_SPD_MAX_DISPATCHES];
static VkShaderModule			crt_vs, crt_fs[CRT_PASSES], crt_cs;
static VkPipeline				crt_pipelines[CRT_PASSES];	// but the last's
static struct { VkFormat format; VkPipeline pipeline; }	crt_lastpipelines[CRT_FORMATS];
static VkPipeline				crt_spdpipeline;
static crt_buffer_t				crt_params;				// a region a frame in flight
static VkDeviceSize				crt_paramsstride;
static crt_buffer_t				crt_spdbuffer;			// the counter, then level 6's texels
static crt_image_t				crt_images[CRT_TEXTURES];
static int						crt_sizes[4];			// the source's and viewport's the images are for
static vid_crt_spd_t			crt_spd[CRT_SPD_MAX_DISPATCHES];
static int						crt_spdgroups[CRT_SPD_MAX_DISPATCHES][2];
static int						crt_spddispatches;
static vid_crt_pass_t			crt_last;				// the last pass's constants, set, offset
static int						crt_lastparity;
static uint32_t					crt_lastoffset;

static bool CRT_Vulkan_Check (VkResult result, const char *what)
{
	if (result >= 0)
		return true;
	Con_Printf ("The CRT shader: %s failed (%d)\n", what, (int)result);
	return false;
}

static uint32_t CRT_Vulkan_MemoryType (uint32_t types, VkMemoryPropertyFlags need)
{
	VkPhysicalDeviceMemoryProperties	memory;
	uint32_t	i;

	vkGetPhysicalDeviceMemoryProperties (crt_gpu, &memory);
	for (i = 0 ; i < memory.memoryTypeCount ; i++)
		if ((types & (1u << i)) && (memory.memoryTypes[i].propertyFlags & need) == need)
			return i;
	return UINT32_MAX;
}

// a buffer the CPU writes, mapped, zeroed
static bool CRT_Vulkan_CreateBuffer (crt_buffer_t *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkMemoryRequirements	req;
	uint32_t				type;

	if (!CRT_Vulkan_Check (vkCreateBuffer (crt_device, &(VkBufferCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = usage,
		}, NULL, &b->buffer), "vkCreateBuffer"))
		return false;
	vkGetBufferMemoryRequirements (crt_device, b->buffer, &req);
	type = CRT_Vulkan_MemoryType (req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX || !CRT_Vulkan_Check (vkAllocateMemory (crt_device, &(VkMemoryAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = type,
		}, NULL, &b->memory), "vkAllocateMemory"))
		return false;
	if (!CRT_Vulkan_Check (vkBindBufferMemory (crt_device, b->buffer, b->memory, 0), "vkBindBufferMemory")
		|| !CRT_Vulkan_Check (vkMapMemory (crt_device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->data), "vkMapMemory"))
		return false;
	memset (b->data, 0, (size_t)size);
	return true;
}

static void CRT_Vulkan_FreeBuffer (crt_buffer_t *b)
{
	if (b->buffer)
		vkDestroyBuffer (crt_device, b->buffer, NULL);
	if (b->memory)
		vkFreeMemory (crt_device, b->memory, NULL);
	memset (b, 0, sizeof(*b));
}

static void CRT_Vulkan_FreeImage (crt_image_t *t)
{
	int		i;

	for (i = 0 ; i < CRT_MAX_LEVELS ; i++)
		if (t->levels[i] && t->levels[i] != t->view)
			vkDestroyImageView (crt_device, t->levels[i], NULL);
	if (t->view)
		vkDestroyImageView (crt_device, t->view, NULL);
	if (t->image)
		vkDestroyImage (crt_device, t->image, NULL);
	if (t->memory)
		vkFreeMemory (crt_device, t->memory, NULL);
	memset (t, 0, sizeof(*t));
}

static VkImageView CRT_Vulkan_View (VkImage image, VkFormat format, int level, int count)
{
	VkImageView	view = VK_NULL_HANDLE;

	CRT_Vulkan_Check (vkCreateImageView (crt_device, &(VkImageViewCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, (uint32_t)count, 0, 1},
		}, NULL, &view), "vkCreateImageView");
	return view;
}

// an image of vid_crt_textures' kind, in the GPU's own memory, its layout undefined
static bool CRT_Vulkan_CreateImage (int index, int width, int height)
{
	const vid_crt_texture_t	*kind = &vid_crt_textures[index];
	crt_image_t				*t = &crt_images[index];
	VkFormat				format = kind->format == CRT_RGBA16F ? VK_FORMAT_R16G16B16A16_SFLOAT
								: VK_FORMAT_R8G8B8A8_UNORM;
	VkImageUsageFlags		usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkMemoryRequirements	req;
	uint32_t				type;
	int						i;

	CRT_Vulkan_FreeImage (t);
	t->numlevels = kind->mips ? VID_CRT_MipLevels (width, height) : 1;
	if (t->numlevels > CRT_MAX_LEVELS)
		t->numlevels = CRT_MAX_LEVELS;
	if (kind->size != CRT_SIZE_LUT)
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (kind->mips)
		usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;	// crt_spd's, and the tests read it back
	if (!CRT_Vulkan_Check (vkCreateImage (crt_device, &(VkImageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = format,
			.extent = {(uint32_t)width, (uint32_t)height, 1},
			.mipLevels = (uint32_t)t->numlevels,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = usage,
		}, NULL, &t->image), "vkCreateImage"))
		return false;
	vkGetImageMemoryRequirements (crt_device, t->image, &req);
	type = CRT_Vulkan_MemoryType (req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = CRT_Vulkan_MemoryType (req.memoryTypeBits, 0);
	if (!CRT_Vulkan_Check (vkAllocateMemory (crt_device, &(VkMemoryAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = type,
		}, NULL, &t->memory), "vkAllocateMemory")
		|| !CRT_Vulkan_Check (vkBindImageMemory (crt_device, t->image, t->memory, 0), "vkBindImageMemory"))
		return false;
	t->view = CRT_Vulkan_View (t->image, format, 0, t->numlevels);
	if (!t->view)
		return false;
	t->levels[0] = t->view;
	if (t->numlevels > 1)
		for (i = 0 ; i < t->numlevels ; i++)
			if (!(t->levels[i] = CRT_Vulkan_View (t->image, format, i, 1)))
				return false;
	t->width = width;
	t->height = height;
	t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	t->clear = kind->feedback;
	return true;
}

static VkShaderModule CRT_Vulkan_Module (const unsigned char *code, size_t size)
{
	VkShaderModule	module = VK_NULL_HANDLE;

	CRT_Vulkan_Check (vkCreateShaderModule (crt_device, &(VkShaderModuleCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = size,
			.pCode = (const uint32_t *)code,
		}, NULL, &module), "vkCreateShaderModule");
	return module;
}

// a pass's pipeline, drawing in an image of format
static VkPipeline CRT_Vulkan_Pipeline (VkShaderModule fs, VkFormat format)
{
	static const VkDynamicState	dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipeline	pipeline = VK_NULL_HANDLE;

	CRT_Vulkan_Check (vkCreateGraphicsPipelines (crt_device, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &(VkPipelineRenderingCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.colorAttachmentCount = 1,
				.pColorAttachmentFormats = &format,
			},
			.stageCount = 2,
			.pStages = (VkPipelineShaderStageCreateInfo[]){
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
					.module = crt_vs, .pName = "main"},
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
			.layout = crt_layout,
		}, NULL, &pipeline), "vkCreateGraphicsPipelines");
	return pipeline;
}

static bool CRT_Vulkan_CreateSampler (VkFilter filter, VkSamplerMipmapMode mipmap, VkSampler *sampler)
{
	// black past the edges, as RetroArch's default wrap mode is
	return CRT_Vulkan_Check (vkCreateSampler (crt_device, &(VkSamplerCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = filter,
			.minFilter = filter,
			.mipmapMode = mipmap,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			.maxLod = VK_LOD_CLAMP_NONE,
			.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		}, NULL, sampler), "vkCreateSampler");
}

static void CRT_Vulkan_Barrier (VkCommandBuffer commands, const VkImageMemoryBarrier2 *image, int numimages,
	const VkMemoryBarrier2 *memory)
{
	vkCmdPipelineBarrier2 (commands, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = memory ? 1 : 0,
		.pMemoryBarriers = memory,
		.imageMemoryBarrierCount = (uint32_t)numimages,
		.pImageMemoryBarriers = image,
	});
}

// what last used an image in a layout, for a barrier after it
static void CRT_Vulkan_LayoutUse (VkImageLayout layout, VkPipelineStageFlags2 *stage, VkAccessFlags2 *access)
{
	switch (layout)
	{
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		*stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		*access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		// read the frame before: no writes to make visible
		*stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		*access = 0;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
		*stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		*access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		*stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
		*access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		break;
	default:
		*stage = VK_PIPELINE_STAGE_2_NONE;
		*access = 0;
		break;
	}
}

// an image, all its levels, from its layout to another: its contents kept, or
// for one about to be drawn over discarded
static void CRT_Vulkan_Transition (VkCommandBuffer commands, crt_image_t *t, VkImageLayout layout,
	VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool discard)
{
	VkPipelineStageFlags2	srcstage;
	VkAccessFlags2			srcaccess;

	CRT_Vulkan_LayoutUse (t->layout, &srcstage, &srcaccess);
	CRT_Vulkan_Barrier (commands, &(VkImageMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = srcstage,
		.srcAccessMask = srcaccess,
		.dstStageMask = stage,
		.dstAccessMask = access,
		.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : t->layout,
		.newLayout = layout,
		.image = t->image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1},
	}, 1, NULL);
	t->layout = layout;
}

// an image the passes read
static void CRT_Vulkan_ToRead (VkCommandBuffer commands, crt_image_t *t)
{
	if (t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		CRT_Vulkan_Transition (commands, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
}

// an image a pass draws all over
static void CRT_Vulkan_ToDraw (VkCommandBuffer commands, crt_image_t *t)
{
	CRT_Vulkan_Transition (commands, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
}

static void CRT_Vulkan_BeginRendering (VkCommandBuffer commands, const crt_image_t *t)
{
	vkCmdBeginRendering (commands, &(VkRenderingInfo){
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {{0, 0}, {(uint32_t)t->width, (uint32_t)t->height}},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &(VkRenderingAttachmentInfo){
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = t->levels[0],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		},
	});
	vkCmdSetViewport (commands, 0, 1, &(VkViewport){0, 0, (float)t->width, (float)t->height, 0, 1});
	vkCmdSetScissor (commands, 0, 1, &(VkRect2D){{0, 0}, {(uint32_t)t->width, (uint32_t)t->height}});
}

// the LUTs into their images, on queue, waited for
static bool CRT_Vulkan_UploadLUTs (VkQueue queue, uint32_t family)
{
	VkCommandPool	pool = VK_NULL_HANDLE;
	VkCommandBuffer	commands;
	crt_buffer_t	staging = {0};
	byte			*pixels;
	int				i, w, h;
	bool			ok;

	if (!CRT_Vulkan_CreateBuffer (&staging, 4 * 1024 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
	{
		CRT_Vulkan_FreeBuffer (&staging);
		return false;
	}
	ok = CRT_Vulkan_Check (vkCreateCommandPool (crt_device, &(VkCommandPoolCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			.queueFamilyIndex = family,
		}, NULL, &pool), "vkCreateCommandPool")
		&& CRT_Vulkan_Check (vkAllocateCommandBuffers (crt_device, &(VkCommandBufferAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		}, &commands), "vkAllocateCommandBuffers");
	if (ok)
	{
		vkBeginCommandBuffer (commands, &(VkCommandBufferBeginInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		});
		for (i = 0 ; i < 4 && ok ; i++)
		{
			pixels = VID_CRT_LoadLUT (i, &w, &h);
			if (!pixels || w != 1024 || h != 32 || !CRT_Vulkan_CreateImage (CRT_LUT1 + i, w, h))
			{
				Con_Printf ("The CRT shader: LUT %d is broken\n", i + 1);
				Mem_Free (pixels);
				ok = false;
				break;
			}
			memcpy ((byte *)staging.data + i * 1024 * 32 * 4, pixels, 1024 * 32 * 4);
			Mem_Free (pixels);
			CRT_Vulkan_Transition (commands, &crt_images[CRT_LUT1 + i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
			vkCmdCopyBufferToImage (commands, staging.buffer, crt_images[CRT_LUT1 + i].image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
					.bufferOffset = (VkDeviceSize)i * 1024 * 32 * 4,
					.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
					.imageExtent = {1024, 32, 1},
				});
			CRT_Vulkan_ToRead (commands, &crt_images[CRT_LUT1 + i]);
		}
		vkEndCommandBuffer (commands);
		ok = ok && CRT_Vulkan_Check (vkQueueSubmit (queue, 1, &(VkSubmitInfo){
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &commands,
			}, VK_NULL_HANDLE), "vkQueueSubmit");
		vkQueueWaitIdle (queue);
	}
	if (pool)
		vkDestroyCommandPool (crt_device, pool, NULL);
	CRT_Vulkan_FreeBuffer (&staging);
	return ok;
}

bool CRT_Vulkan_Init (VkPhysicalDevice gpu, VkDevice device, VkQueue queue, uint32_t family, int slots)
{
	VkPhysicalDeviceProperties	props;
	VkDescriptorSetLayoutBinding	bindings[1 + 8];
	VkDescriptorSetLayout		setlayouts[CRT_PASSES * 2];
	int							i;
	bool						ok;

	crt_gpu = gpu;
	crt_device = device;
	vkGetPhysicalDeviceProperties (gpu, &props);

	// a pass's: its parameters at an offset a frame in flight, its textures
	bindings[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
	for (i = 1 ; i <= 8 ; i++)
		bindings[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
	ok = CRT_Vulkan_Check (vkCreateDescriptorSetLayout (device, &(VkDescriptorSetLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 9,
			.pBindings = bindings,
		}, NULL, &crt_setlayout), "vkCreateDescriptorSetLayout");
	// crt_spd's: the base level, the levels it makes, the buffer
	ok = ok && CRT_Vulkan_Check (vkCreateDescriptorSetLayout (device, &(VkDescriptorSetLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 3,
			.pBindings = (VkDescriptorSetLayoutBinding[]){
				{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
				{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CRT_SPD_LEVELS, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
				{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
			},
		}, NULL, &crt_spdsetlayout), "vkCreateDescriptorSetLayout");
	ok = ok && CRT_Vulkan_Check (vkCreatePipelineLayout (device, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &crt_setlayout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vid_crt_pass_t)},
		}, NULL, &crt_layout), "vkCreatePipelineLayout");
	ok = ok && CRT_Vulkan_Check (vkCreatePipelineLayout (device, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &crt_spdsetlayout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vid_crt_spd_t)},
		}, NULL, &crt_spdlayout), "vkCreatePipelineLayout");
	ok = ok && CRT_Vulkan_Check (vkCreateDescriptorPool (device, &(VkDescriptorPoolCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = CRT_PASSES * 2 + CRT_SPD_MAX_DISPATCHES,
			.poolSizeCount = 4,
			.pPoolSizes = (VkDescriptorPoolSize[]){
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, CRT_PASSES * 2},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CRT_PASSES * 2 * 8},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CRT_SPD_MAX_DISPATCHES * (1 + CRT_SPD_LEVELS)},
				{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, CRT_SPD_MAX_DISPATCHES},
			},
		}, NULL, &crt_pool), "vkCreateDescriptorPool");
	for (i = 0 ; i < CRT_PASSES * 2 ; i++)
		setlayouts[i] = crt_setlayout;
	ok = ok && CRT_Vulkan_Check (vkAllocateDescriptorSets (device, &(VkDescriptorSetAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = crt_pool,
			.descriptorSetCount = CRT_PASSES * 2,
			.pSetLayouts = setlayouts,
		}, &crt_sets[0][0]), "vkAllocateDescriptorSets");
	for (i = 0 ; i < CRT_SPD_MAX_DISPATCHES ; i++)
		setlayouts[i] = crt_spdsetlayout;
	ok = ok && CRT_Vulkan_Check (vkAllocateDescriptorSets (device, &(VkDescriptorSetAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = crt_pool,
			.descriptorSetCount = CRT_SPD_MAX_DISPATCHES,
			.pSetLayouts = setlayouts,
		}, crt_spdsets), "vkAllocateDescriptorSets");

	ok = ok && CRT_Vulkan_CreateSampler (VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, &crt_samplers[0])
		&& CRT_Vulkan_CreateSampler (VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, &crt_samplers[1]);

	// the parameters, a region a frame in flight where a dynamic offset may start
	crt_paramsstride = (VID_CRT_PARAMS_SIZE + props.limits.minUniformBufferOffsetAlignment - 1)
		/ props.limits.minUniformBufferOffsetAlignment * props.limits.minUniformBufferOffsetAlignment;
	ok = ok && CRT_Vulkan_CreateBuffer (&crt_params, crt_paramsstride * (VkDeviceSize)slots,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	ok = ok && CRT_Vulkan_CreateBuffer (&crt_spdbuffer, CRT_SPD_BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	// the shaders, and the pipelines of all passes but the last, whose format is the window's
	ok = ok && (crt_vs = CRT_Vulkan_Module (vid_crt_vert, vid_crt_vert_size)) != VK_NULL_HANDLE
		&& (crt_cs = CRT_Vulkan_Module (vid_crt_comp, vid_crt_comp_size)) != VK_NULL_HANDLE;
	for (i = 0 ; i < CRT_PASSES && ok ; i++)
		ok = (crt_fs[i] = CRT_Vulkan_Module (vid_crt_frag[i].code, vid_crt_frag[i].size)) != VK_NULL_HANDLE;
	for (i = 0 ; i < CRT_PASSES && ok ; i++)
		if (vid_crt_passes[i].output != CRT_SCREEN)
			ok = (crt_pipelines[i] = CRT_Vulkan_Pipeline (crt_fs[i],
				vid_crt_textures[vid_crt_passes[i].output].format == CRT_RGBA16F ? VK_FORMAT_R16G16B16A16_SFLOAT
				: VK_FORMAT_R8G8B8A8_UNORM)) != VK_NULL_HANDLE;
	ok = ok && CRT_Vulkan_Check (vkCreateComputePipelines (device, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = crt_cs, .pName = "main"},
			.layout = crt_spdlayout,
		}, NULL, &crt_spdpipeline), "vkCreateComputePipelines");

	ok = ok && CRT_Vulkan_UploadLUTs (queue, family);
	memset (crt_sizes, 0, sizeof(crt_sizes));
	if (!ok)
	{
		vkDeviceWaitIdle (device);
		CRT_Vulkan_Shutdown ();
	}
	return ok;
}

void CRT_Vulkan_Shutdown (void)
{
	int		i;

	if (!crt_device)
		return;
	for (i = 0 ; i < CRT_TEXTURES ; i++)
		CRT_Vulkan_FreeImage (&crt_images[i]);
	for (i = 0 ; i < CRT_FORMATS ; i++)
		if (crt_lastpipelines[i].pipeline)
			vkDestroyPipeline (crt_device, crt_lastpipelines[i].pipeline, NULL);
	memset (crt_lastpipelines, 0, sizeof(crt_lastpipelines));
	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		if (crt_pipelines[i])
			vkDestroyPipeline (crt_device, crt_pipelines[i], NULL);
		if (crt_fs[i])
			vkDestroyShaderModule (crt_device, crt_fs[i], NULL);
		crt_pipelines[i] = VK_NULL_HANDLE;
		crt_fs[i] = VK_NULL_HANDLE;
	}
	if (crt_spdpipeline)
		vkDestroyPipeline (crt_device, crt_spdpipeline, NULL);
	if (crt_vs)
		vkDestroyShaderModule (crt_device, crt_vs, NULL);
	if (crt_cs)
		vkDestroyShaderModule (crt_device, crt_cs, NULL);
	CRT_Vulkan_FreeBuffer (&crt_params);
	CRT_Vulkan_FreeBuffer (&crt_spdbuffer);
	for (i = 0 ; i < 2 ; i++)
		if (crt_samplers[i])
			vkDestroySampler (crt_device, crt_samplers[i], NULL);
	if (crt_pool)
		vkDestroyDescriptorPool (crt_device, crt_pool, NULL);
	if (crt_layout)
		vkDestroyPipelineLayout (crt_device, crt_layout, NULL);
	if (crt_spdlayout)
		vkDestroyPipelineLayout (crt_device, crt_spdlayout, NULL);
	if (crt_setlayout)
		vkDestroyDescriptorSetLayout (crt_device, crt_setlayout, NULL);
	if (crt_spdsetlayout)
		vkDestroyDescriptorSetLayout (crt_device, crt_spdsetlayout, NULL);
	crt_spdpipeline = VK_NULL_HANDLE;
	crt_vs = crt_cs = VK_NULL_HANDLE;
	crt_samplers[0] = crt_samplers[1] = VK_NULL_HANDLE;
	crt_pool = VK_NULL_HANDLE;
	crt_layout = crt_spdlayout = VK_NULL_HANDLE;
	crt_setlayout = crt_spdsetlayout = VK_NULL_HANDLE;
	crt_spddispatches = 0;
	crt_device = VK_NULL_HANDLE;
}

// the descriptor sets for the images made: each pass's, both ways the feedback
// textures stand; the passes' unused textures the frame's own, crt_spd's
// unused levels its last
static void CRT_Vulkan_WriteSets (void)
{
	VkDescriptorImageInfo	images[CRT_PASSES * 2][8], storage[CRT_SPD_MAX_DISPATCHES][1 + CRT_SPD_LEVELS];
	VkWriteDescriptorSet	writes[CRT_PASSES * 2 * 2 + CRT_SPD_MAX_DISPATCHES * 3];
	VkDescriptorBufferInfo	params = {crt_params.buffer, 0, VID_CRT_PARAMS_SIZE};
	VkDescriptorBufferInfo	spdbuffer = {crt_spdbuffer.buffer, 0, CRT_SPD_BUFFER_SIZE};
	const vid_crt_pass_info_t	*pass;
	const crt_image_t		*t, *prepass = &crt_images[CRT_PREPASS];
	int						i, p, k, n = 0, level;

	for (i = 0 ; i < CRT_PASSES ; i++)
		for (p = 0 ; p < 2 ; p++)
		{
			pass = &vid_crt_passes[i];
			for (k = 0 ; k < 8 ; k++)
			{
				t = &crt_images[k < pass->numinputs ? VID_CRT_Physical (pass->inputs[k], p) : CRT_ORIGINAL];
				images[i * 2 + p][k] = (VkDescriptorImageInfo){
					.sampler = crt_samplers[k < pass->numinputs && vid_crt_textures[pass->inputs[k]].linear],
					.imageView = t->view,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
			}
			writes[n++] = (VkWriteDescriptorSet){
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = crt_sets[i][p],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
				.pBufferInfo = &params,
			};
			writes[n++] = (VkWriteDescriptorSet){
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = crt_sets[i][p],
				.dstBinding = 1,
				.descriptorCount = 8,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = images[i * 2 + p],
			};
		}

	for (i = 0 ; i < crt_spddispatches ; i++)
	{
		for (k = 0 ; k <= CRT_SPD_LEVELS ; k++)
		{
			level = (int)crt_spd[i].base + k;
			if (level >= prepass->numlevels)
				level = prepass->numlevels - 1;
			storage[i][k] = (VkDescriptorImageInfo){.imageView = prepass->levels[level],
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL};
		}
		writes[n++] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = crt_spdsets[i],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &storage[i][0],
		};
		writes[n++] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = crt_spdsets[i],
			.dstBinding = 1,
			.descriptorCount = CRT_SPD_LEVELS,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &storage[i][1],
		};
		writes[n++] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = crt_spdsets[i],
			.dstBinding = 2,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &spdbuffer,
		};
	}
	vkUpdateDescriptorSets (crt_device, (uint32_t)n, writes, 0, NULL);
}

bool CRT_Vulkan_SizeChanged (int sourcewidth, int sourceheight, int viewwidth, int viewheight)
{
	return crt_sizes[0] != sourcewidth || crt_sizes[1] != sourceheight || crt_sizes[2] != viewwidth
		|| crt_sizes[3] != viewheight;
}

void CRT_Vulkan_SetSize (int sourcewidth, int sourceheight, int viewwidth, int viewheight)
{
	crt_image_t	*t;
	int			i, w, h;

	for (i = 0 ; i < CRT_TEXTURES ; i++)
	{
		if (vid_crt_textures[i].size == CRT_SIZE_LUT)
			continue;
		t = &crt_images[i];
		VID_CRT_TextureSize (i, sourcewidth, sourceheight, viewwidth, viewheight, &w, &h);
		if (t->image && t->width == w && t->height == h)
			continue;
		if (!CRT_Vulkan_CreateImage (i, w, h))
		{
			CRT_Vulkan_FreeImage (t);
			continue;
		}
		if (vid_crt_textures[i].mips)
			crt_spddispatches = VID_CRT_SpdDispatches (w, h, crt_spd, crt_spdgroups);
	}
	for (i = 0 ; i < CRT_TEXTURES ; i++)
		if (!crt_images[i].image)
			return;		// a size that couldn't be made: nothing is drawn
	CRT_Vulkan_WriteSets ();
	crt_sizes[0] = sourcewidth;
	crt_sizes[1] = sourceheight;
	crt_sizes[2] = viewwidth;
	crt_sizes[3] = viewheight;
}

void CRT_Vulkan_BeginOriginal (VkCommandBuffer commands, VkExtent2D *extent)
{
	crt_image_t	*t = &crt_images[CRT_ORIGINAL];

	CRT_Vulkan_ToDraw (commands, t);
	CRT_Vulkan_BeginRendering (commands, t);
	*extent = (VkExtent2D){(uint32_t)t->width, (uint32_t)t->height};
}

void CRT_Vulkan_EndOriginal (VkCommandBuffer commands)
{
	vkCmdEndRendering (commands);
}

// PrePass's mip chain from its top level, just drawn
static void CRT_Vulkan_Mips (VkCommandBuffer commands)
{
	crt_image_t	*t = &crt_images[CRT_PREPASS];
	int			d;

	// its levels for crt_spd, and the dispatch before's writes to the buffer
	CRT_Vulkan_Transition (commands, t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, false);
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_COMPUTE, crt_spdpipeline);
	for (d = 0 ; d < crt_spddispatches ; d++)
	{
		CRT_Vulkan_Barrier (commands, NULL, 0, &(VkMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		});
		vkCmdBindDescriptorSets (commands, VK_PIPELINE_BIND_POINT_COMPUTE, crt_spdlayout, 0, 1, &crt_spdsets[d], 0,
			NULL);
		vkCmdPushConstants (commands, crt_spdlayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vid_crt_spd_t), &crt_spd[d]);
		vkCmdDispatch (commands, (uint32_t)crt_spdgroups[d][0], (uint32_t)crt_spdgroups[d][1], 1);
	}
	CRT_Vulkan_ToRead (commands, t);
}

void CRT_Vulkan_RecordPasses (VkCommandBuffer commands, int slot, const vid_crt_params_t *params,
	const vid_crt_pass_t passes[CRT_PASSES], int parity)
{
	const vid_crt_pass_info_t	*pass;
	crt_image_t					*t, *out;
	uint32_t					offset = (uint32_t)(crt_paramsstride * (VkDeviceSize)slot);
	int							i, k;

	memcpy ((byte *)crt_params.data + offset, params, sizeof(*params));

	// the feedback textures made since: black, as the frame before had them
	for (i = 0 ; i < CRT_TEXTURES ; i++)
	{
		t = &crt_images[i];
		if (!t->clear)
			continue;
		CRT_Vulkan_Transition (commands, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
		vkCmdClearColorImage (commands, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			&(VkClearColorValue){.float32 = {0, 0, 0, 0}}, 1,
			&(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1});
		CRT_Vulkan_ToRead (commands, t);
		t->clear = false;
	}

	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		pass = &vid_crt_passes[i];
		for (k = 0 ; k < pass->numinputs ; k++)
			CRT_Vulkan_ToRead (commands, &crt_images[VID_CRT_Physical (pass->inputs[k], parity)]);
		if (pass->output == CRT_SCREEN)
			break;		// CRT_Vulkan_RecordLast's, in the caller's rendering
		out = &crt_images[VID_CRT_Physical (pass->output, parity)];
		CRT_Vulkan_ToDraw (commands, out);
		CRT_Vulkan_BeginRendering (commands, out);
		vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_GRAPHICS, crt_pipelines[i]);
		vkCmdBindDescriptorSets (commands, VK_PIPELINE_BIND_POINT_GRAPHICS, crt_layout, 0, 1, &crt_sets[i][parity], 1,
			&offset);
		vkCmdPushConstants (commands, crt_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vid_crt_pass_t), &passes[i]);
		vkCmdDraw (commands, 3, 1, 0, 0);
		vkCmdEndRendering (commands);
		if (i == CRT_PASS_PREPASS)
			CRT_Vulkan_Mips (commands);
	}
	crt_last = passes[CRT_PASSES - 1];
	crt_lastparity = parity;
	crt_lastoffset = offset;
}

void CRT_Vulkan_RecordLast (VkCommandBuffer commands, VkFormat format, const VkViewport *viewport,
	const VkRect2D *scissor)
{
	int		i;

	for (i = 0 ; i < CRT_FORMATS && crt_lastpipelines[i].pipeline ; i++)
		if (crt_lastpipelines[i].format == format)
			break;
	if (i == CRT_FORMATS)
		return;		// a fifth format of the window's
	if (!crt_lastpipelines[i].pipeline)
	{
		crt_lastpipelines[i].pipeline = CRT_Vulkan_Pipeline (crt_fs[CRT_PASSES - 1], format);
		crt_lastpipelines[i].format = format;
		if (!crt_lastpipelines[i].pipeline)
			return;
	}
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_GRAPHICS, crt_lastpipelines[i].pipeline);
	vkCmdBindDescriptorSets (commands, VK_PIPELINE_BIND_POINT_GRAPHICS, crt_layout, 0, 1,
		&crt_sets[CRT_PASSES - 1][crt_lastparity], 1, &crt_lastoffset);
	vkCmdPushConstants (commands, crt_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vid_crt_pass_t), &crt_last);
	vkCmdSetViewport (commands, 0, 1, viewport);
	vkCmdSetScissor (commands, 0, 1, scissor);
	vkCmdDraw (commands, 3, 1, 0, 0);
}

VkImage CRT_Vulkan_Image (int texture, int *levels)
{
	*levels = crt_images[texture].numlevels;
	return crt_images[texture].image;
}
