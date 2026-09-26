sw_add_module(sw_sound
	snd_dma.c snd_mem.c snd_mix.c)
target_link_libraries(sw_sound PUBLIC sw_common)
target_include_directories(sw_sound PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
