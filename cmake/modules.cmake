# sw_add_module(<name> <sources>...)
# Adds a static library for one engine module with the project's warning and
# architecture settings. Dependencies and include directories are added by the caller.
function(sw_add_module name)
	add_library(${name} STATIC ${ARGN})
	sw_set_warnings(${name})
	sw_set_arch(${name})
endfunction()

# sw_add_program(<name> [WIN32] <sources>...)
# Adds an executable with the project's warning and architecture settings.
function(sw_add_program name)
	add_executable(${name} ${ARGN})
	sw_set_warnings(${name})
	sw_set_arch(${name})
endfunction()
