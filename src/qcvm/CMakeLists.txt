sw_add_module(sw_qcvm
	qc_builtins.c qc_call.c qc_config.c qc_disasm.c qc_dtoa.c qc_error.c qc_exec.c qc_heap.c qc_lib.c
	qc_lib_charset.c qc_lib_convert.c qc_lib_entity.c qc_lib_format.c qc_lib_hostcalls.c qc_lib_introspect.c
	qc_lib_math.c qc_lib_reflect.c qc_lib_strftime.c qc_lib_string.c qc_lib_time.c qc_lib_tokenize.c qc_lib_vector.c qc_map.c qc_mem.c qc_numbers.c qc_opcode.c qc_progs.c qc_strings.c
	qc_threads.c qc_vm.c)
target_link_libraries(sw_qcvm PUBLIC sw_common)
target_include_directories(sw_qcvm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
