# cmake -DFTEQCC=<fteqcc> -DSRC=<dir> -DOUT=<dir> -DDAT=<name.dat> ["-DFLAGS=<flags>"] -P qcc.cmake
#
# Compiles a QuakeC directory (qw-qc, menu-qc) into OUT/DAT. fteqcc writes
# where progs.src says, beside it, so it compiles a copy in OUT/src and the
# source tree is left alone. Quiet unless it fails: id's QuakeC has warnings
# aplenty.

file(REMOVE_RECURSE "${OUT}/src")
file(MAKE_DIRECTORY "${OUT}/src")
file(GLOB sources "${SRC}/*.qc" "${SRC}/*.qh" "${SRC}/progs.src")
file(COPY ${sources} DESTINATION "${OUT}/src")

# from inside the copy: fteqcc keeps the paths it was given in the progs, so
# the same sources give the same progs wherever they are built (and given a /
# path, it takes progs.src's "./" from the root of the drive)
separate_arguments(flags NATIVE_COMMAND "${FLAGS}")
execute_process(COMMAND "${FTEQCC}" -src . ${flags}
	WORKING_DIRECTORY "${OUT}/src"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE output)
if(NOT result EQUAL 0 OR NOT EXISTS "${OUT}/src/${DAT}")
	message(FATAL_ERROR "fteqcc couldn't compile ${SRC} (${result}):\n${output}")
endif()
file(COPY_FILE "${OUT}/src/${DAT}" "${OUT}/${DAT}")
