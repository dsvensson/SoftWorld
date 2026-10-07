cmake_minimum_required(VERSION 3.28)

if(CMAKE_HOST_APPLE)
	# Metal 4, which the macOS presenter draws with, is macOS 26 on Apple silicon
	set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Oldest macOS the programs run on")
	set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Architectures of the macOS programs")
endif()

project(SoftWorld VERSION 3.07 LANGUAGES C)

if(WIN32 AND MSVC)
	enable_language(RC)
elseif(APPLE)
	enable_language(OBJC)
elseif(NOT EMSCRIPTEN AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "^(Clang|GNU)$"))
	message(FATAL_ERROR "SoftWorld builds with MSVC or clang-cl on Windows, Apple clang on macOS, "
		"clang or GCC on Linux, or Emscripten for the web")
endif()

# a preset for one word size (msvc-x86's 32) takes the compiler for it from the
# developer prompt, which the preset can't choose
if(DEFINED SW_POINTER_BITS)
	math(EXPR sw_bits "${CMAKE_SIZEOF_VOID_P} * 8")
	if(NOT sw_bits EQUAL SW_POINTER_BITS)
		message(FATAL_ERROR "This build is of ${SW_POINTER_BITS}-bit programs, but the compiler makes ${sw_bits}-bit "
			"ones: configure it from a developer prompt for them (for 32-bit x86, vcvarsamd64_x86.bat, or "
			"Enter-VsDevShell -DevCmdArguments '-arch=x86 -host_arch=x64')")
	endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(arch)
if(WIN32)
	include(hlsl)
endif()
include(modules)
include(sanitize)

# the version the programs tell (version.h), as the project has it
add_compile_definitions(SW_VERSION=${PROJECT_VERSION})

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

# where what the build fetches and builds itself is installed (fteqcc, and
# WebRTC's libraries), for later configures (and CI's caches) to find
set(SW_DEPS_DIR "${CMAKE_BINARY_DIR}/deps" CACHE PATH
	"Where the build installs what it fetches and builds itself (fteqcc, SW_WEBRTC=ON's libraries)")

# WebRTC (rtc://) for the native programs: libdatachannel where there is one,
# or built when asked (SW_WEBRTC), with the runtime library above
if(NOT EMSCRIPTEN)
	include(webrtc)
endif()

# fteqcc, found or built, and the QuakeC it compiles into the programs
include(fteqcc)
include(qcprogs)

if(MSVC)
	# Release keeps a program database too, for profilers and crash dumps; the code is the same
	set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo,Release>:ProgramDatabase>")
	add_link_options("$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
	# a 32-bit program has 4 GB on 64-bit Windows rather than 2
	if(CMAKE_SIZEOF_VOID_P EQUAL 4)
		add_link_options(/LARGEADDRESSAWARE)
	endif()

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
endif()

if(APPLE)
	add_compile_options("$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
	add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:-Wl,-dead_strip>")
	# static libraries are listed again for the ones after them that need them
	add_link_options(-Wl,-no_warn_duplicate_libraries)
	set(CMAKE_OBJC_STANDARD 23)
	set(CMAKE_OBJC_STANDARD_REQUIRED ON)
	set(CMAKE_OBJC_EXTENSIONS OFF)
elseif(EMSCRIPTEN)
	# the web: POSIX as musl has it (dirent's types), which strict C23 hides; the
	# worker threads and the shared memory the page's workers read; longjmp
	# (Host_Error's) in WebAssembly, not through JavaScript; Windows' 1 MB of stack
	# (the refresh takes about 256 KB); memory grown as used, up to 2 GB
	add_compile_definitions(_GNU_SOURCE)
	add_compile_options(-pthread -sSUPPORT_LONGJMP=wasm)
	add_link_options(-pthread -sSUPPORT_LONGJMP=wasm -sSTACK_SIZE=1MB -sINITIAL_MEMORY=256MB
		-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB -Wno-pthreads-mem-growth
		"$<$<CONFIG:Release>:--profiling-funcs>")
	# the threads made ahead, as one started later waits for the page to yield:
	# the most worker threads r_threads gets
	set(SW_WEB_WORKERS 7 CACHE STRING "Worker threads the web programs make ahead")
elseif(NOT WIN32)
	# Linux: POSIX and glibc's own (futexes, epoll, timerfd), which strict C23 hides
	add_compile_definitions(_GNU_SOURCE)
	link_libraries(m)
	add_compile_options("$<$<CONFIG:Release,RelWithDebInfo>:-ffunction-sections;-fdata-sections>")
	add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:-Wl,--gc-sections>")
	# the modules need each other both ways (the engine calls the platform, the
	# platform the engine): the libraries are one group, searched until nothing
	# more is found, for linkers that search each once in order (GNU ld)
	set(CMAKE_C_LINK_EXECUTABLE
		"<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
endif()

# all executables land in <build dir>/<config>, whichever directory defines them
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
