# On the web the tests run in node (the toolchain's emulator), on its file
# system, with their exit codes.
if(EMSCRIPTEN)
	add_link_options(-sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1)
endif()

# cpu_supported tells whether this CPU runs the build, so the tests can be: not a
# test, and built without the architecture's flags (sw_cpu_check's) to run on any.
add_executable(cpu_supported cpu_supported.c $<TARGET_OBJECTS:sw_cpu_check>)
target_include_directories(cpu_supported PRIVATE "${PROJECT_SOURCE_DIR}/src/platform")
set_target_properties(cpu_supported PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
sw_set_warnings(cpu_supported)

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

# test_md5 and test_sha2 check the digests QuakeC's digest builtins make
# against RFC 1321's and FIPS 180's.
sw_add_program(test_md5 test_md5.c)
target_link_libraries(test_md5 PRIVATE sw_common)
add_test(NAME md5 COMMAND test_md5)
sw_add_program(test_sha2 test_sha2.c)
target_link_libraries(test_sha2 PRIVATE sw_common)
add_test(NAME sha2 COMMAND test_sha2)

# test_ws reads WebSocket handshakes and frames as the server's WebSocket port
# takes them (net_wsproto.c).
sw_add_program(test_ws test_ws.c)
target_link_libraries(test_ws PRIVATE sw_net sw_common)
add_test(NAME ws COMMAND test_ws)

# test_markup reads ezQuake's and FTE's colors out of text.
sw_add_program(test_markup test_markup.c)
target_link_libraries(test_markup PRIVATE sw_common)
add_test(NAME markup COMMAND test_markup)

# test_tga reads TGA images of each kind, and ones cut short.
sw_add_program(test_tga test_tga.c)
target_link_libraries(test_tga PRIVATE sw_common)
add_test(NAME tga COMMAND test_tga)

# test_bspload loads every map under the directory SW_BASEDIR names as a
# collision map and as a render model. It needs the maps, so only the -maps
# test presets run it (CI's with the shareware's), and without SW_BASEDIR it is
# skipped.
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
if(EMSCRIPTEN)
	# the threads made ahead, as the programs make them
	target_link_options(test_workers PRIVATE -sPTHREAD_POOL_SIZE=${SW_WEB_WORKERS})
endif()
add_test(NAME workers COMMAND test_workers)

# test_present draws present.metal with Metal 4 as vid_metal.m does, and compares
# what it shows with VID_FrameToRGB. It needs a GPU with Metal 4, and without one
# is skipped.
if(APPLE)
	sw_add_program(test_present test_present.m
		"${PROJECT_SOURCE_DIR}/src/platform/vid_common.c"
		$<TARGET_OBJECTS:sw_present_metallib>)
	target_include_directories(test_present PRIVATE
		"${PROJECT_SOURCE_DIR}/src/platform" "${PROJECT_SOURCE_DIR}/src/renderer")
	# the file system's calls into the platform: only link-time optimisation
	# (Release) leaves them out
	target_link_libraries(test_present PRIVATE sw_platform_posix sw_common "-framework Metal"
		"-framework Foundation")
	add_test(NAME present COMMAND test_present)
	set_tests_properties(present PROPERTIES SKIP_RETURN_CODE 77)
endif()

# test_present_d3d11 draws present.hlsl with Direct3D 11 as vid_d3d11.c does, and
# compares what it shows with VID_FrameToRGB, and its HDR with what it should be;
# on the GPU, else WARP.
if(WIN32)
	sw_add_program(test_present_d3d11 test_present_d3d11.c
		"${PROJECT_SOURCE_DIR}/src/platform/vid_common.c")
	add_dependencies(test_present_d3d11 sw_platform_gui)	# the shaders' headers
	target_include_directories(test_present_d3d11 PRIVATE
		"${PROJECT_SOURCE_DIR}/src/platform" "${PROJECT_SOURCE_DIR}/src/platform/win"
		"${PROJECT_SOURCE_DIR}/src/renderer" "${PROJECT_BINARY_DIR}/src/platform/shaders")
	target_link_libraries(test_present_d3d11 PRIVATE sw_platform_win sw_common d3d11 dxgi dxguid)
	add_test(NAME present_d3d11 COMMAND test_present_d3d11)
	add_test(NAME present_d3d11_warp COMMAND test_present_d3d11 -warp)
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

# test_present_webgl draws the web's present.glsl with WebGL 2 as vid_webgl.c
# does, and compares what it shows with VID_FrameToRGB; test_sound_web plays a
# wave through the sound's AudioWorklet and checks what it plays. They run in a
# browser: browser_test.mjs serves each and runs it in a headless Chrome of its
# own (drawing in software, SwiftShader; sound playing without a gesture,
# muted). Without Chrome they aren't made, and a Chrome that can't skips them.
if(EMSCRIPTEN)
	find_program(SW_CHROME NAMES chrome google-chrome google-chrome-stable chromium chromium-browser "Google Chrome"
		PATHS "C:/Program Files/Google/Chrome/Application" "/Applications/Google Chrome.app/Contents/MacOS")
endif()
if(EMSCRIPTEN AND SW_CHROME AND CMAKE_CROSSCOMPILING_EMULATOR)
	set(SW_WEB_SOURCE "${PROJECT_SOURCE_DIR}/src/platform/web")

	sw_add_program(test_present_webgl test_present_webgl.c
		"${PROJECT_SOURCE_DIR}/src/platform/vid_common.c" "${SW_WEB_SOURCE}/present_glsl.c")
	set_source_files_properties("${SW_WEB_SOURCE}/present_glsl.c" PROPERTIES OBJECT_DEPENDS "${SW_WEB_SOURCE}/present.glsl")
	target_include_directories(test_present_webgl PRIVATE
		"${PROJECT_SOURCE_DIR}/src/platform" "${PROJECT_SOURCE_DIR}/src/renderer")
	target_link_libraries(test_present_webgl PRIVATE sw_common)

	sw_add_program(test_sound_web test_sound_web.c)
	target_include_directories(test_sound_web PRIVATE
		"${SW_WEB_SOURCE}" "${PROJECT_SOURCE_DIR}/src/platform/posix" "${PROJECT_SOURCE_DIR}/src/sound")
	target_link_libraries(test_sound_web PRIVATE sw_common)
	add_custom_command(TARGET test_sound_web POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SW_WEB_SOURCE}/snd_worklet.js"
			"$<TARGET_FILE_DIR:test_sound_web>/softworld-sound.js"
		VERBATIM)

	# pages, not node's: the libraries of the page's side, and emrun's reports
	# of what they print and their exit (--emrun), which browser_test.mjs reads
	set(SW_LIBRARY_present_webgl vid_webgl.js)
	set(SW_LIBRARY_sound_web snd_webaudio.js)
	foreach(test present_webgl sound_web)
		set_target_properties(test_${test} PROPERTIES SUFFIX ".html"
			LINK_DEPENDS "${SW_WEB_SOURCE}/vid_webgl.js;${SW_WEB_SOURCE}/snd_webaudio.js")
		target_link_options(test_${test} PRIVATE -sENVIRONMENT=web,worker -sNODERAWFS=0 --emrun
			"SHELL:--js-library ${SW_WEB_SOURCE}/${SW_LIBRARY_${test}}")
		add_test(NAME ${test} COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${CMAKE_CURRENT_SOURCE_DIR}/browser_test.mjs"
			"${SW_CHROME}" "$<TARGET_FILE:test_${test}>" 60)
		set_tests_properties(${test} PROPERTIES SKIP_RETURN_CODE 77)
	endforeach()
endif()

# the QuakeC VM's tests
add_subdirectory(qcvm)
