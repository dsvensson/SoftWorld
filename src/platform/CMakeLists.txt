# The platform layers: the process entry points, the system services, and for
# the programs with a client the window, drawing, input and sound. Files named
# _posix are macOS's and Linux's both (and some the web's), and the systems' own
# halves of them _macos, _linux and _web: each system takes its own here, as
# each compiler takes its own of the CPU check's.

# the CPU check, and for x86-64-v3 and v4 CPUID as the compiler has it
set(SW_CPU_CHECK_SOURCES cpu/cpu_check_${SW_ARCH_SUFFIX}.c)
if(SW_ARCH MATCHES "^x86-64-v[34]$" AND MSVC)
	list(APPEND SW_CPU_CHECK_SOURCES cpu/cpuid_msvc.c)
elseif(SW_ARCH MATCHES "^x86-64-v[34]$")
	list(APPEND SW_CPU_CHECK_SOURCES cpu/cpuid_gnu.c)
endif()

# the CPU check alone, as the entry points build it, for tests/cpu_supported.c
add_library(sw_cpu_check OBJECT ${SW_CPU_CHECK_SOURCES})
target_include_directories(sw_cpu_check PUBLIC .)
set_target_properties(sw_cpu_check PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
sw_set_warnings(sw_cpu_check)

# the programs with a client sandboxed: macOS's App Sandbox, a Windows AppContainer
# (its Release builds); the game directory is one the player chooses
option(SW_SANDBOX "The programs with a client run sandboxed, in a game directory the player chooses" ON)

# the services of macOS and Linux (posix/), over what each system has
set(SW_POSIX_SOURCES
	posix/sys_posix.c posix/sys_memory_posix.c posix/sys_workers_posix.c posix/net_udp_posix.c posix/net_tcp_posix.c)

if(WIN32)
	# Process entry points and the CPU check: built without architecture flags and
	# outside link-time optimization so the check runs before any code that may use
	# instructions the CPU lacks.
	set(SW_ENTRY_gui win/entry_gui.c)
	set(SW_ENTRY_con entry_con.c)
	foreach(kind gui con)
		add_library(sw_entry_${kind} OBJECT
			${SW_ENTRY_${kind}}
			${SW_CPU_CHECK_SOURCES})
		target_include_directories(sw_entry_${kind} PRIVATE .)
		set_target_properties(sw_entry_${kind} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		sw_set_warnings(sw_entry_${kind})
	endforeach()

	# Windows services shared by every program
	sw_add_module(sw_platform_win
		win/sys_win.c win/sys_memory_win.c win/sys_win_workers.c win/net_udp_win.c win/net_tcp_win.c)
	target_link_libraries(sw_platform_win PUBLIC sw_net sw_common ws2_32 synchronization)

	set(SW_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	file(MAKE_DIRECTORY "${SW_SHADER_DIR}")
	sw_compile_hlsl("${SW_SHADER_DIR}/present_vs.h" "${CMAKE_CURRENT_SOURCE_DIR}/win/present.hlsl" vs_5_0 VSMain g_present_vs)
	sw_compile_hlsl("${SW_SHADER_DIR}/present_ps.h" "${CMAKE_CURRENT_SOURCE_DIR}/win/present.hlsl" ps_5_0 PSMain g_present_ps)

	# the window, drawing, input and sound of the programs with a client
	sw_add_module(sw_platform_gui
		vid_common.c
		win/sys_win_gui.c win/vid_d3d11.c win/in_rawinput.c win/in_xinput.c win/snd_wasapi.c
		"${SW_SHADER_DIR}/present_vs.h" "${SW_SHADER_DIR}/present_ps.h"
		# the AppContainer's launcher, but in Debug builds (a debugger sees the game)
		$<IF:$<AND:$<BOOL:${SW_SANDBOX}>,$<NOT:$<CONFIG:Debug>>>,win/sys_win_sandbox.c,win/sys_win_nosandbox.c>)
	target_include_directories(sw_platform_gui PRIVATE "${SW_SHADER_DIR}" . win)
	target_link_libraries(sw_platform_gui PUBLIC sw_client sw_simd sw_platform_win d3d11 dxgi dxguid ole32 avrt xinput
		userenv oleaut32 shell32 advapi32)

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
			${SW_CPU_CHECK_SOURCES})
		target_include_directories(sw_entry_${kind} PRIVATE .)
		set_target_properties(sw_entry_${kind} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		sw_set_warnings(sw_entry_${kind})
	endforeach()

	# the system services shared by every program: macOS's and Linux's, and macOS's own
	sw_add_module(sw_platform_posix
		${SW_POSIX_SOURCES}
		mac/sys_macos.c mac/sys_workers_macos.c)
	target_include_directories(sw_platform_posix PUBLIC posix PRIVATE "${SW_CPU_RELAX_DIR}")
	target_link_libraries(sw_platform_posix PUBLIC sw_net sw_common)

	# the console of the dedicated server
	sw_add_module(sw_platform_con
		posix/sys_con_posix.c)
	target_include_directories(sw_platform_con PRIVATE .)
	target_link_libraries(sw_platform_con PUBLIC sw_platform_posix)

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
	target_link_libraries(sw_platform_gui PUBLIC sw_client sw_simd sw_platform_posix
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
elseif(EMSCRIPTEN)
	# the web: the system services of a page, the POSIX ones Emscripten has and
	# the web's own (the name kept: the tests link it as macOS's and Linux's)
	sw_add_module(sw_platform_posix
		posix/sys_posix.c posix/sys_workers_posix.c
		web/sys_web.c web/sys_memory_web.c)
	target_include_directories(sw_platform_posix PUBLIC posix PRIVATE "${SW_CPU_RELAX_DIR}")
	target_compile_definitions(sw_platform_posix PRIVATE SW_MAX_WORKERS=${SW_WEB_WORKERS})
	target_link_libraries(sw_platform_posix PUBLIC sw_net sw_common)

	# the process entry point and the CPU check, built as on the other systems
	add_library(sw_entry_gui OBJECT web/entry_gui.c ${SW_CPU_CHECK_SOURCES})
	target_include_directories(sw_entry_gui PRIVATE .)
	set_target_properties(sw_entry_gui PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
	sw_set_warnings(sw_entry_gui)

	# the page, its drawing, input, sound and network: C files, and the
	# JavaScript libraries of the page's side of them; present.glsl inside the
	# program, compiled by WebGL
	set(SW_WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
	set(SW_WEB_LIBRARIES sys_web.js fs_web.js vid_webgl.js in_web.js snd_webaudio.js net_ws_web.js)
	sw_add_module(sw_platform_gui
		vid_common.c
		web/sys_web_gui.c web/vid_webgl.c web/present_glsl.c web/in_web.c web/in_gamepad_web.c
		web/snd_webaudio.c web/net_ws_web.c)
	set_source_files_properties(web/present_glsl.c PROPERTIES OBJECT_DEPENDS "${SW_WEB_DIR}/present.glsl")
	target_include_directories(sw_platform_gui PRIVATE . web)
	target_link_libraries(sw_platform_gui PUBLIC sw_client sw_simd sw_platform_posix)

	set(SW_HOST_DIR "${PROJECT_SOURCE_DIR}/src/host")

	# client with a local server, and the client alone: softworld.html, .js and
	# .wasm, and next to them the file system's fetch worker (softworld-fs.js)
	# and the sound's AudioWorklet (softworld-sound.js)
	set(SW_WEB_LINK_OPTIONS -sENVIRONMENT=web,worker -sCASE_INSENSITIVE_FS=1
		-sPTHREAD_POOL_SIZE=${SW_WEB_WORKERS} -sPTHREAD_POOL_SIZE_STRICT=2
		"SHELL:--shell-file ${SW_WEB_DIR}/shell.html")
	# the workers are copied as the programs are linked
	set(SW_WEB_LINK_DEPENDS "${SW_WEB_DIR}/shell.html" "${SW_WEB_DIR}/fs_web_worker.js" "${SW_WEB_DIR}/snd_worklet.js")
	foreach(library ${SW_WEB_LIBRARIES})
		list(APPEND SW_WEB_LINK_OPTIONS "SHELL:--js-library ${SW_WEB_DIR}/${library}")
		list(APPEND SW_WEB_LINK_DEPENDS "${SW_WEB_DIR}/${library}")
	endforeach()
	foreach(target softworld softworld-client)
		sw_add_program(${target}
			"${SW_HOST_DIR}/host.c"
			$<TARGET_OBJECTS:sw_entry_gui>)
		set_target_properties(${target} PROPERTIES SUFFIX ".html" LINK_DEPENDS "${SW_WEB_LINK_DEPENDS}")
		target_link_options(${target} PRIVATE ${SW_WEB_LINK_OPTIONS})
		target_link_libraries(${target} PRIVATE sw_platform_gui)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SW_WEB_DIR}/fs_web_worker.js"
				"$<TARGET_FILE_DIR:${target}>/softworld-fs.js"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SW_WEB_DIR}/snd_worklet.js"
				"$<TARGET_FILE_DIR:${target}>/softworld-sound.js"
			VERBATIM)
	endforeach()
	target_link_libraries(softworld PRIVATE sw_server)
	target_link_libraries(softworld-client PRIVATE sw_server_null)
else()
	# Linux: the process entry points and the CPU check, built as on Windows
	set(SW_ENTRY_gui linux/entry_gui.c)
	set(SW_ENTRY_con entry_con.c)
	foreach(kind gui con)
		add_library(sw_entry_${kind} OBJECT
			${SW_ENTRY_${kind}}
			${SW_CPU_CHECK_SOURCES})
		target_include_directories(sw_entry_${kind} PRIVATE .)
		set_target_properties(sw_entry_${kind} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		sw_set_warnings(sw_entry_${kind})
	endforeach()

	# the system services shared by every program: macOS's and Linux's, and Linux's own
	sw_add_module(sw_platform_posix
		${SW_POSIX_SOURCES}
		linux/sys_linux.c linux/sys_workers_linux.c)
	target_include_directories(sw_platform_posix PUBLIC posix PRIVATE "${SW_CPU_RELAX_DIR}")
	target_link_libraries(sw_platform_posix PUBLIC sw_net sw_common)

	# the console of the dedicated server
	sw_add_module(sw_platform_con
		posix/sys_con_posix.c)
	target_include_directories(sw_platform_con PRIVATE .)
	target_link_libraries(sw_platform_con PUBLIC sw_platform_posix)

	# Keep each backend's headers optional so either backend can build alone.
	option(SW_WAYLAND "Build the Wayland window backend" ON)
	option(SW_X11 "Build the X11 window backend" ON)
	if(NOT SW_WAYLAND AND NOT SW_X11)
		message(FATAL_ERROR "Enable at least one Linux window backend: SW_WAYLAND or SW_X11")
	endif()
	find_package(PkgConfig REQUIRED)
	include(glsl)
	find_package(Vulkan 1.3 REQUIRED)
	pkg_check_modules(SW_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

	set(SW_WINDOW_SOURCES "")
	set(SW_WINDOW_LIBRARIES "")
	if(SW_WAYLAND)
		include(wayland)
		pkg_check_modules(SW_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
		set(SW_PROTOCOL_SOURCES "")
		sw_wayland_protocols(SW_PROTOCOL_SOURCES
			stable/xdg-shell/xdg-shell.xml
			stable/viewporter/viewporter.xml
			stable/presentation-time/presentation-time.xml
			stable/linux-dmabuf/linux-dmabuf-v1.xml
			stable/tablet/tablet-v2.xml
			staging/fractional-scale/fractional-scale-v1.xml
			staging/color-management/color-management-v1.xml
			staging/content-type/content-type-v1.xml
			staging/cursor-shape/cursor-shape-v1.xml
			staging/xdg-activation/xdg-activation-v1.xml
			unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
			unstable/relative-pointer/relative-pointer-unstable-v1.xml
			unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
			unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
		add_library(sw_wayland_protocols OBJECT ${SW_PROTOCOL_SOURCES})
		target_include_directories(sw_wayland_protocols PUBLIC "${SW_PROTOCOL_DIR}")
		target_link_libraries(sw_wayland_protocols PUBLIC PkgConfig::SW_WAYLAND)
		list(APPEND SW_WINDOW_SOURCES linux/wl_linux.c linux/wl_hyprland.c linux/in_wayland.c
			$<TARGET_OBJECTS:sw_wayland_protocols>)
		list(APPEND SW_WINDOW_LIBRARIES PkgConfig::SW_WAYLAND PkgConfig::SW_XKBCOMMON)
	endif()
	if(SW_X11)
		pkg_check_modules(SW_XLIB REQUIRED IMPORTED_TARGET x11 xi xscrnsaver)
		list(APPEND SW_WINDOW_SOURCES linux/x11_linux.c linux/in_x11.c)
		list(APPEND SW_WINDOW_LIBRARIES PkgConfig::SW_XLIB)
	endif()

	set(SW_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	file(MAKE_DIRECTORY "${SW_SHADER_DIR}")
	sw_compile_glsl("${SW_SHADER_DIR}/present_vert.spv" "${CMAKE_CURRENT_SOURCE_DIR}/linux/present.glsl" vert)
	sw_compile_glsl("${SW_SHADER_DIR}/present_frag.spv" "${CMAKE_CURRENT_SOURCE_DIR}/linux/present.glsl" frag)
	add_library(sw_present_spirv OBJECT linux/present_spirv.c
		"${SW_SHADER_DIR}/present_vert.spv" "${SW_SHADER_DIR}/present_frag.spv")
	set_source_files_properties(linux/present_spirv.c PROPERTIES
		OBJECT_DEPENDS "${SW_SHADER_DIR}/present_vert.spv;${SW_SHADER_DIR}/present_frag.spv"
		COMPILE_OPTIONS "--embed-dir=${SW_SHADER_DIR}")
	target_include_directories(sw_present_spirv PRIVATE . "${PROJECT_SOURCE_DIR}/src/common"
		"${PROJECT_SOURCE_DIR}/src/renderer")
	sw_set_warnings(sw_present_spirv)

	# the window, drawing, input and sound of the programs with a client
	sw_add_module(sw_platform_gui
		vid_common.c
		linux/sys_linux_gui.c linux/vid_vulkan.c
		linux/in_evdev.c linux/snd_pipewire.c
		linux/window.c linux/in_linux.c
		$<TARGET_OBJECTS:sw_present_spirv> ${SW_WINDOW_SOURCES})
	target_include_directories(sw_platform_gui PRIVATE . linux "${SW_PROTOCOL_DIR}")
	target_link_libraries(sw_platform_gui PUBLIC sw_client sw_simd sw_platform_posix
		Vulkan::Vulkan PkgConfig::SW_PIPEWIRE ${SW_WINDOW_LIBRARIES})
	if(SW_WAYLAND)
		target_compile_definitions(sw_platform_gui PRIVATE SW_WAYLAND)
		add_dependencies(sw_platform_gui sw_wayland_protocols)
	endif()
	if(SW_X11)
		target_compile_definitions(sw_platform_gui PRIVATE SW_X11)
	endif()

	set(SW_HOST_DIR "${PROJECT_SOURCE_DIR}/src/host")

	# client with a local server, and the client alone
	foreach(target softworld softworld-client)
		sw_add_program(${target}
			"${SW_HOST_DIR}/host.c"
			$<TARGET_OBJECTS:sw_entry_gui>)
		target_link_libraries(${target} PRIVATE sw_platform_gui)
	endforeach()
	target_link_libraries(softworld PRIVATE sw_server)
	target_link_libraries(softworld-client PRIVATE sw_server_null)

	# dedicated server
	sw_add_program(softworld-server
		"${SW_HOST_DIR}/host_ded.c"
		$<TARGET_OBJECTS:sw_entry_con>)
	target_link_libraries(softworld-server PRIVATE sw_server sw_platform_con)
endif()
