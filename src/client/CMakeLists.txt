sw_add_module(sw_client
	cl_cam.c cl_demo.c cl_download.c cl_ents.c cl_input.c cl_main.c cl_parse.c cl_pred.c cl_tent.c
	console.c keys.c menu.c sbar.c screen.c skin.c view.c)
target_link_libraries(sw_client PUBLIC sw_render sw_sound sw_game sw_net sw_common)
target_include_directories(sw_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# the client may stop a local server (sv_public.h); which server it gets is the program's choice
target_include_directories(sw_client PRIVATE ${PROJECT_SOURCE_DIR}/src/server)
