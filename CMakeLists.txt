cmake_minimum_required(VERSION 3.28)

project(SoftWorld VERSION 0.1.0 LANGUAGES C RC)

if(NOT MSVC)
	message(FATAL_ERROR "SoftWorld currently builds with MSVC or clang-cl on Windows")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(arch)
include(hlsl)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(CheckIPOSupported)
check_ipo_supported(RESULT sw_ipo_supported OUTPUT sw_ipo_output LANGUAGES C)
if(sw_ipo_supported)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
else()
	message(WARNING "Link-time optimization is not available: ${sw_ipo_output}")
endif()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/utf-8 /permissive-)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
	# keep scalar and SIMD floating-point results identical
	add_compile_options(/clang:-ffp-contract=off)
endif()

set(SW_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Process entry points and the CPU check: no architecture flags, no LTO.
add_library(sw_entry_gui OBJECT
	src/platform/win/entry_gui.c
	src/platform/cpu/cpu_check_${SW_ARCH_SUFFIX}.c)
add_library(sw_entry_con OBJECT
	src/platform/win/entry_con.c
	src/platform/cpu/cpu_check_${SW_ARCH_SUFFIX}.c)
foreach(entry sw_entry_gui sw_entry_con)
	set_target_properties(${entry} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
	sw_set_warnings(${entry})
endforeach()

set(SW_CLIENT_SOURCES
	cl_cam.c cl_demo.c cl_ents.c cl_input.c cl_main.c cl_parse.c cl_pred.c cl_tent.c
	cmd.c common.c console.c crc.c cvar.c
	d_edge.c d_fill.c d_init.c d_modech.c d_part.c d_polyse.c d_scan.c d_sky.c d_sprite.c
	d_surf.c d_vars.c d_zpoint.c draw.c in_win.c keys.c mathlib.c md4.c menu.c model.c
	net_chan.c net_wins.c pmove.c pmovetst.c
	r_aclip.c r_alias.c r_bsp.c r_draw.c r_edge.c r_efrag.c r_light.c r_main.c r_misc.c
	r_part.c r_sky.c r_sprite.c r_surf.c r_vars.c
	sbar.c screen.c skin.c snd_dma.c snd_mem.c snd_mix.c snd_win.c sys_win.c view.c wad.c zone.c)
list(TRANSFORM SW_CLIENT_SOURCES PREPEND "src/client/")

set(SW_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SW_SHADER_DIR}")
sw_compile_hlsl("${SW_SHADER_DIR}/present_vs.h" "${SW_SRC}/platform/win/present.hlsl" vs_5_0 VSMain g_present_vs)
sw_compile_hlsl("${SW_SHADER_DIR}/present_ps.h" "${SW_SRC}/platform/win/present.hlsl" ps_5_0 PSMain g_present_ps)

add_executable(softworld WIN32
	${SW_CLIENT_SOURCES}
	"${SW_SHADER_DIR}/present_vs.h"
	"${SW_SHADER_DIR}/present_ps.h"
	src/platform/win/vid_d3d11.c
	src/platform/win/softworld.rc
	src/platform/win/softworld.manifest
	$<TARGET_OBJECTS:sw_entry_gui>)
target_include_directories(softworld PRIVATE src/client "${SW_SHADER_DIR}")
target_link_libraries(softworld PRIVATE d3d11 dxgi dxguid ws2_32 winmm dsound)
sw_set_warnings(softworld)
sw_set_arch(softworld)

set(SW_SERVER_SHARED_SOURCES
	cmd.c common.c crc.c cvar.c mathlib.c md4.c net_chan.c net_wins.c pmove.c pmovetst.c zone.c)
list(TRANSFORM SW_SERVER_SHARED_SOURCES PREPEND "src/client/")
set(SW_SERVER_SOURCES
	model.c pr_cmds.c pr_edict.c pr_exec.c sv_ccmds.c sv_ents.c sv_init.c sv_main.c sv_move.c
	sv_nchan.c sv_phys.c sv_send.c sv_user.c sys_win.c world.c)
list(TRANSFORM SW_SERVER_SOURCES PREPEND "src/server/")

add_executable(softworld-server
	${SW_SERVER_SOURCES}
	${SW_SERVER_SHARED_SOURCES}
	src/platform/win/softworld.rc
	src/platform/win/softworld.manifest
	$<TARGET_OBJECTS:sw_entry_con>)
target_include_directories(softworld-server PRIVATE src/server src/client)
target_compile_definitions(softworld-server PRIVATE SERVERONLY)
target_link_libraries(softworld-server PRIVATE ws2_32 winmm)
sw_set_warnings(softworld-server)
sw_set_arch(softworld-server)

# The listen/dedicated server loads qwprogs.dat from <exe dir>/qw.
foreach(target softworld softworld-server)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/qw"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_CURRENT_SOURCE_DIR}/qw-qc/qwprogs.dat" "$<TARGET_FILE_DIR:${target}>/qw/qwprogs.dat")
endforeach()
