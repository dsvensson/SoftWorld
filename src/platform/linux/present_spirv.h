// present_spirv.h -- present.glsl compiled (cmake/glsl.cmake) and carried inside
// the program (present_spirv.c), and the push constants its fragment stage takes
#pragma once

#include "vid_common.h"

#include <stddef.h>
#include <stdint.h>

extern const unsigned char	vid_present_vert[], vid_present_frag[];
extern const size_t			vid_present_vert_size, vid_present_frag_size;

// present.glsl's Present: the shaders' constants, then the layers
typedef struct
{
	vid_present_constants_t	constants;
	uint32_t	view[2];		// the 3D view's device address, the low word first
	uint32_t	hud[2];			// the 2D's
	uint32_t	rowpixels;		// pixels from one row of a layer to the next
} vid_present_push_t;
