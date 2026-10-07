# The server list (slist.h): qualia's indexer, its parts with the time handed
# in, which run on the list's thread with malloc's memory. The native programs
# scan on threads of their own and read url sources over HTTP and HTTPS
# (net_http.h); a page has no UDP, and its list is empty (slist_none.c).
sw_add_module(sw_slist
	slist_cache.c slist_proto.c slist_routes.c slist_sched.c slist_sources.c slist_view.c
	$<IF:$<BOOL:${EMSCRIPTEN}>,slist_none.c,slist_engine.c>)
target_link_libraries(sw_slist PUBLIC sw_net sw_common)
target_include_directories(sw_slist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT EMSCRIPTEN)
	# C11's atomics, which MSVC has behind a switch (the lock the threads take)
	target_compile_options(sw_slist PRIVATE $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
endif()
