sw_add_module(sw_client
	cl_cam.c cl_csqc.c cl_demo.c cl_download.c cl_ents.c cl_fchecks.c cl_fmod.c cl_input.c cl_items.c cl_main.c cl_menu.c
	cl_mvd.c cl_parse.c cl_qc.c cl_qtv.c cl_pred.c cl_slist.c cl_tent.c
	console.c keys.c sbar.c screen.c skin.c view.c)
target_link_libraries(sw_client PUBLIC sw_render sw_sound sw_game sw_slist sw_net sw_qcvm sw_common)
target_include_directories(sw_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# the client may stop a local server (sv_public.h); which server it gets is the program's choice
target_include_directories(sw_client PRIVATE ${PROJECT_SOURCE_DIR}/src/server)
# C11's atomics, which MSVC has behind a switch (a web download's thread, cl_download.c)
target_compile_options(sw_client PRIVATE $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)

# the menu the client runs when the game directory has none: menu-qc, built
# in, the version the program tells (SW_VERSION) in it
sw_qc_progs(sw_client SOURCE_DIR "${PROJECT_SOURCE_DIR}/menu-qc" DAT menu.dat SYMBOL cl_menuprogs
	STRINGS "SW_VERSION=${PROJECT_VERSION}")
