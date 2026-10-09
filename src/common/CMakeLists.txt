sw_add_module(sw_common
	args.c arena.c bspfile.c cmd.c crc.c cvar.c fs.c info.c loader.c markup.c mathlib.c md4.c md5.c mem.c msg.c png.c print.c
	q_string.c sha1.c sha2.c tga.c version.c vispatch.c vmarray.c)
target_include_directories(sw_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# C11's atomics, which MSVC has behind a switch (the loader's thread, loader.c; the arenas' list,
# which it changes)
target_compile_options(sw_common PRIVATE $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
