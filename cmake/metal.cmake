# Compiles Metal shaders into a metallib (xcrun metal), which the programs carry
# inside them. The compiler is Xcode's Metal Toolchain component; the Command Line
# Tools alone don't have it, so when the active developer directory lacks it, the
# build points DEVELOPER_DIR at Xcode's.
set(SW_XCODE_DEVELOPER_DIR "/Applications/Xcode.app/Contents/Developer" CACHE PATH
	"Xcode's developer directory, for its Metal compiler when the active one has none")

execute_process(COMMAND xcrun -sdk macosx -f metal
	RESULT_VARIABLE sw_metal_result OUTPUT_QUIET ERROR_QUIET)
if(sw_metal_result EQUAL 0)
	set(SW_METAL_ENV "")
else()
	execute_process(COMMAND "${CMAKE_COMMAND}" -E env "DEVELOPER_DIR=${SW_XCODE_DEVELOPER_DIR}"
			xcrun -sdk macosx metal --version
		RESULT_VARIABLE sw_metal_result OUTPUT_QUIET ERROR_QUIET)
	if(NOT sw_metal_result EQUAL 0)
		message(FATAL_ERROR "No Metal compiler: install Xcode and its Metal Toolchain "
			"(xcodebuild -downloadComponent MetalToolchain), and set SW_XCODE_DEVELOPER_DIR "
			"if Xcode isn't at ${SW_XCODE_DEVELOPER_DIR}")
	endif()
	set(SW_METAL_ENV "DEVELOPER_DIR=${SW_XCODE_DEVELOPER_DIR}")
endif()

# sw_compile_metal(<output metallib> <metal file>)
function(sw_compile_metal out_lib metal)
	add_custom_command(
		OUTPUT "${out_lib}"
		COMMAND "${CMAKE_COMMAND}" -E env ${SW_METAL_ENV}
			xcrun -sdk macosx metal -std=metal4.0 -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}
			-fmetal-math-mode=safe -Werror -o "${out_lib}" "${metal}"
		MAIN_DEPENDENCY "${metal}"
		COMMENT "Compiling ${metal}"
		VERBATIM)
endfunction()
