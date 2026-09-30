sw_add_module(sw_qcvm
	qc_disasm.c qc_map.c qc_opcode.c qc_progs.c)
target_link_libraries(sw_qcvm PUBLIC sw_common)
target_include_directories(sw_qcvm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
