sw_add_module(sw_server
	pr_cmds.c pr_edict.c sv_ccmds.c sv_ents.c sv_init.c sv_main.c sv_move.c
	sv_mvd.c sv_nchan.c sv_nqmsg.c sv_phys.c sv_send.c sv_user.c world.c)
target_link_libraries(sw_server PUBLIC sw_game sw_net sw_qcvm sw_common)
target_include_directories(sw_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# the game the server runs when the game directory has none: qw-qc, built in
sw_qc_progs(sw_server SOURCE_DIR "${PROJECT_SOURCE_DIR}/qw-qc" DAT qwprogs.dat SYMBOL sv_qwprogs)

# the client-only program's server: sv_public.h with nothing behind it
sw_add_module(sw_server_null
	sv_null.c)
target_link_libraries(sw_server_null PUBLIC sw_common)
target_include_directories(sw_server_null PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
