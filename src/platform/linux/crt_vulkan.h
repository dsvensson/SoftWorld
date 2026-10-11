// crt_vulkan.h -- the CRT shader's passes (vid_crt.h) in Vulkan, which
// vid_vulkan.c draws the frame through and test_present_vulkan.c tests
#pragma once

#include <vulkan/vulkan.h>

#include "vid_crt.h"

// the shaders, samplers, descriptors, buffers and LUTs on device, of gpu,
// queue (of family) uploading the LUTs, waited for; slots frames in flight,
// each with its own parameters. False (and why printed) if they couldn't be made
bool	CRT_Vulkan_Init (VkPhysicalDevice gpu, VkDevice device, VkQueue queue, uint32_t family, int slots);
// all of it freed, the device idle
void	CRT_Vulkan_Shutdown (void);

// the textures for a frame of source pixels shown in a viewport of view pixels
// aren't the ones made: the frames in flight are to be finished before
// CRT_Vulkan_SetSize makes them (AFTERGLOW's and AVGLUM's black)
bool	CRT_Vulkan_SizeChanged (int sourcewidth, int sourceheight, int viewwidth, int viewheight);
void	CRT_Vulkan_SetSize (int sourcewidth, int sourceheight, int viewwidth, int viewheight);

// CRT_ORIGINAL being drawn in, R8G8B8A8_UNORM, its size: present.glsl's
// pipeline draws the frame at its own size between these
void	CRT_Vulkan_BeginOriginal (VkCommandBuffer commands, VkExtent2D *extent);
void	CRT_Vulkan_EndOriginal (VkCommandBuffer commands);

// the passes but the last, after the frame is in CRT_ORIGINAL: slot's
// parameters, the frame of parity's
void	CRT_Vulkan_RecordPasses (VkCommandBuffer commands, int slot, const vid_crt_params_t *params,
			const vid_crt_pass_t passes[CRT_PASSES], int parity);
// the last, into the caller's rendering to an image of format, at viewport
void	CRT_Vulkan_RecordLast (VkCommandBuffer commands, VkFormat format, const VkViewport *viewport,
			const VkRect2D *scissor);

// a texture's image, its mip levels SHADER_READ_ONLY_OPTIMAL after the passes, for the tests
VkImage	CRT_Vulkan_Image (int texture, int *levels);
