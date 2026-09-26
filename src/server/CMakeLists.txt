sw_add_module(sw_server
	pr_cmds.c pr_edict.c pr_exec.c sv_ccmds.c sv_ents.c sv_init.c sv_main.c sv_move.c
	sv_nchan.c sv_phys.c sv_send.c sv_user.c world.c)
target_link_libraries(sw_server PUBLIC sw_game sw_net sw_common)
target_include_directories(sw_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
