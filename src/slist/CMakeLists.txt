# The server list (slist.h): qualia's indexer, its parts with the time handed
# in, which run on the list's thread with malloc's memory
sw_add_module(sw_slist
	slist_cache.c slist_proto.c slist_routes.c slist_sched.c slist_sources.c slist_view.c)
target_link_libraries(sw_slist PUBLIC sw_net sw_common)
target_include_directories(sw_slist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
