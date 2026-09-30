# cmake -DFTEQCC=<fteqcc> -DSRC=<fixture dir> -DFIXTURE=<name.qc> "-DFLAGS=<flags>"
#       -DOUT=<file.dat> [-DRUNNER=OFF] -P qcfixture.cmake
#
# Compiles a QuakeC test fixture after runner.qh, the builtins of the test's
# runner (unless RUNNER is OFF: the fixture declares its own), into OUT, as
# qcvm-rs's tests do: in a directory of its own with a progs.src naming them, as
# fteqcc writes where progs.src says.

get_filename_component(dir "${OUT}" DIRECTORY)
get_filename_component(name "${OUT}" NAME_WE)
set(work "${dir}/${name}.src")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
if(DEFINED RUNNER AND NOT RUNNER)
	file(COPY "${SRC}/${FIXTURE}" DESTINATION "${work}")
	file(WRITE "${work}/progs.src" "out.dat\n${FIXTURE}\n")
else()
	file(COPY "${SRC}/runner.qh" "${SRC}/${FIXTURE}" DESTINATION "${work}")
	file(WRITE "${work}/progs.src" "out.dat\nrunner.qh\n${FIXTURE}\n")
endif()
separate_arguments(flags NATIVE_COMMAND "${FLAGS}")
execute_process(COMMAND "${FTEQCC}" -srcfile progs.src ${flags}
	WORKING_DIRECTORY "${work}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE output)
if(NOT result EQUAL 0 OR NOT EXISTS "${work}/out.dat")
	message(FATAL_ERROR "fteqcc couldn't compile ${FIXTURE} (${FLAGS}):\n${output}")
endif()
file(COPY_FILE "${work}/out.dat" "${OUT}")
