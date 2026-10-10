# Warning policy: MSVC /W4, clang-cl /W4 (= -Wall -Wextra) and Apple clang -Wall -Wextra,
# treated as errors when SW_WARNINGS_AS_ERRORS is on. No per-warning suppressions.
option(SW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# What GCC checks and clang doesn't, where the codebase is already clean of it.
# The front end's checks hold however the build is optimized; the middle end's
# want an optimized one, and are compiled, not linked, so a Release build's LTO
# link (which carries no warning flags) is not where they are found. Each is
# asked for rather than looked up by version, as the older GCC a distribution
# has takes only the ones it knows.
set(sw_gnu_warnings
	# the front end
	-Wduplicated-cond -Wjump-misses-init -Wenum-int-mismatch -Wformat-signedness
	-Wflex-array-member-not-at-end -Wcalloc-transposed-args
	-Wunterminated-string-initialization -Wtrampolines -Wbidi-chars=any
	-Wvla -Walloca -Wundef -Wold-style-definition
	# the middle end
	-Warray-bounds=2 -Wstringop-overflow=4 -Wshift-overflow=2 -Walloc-zero
	-Wnull-dereference -Wuse-after-free=3 -Wdangling-pointer=2)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT DEFINED SW_GNU_WARNINGS)
	include(CheckCCompilerFlag)
	set(sw_taken "")
	foreach(sw_flag IN LISTS sw_gnu_warnings)
		string(MAKE_C_IDENTIFIER "sw_has_${sw_flag}" sw_var)
		check_c_compiler_flag("${sw_flag}" ${sw_var})
		if(${sw_var})
			list(APPEND sw_taken "${sw_flag}")
		endif()
	endforeach()
	set(SW_GNU_WARNINGS "${sw_taken}" CACHE INTERNAL "GCC's own warnings this GCC takes")
	message(STATUS "GCC's own warnings: ${SW_GNU_WARNINGS}")
endif()

function(sw_set_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4)
		if(SW_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
		if(SW_GNU_WARNINGS)
			target_compile_options(${target} PRIVATE ${SW_GNU_WARNINGS})
		endif()
		if(SW_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
endfunction()
