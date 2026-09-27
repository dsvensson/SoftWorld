# test_simd runs every AVX-512 kernel against the scalar reference, so it
# needs the x86-64-v4 build.
if(SW_ARCH STREQUAL "x86-64-v4")
	sw_add_program(test_simd test_simd.c)
	target_link_libraries(test_simd PRIVATE sw_simd)
	add_test(NAME simd COMMAND test_simd)
endif()

# test_msg writes and reads coordinates and angles in each encoding.
sw_add_program(test_msg test_msg.c)
target_link_libraries(test_msg PRIVATE sw_common)
add_test(NAME msg COMMAND test_msg)

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
