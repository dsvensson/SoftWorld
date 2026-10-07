# sw_qc_progs(<target> SOURCE_DIR <dir> DAT <name.dat> SYMBOL <name> [FLAGS <flags>]
#             [STRINGS <NAME=value ...>])
#
# A QuakeC directory (qw-qc, menu-qc) compiled with fteqcc (fteqcc.cmake) into
# <build dir>/<dir's name>/<DAT>, with STRINGS defined as strings for its
# preprocessor, carried inside <target> as a C array, SYMBOL and SYMBOL_size
# (embed.cmake); <DAT's name>_dat builds it, for tests.

function(sw_qc_progs target)
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE_DIR;DAT;SYMBOL;FLAGS;STRINGS" "")
	get_filename_component(dir "${arg_SOURCE_DIR}" NAME)
	get_filename_component(base "${arg_DAT}" NAME_WE)
	set(out "${CMAKE_BINARY_DIR}/${dir}")
	file(GLOB sources CONFIGURE_DEPENDS "${arg_SOURCE_DIR}/*.qc" "${arg_SOURCE_DIR}/*.qh"
		"${arg_SOURCE_DIR}/progs.src")

	add_custom_command(OUTPUT "${out}/${arg_DAT}"
		COMMAND "${CMAKE_COMMAND}" "-DFTEQCC=${SW_FTEQCC_EXE}" "-DSRC=${arg_SOURCE_DIR}" "-DOUT=${out}"
			"-DDAT=${arg_DAT}" "-DFLAGS=${arg_FLAGS}" "-DSTRINGS=${arg_STRINGS}"
			-P "${PROJECT_SOURCE_DIR}/cmake/qcc.cmake"
		DEPENDS ${sources} "${SW_FTEQCC_EXE}" "${PROJECT_SOURCE_DIR}/cmake/qcc.cmake"
		COMMENT "Compiling ${dir} with fteqcc"
		VERBATIM)
	add_custom_command(OUTPUT "${out}/${base}_data.c"
		COMMAND "${CMAKE_COMMAND}" "-DIN=${out}/${arg_DAT}" "-DOUT=${out}/${base}_data.c"
			"-DSYMBOL=${arg_SYMBOL}" -P "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
		DEPENDS "${out}/${arg_DAT}" "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
		COMMENT "Building ${arg_DAT} in"
		VERBATIM)
	target_sources(${target} PRIVATE "${out}/${base}_data.c")
	add_custom_target(${base}_dat DEPENDS "${out}/${arg_DAT}")
endfunction()
