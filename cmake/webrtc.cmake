# WebRTC for the native programs (src/net/net_rtc.c): libdatachannel's data
# channels and the brokers' WebSockets, over mbedTLS (DTLS, and TLS for
# rtcs://), both fetched and built with the programs (not the web's).
#
# They are built as their own projects: their own C standard, without the
# project's warnings, static, and without what the programs don't use (media,
# examples, tests, mbedTLS's programs).

include(FetchContent)

set(SW_MBEDTLS_VERSION 3.6.7)
set(SW_LIBDATACHANNEL_VERSION v0.24.6)

function(sw_fetch_webrtc)
	set(CMAKE_C_STANDARD 11)
	set(CMAKE_C_EXTENSIONS ON)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(BUILD_SHARED_LIBS OFF)
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	# their options taken from the variables set here, whatever CMake they ask for
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

	# mbedTLS's release, with its generated sources (no Python); libdatachannel
	# takes it by the name its find module gives it
	set(ENABLE_PROGRAMS OFF)
	set(ENABLE_TESTING OFF)
	set(MBEDTLS_FATAL_WARNINGS OFF)
	set(USE_STATIC_MBEDTLS_LIBRARY ON)
	set(USE_SHARED_MBEDTLS_LIBRARY OFF)
	# its targets exported, as libdatachannel's export of itself names them
	set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF)
	# DTLS's SRTP extension, which libdatachannel's DTLS takes
	set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mbedtls_user_config.h" CACHE FILEPATH
		"Mbed TLS user config file (appended to default)." FORCE)
	FetchContent_Declare(mbedtls
		URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${SW_MBEDTLS_VERSION}/mbedtls-${SW_MBEDTLS_VERSION}.tar.bz2"
		URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)
	FetchContent_MakeAvailable(mbedtls)
	add_library(MbedTLS::MbedTLS ALIAS mbedtls)

	# libdatachannel with its ICE (libjuice) and SCTP (usrsctp), data channels
	# and WebSockets only
	set(USE_MBEDTLS ON)
	set(NO_MEDIA ON)
	set(NO_EXAMPLES ON)
	set(NO_TESTS ON)
	set(NO_WEBSOCKET OFF)
	FetchContent_Declare(libdatachannel
		GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
		GIT_TAG ${SW_LIBDATACHANNEL_VERSION}
		GIT_SHALLOW TRUE
		GIT_SUBMODULES deps/libjuice deps/usrsctp deps/plog)

	FetchContent_MakeAvailable(libdatachannel)
endfunction()

sw_fetch_webrtc()
