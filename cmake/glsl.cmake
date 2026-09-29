# Compiles GLSL into SPIR-V (glslangValidator), which the programs carry inside
# them. One file holds a pipeline's stages; each is compiled with its name
# defined (VERTEX, FRAGMENT).
find_program(SW_GLSLANG NAMES glslangValidator glslang REQUIRED DOC "glslang, the GLSL compiler")

# sw_compile_glsl(<output spv> <glsl file> <stage: vert or frag>)
function(sw_compile_glsl out_spv glsl stage)
	if(stage STREQUAL "vert")
		set(define VERTEX)
	else()
		set(define FRAGMENT)
	endif()
	add_custom_command(
		OUTPUT "${out_spv}"
		COMMAND "${SW_GLSLANG}" -V --target-env vulkan1.3 -S ${stage} -D${define} -o "${out_spv}" "${glsl}"
		MAIN_DEPENDENCY "${glsl}"
		COMMENT "Compiling ${glsl} (${stage})"
		VERBATIM)
endfunction()
