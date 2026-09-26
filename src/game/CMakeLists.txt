sw_add_module(sw_game
	cmodel.c pmove.c pmovetst.c)
target_link_libraries(sw_game PUBLIC sw_common)
target_include_directories(sw_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
