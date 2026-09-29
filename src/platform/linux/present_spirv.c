// present_spirv.c -- present.glsl compiled (cmake/glsl.cmake), inside the program:
// the SPIR-V vid_vulkan.c makes its shaders of, words aligned as Vulkan takes them

#include "present_spirv.h"

alignas(4) const unsigned char	vid_present_vert[] = {
#embed "present_vert.spv"
};
const size_t	vid_present_vert_size = sizeof(vid_present_vert);

alignas(4) const unsigned char	vid_present_frag[] = {
#embed "present_frag.spv"
};
const size_t	vid_present_frag_size = sizeof(vid_present_frag);
