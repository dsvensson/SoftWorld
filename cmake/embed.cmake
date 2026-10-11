# cmake -DIN=<file> -DOUT=<file.c> -DSYMBOL=<name> -P embed.cmake
#
# A file as a C array, SYMBOL and SYMBOL_size: a compiled QuakeC program, the
# program's own copy, used when the game directory has none (qwprogs.dat in
# progs.h, menu.dat in menu.h), or the CRT shader's LUTs (vid_crt.c).

get_filename_component(name "${IN}" NAME)
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,))"
	"\\1\n\t" bytes "${bytes}")
file(WRITE "${OUT}"
"// ${name}, the one the program was built with (cmake/embed.cmake)

#include <stddef.h>

const unsigned char	${SYMBOL}[${size}] = {
	${bytes}
};
const size_t	${SYMBOL}_size = ${size};
")
