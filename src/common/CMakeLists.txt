sw_add_module(sw_common
	args.c arena.c cmd.c crc.c cvar.c fs.c info.c mathlib.c md4.c mem.c msg.c png.c print.c q_string.c version.c vmarray.c)
target_include_directories(sw_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
