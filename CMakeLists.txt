cmake_minimum_required(VERSION 3.28)

if(CMAKE_HOST_APPLE)
	# Metal 4, which the macOS presenter draws with, is macOS 26 on Apple silicon
	set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Oldest macOS the programs run on")
	set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Architectures of the macOS programs")
endif()

project(SoftWorld VERSION 0.1.0 LANGUAGES C)

if(WIN32 AND MSVC)
	enable_language(RC)
elseif(APPLE)
	enable_language(OBJC)
else()
	message(FATAL_ERROR "SoftWorld builds with MSVC or clang-cl on Windows, or Apple clang on macOS")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(arch)
if(WIN32)
	include(hlsl)
endif()
include(modules)
include(sanitize)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(SW_LTO "Link-time optimization in Release and RelWithDebInfo builds" ON)
include(CheckIPOSupported)
check_ipo_supported(RESULT sw_ipo_supported OUTPUT sw_ipo_output LANGUAGES C)
if(NOT SW_LTO)
	message(STATUS "Link-time optimization disabled")
elseif(sw_ipo_supported)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
else()
	message(WARNING "Link-time optimization is not available: ${sw_ipo_output}")
endif()

if(MSVC)
	# Release keeps a program database too, for profilers and crash dumps; the code is the same
	set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo,Release>:ProgramDatabase>")
	add_link_options("$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")

	add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
	add_compile_options(/utf-8 /permissive- /diagnostics:column)
	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		# keep scalar and SIMD floating-point results identical
		add_compile_options(/clang:-ffp-contract=off)
	endif()
else()
	# keep scalar and SIMD floating-point results identical, and the same as the
	# Windows builds' (arm64 fuses multiplies and adds unless told not to); and no
	# type-based aliasing, which MSVC never did and the progs' fields rely on
	add_compile_options(-ffp-contract=off -fno-strict-aliasing)
	add_compile_options("$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
	add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:-Wl,-dead_strip>")
	# static libraries are listed again for the ones after them that need them
	add_link_options(-Wl,-no_warn_duplicate_libraries)
	set(CMAKE_OBJC_STANDARD 23)
	set(CMAKE_OBJC_STANDARD_REQUIRED ON)
	set(CMAKE_OBJC_EXTENSIONS OFF)
endif()

# all executables land in <build dir>/<config>, whichever directory defines them
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
