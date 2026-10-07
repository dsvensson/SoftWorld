# What the build fetches and builds itself at configure (webrtc.cmake's
# libraries, mbedtls.cmake's): a CMake project of its own under cmake/,
# configured, built and installed with the programs' compilers, runtime library
# and configurations into a prefix under SW_DEPS_DIR named for what builds it,
# where configures after (and CI's caches) find it.

include_guard(GLOBAL)

# sw_deps_prefix(<var> <name> <file>...): SW_DEPS_DIR's <name>-<hash>, the hash
# of the files that build it (a relative SW_DEPS_DIR is the source directory's)
function(sw_deps_prefix var name)
	file(TO_CMAKE_PATH "${SW_DEPS_DIR}" dir)
	get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	set(hashes "")
	foreach(file IN LISTS ARGN)
		file(SHA1 "${file}" hash)
		string(APPEND hashes "${hash}")
	endforeach()
	string(SHA1 hash "${hashes}")
	string(SUBSTRING "${hash}" 0 12 hash)
	set(${var} "${dir}/${name}-${hash}" PARENT_SCOPE)
endfunction()

# sw_deps_forget(<prefix> <var>...): the packages' places (their <name>_DIR)
# forgotten where the cache has them in a prefix of the same name the files
# built before they changed, so find_package looks again rather than take the
# old one
function(sw_deps_forget prefix)
	string(REGEX REPLACE "-[0-9a-f]+$" "-" stem "${prefix}")
	foreach(var IN LISTS ARGN)
		if(DEFINED CACHE{${var}})
			string(FIND "$CACHE{${var}}" "${stem}" at)
			string(FIND "$CACHE{${var}}" "${prefix}/" here)
			if(at EQUAL 0 AND NOT here EQUAL 0)
				unset(${var} CACHE)
			endif()
		endif()
	endforeach()
endfunction()

# a step of sw_deps_build's, tried as many times; what it prints goes to the
# log, and shows when it fails at last
function(sw_deps_step label what log tries)
	foreach(attempt RANGE 1 ${tries})
		execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
		file(APPEND "${log}" "${output}")
		if(NOT result)
			return()
		endif()
		if(attempt LESS tries)
			message(STATUS "${label}: that failed, trying again in 15 s (${log})")
			execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 15)
		endif()
	endforeach()
	message("${output}")
	message(FATAL_ERROR "${label}: ${what} didn't build (${log})")
endfunction()

# sw_deps_build(<label> <what> <source dir> <prefix>): the project in the source
# directory built as the programs' Debug and Release (the programs'
# configurations, the others as Release's) and installed into the prefix. It
# is built beside the programs (<name>-deps), installed apart and moved in
# place whole, so a build that fails leaves no prefix behind. Its configure
# fetches the sources, and is tried again after a while when it fails, as
# downloads now and then do.
function(sw_deps_build label what source prefix)
	get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
	if(multi)
		set(configs ${CMAKE_CONFIGURATION_TYPES})
	else()
		set(configs ${CMAKE_BUILD_TYPE})
	endif()
	set(deps_configs "")
	foreach(config IN LISTS configs)
		if(config STREQUAL "Debug")
			list(APPEND deps_configs Debug)
		else()
			list(APPEND deps_configs Release)
		endif()
	endforeach()
	list(REMOVE_DUPLICATES deps_configs)
	if(NOT deps_configs)
		set(deps_configs Release)
	endif()

	# the programs' tools, runtime library, target and configurations, as the
	# project's initial cache
	get_filename_component(name "${source}" NAME)
	set(build "${CMAKE_BINARY_DIR}/${name}-deps")
	if(multi)
		set(init "set(CMAKE_CONFIGURATION_TYPES \"${deps_configs}\" CACHE STRING \"\")\n")
	else()
		set(init "set(CMAKE_BUILD_TYPE \"${deps_configs}\" CACHE STRING \"\")\n")
	endif()
	foreach(var CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_TOOLCHAIN_FILE CMAKE_LINKER_TYPE CMAKE_MSVC_RUNTIME_LIBRARY
		CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES CMAKE_MAKE_PROGRAM)
		if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
			string(APPEND init "set(${var} \"${${var}}\" CACHE STRING \"\")\n")
		endif()
	endforeach()
	file(WRITE "${build}-init.cmake" "${init}")
	set(args -G "${CMAKE_GENERATOR}" -C "${build}-init.cmake")
	if(CMAKE_GENERATOR_PLATFORM)
		list(APPEND args -A "${CMAKE_GENERATOR_PLATFORM}")
	endif()
	if(CMAKE_GENERATOR_TOOLSET)
		list(APPEND args -T "${CMAKE_GENERATOR_TOOLSET}")
	endif()

	message(STATUS "${label}: building ${what} (${deps_configs}) into ${prefix}")
	set(log "${build}.log")
	file(WRITE "${log}" "")
	file(REMOVE_RECURSE "${prefix}.partial")
	sw_deps_step("${label}" "${what}" "${log}" 3 "${CMAKE_COMMAND}" -S "${source}" -B "${build}" ${args})
	foreach(config IN LISTS deps_configs)
		sw_deps_step("${label}" "${what}" "${log}" 1 "${CMAKE_COMMAND}" --build "${build}" --config ${config})
		sw_deps_step("${label}" "${what}" "${log}" 1 "${CMAKE_COMMAND}" --install "${build}" --config ${config}
			--prefix "${prefix}.partial")
	endforeach()
	file(RENAME "${prefix}.partial" "${prefix}")
endfunction()

# What the programs' configurations link from a package built as Debug and
# Release (sw_deps_build's, vcpkg's): Debug's own, the others Release's, where
# CMake would take the first there is, Debug's. Only on those: a distribution's
# package has the one configuration it was built as (Arch's and Debian's
# None), which CMake takes for all, and a find module's library (Vulkan's) has
# none.
function(sw_deps_map_configs)
	get_directory_property(imported IMPORTED_TARGETS)
	foreach(target IN LISTS imported)
		get_target_property(configs ${target} IMPORTED_CONFIGURATIONS)
		if("RELEASE" IN_LIST configs)
			set_target_properties(${target} PROPERTIES
				MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release MAP_IMPORTED_CONFIG_MINSIZEREL Release)
		endif()
	endforeach()
endfunction()
