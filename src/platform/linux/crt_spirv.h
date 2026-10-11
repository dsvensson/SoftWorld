// crt_spirv.h -- crt.glsl compiled (cmake/glsl.cmake) and carried inside the
// program (crt_spirv.c): the vertex stage of the CRT shader's passes, a
// fragment stage a pass in vid_crt_passes' order, and crt_spd's compute stage
#pragma once

#include "vid_crt.h"

#include <stddef.h>

typedef struct
{
	const unsigned char	*code;
	size_t				size;
} vid_spirv_t;

extern const unsigned char	vid_crt_vert[], vid_crt_comp[];
extern const size_t			vid_crt_vert_size, vid_crt_comp_size;
extern const vid_spirv_t	vid_crt_frag[CRT_PASSES];
