sw_add_module(sw_particles
	p_run.c p_script.c p_spawn.c)
target_link_libraries(sw_particles PUBLIC sw_render sw_common)
target_include_directories(sw_particles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
