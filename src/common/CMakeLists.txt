sw_add_module(sw_common
	args.c arena.c bspfile.c cmd.c crc.c cvar.c fs.c info.c markup.c mathlib.c md4.c md5.c mem.c msg.c png.c print.c
	q_string.c sha1.c sha2.c version.c vmarray.c)
target_include_directories(sw_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
