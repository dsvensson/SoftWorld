sw_add_module(sw_render
	d_edge.c d_init.c d_modech.c d_part.c d_polyse.c d_scan.c d_sky.c d_sprite.c d_surf.c
	d_vars.c draw.c model.c
	r_aclip.c r_alias.c r_band.c r_bsp.c r_draw.c r_edge.c r_efrag.c r_fence.c r_fog.c r_image.c r_light.c r_main.c r_misc.c r_palette.c
	r_decal.c r_lightdata.c r_part.c r_partdraw.c r_partimage.c r_ring.c r_sky.c r_skybox.c r_sprite.c r_surf.c r_textures.c r_worldspawn.c wad.c)
target_link_libraries(sw_render PUBLIC sw_simd sw_common)
target_include_directories(sw_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
