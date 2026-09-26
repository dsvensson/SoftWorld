# test_simd runs every AVX-512 kernel against the scalar reference, so it
# needs the x86-64-v4 build.
if(SW_ARCH STREQUAL "x86-64-v4")
	sw_add_program(test_simd test_simd.c)
	target_link_libraries(test_simd PRIVATE sw_simd)
	add_test(NAME simd COMMAND test_simd)
endif()
