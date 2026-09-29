# The platform layers: the process entry points, the system services, and for
# the programs with a client the window, drawing, input and sound.

if(WIN32)
	# Process entry points and the CPU check: built without architecture flags and
	# outside link-time optimization so the check runs before any code that may use
	# instructions the CPU lacks.
	set(SW_ENTRY_gui win/entry_gui.c)
	set(SW_ENTRY_con entry_con.c)
	foreach(kind gui con)
		add_library(sw_entry_${kind} OBJECT
			${SW_ENTRY_${kind}}
			cpu/cpu_check_${SW_ARCH_SUFFIX}.c)
		target_include_directories(sw_entry_${kind} PRIVATE .)
		set_target_properties(sw_entry_${kind} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		sw_set_warnings(sw_entry_${kind})
	endforeach()

	# Windows services shared by every program
	sw_add_module(sw_platform_win
		win/sys_win.c win/sys_win_workers.c win/net_udp_win.c win/net_tcp_win.c)
	target_link_libraries(sw_platform_win PUBLIC sw_net sw_common ws2_32 synchronization)

	set(SW_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	file(MAKE_DIRECTORY "${SW_SHADER_DIR}")
	sw_compile_hlsl("${SW_SHADER_DIR}/present_vs.h" "${CMAKE_CURRENT_SOURCE_DIR}/win/present.hlsl" vs_5_0 VSMain g_present_vs)
	sw_compile_hlsl("${SW_SHADER_DIR}/present_ps.h" "${CMAKE_CURRENT_SOURCE_DIR}/win/present.hlsl" ps_5_0 PSMain g_present_ps)

	# the window, drawing, input and sound of the programs with a client
	sw_add_module(sw_platform_gui
		vid_common.c
		win/sys_win_gui.c win/vid_d3d11.c win/in_rawinput.c win/in_xinput.c win/snd_wasapi.c
		"${SW_SHADER_DIR}/present_vs.h" "${SW_SHADER_DIR}/present_ps.h")
	target_include_directories(sw_platform_gui PRIVATE "${SW_SHADER_DIR}" . win)
	target_link_libraries(sw_platform_gui PUBLIC sw_client sw_simd sw_platform_win d3d11 dxgi dxguid ole32 avrt xinput)

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
		# in the App Sandbox: the game directory is one the player chooses
		if(SW_SANDBOX)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND codesign --force --sign - --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/mac/softworld.entitlements"
					"$<TARGET_BUNDLE_DIR:${target}>"
				COMMENT "Signing ${target}.app for the App Sandbox"
				VERBATIM)
		endif()
	endforeach()
	target_link_libraries(softworld PRIVATE sw_server)
	target_link_libraries(softworld-client PRIVATE sw_server_null)

	# dedicated server
	sw_add_program(softworld-server
		"${SW_HOST_DIR}/host_ded.c"
		win/softworld.rc win/softworld.manifest
		$<TARGET_OBJECTS:sw_entry_con>)
	target_link_libraries(softworld-server PRIVATE sw_server sw_platform_con)

	foreach(target softworld softworld-server)
		sw_copy_sanitizer_runtime(${target})
	endforeach()
elseif(APPLE)
	# the process entry points and the CPU check, built as on Windows
	set(SW_ENTRY_gui mac/entry_gui.c)
	set(SW_ENTRY_con entry_con.c)
	foreach(kind gui con)
		add_library(sw_entry_${kind} OBJECT
			${SW_ENTRY_${kind}}
			cpu/cpu_check_${SW_ARCH_SUFFIX}.c)
		target_include_directories(sw_entry_${kind} PRIVATE .)
		set_target_properties(sw_entry_${kind} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		sw_set_warnings(sw_entry_${kind})
	endforeach()

	# macOS services shared by every program
	sw_add_module(sw_platform_mac
		mac/sys_mac.c mac/sys_mac_workers.c mac/net_udp_mac.c mac/net_tcp_mac.c)
	target_include_directories(sw_platform_mac PRIVATE mac)
	target_link_libraries(sw_platform_mac PUBLIC sw_net sw_common)

	# the console of the dedicated server
	sw_add_module(sw_platform_con
		mac/sys_mac_con.c)
	target_include_directories(sw_platform_con PRIVATE . mac)
	target_link_libraries(sw_platform_con PUBLIC sw_platform_mac)

	option(SW_SANDBOX "The applications run in the App Sandbox, in a game directory the player chooses" ON)

	# the shaders, compiled at build time and carried inside the programs
	include(metal)
	set(SW_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	file(MAKE_DIRECTORY "${SW_SHADER_DIR}")
	sw_compile_metal("${SW_SHADER_DIR}/present.metallib" "${CMAKE_CURRENT_SOURCE_DIR}/mac/present.metal")
	add_library(sw_present_metallib OBJECT mac/present_metallib.c "${SW_SHADER_DIR}/present.metallib")
	set_source_files_properties(mac/present_metallib.c PROPERTIES
		OBJECT_DEPENDS "${SW_SHADER_DIR}/present.metallib"
		COMPILE_OPTIONS "--embed-dir=${SW_SHADER_DIR}")
	sw_set_warnings(sw_present_metallib)

	# the window, drawing, input and sound of the programs with a client
	sw_add_module(sw_platform_gui
		vid_common.c
		mac/sys_mac_gui.m mac/vid_metal.m mac/in_mac.m mac/in_gamepad_mac.m mac/snd_coreaudio.c
		$<TARGET_OBJECTS:sw_present_metallib>)
	target_include_directories(sw_platform_gui PRIVATE . mac)
	target_link_libraries(sw_platform_gui PUBLIC sw_client sw_simd sw_platform_mac
		"-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework AudioToolbox"
		"-framework Carbon" "-framework GameController")

	# the application's icon, from the Windows one
	set(SW_ICON "${CMAKE_CURRENT_BINARY_DIR}/softworld.icns")
	add_custom_command(OUTPUT "${SW_ICON}"
		COMMAND sips -s format icns "${CMAKE_CURRENT_SOURCE_DIR}/win/softworld.ico" --out "${SW_ICON}"
		MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/win/softworld.ico"
		COMMENT "Converting the icon"
		VERBATIM)
	set_source_files_properties("${SW_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

	set(SW_HOST_DIR "${PROJECT_SOURCE_DIR}/src/host")

	# client with a local server, and the client alone: applications, which run
	# from a terminal too (softworld.app/Contents/MacOS/softworld)
	foreach(target softworld softworld-client)
		sw_add_program(${target} MACOSX_BUNDLE
			"${SW_HOST_DIR}/host.c"
			"${SW_ICON}"
			$<TARGET_OBJECTS:sw_entry_gui>)
		set_target_properties(${target} PROPERTIES
			MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
			MACOSX_BUNDLE_BUNDLE_NAME "SoftWorld"
			MACOSX_BUNDLE_GUI_IDENTIFIER "org.softworld.${target}"
			MACOSX_BUNDLE_ICON_FILE "softworld.icns"
			MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
			MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
		target_link_libraries(${target} PRIVATE sw_platform_gui)
		# in the App Sandbox: the game directory is one the player chooses
		if(SW_SANDBOX)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND codesign --force --sign - --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/mac/softworld.entitlements"
					"$<TARGET_BUNDLE_DIR:${target}>"
				COMMENT "Signing ${target}.app for the App Sandbox"
				VERBATIM)
		endif()
	endforeach()
	target_link_libraries(softworld PRIVATE sw_server)
	target_link_libraries(softworld-client PRIVATE sw_server_null)

	# dedicated server
	sw_add_program(softworld-server
		"${SW_HOST_DIR}/host_ded.c"
		$<TARGET_OBJECTS:sw_entry_con>)
	target_link_libraries(softworld-server PRIVATE sw_server sw_platform_con)
endif()
