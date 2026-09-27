# cmake -DFTEQCC=<fteqcc> -DSRC=<qw-qc> -DOUT=<dir> -P fteqcc.cmake
#
# Compiles qw-qc into OUT/qwprogs.dat. fteqcc writes where progs.src says,
# beside it, so it compiles a copy in OUT/src and the source tree is left
# alone. Quiet unless it fails: id's QuakeC has warnings aplenty.

file(REMOVE_RECURSE "${OUT}/src")
file(MAKE_DIRECTORY "${OUT}/src")
file(GLOB sources "${SRC}/*.qc" "${SRC}/progs.src")
file(COPY ${sources} DESTINATION "${OUT}/src")

# from inside the copy: fteqcc keeps the paths it was given in the progs, so
# the same qw-qc gives the same qwprogs.dat wherever it is built (and given a
# / path, it takes progs.src's "./" from the root of the drive)
execute_process(COMMAND "${FTEQCC}" -src .
	WORKING_DIRECTORY "${OUT}/src"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE output)
if(NOT result EQUAL 0 OR NOT EXISTS "${OUT}/src/qwprogs.dat")
	message(FATAL_ERROR "fteqcc couldn't compile qw-qc (${result}):\n${output}")
endif()
file(COPY_FILE "${OUT}/src/qwprogs.dat" "${OUT}/qwprogs.dat")
