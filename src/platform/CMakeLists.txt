# Process entry points and the CPU check: built without architecture flags and
# outside link-time optimization so the check runs before any code that may use
# instructions the CPU lacks.
foreach(kind gui con)
	add_library(sw_entry_${kind} OBJECT
		win/entry_${kind}.c
		cpu/cpu_check_${SW_ARCH_SUFFIX}.c)
	target_include_directories(sw_entry_${kind} PRIVATE .)
	set_target_properties(sw_entry_${kind} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
	sw_set_warnings(sw_entry_${kind})
endforeach()

# Windows services shared by every program
sw_add_module(sw_platform_win
	win/sys_win.c win/net_udp_win.c)
target_link_libraries(sw_platform_win PUBLIC sw_net sw_common ws2_32)

set(SW_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SW_SHADER_DIR}")
sw_compile_hlsl("${SW_SHADER_DIR}/present_vs.h" "${CMAKE_CURRENT_SOURCE_DIR}/win/present.hlsl" vs_5_0 VSMain g_present_vs)
sw_compile_hlsl("${SW_SHADER_DIR}/present_ps.h" "${CMAKE_CURRENT_SOURCE_DIR}/win/present.hlsl" ps_5_0 PSMain g_present_ps)

# the window, drawing, input and sound of the programs with a client
sw_add_module(sw_platform_gui
	win/sys_win_gui.c win/vid_d3d11.c win/in_rawinput.c win/in_xinput.c win/snd_wasapi.c
	"${SW_SHADER_DIR}/present_vs.h" "${SW_SHADER_DIR}/present_ps.h")
target_include_directories(sw_platform_gui PRIVATE "${SW_SHADER_DIR}" win)
target_link_libraries(sw_platform_gui PUBLIC sw_client sw_platform_win d3d11 dxgi dxguid ole32 avrt xinput)

# the console of the dedicated server
sw_add_module(sw_platform_con
	win/sys_win_con.c)
target_include_directories(sw_platform_con PRIVATE win)
target_link_libraries(sw_platform_con PUBLIC sw_platform_win)

set(SW_HOST_DIR "${PROJECT_SOURCE_DIR}/src/host")

# client with a local server, and the client alone
foreach(target softworld softworld-client)
	sw_add_program(${target} WIN32
		"${SW_HOST_DIR}/host.c"
		win/softworld.rc win/softworld.manifest
		$<TARGET_OBJECTS:sw_entry_gui>)
	target_link_libraries(${target} PRIVATE sw_platform_gui)
endforeach()
target_link_libraries(softworld PRIVATE sw_server)
target_link_libraries(softworld-client PRIVATE sw_server_null)

# dedicated server
sw_add_program(softworld-server
	"${SW_HOST_DIR}/host_ded.c"
	win/softworld.rc win/softworld.manifest
	$<TARGET_OBJECTS:sw_entry_con>)
target_link_libraries(softworld-server PRIVATE sw_server sw_platform_con)

# The server loads qwprogs.dat from <exe dir>/qw.
foreach(target softworld softworld-server)
	sw_copy_sanitizer_runtime(${target})
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/qw"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${PROJECT_SOURCE_DIR}/qw-qc/qwprogs.dat" "$<TARGET_FILE_DIR:${target}>/qw/qwprogs.dat")
endforeach()
