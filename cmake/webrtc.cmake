# WebRTC for the native programs (src/net/net_rtc.c): libdatachannel, for its
# data channels and the brokers' WebSockets.
#
# SW_WEBRTC AUTO (the default) takes the libdatachannel find_package finds, the
# system's or one SW_WEBRTC ON built before, and leaves WebRTC out where there
# is none. ON takes it too, or else builds libdatachannel and mbedTLS
# (cmake/webrtc) at configure with the programs' compilers, runtime library
# and configurations, and installs them under SW_DEPS_DIR, in a directory named
# for what builds them: configures after find them there, and CI caches it.
# OFF leaves WebRTC out. SW_WEBRTC_FOUND tells the rest whether it is in.

set(SW_WEBRTC AUTO CACHE STRING
	"WebRTC (rtc://): AUTO with the libdatachannel there is, ON building it where there is none, OFF without")
set_property(CACHE SW_WEBRTC PROPERTY STRINGS AUTO ON OFF)
set(SW_DEPS_DIR "${CMAKE_BINARY_DIR}/deps" CACHE PATH
	"Where SW_WEBRTC=ON installs the libraries it builds, for later configures (and caches) to find")

set(SW_WEBRTC_FOUND OFF)
if(NOT SW_WEBRTC MATCHES "^(AUTO|ON|OFF)$")
	message(FATAL_ERROR "SW_WEBRTC is AUTO, ON or OFF, not ${SW_WEBRTC}")
endif()
if(SW_WEBRTC STREQUAL "OFF")
	message(STATUS "WebRTC: left out (SW_WEBRTC=OFF)")
	return()
endif()

# a static libdatachannel needs a C++ link, and the libraries it was built with
enable_language(CXX)

# the prefix SW_WEBRTC=ON builds into, named for what builds it (a relative
# SW_DEPS_DIR is the source directory's)
file(TO_CMAKE_PATH "${SW_DEPS_DIR}" sw_deps_dir)
get_filename_component(sw_deps_dir "${sw_deps_dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
file(SHA1 "${CMAKE_CURRENT_LIST_DIR}/webrtc/CMakeLists.txt" sw_webrtc_hash1)
file(SHA1 "${CMAKE_CURRENT_LIST_DIR}/webrtc/mbedtls_user_config.h" sw_webrtc_hash2)
string(SHA1 sw_webrtc_hash "${sw_webrtc_hash1}${sw_webrtc_hash2}")
string(SUBSTRING "${sw_webrtc_hash}" 0 12 sw_webrtc_hash)
set(sw_webrtc_prefix "${sw_deps_dir}/webrtc-${sw_webrtc_hash}")
list(PREPEND CMAKE_PREFIX_PATH "${sw_webrtc_prefix}")

# what the programs' configurations link: Debug's own, the others Release's
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)

macro(sw_find_libdatachannel)
	find_package(Threads)
	find_package(MbedTLS 3 CONFIG QUIET)
	find_package(LibDataChannel 0.24 CONFIG QUIET)
endmacro()

sw_find_libdatachannel()

if(NOT LibDataChannel_FOUND AND SW_WEBRTC STREQUAL "ON")
	# the configurations to build: the programs', Debug's and Release's
	get_property(sw_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
	if(sw_multi)
		set(sw_configs ${CMAKE_CONFIGURATION_TYPES})
	else()
		set(sw_configs ${CMAKE_BUILD_TYPE})
	endif()
	set(sw_deps_configs "")
	foreach(config IN LISTS sw_configs)
		if(config STREQUAL "Debug")
			list(APPEND sw_deps_configs Debug)
		else()
			list(APPEND sw_deps_configs Release)
		endif()
	endforeach()
	list(REMOVE_DUPLICATES sw_deps_configs)
	if(NOT sw_deps_configs)
		set(sw_deps_configs Release)
	endif()

	# the programs' tools, runtime library, target and configurations, as the
	# project's initial cache
	set(sw_build "${CMAKE_BINARY_DIR}/webrtc-deps")
	if(sw_multi)
		set(sw_init "set(CMAKE_CONFIGURATION_TYPES \"${sw_deps_configs}\" CACHE STRING \"\")\n")
	else()
		set(sw_init "set(CMAKE_BUILD_TYPE \"${sw_deps_configs}\" CACHE STRING \"\")\n")
	endif()
	foreach(var CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_TOOLCHAIN_FILE CMAKE_LINKER_TYPE CMAKE_MSVC_RUNTIME_LIBRARY
		CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES CMAKE_MAKE_PROGRAM)
		if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
			string(APPEND sw_init "set(${var} \"${${var}}\" CACHE STRING \"\")\n")
		endif()
	endforeach()
	file(WRITE "${sw_build}-init.cmake" "${sw_init}")
	set(sw_args -G "${CMAKE_GENERATOR}" -C "${sw_build}-init.cmake")
	if(CMAKE_GENERATOR_PLATFORM)
		list(APPEND sw_args -A "${CMAKE_GENERATOR_PLATFORM}")
	endif()
	if(CMAKE_GENERATOR_TOOLSET)
		list(APPEND sw_args -T "${CMAKE_GENERATOR_TOOLSET}")
	endif()

	# built beside the programs, installed apart and moved in place whole, so
	# a build that fails leaves no prefix behind; what the steps print goes to
	# a log, and shows when one fails. The configure fetches the sources, and
	# is tried again after a while when it fails, as downloads now and then do.
	message(STATUS "WebRTC: building libdatachannel and mbedTLS (${sw_deps_configs}) into ${sw_webrtc_prefix}")
	set(sw_log "${sw_build}.log")
	file(WRITE "${sw_log}" "")
	function(sw_webrtc_step tries)
		foreach(attempt RANGE 1 ${tries})
			execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
			file(APPEND "${sw_log}" "${output}")
			if(NOT result)
				return()
			endif()
			if(attempt LESS tries)
				message(STATUS "WebRTC: that failed, trying again in 15 s (${sw_log})")
				execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 15)
			endif()
		endforeach()
		message("${output}")
		message(FATAL_ERROR "WebRTC: libdatachannel and mbedTLS didn't build (${sw_log})")
	endfunction()
	file(REMOVE_RECURSE "${sw_webrtc_prefix}.partial")
	sw_webrtc_step(3 "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/webrtc" -B "${sw_build}" ${sw_args})
	foreach(config IN LISTS sw_deps_configs)
		sw_webrtc_step(1 "${CMAKE_COMMAND}" --build "${sw_build}" --config ${config})
		sw_webrtc_step(1 "${CMAKE_COMMAND}" --install "${sw_build}" --config ${config}
			--prefix "${sw_webrtc_prefix}.partial")
	endforeach()
	file(RENAME "${sw_webrtc_prefix}.partial" "${sw_webrtc_prefix}")
	sw_find_libdatachannel()
	if(NOT LibDataChannel_FOUND)
		message(FATAL_ERROR "WebRTC: libdatachannel built into ${sw_webrtc_prefix}, but not found there")
	endif()
endif()

if(LibDataChannel_FOUND)
	set(SW_WEBRTC_FOUND ON)
	message(STATUS "WebRTC: libdatachannel ${LibDataChannel_VERSION} (${LibDataChannel_DIR})")
else()
	message(STATUS "WebRTC: left out, no libdatachannel 0.24 (SW_WEBRTC=ON builds it)")
endif()
