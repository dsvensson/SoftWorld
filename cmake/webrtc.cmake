# WebRTC for the native programs (src/net/net_rtc.c): libdatachannel, for its
# data channels and the brokers' WebSockets.
#
# SW_WEBRTC AUTO (the default) takes the libdatachannel find_package finds, the
# system's or one SW_WEBRTC ON built before, and leaves WebRTC out where there
# is none. ON takes it too, or else builds libdatachannel and mbedTLS
# (cmake/webrtc) at configure (deps.cmake) and installs them under
# SW_DEPS_DIR: configures after find them there, and CI caches it. The mbedTLS
# found with it is the one the programs' HTTPS takes too (mbedtls.cmake).
# OFF leaves WebRTC out. SW_WEBRTC_FOUND tells the rest whether it is in.

set(SW_WEBRTC AUTO CACHE STRING
	"WebRTC (rtc://): AUTO with the libdatachannel there is, ON building it where there is none, OFF without")
set_property(CACHE SW_WEBRTC PROPERTY STRINGS AUTO ON OFF)

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

include(deps)

# the prefix SW_WEBRTC=ON builds into, named for what builds it
sw_deps_prefix(sw_webrtc_prefix webrtc "${CMAKE_CURRENT_LIST_DIR}/webrtc/CMakeLists.txt"
	"${CMAKE_CURRENT_LIST_DIR}/mbedtls/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/mbedtls/mbedtls_user_config.h")
list(PREPEND CMAKE_PREFIX_PATH "${sw_webrtc_prefix}")
sw_deps_forget("${sw_webrtc_prefix}" LibDataChannel_DIR MbedTLS_DIR)

macro(sw_find_libdatachannel)
	find_package(Threads)
	find_package(MbedTLS 3 CONFIG QUIET)
	find_package(LibDataChannel 0.24 CONFIG QUIET)
endmacro()

sw_find_libdatachannel()

if(NOT LibDataChannel_FOUND AND SW_WEBRTC STREQUAL "ON")
	sw_deps_build(WebRTC "libdatachannel and mbedTLS" "${CMAKE_CURRENT_LIST_DIR}/webrtc" "${sw_webrtc_prefix}")
	sw_find_libdatachannel()
	if(NOT LibDataChannel_FOUND)
		message(FATAL_ERROR "WebRTC: libdatachannel built into ${sw_webrtc_prefix}, but not found there")
	endif()
endif()

if(LibDataChannel_FOUND)
	sw_deps_map_configs()
	set(SW_WEBRTC_FOUND ON)
	message(STATUS "WebRTC: libdatachannel ${LibDataChannel_VERSION} (${LibDataChannel_DIR})")
else()
	message(STATUS "WebRTC: left out, no libdatachannel 0.24 (SW_WEBRTC=ON builds it)")
endif()
