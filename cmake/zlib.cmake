# zlib for the programs' gzip (src/common/vfs_gzip.c). A page has
# Emscripten's port; elsewhere a static zlib, the one find_package finds (the
# system's, or one built here before), else one built here at configure
# (cmake/zlib, deps.cmake) and installed under SW_DEPS_DIR, where configures
# after (and CI's caches) find it. ZLIB::ZLIB is the library to link.

include(deps)

if(EMSCRIPTEN)
	# the port, compiled and linked with the programs' flags (threads too)
	if(NOT TARGET ZLIB::ZLIB)
		add_library(ZLIB::ZLIB INTERFACE IMPORTED)
		target_compile_options(ZLIB::ZLIB INTERFACE -sUSE_ZLIB=1)
		target_link_options(ZLIB::ZLIB INTERFACE -sUSE_ZLIB=1)
	endif()
	message(STATUS "zlib: Emscripten's port")
	return()
endif()

if(NOT TARGET ZLIB::ZLIB)
	# static, as the programs' other libraries are: the systems that have only
	# a shared one (Arch's, the macOS SDK's) build it too
	set(ZLIB_USE_STATIC_LIBS ON)
	sw_deps_prefix(sw_zlib_prefix zlib "${CMAKE_CURRENT_LIST_DIR}/zlib/CMakeLists.txt")
	list(PREPEND CMAKE_PREFIX_PATH "${sw_zlib_prefix}")
	sw_deps_forget("${sw_zlib_prefix}" ZLIB_INCLUDE_DIR ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG)
	# the headers and the library of one zlib: a search that found no static
	# library kept the system's headers (its shared one's), which go once the
	# prefix has its own
	if(EXISTS "${sw_zlib_prefix}/include/zlib.h" AND DEFINED CACHE{ZLIB_INCLUDE_DIR}
		AND NOT "$CACHE{ZLIB_INCLUDE_DIR}" STREQUAL "${sw_zlib_prefix}/include")
		unset(ZLIB_INCLUDE_DIR CACHE)
	endif()
	find_package(ZLIB 1.2 QUIET)
	if(NOT TARGET ZLIB::ZLIB)
		sw_deps_build(zlib "zlib" "${CMAKE_CURRENT_LIST_DIR}/zlib" "${sw_zlib_prefix}")
		unset(ZLIB_INCLUDE_DIR CACHE)
		unset(ZLIB_LIBRARY_RELEASE CACHE)
		unset(ZLIB_LIBRARY_DEBUG CACHE)
		find_package(ZLIB 1.2 QUIET)
		if(NOT TARGET ZLIB::ZLIB)
			message(FATAL_ERROR "zlib: built into ${sw_zlib_prefix}, but not found there")
		endif()
	endif()
	sw_deps_map_configs()
endif()
message(STATUS "zlib: ${ZLIB_VERSION} (${ZLIB_INCLUDE_DIR})")
