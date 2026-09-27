# qwprogs.dat, the server's game: qw-qc compiled with fteqcc, found on the PATH
# or named by the FTEQCC environment variable; without it, the qwprogs.dat
# qw-qc holds. The programs with a server carry it inside them: sw_server gets
# it as a C array (qwprogs_data.c). Included where sw_server is defined.

# cached either way, so one configure with FTEQCC set is enough
if(DEFINED ENV{FTEQCC} AND NOT "$ENV{FTEQCC}" STREQUAL "")
	set(SW_FTEQCC "$ENV{FTEQCC}" CACHE FILEPATH "fteqcc, which compiles qw-qc" FORCE)
else()
	find_program(SW_FTEQCC NAMES fteqcc64 fteqcc DOC "fteqcc, which compiles qw-qc")
endif()

set(SW_QC_DIR "${PROJECT_SOURCE_DIR}/qw-qc")
set(SW_QC_BUILD "${CMAKE_BINARY_DIR}/qw-qc")
file(GLOB SW_QC_SOURCES CONFIGURE_DEPENDS "${SW_QC_DIR}/*.qc" "${SW_QC_DIR}/progs.src")

if(SW_FTEQCC)
	message(STATUS "qw-qc: compiled with ${SW_FTEQCC}")
	add_custom_command(OUTPUT "${SW_QC_BUILD}/qwprogs.dat"
		COMMAND "${CMAKE_COMMAND}" "-DFTEQCC=${SW_FTEQCC}" "-DSRC=${SW_QC_DIR}" "-DOUT=${SW_QC_BUILD}"
			-P "${CMAKE_CURRENT_LIST_DIR}/fteqcc.cmake"
		DEPENDS ${SW_QC_SOURCES} "${CMAKE_CURRENT_LIST_DIR}/fteqcc.cmake"
		COMMENT "Compiling qw-qc with fteqcc"
		VERBATIM)
else()
	message(STATUS "qw-qc: no fteqcc (on the PATH, or FTEQCC); qw-qc/qwprogs.dat as it is")
	add_custom_command(OUTPUT "${SW_QC_BUILD}/qwprogs.dat"
		COMMAND "${CMAKE_COMMAND}" -E copy "${SW_QC_DIR}/qwprogs.dat" "${SW_QC_BUILD}/qwprogs.dat"
		DEPENDS "${SW_QC_DIR}/qwprogs.dat"
		VERBATIM)
endif()

add_custom_command(OUTPUT "${SW_QC_BUILD}/qwprogs_data.c"
	COMMAND "${CMAKE_COMMAND}" "-DIN=${SW_QC_BUILD}/qwprogs.dat" "-DOUT=${SW_QC_BUILD}/qwprogs_data.c"
		-P "${CMAKE_CURRENT_LIST_DIR}/embed.cmake"
	DEPENDS "${SW_QC_BUILD}/qwprogs.dat" "${CMAKE_CURRENT_LIST_DIR}/embed.cmake"
	COMMENT "Building qwprogs.dat in"
	VERBATIM)
