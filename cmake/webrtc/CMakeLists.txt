# libdatachannel and mbedTLS for the native programs' WebRTC, fetched, built and
# installed as a project of their own: cmake/webrtc.cmake configures, builds and
# installs it into a prefix when SW_WEBRTC is ON and the system has no
# libdatachannel, with the programs' compilers, runtime library and
# configurations, and finds them there as it would the system's.
#
# Static, without what the programs don't use (media, examples, tests); mbedTLS
# as cmake/mbedtls builds it alone, with DTLS's SRTP extension, which
# libdatachannel's DTLS takes. Debug's libraries end in d, so one prefix holds
# the configurations side by side.

cmake_minimum_required(VERSION 3.28)
project(sw_webrtc_deps C CXX)

include(FetchContent)

set(SW_LIBDATACHANNEL_VERSION v0.24.6)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_DEBUG_POSTFIX d)
# their options taken from the variables set here, and the runtime library the
# programs ask for, whatever CMake they ask for
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# mbedTLS as the programs' HTTPS has it alone (cmake/mbedtls); libdatachannel
# takes it by the name its find module gives it, and installs against its
# exported targets
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../mbedtls" mbedtls)
add_library(MbedTLS::MbedTLS ALIAS mbedtls)

# libdatachannel with its ICE (libjuice) and SCTP (usrsctp), installed with
# them; data channels and WebSockets only
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
