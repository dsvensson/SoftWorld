# mbedTLS for the native programs' HTTPS (the server list's url sources),
# fetched, built and installed as a project of its own: cmake/mbedtls.cmake
# configures, builds and installs it into a prefix when there is none (no
# WebRTC built on one, none of the system's), and cmake/webrtc takes it in to
# build libdatachannel on, so a program has the one.
#
# Static, without its programs and tests, from the release with its generated
# sources (no Python); with DTLS's SRTP extension (mbedtls_user_config.h).
# Debug's libraries end in d, so one prefix holds the configurations side by
# side.

cmake_minimum_required(VERSION 3.28)
project(sw_mbedtls_deps C)

include(FetchContent)

set(SW_MBEDTLS_VERSION 3.6.7)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_DEBUG_POSTFIX d)
# their options taken from the variables set here, and the runtime library the
# programs ask for, whatever CMake they ask for
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

set(ENABLE_PROGRAMS OFF)
set(ENABLE_TESTING OFF)
set(MBEDTLS_FATAL_WARNINGS OFF)
set(USE_STATIC_MBEDTLS_LIBRARY ON)
set(USE_SHARED_MBEDTLS_LIBRARY OFF)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h" CACHE FILEPATH
	"Mbed TLS user config file (appended to default)." FORCE)
FetchContent_Declare(mbedtls
	URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${SW_MBEDTLS_VERSION}/mbedtls-${SW_MBEDTLS_VERSION}.tar.bz2"
	URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)
FetchContent_MakeAvailable(mbedtls)
