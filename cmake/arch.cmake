# Instruction-set target. Selects compiler flags, the SIMD kernel backend and the
# startup CPU check implementation.
set(SW_ARCH "x86-64-v4" CACHE STRING "Target instruction set (x86-64-v4 or generic)")
set_property(CACHE SW_ARCH PROPERTY STRINGS x86-64-v4 generic)

if(SW_ARCH STREQUAL "x86-64-v4")
	set(SW_ARCH_SUFFIX x86_64_v4)
	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		set(SW_ARCH_FLAGS "/clang:-march=x86-64-v4")
	else()
		set(SW_ARCH_FLAGS "/arch:AVX512")
	endif()
elseif(SW_ARCH STREQUAL "generic")
	set(SW_ARCH_SUFFIX generic)
	set(SW_ARCH_FLAGS "")
else()
	message(FATAL_ERROR "Unknown SW_ARCH '${SW_ARCH}'")
endif()

# Applies the architecture flags to a target. Entry/CPU-check code must not use this.
function(sw_set_arch target)
	if(SW_ARCH_FLAGS)
		target_compile_options(${target} PRIVATE ${SW_ARCH_FLAGS})
	endif()
endfunction()
