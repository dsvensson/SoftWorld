# The Wayland protocols the Linux client speaks: each protocol's XML, from
# wayland-protocols, made into a client header and its interfaces' code by
# wayland-scanner, in <build>/protocols.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SW_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
# 1.41: color-management-v1
pkg_check_modules(SW_WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.41)
pkg_get_variable(SW_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(SW_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED DOC "wayland-scanner, from wayland")

set(SW_PROTOCOL_DIR "${CMAKE_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${SW_PROTOCOL_DIR}")

# sw_wayland_protocols(<sources variable> <xml, from wayland-protocols' directory>...)
# Appends the generated code to the variable; the headers are in SW_PROTOCOL_DIR.
function(sw_wayland_protocols out)
	set(sources ${${out}})
	foreach(xml ${ARGN})
		get_filename_component(name "${xml}" NAME_WE)
		set(path "${SW_WAYLAND_PROTOCOLS_DIR}/${xml}")
		add_custom_command(
			OUTPUT "${SW_PROTOCOL_DIR}/${name}.h" "${SW_PROTOCOL_DIR}/${name}.c"
			COMMAND "${SW_WAYLAND_SCANNER}" client-header "${path}" "${SW_PROTOCOL_DIR}/${name}.h"
			COMMAND "${SW_WAYLAND_SCANNER}" private-code "${path}" "${SW_PROTOCOL_DIR}/${name}.c"
			MAIN_DEPENDENCY "${path}"
			COMMENT "Generating the ${name} protocol"
			VERBATIM)
		list(APPEND sources "${SW_PROTOCOL_DIR}/${name}.c" "${SW_PROTOCOL_DIR}/${name}.h")
	endforeach()
	set(${out} ${sources} PARENT_SCOPE)
endfunction()
