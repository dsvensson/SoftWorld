# cmake -DIN=<qwprogs.dat> -DOUT=<qwprogs_data.c> -P embed.cmake
#
# qwprogs.dat as a C array: the server's game inside the program (progs.h).

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,))"
	"\\1\n\t" bytes "${bytes}")
file(WRITE "${OUT}"
"// qwprogs.dat, the one the program was built with (cmake/embed.cmake)

#include <stddef.h>

const unsigned char	sv_qwprogs[${size}] = {
	${bytes}
};
const size_t	sv_qwprogs_size = ${size};
")
