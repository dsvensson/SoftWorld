// present_wgsl.c -- present.wgsl inside the program, its text for vid_webgpu.js to
// compile (WebGPU compiles shaders from their source)

const char	vid_present_wgsl[] = {
#embed "present.wgsl"
	, 0
};
