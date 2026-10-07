# mbedTLS for the native programs' HTTPS (the server list's url sources,
# src/slist/slist_http.c). A program has one mbedTLS, of one configuration:
# WebRTC's where it was found with libdatachannel (webrtc.cmake), else the one
# find_package finds, the system's or one built here before, else one built
# here at configure (cmake/mbedtls, deps.cmake) and installed under
# SW_DEPS_DIR, where configures after (and CI's caches) find it. Included after
# webrtc.cmake; MbedTLS::mbedtls is the library to link.

include(deps)

if(NOT TARGET MbedTLS::mbedtls)
	sw_deps_prefix(sw_mbedtls_prefix mbedtls "${CMAKE_CURRENT_LIST_DIR}/mbedtls/CMakeLists.txt"
		"${CMAKE_CURRENT_LIST_DIR}/mbedtls/mbedtls_user_config.h")
	list(PREPEND CMAKE_PREFIX_PATH "${sw_mbedtls_prefix}")
	sw_deps_forget("${sw_mbedtls_prefix}" MbedTLS_DIR)
	find_package(MbedTLS 3 CONFIG QUIET)
	if(NOT TARGET MbedTLS::mbedtls)
		sw_deps_build(mbedTLS "mbedTLS" "${CMAKE_CURRENT_LIST_DIR}/mbedtls" "${sw_mbedtls_prefix}")
		find_package(MbedTLS 3 CONFIG QUIET)
		if(NOT TARGET MbedTLS::mbedtls)
			message(FATAL_ERROR "mbedTLS: built into ${sw_mbedtls_prefix}, but not found there")
		endif()
	endif()
	sw_deps_map_configs()
endif()
message(STATUS "mbedTLS: ${MbedTLS_VERSION} (${MbedTLS_DIR})")
