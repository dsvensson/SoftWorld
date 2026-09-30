# The QuakeC VM's tests share an assembler that writes every progs format, and
# checks (qc_testsupport).
sw_add_module(qc_testsupport qc_asm.c qc_test.c)
target_link_libraries(qc_testsupport PUBLIC sw_qcvm)
if(WIN32)
	target_link_libraries(qc_testsupport PUBLIC sw_platform_win)
else()
	target_link_libraries(qc_testsupport PUBLIC sw_platform_posix)
endif()
target_include_directories(qc_testsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(qc_testsupport PUBLIC QT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# test_qc_loader loads programs in every format, with the load-time rewrites,
# and malformed and corrupted ones; qw-qc's qwprogs.dat, and KTX's csprogs.dat
# when QCVM_CSPROGS names one.
sw_add_program(test_qc_loader test_qc_loader.c)
target_link_libraries(test_qc_loader PRIVATE qc_testsupport)
add_test(NAME qc_loader COMMAND test_qc_loader)

# test_qc_memory checks the VM's memory before anything runs: its layout, where
# addresses land, the host's header in each entity, entity slots and their
# reuse, fields added later, the heap, and temp and static strings and their
# collection.
sw_add_program(test_qc_memory test_qc_memory.c)
target_link_libraries(test_qc_memory PRIVATE qc_testsupport)
add_test(NAME qc_memory COMMAND test_qc_memory)
