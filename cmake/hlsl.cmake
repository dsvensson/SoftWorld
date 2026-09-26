# Compiles HLSL entry points into C headers holding the bytecode (fxc /Fh).
find_program(SW_FXC NAMES fxc REQUIRED DOC "HLSL compiler from the Windows SDK")

# sw_compile_hlsl(<output header> <hlsl file> <profile> <entry point> <variable name>)
function(sw_compile_hlsl out_header hlsl profile entry var)
	add_custom_command(
		OUTPUT "${out_header}"
		COMMAND "${SW_FXC}" /nologo /O3 /WX /T ${profile} /E ${entry} /Vn ${var} /Fh "${out_header}" "${hlsl}"
		MAIN_DEPENDENCY "${hlsl}"
		COMMENT "Compiling ${entry} (${profile}) from ${hlsl}"
		VERBATIM)
endfunction()
