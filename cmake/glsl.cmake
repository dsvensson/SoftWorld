# Compiles GLSL into SPIR-V (glslangValidator), which the programs carry inside
# them. One file holds a pipeline's stages; each is compiled with its name
# defined (VERTEX, FRAGMENT, COMPUTE), and the DEFINES given.
find_program(SW_GLSLANG NAMES glslangValidator glslang REQUIRED DOC "glslang, the GLSL compiler")

# sw_compile_glsl(<output spv> <glsl file> <stage: vert, frag or comp> [DEFINES <name>...]
#                 [DEPENDS <included file>...])
function(sw_compile_glsl out_spv glsl stage)
	cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "DEFINES;DEPENDS")
	if(stage STREQUAL "vert")
		set(defines -DVERTEX)
	elseif(stage STREQUAL "comp")
		set(defines -DCOMPUTE)
	else()
		set(defines -DFRAGMENT)
	endif()
	foreach(define ${arg_DEFINES})
		list(APPEND defines -D${define})
	endforeach()
	add_custom_command(
		OUTPUT "${out_spv}"
		COMMAND "${SW_GLSLANG}" -V --target-env vulkan1.3 -S ${stage} ${defines} -o "${out_spv}" "${glsl}"
		MAIN_DEPENDENCY "${glsl}"
		DEPENDS ${arg_DEPENDS}
		COMMENT "Compiling ${glsl} (${stage})"
		VERBATIM)
endfunction()
