# AddressSanitizer builds (the msvc-asan preset, or SW_ASAN on macOS). With MSVC the
# sanitizer runtime is a DLL next to the compiler; sw_copy_sanitizer_runtime puts it
# next to an executable. SW_UBSAN adds UndefinedBehaviorSanitizer (clang and GCC
# only), which stops at the first report: what a C port of a VM gets wrong
# (shifts, overflow, float to integer conversions) ASan alone doesn't see.
option(SW_ASAN "Build with AddressSanitizer" OFF)
option(SW_UBSAN "Build with UndefinedBehaviorSanitizer (clang and GCC)" OFF)

if(SW_UBSAN AND MSVC)
	message(FATAL_ERROR "SW_UBSAN: MSVC has no UndefinedBehaviorSanitizer")
elseif(SW_UBSAN)
	add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
	add_link_options(-fsanitize=undefined)
endif()

if(SW_ASAN AND MSVC)
	add_compile_options(/fsanitize=address)
	add_link_options(/INCREMENTAL:NO)
	# run-time checks and the sanitizer can't be combined
	string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	get_filename_component(SW_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
	file(GLOB SW_ASAN_RUNTIME "${SW_COMPILER_DIR}/clang_rt.asan*dynamic-x86_64.dll")
	if(NOT SW_ASAN_RUNTIME)
		message(FATAL_ERROR "SW_ASAN: no AddressSanitizer runtime next to ${CMAKE_C_COMPILER}")
	endif()
elseif(SW_ASAN)
	add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address)
endif()

function(sw_copy_sanitizer_runtime target)
	if(SW_ASAN AND MSVC)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SW_ASAN_RUNTIME} "$<TARGET_FILE_DIR:${target}>")
	endif()
endfunction()
