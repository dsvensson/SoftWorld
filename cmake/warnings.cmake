# Warning policy: MSVC /W4 and clang-cl /W4 (= -Wall -Wextra), treated as errors when
# SW_WARNINGS_AS_ERRORS is on. No per-warning suppressions.
option(SW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

function(sw_set_warnings target)
	target_compile_options(${target} PRIVATE /W4)
	if(SW_WARNINGS_AS_ERRORS)
		target_compile_options(${target} PRIVATE /WX)
	endif()
endfunction()
