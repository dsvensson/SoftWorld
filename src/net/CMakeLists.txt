sw_add_module(sw_net
	net_chan.c net_main.c)
target_link_libraries(sw_net PUBLIC sw_common)
target_include_directories(sw_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
