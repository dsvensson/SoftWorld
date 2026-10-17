# zlib for the programs' gzip (src/common/vfs_gzip.c: the hub's demos come as
# .mvd.gz), fetched, built and installed as a project of its own:
# cmake/zlib.cmake configures, builds and installs it into a prefix when none
# is found (the system's static one, or one built here before).
#
# Static, its sources compiled here rather than through zlib's own
# CMakeLists, which builds and installs a DLL beside the static library and
# has no switch against it. Debug's library ends in d, so one prefix holds the
# configurations side by side; zlibstatic(d) is a name FindZLIB looks for.

cmake_minimum_required(VERSION 3.28)
project(sw_zlib_deps C)

include(FetchContent)

set(SW_ZLIB_VERSION 1.3.1)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_DEBUG_POSTFIX d)
# the runtime library the programs ask for
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# fetched only: a SOURCE_SUBDIR without a CMakeLists isn't added to the build
FetchContent_Declare(zlib
	URL "https://github.com/madler/zlib/releases/download/v${SW_ZLIB_VERSION}/zlib-${SW_ZLIB_VERSION}.tar.gz"
	URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
	SOURCE_SUBDIR not-built-by-its-own-cmake)
FetchContent_MakeAvailable(zlib)

set(sources adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c inflate.c infback.c
	inftrees.c inffast.c trees.c uncompr.c zutil.c)
list(TRANSFORM sources PREPEND "${zlib_SOURCE_DIR}/")
add_library(zlibstatic STATIC ${sources})
target_include_directories(zlibstatic PUBLIC "${zlib_SOURCE_DIR}")
if(MSVC)
	target_compile_definitions(zlibstatic PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
else()
	# the gz* functions' read, write, lseek and close
	target_compile_definitions(zlibstatic PRIVATE HAVE_UNISTD_H)
endif()

install(TARGETS zlibstatic ARCHIVE DESTINATION lib)
install(FILES "${zlib_SOURCE_DIR}/zlib.h" "${zlib_SOURCE_DIR}/zconf.h" DESTINATION include)
