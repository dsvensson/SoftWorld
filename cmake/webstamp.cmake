# webstamp.cmake -- a web program's build named in its page, in place of the
# word @SW_BUILD@: a hash of the files the page loads (the program's .js and
# .wasm, and its workers), which the page asks for them by (shell.html), so a
# browser's cache never hands it another build's. Run after each link, which
# writes the page anew:
#
#   cmake -DDIR=<the build's directory> -DNAME=<the program> -P webstamp.cmake

foreach(file "${NAME}.js" "${NAME}.wasm" softworld-fs.js softworld-sound.js)
	file(SHA256 "${DIR}/${file}" hash)
	string(APPEND hashes "${hash}")
endforeach()
string(SHA256 stamp "${hashes}")
string(SUBSTRING "${stamp}" 0 16 stamp)

file(READ "${DIR}/${NAME}.html" page)
string(FIND "${page}" "@SW_BUILD@" at)
if(at EQUAL -1)
	message(FATAL_ERROR "${NAME}.html has no @SW_BUILD@ to name its build in")
endif()
string(REPLACE "@SW_BUILD@" "${stamp}" page "${page}")
file(WRITE "${DIR}/${NAME}.html" "${page}")
