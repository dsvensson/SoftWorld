# fteqcc, which compiles the QuakeC the programs carry (qw-qc, menu-qc) and the
# VM's test fixtures: the one the FTEQCC environment variable names, else one
# on the PATH (SW_FTEQCC), else one built here at configure. That one is
# fteqw's qclib at the commit below (cmake/fteqcc), fetched with git and built
# with the host's C compiler, also when the programs are the web's; it is
# installed under SW_DEPS_DIR in a directory named for what builds it, where
# configures after find it, as webrtc.cmake's libraries are. SW_FTEQCC_EXE is
# the one used.

set(SW_FTEQW_COMMIT f937b9d88f71fc4429db5fe56c6a98d922711b2e)

# cached either way, so one configure with FTEQCC set is enough
if(DEFINED ENV{FTEQCC} AND NOT "$ENV{FTEQCC}" STREQUAL "")
	set(SW_FTEQCC "$ENV{FTEQCC}" CACHE FILEPATH "fteqcc, which compiles the QuakeC" FORCE)
else()
	find_program(SW_FTEQCC NAMES fteqcc64 fteqcc DOC "fteqcc, which compiles the QuakeC")
endif()

if(SW_FTEQCC)
	set(SW_FTEQCC_EXE "${SW_FTEQCC}")
	message(STATUS "fteqcc: ${SW_FTEQCC_EXE}")
	return()
endif()

file(TO_CMAKE_PATH "${SW_DEPS_DIR}" sw_deps_dir)
get_filename_component(sw_deps_dir "${sw_deps_dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
file(SHA1 "${CMAKE_CURRENT_LIST_DIR}/fteqcc/CMakeLists.txt" sw_fteqcc_hash)
string(SHA1 sw_fteqcc_hash "${SW_FTEQW_COMMIT}${sw_fteqcc_hash}")
string(SUBSTRING "${sw_fteqcc_hash}" 0 12 sw_fteqcc_hash)
set(sw_fteqcc_prefix "${sw_deps_dir}/fteqcc-${sw_fteqcc_hash}")
# the host's suffix, not the programs' (the web's is .js)
if(CMAKE_HOST_WIN32)
	set(SW_FTEQCC_EXE "${sw_fteqcc_prefix}/bin/fteqcc.exe")
else()
	set(SW_FTEQCC_EXE "${sw_fteqcc_prefix}/bin/fteqcc")
endif()

if(NOT EXISTS "${SW_FTEQCC_EXE}")
	find_package(Git REQUIRED)
	message(STATUS "fteqcc: none on the PATH or in FTEQCC; building fteqw ${SW_FTEQW_COMMIT}'s into ${sw_fteqcc_prefix}")

	# fetched, built and installed beside the programs, moved in place whole, so
	# a build that fails leaves no prefix behind; what the steps print goes to a
	# log, and shows when one fails. The fetch is tried again after a while.
	set(sw_src "${CMAKE_BINARY_DIR}/fteqcc-src")
	set(sw_build "${CMAKE_BINARY_DIR}/fteqcc-build")
	set(sw_log "${CMAKE_BINARY_DIR}/fteqcc-build.log")
	file(WRITE "${sw_log}" "")
	function(sw_fteqcc_step tries)
		foreach(attempt RANGE 1 ${tries})
			execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
			file(APPEND "${sw_log}" "${output}")
			if(NOT result)
				return()
			endif()
			if(attempt LESS tries)
				message(STATUS "fteqcc: that failed, trying again in 15 s (${sw_log})")
				execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 15)
			endif()
		endforeach()
		message("${output}")
		message(FATAL_ERROR "fteqcc: couldn't build it (${sw_log}); or put one on the PATH, or name it in FTEQCC")
	endfunction()

	# the one commit, and of it qclib's files only
	file(REMOVE_RECURSE "${sw_src}" "${sw_build}" "${sw_fteqcc_prefix}.partial")
	file(MAKE_DIRECTORY "${sw_src}")
	set(sw_git "${GIT_EXECUTABLE}" -C "${sw_src}" -c advice.detachedHead=false)
	sw_fteqcc_step(1 ${sw_git} init -q)
	sw_fteqcc_step(3 ${sw_git} fetch -q --depth 1 --filter=blob:none https://github.com/fte-team/fteqw.git
		${SW_FTEQW_COMMIT})
	sw_fteqcc_step(1 ${sw_git} sparse-checkout set --no-cone /engine/qclib/)
	sw_fteqcc_step(3 ${sw_git} checkout -q FETCH_HEAD)
	execute_process(COMMAND ${sw_git} rev-parse --short=7 HEAD OUTPUT_VARIABLE sw_rev OUTPUT_STRIP_TRAILING_WHITESPACE)
	execute_process(COMMAND ${sw_git} log -1 --format=%cs OUTPUT_VARIABLE sw_date OUTPUT_STRIP_TRAILING_WHITESPACE)

	# the host's compiler: the programs' where it makes the host's programs; a
	# cross build's (the web's) goes without its toolchain, taking the system's
	# compiler (Ninja on Linux and macOS, Visual Studio's on Windows, which
	# needs no developer prompt)
	set(sw_args -S "${CMAKE_CURRENT_LIST_DIR}/fteqcc" -B "${sw_build}" "-DFTEQW_QCLIB=${sw_src}/engine/qclib"
		"-DFTEQW_REVISION=git-${sw_rev}" "-DFTEQW_DATE=${sw_date}")
	if(NOT CMAKE_CROSSCOMPILING)
		list(APPEND sw_args -G Ninja "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
			"-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" -DCMAKE_BUILD_TYPE=Release)
		set(sw_configure "${CMAKE_COMMAND}" ${sw_args})
	elseif(CMAKE_HOST_WIN32)
		set(sw_configure "${CMAKE_COMMAND}" ${sw_args})
	else()
		list(APPEND sw_args -G Ninja "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" -DCMAKE_BUILD_TYPE=Release)
		set(sw_configure "${CMAKE_COMMAND}" -E env --unset=CC --unset=CFLAGS --unset=LDFLAGS "${CMAKE_COMMAND}"
			${sw_args})
	endif()
	sw_fteqcc_step(1 ${sw_configure})
	sw_fteqcc_step(1 "${CMAKE_COMMAND}" --build "${sw_build}" --config Release)
	sw_fteqcc_step(1 "${CMAKE_COMMAND}" --install "${sw_build}" --config Release --prefix "${sw_fteqcc_prefix}.partial")
	file(RENAME "${sw_fteqcc_prefix}.partial" "${sw_fteqcc_prefix}")
	file(REMOVE_RECURSE "${sw_src}" "${sw_build}")
	if(NOT EXISTS "${SW_FTEQCC_EXE}")
		message(FATAL_ERROR "fteqcc: built into ${sw_fteqcc_prefix}, but there is no ${SW_FTEQCC_EXE}")
	endif()
endif()
message(STATUS "fteqcc: ${SW_FTEQCC_EXE}")
