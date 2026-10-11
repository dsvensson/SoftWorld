// crt_spirv.c -- crt.glsl compiled (cmake/glsl.cmake), inside the program: the
// SPIR-V crt_vulkan.c makes its shaders of, words aligned as Vulkan takes them

#include "crt_spirv.h"

alignas(4) const unsigned char	vid_crt_vert[] = {
#embed "crt_vert.spv"
};
const size_t	vid_crt_vert_size = sizeof(vid_crt_vert);

alignas(4) const unsigned char	vid_crt_comp[] = {
#embed "crt_comp.spv"
};
const size_t	vid_crt_comp_size = sizeof(vid_crt_comp);

alignas(4) static const unsigned char	crt_afterglow[] = {
#embed "crt_afterglow_frag.spv"
};
alignas(4) static const unsigned char	crt_prepass[] = {
#embed "crt_prepass_frag.spv"
};
alignas(4) static const unsigned char	crt_avglum[] = {
#embed "crt_avglum_frag.spv"
};
alignas(4) static const unsigned char	crt_linearize[] = {
#embed "crt_linearize_frag.spv"
};
alignas(4) static const unsigned char	crt_gauss_h[] = {
#embed "crt_gauss_h_frag.spv"
};
alignas(4) static const unsigned char	crt_gauss_v[] = {
#embed "crt_gauss_v_frag.spv"
};
alignas(4) static const unsigned char	crt_bloom_h[] = {
#embed "crt_bloom_h_frag.spv"
};
alignas(4) static const unsigned char	crt_bloom_v[] = {
#embed "crt_bloom_v_frag.spv"
};
alignas(4) static const unsigned char	crt_crt[] = {
#embed "crt_crt_frag.spv"
};
alignas(4) static const unsigned char	crt_deconvergence[] = {
#embed "crt_deconvergence_frag.spv"
};

// vid_crt_passes' order
const vid_spirv_t	vid_crt_frag[CRT_PASSES] = {
	{crt_afterglow, sizeof(crt_afterglow)},
	{crt_prepass, sizeof(crt_prepass)},
	{crt_avglum, sizeof(crt_avglum)},
	{crt_linearize, sizeof(crt_linearize)},
	{crt_gauss_h, sizeof(crt_gauss_h)},
	{crt_gauss_v, sizeof(crt_gauss_v)},
	{crt_bloom_h, sizeof(crt_bloom_h)},
	{crt_bloom_v, sizeof(crt_bloom_v)},
	{crt_crt, sizeof(crt_crt)},
	{crt_deconvergence, sizeof(crt_deconvergence)},
};
