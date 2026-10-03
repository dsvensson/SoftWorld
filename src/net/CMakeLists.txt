# WebRTC over libdatachannel where the build has it (SW_WEBRTC, cmake/webrtc.cmake)
sw_add_module(sw_net
	net_chan.c net_main.c net_ws.c net_wsproto.c
	$<IF:$<BOOL:${SW_WEBRTC}>,net_rtc.c,net_rtc_none.c>)
target_link_libraries(sw_net PUBLIC sw_common)
if(SW_WEBRTC)
	target_link_libraries(sw_net PRIVATE datachannel-static)
	# C11's atomics, which MSVC has behind a switch (the lock libdatachannel's threads take)
	target_compile_options(sw_net PRIVATE $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
endif()
target_include_directories(sw_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
