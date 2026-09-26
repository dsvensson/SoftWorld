# The kernels behind simd.h: the scalar reference, and for x86-64-v4 the
# AVX-512 backend that the bind file forwards to.
if(SW_ARCH STREQUAL "x86-64-v4")
	sw_add_module(sw_simd simd_scalar.c simd_x86_64_v4.c simd_bind_x86_64_v4.c)
else()
	sw_add_module(sw_simd simd_scalar.c simd_bind_scalar.c)
endif()
target_link_libraries(sw_simd PUBLIC sw_common)
target_include_directories(sw_simd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
