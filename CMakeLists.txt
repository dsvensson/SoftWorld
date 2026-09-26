cmake_minimum_required(VERSION 3.28)

project(SoftWorld VERSION 0.1.0 LANGUAGES C RC)

if(NOT MSVC)
	message(FATAL_ERROR "SoftWorld currently builds with MSVC or clang-cl on Windows")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(arch)
include(hlsl)
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

add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/utf-8 /permissive- /diagnostics:column)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
	# keep scalar and SIMD floating-point results identical
	add_compile_options(/clang:-ffp-contract=off)
endif()

# all executables land in <build dir>/<config>, whichever directory defines them
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(src)
