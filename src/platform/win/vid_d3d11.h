// vid_d3d11.h -- what present.hlsl takes, as vid_d3d11.c and test_present_d3d11.c
// hand it over
#pragma once

#include "vid_common.h"

#include <stdint.h>

// the presenters' constants, then the layers' row length: a layer is a buffer
// read raw, a pixel a uint, rowpixels from one row to the next
typedef struct
{
	vid_present_constants_t	c;
	uint32_t				rowpixels;
	uint32_t				pad[3];
} d3d_present_t;

static_assert (sizeof(d3d_present_t) == 80, "present.hlsl's constants are 80 bytes");
