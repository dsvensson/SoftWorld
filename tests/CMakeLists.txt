# test_simd runs every kernel of the build's SIMD backend (AVX-512, AVX2 or
# NEON) against the scalar reference, so it needs a build that has one; the
# backend's kernels are named by their prefix.
set(SW_SIMD_PREFIX_x86-64-v4 Simd_V4_)
set(SW_SIMD_PREFIX_x86-64-v3 Simd_V3_)
set(SW_SIMD_PREFIX_apple-m3 Simd_M3_)
if(DEFINED SW_SIMD_PREFIX_${SW_ARCH})
	sw_add_program(test_simd test_simd.c)
	target_link_libraries(test_simd PRIVATE sw_simd)
	target_compile_definitions(test_simd PRIVATE SW_SIMD_PREFIX=${SW_SIMD_PREFIX_${SW_ARCH}})
	add_test(NAME simd COMMAND test_simd)
endif()

# test_msg writes and reads coordinates and angles in each encoding.
sw_add_program(test_msg test_msg.c)
target_link_libraries(test_msg PRIVATE sw_common)
add_test(NAME msg COMMAND test_msg)

# test_md4 checks the map checksum's MD4 against RFC 1320's digests.
sw_add_program(test_md4 test_md4.c)
target_link_libraries(test_md4 PRIVATE sw_common)
add_test(NAME md4 COMMAND test_md4)

# test_sha1 checks the SHA-1 f_modified hashes files with against FIPS 180's
# digests.
sw_add_program(test_sha1 test_sha1.c)
target_link_libraries(test_sha1 PRIVATE sw_common)
add_test(NAME sha1 COMMAND test_sha1)

# test_markup reads ezQuake's and FTE's colors out of text.
sw_add_program(test_markup test_markup.c)
target_link_libraries(test_markup PRIVATE sw_common)
add_test(NAME markup COMMAND test_markup)

# test_bspload loads every map under the directory SW_BASEDIR names as a
# collision map and as a render model. It needs the maps, so only the
# msvc-v4-maps test preset runs it, and without SW_BASEDIR it is skipped.
sw_add_program(test_bspload test_bspload.c)
target_link_libraries(test_bspload PRIVATE sw_game sw_render sw_common)
add_test(NAME bspload COMMAND test_bspload)
set_tests_properties(bspload PROPERTIES LABELS maps SKIP_RETURN_CODE 77)

# test_workers runs Sys_Parallel over the platform's worker threads.
sw_add_program(test_workers test_workers.c)
if(WIN32)
	target_link_libraries(test_workers PRIVATE sw_platform_win)
else()
	target_link_libraries(test_workers PRIVATE sw_platform_posix)
endif()
add_test(NAME workers COMMAND test_workers)

# test_present draws present.metal with Metal 4 as vid_metal.m does, and compares
# what it shows with VID_FrameToRGB. It needs the GPU.
if(APPLE)
	sw_add_program(test_present test_present.m
		"${PROJECT_SOURCE_DIR}/src/platform/vid_common.c"
		$<TARGET_OBJECTS:sw_present_metallib>)
	target_include_directories(test_present PRIVATE
		"${PROJECT_SOURCE_DIR}/src/platform" "${PROJECT_SOURCE_DIR}/src/renderer")
	target_link_libraries(test_present PRIVATE sw_common "-framework Metal" "-framework Foundation")
	add_test(NAME present COMMAND test_present)
endif()

# test_present_vulkan draws present.glsl with Vulkan as vid_vulkan.c does, and
# compares what it shows with VID_FrameToRGB, and its HDR and PQ with what they
# should be. It needs a GPU with Vulkan 1.3, and without one is skipped.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(Vulkan 1.3 REQUIRED)
	sw_add_program(test_present_vulkan test_present_vulkan.c
		"${PROJECT_SOURCE_DIR}/src/platform/vid_common.c"
		$<TARGET_OBJECTS:sw_present_spirv>)
	target_include_directories(test_present_vulkan PRIVATE
		"${PROJECT_SOURCE_DIR}/src/platform" "${PROJECT_SOURCE_DIR}/src/platform/linux"
		"${PROJECT_SOURCE_DIR}/src/renderer")
	target_link_libraries(test_present_vulkan PRIVATE sw_common Vulkan::Vulkan)
	add_test(NAME present_vulkan COMMAND test_present_vulkan)
	set_tests_properties(present_vulkan PROPERTIES SKIP_RETURN_CODE 77)
endif()

# the QuakeC VM's tests
add_subdirectory(qcvm)
