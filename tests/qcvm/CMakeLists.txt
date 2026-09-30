# The QuakeC VM's tests share an assembler that writes every progs format,
# checks, and a harness that calls builtins by name (qc_testsupport).
sw_add_module(qc_testsupport qc_asm.c qc_harness.c qc_test.c)
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

# test_qc_opcodes runs every opcode, in 16- and 32-bit statements.
sw_add_program(test_qc_opcodes test_qc_opcodes.c)
target_link_libraries(test_qc_opcodes PRIVATE qc_testsupport)
add_test(NAME qc_opcodes COMMAND test_qc_opcodes)

# test_qc_vm checks calls, builtins, re-entrancy, errors and their backtraces,
# the limits, abort, tracing, the animation opcodes, and a longjmp out of the VM.
sw_add_program(test_qc_vm test_qc_vm.c)
target_link_libraries(test_qc_vm PRIVATE qc_testsupport)
add_test(NAME qc_vm COMMAND test_qc_vm)

# test_qc_fuzz runs random programs (QC_FUZZ_ITERS of them, 256 by default),
# which must not harm the VM, and checks the interpreter against its tracing
# instance and against a budget handed out in chunks.
sw_add_program(test_qc_fuzz test_qc_fuzz.c)
target_link_libraries(test_qc_fuzz PRIVATE qc_testsupport)
add_test(NAME qc_fuzz COMMAND test_qc_fuzz)

# test_qc_lib_<area> test the standard builtins of an area through the harness:
# the ports of qcvm-rs's tests/all/builtins_misc and builtins_strings.
foreach(area IN ITEMS charset convert digest dtoa entity format hash hostcalls introspect json math memory reflect strbuf
		strftime string time tokenize vector)
	sw_add_program(test_qc_lib_${area} test_qc_lib_${area}.c)
	target_link_libraries(test_qc_lib_${area} PRIVATE qc_testsupport)
	add_test(NAME qc_lib_${area} COMMAND test_qc_lib_${area})
endforeach()

# test_qc_fixtures runs the QuakeC fixtures in qc/, each compiled with fteqcc for
# the targets it is meant for, with the builtins of FTE's standalone runner
# (qc_runner.c): their output must be the .expected file's, and FTE_QCVM's when
# it names FTE's runner. Without fteqcc it is skipped.
set(QC_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(QC_FIXTURES
	"arith.qc|fte|-Tfte" "arith.qc|fte-O3|-Tfte -O3" "arith.qc|vanilla|-Tvanilla"
	"control.qc|fte|-Tfte" "control.qc|fte-O3|-Tfte -O3" "control.qc|vanilla|-Tvanilla"
	"ints.qc|fte|-Tfte" "ints.qc|fte-O3|-Tfte -O3"
	"pointers.qc|fte|-Tfte" "pointers.qc|fte-O3|-Tfte -O3"
	"entities.qc|fte|-Tfte" "entities.qc|fte-O3|-Tfte -O3" "entities.qc|vanilla|-Tvanilla"
	"strings.qc|fte|-Tfte" "strings.qc|fte-O3|-Tfte -O3" "strings.qc|vanilla|-Tvanilla"
	"funcs.qc|fte_6614|-Tfte_6614" "funcs.qc|fte_6614-O3|-Tfte_6614 -O3"
	"hexen2.qc|h2|-Th2"
	"state.qc|fte|-Tfte" "state.qc|fte-O3|-Tfte -O3" "state.qc|vanilla|-Tvanilla"
	"advanced.qc|fte|-Tfte" "advanced.qc|fte-O3|-Tfte -O3")
set(qc_fixture_outputs)
set(qc_fixture_list)
if(SW_FTEQCC)
	foreach(entry IN LISTS QC_FIXTURES)
		string(REPLACE "|" ";" parts "${entry}")
		list(GET parts 0 fixture)
		list(GET parts 1 label)
		list(GET parts 2 flags)
		get_filename_component(base "${fixture}" NAME_WE)
		set(out "${QC_FIXTURE_DIR}/${base}-${label}.dat")
		add_custom_command(OUTPUT "${out}"
			COMMAND "${CMAKE_COMMAND}" "-DFTEQCC=${SW_FTEQCC}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/qc"
				"-DFIXTURE=${fixture}" "-DFLAGS=${flags}" "-DOUT=${out}"
				-P "${PROJECT_SOURCE_DIR}/cmake/qcfixture.cmake"
			DEPENDS "qc/${fixture}" qc/runner.qh "${PROJECT_SOURCE_DIR}/cmake/qcfixture.cmake"
			COMMENT "Compiling ${fixture} for ${label}"
			VERBATIM)
		list(APPEND qc_fixture_outputs "${out}")
		list(APPEND qc_fixture_list "${base}|${label}")
	endforeach()
endif()
add_custom_target(qc_fixtures DEPENDS ${qc_fixture_outputs})
string(REPLACE ";" "," qc_fixture_list "${qc_fixture_list}")

sw_add_program(test_qc_fixtures test_qc_fixtures.c qc_runner.c)
target_link_libraries(test_qc_fixtures PRIVATE qc_testsupport)
target_compile_definitions(test_qc_fixtures PRIVATE
	QT_FIXTURE_DIR="${QC_FIXTURE_DIR}" QT_FIXTURE_SOURCES="${CMAKE_CURRENT_SOURCE_DIR}/qc"
	QT_FIXTURES="${qc_fixture_list}")
add_dependencies(test_qc_fixtures qc_fixtures)
add_test(NAME qc_fixtures COMMAND test_qc_fixtures)
set_tests_properties(qc_fixtures PROPERTIES SKIP_RETURN_CODE 77)

# test_qc_multiprogs loads further progs into a VM: linking, calls between
# progs, shared globals, fields unified by name, relocated strings, and the
# limits.
sw_add_program(test_qc_multiprogs test_qc_multiprogs.c)
target_link_libraries(test_qc_multiprogs PRIVATE qc_testsupport)
add_test(NAME qc_multiprogs COMMAND test_qc_multiprogs)

# test_qc_threads runs QuakeC threads (sleep, fork) resumed by QC_RunThreads,
# with qc/threads.qc compiled by fteqcc when there is one.
set(qc_threads_dat "")
if(SW_FTEQCC)
	set(qc_threads_dat "${QC_FIXTURE_DIR}/threads-fte.dat")
	add_custom_command(OUTPUT "${qc_threads_dat}"
		COMMAND "${CMAKE_COMMAND}" "-DFTEQCC=${SW_FTEQCC}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/qc"
			-DFIXTURE=threads.qc -DFLAGS=-Tfte "-DOUT=${qc_threads_dat}" -DRUNNER=OFF
			-P "${PROJECT_SOURCE_DIR}/cmake/qcfixture.cmake"
		DEPENDS qc/threads.qc "${PROJECT_SOURCE_DIR}/cmake/qcfixture.cmake"
		COMMENT "Compiling threads.qc for fte")
endif()
add_custom_target(qc_threads_fixture DEPENDS ${qc_threads_dat})
sw_add_program(test_qc_threads test_qc_threads.c)
target_link_libraries(test_qc_threads PRIVATE qc_testsupport)
target_compile_definitions(test_qc_threads PRIVATE QT_THREADS_DAT="${qc_threads_dat}")
add_dependencies(test_qc_threads qc_threads_fixture)
add_test(NAME qc_threads COMMAND test_qc_threads)
set_tests_properties(qc_threads PROPERTIES SKIP_RETURN_CODE 77)
