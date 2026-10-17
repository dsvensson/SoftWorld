# WebRTC over libdatachannel where the build has it (SW_WEBRTC_FOUND, cmake/webrtc.cmake);
# HTTP and HTTPS GETs with mbedTLS in the native programs (cmake/mbedtls.cmake), on
# threads of their own where they run while the caller polls (a page's are fetch()'s,
# src/platform/web/net_http_web.c); a download as a file (vfs_http.c)
sw_add_module(sw_net
	net_adr.c net_chan.c net_main.c net_qtv.c net_qtvproto.c net_ws.c net_wsproto.c vfs_http.c
	$<IF:$<BOOL:${SW_WEBRTC_FOUND}>,net_rtc.c,net_rtc_none.c>
	$<IF:$<BOOL:${EMSCRIPTEN}>,net_http_none.c,net_http.c>
	$<$<NOT:$<BOOL:${EMSCRIPTEN}>>:net_http_async.c>)
target_link_libraries(sw_net PUBLIC sw_common)
if(NOT EMSCRIPTEN)
	target_link_libraries(sw_net PRIVATE MbedTLS::mbedtls)
endif()
if(SW_WEBRTC_FOUND)
	target_link_libraries(sw_net PRIVATE LibDataChannel::LibDataChannel)
endif()
# C11's atomics, which MSVC has behind a switch (the GETs' threads, the lock libdatachannel's
# threads take)
target_compile_options(sw_net PRIVATE $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
target_include_directories(sw_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
