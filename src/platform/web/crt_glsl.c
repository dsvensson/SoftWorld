// crt_glsl.c -- the CRT shader's GLSL inside the program: crt.glsl and the files
// it includes, their text for vid_webgl.js to put together and compile (WebGL
// compiles shaders from their source)

const char	vid_crt_glsl[] = {
#embed "../crt/crt.glsl"
	, 0
};

const char	vid_crt_core[] = {
#embed "../crt/crt_core.inc"
	, 0
};

const char	vid_crt_params[] = {
#embed "../crt/crt_params.h"
	, 0
};
