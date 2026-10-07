# Engine modules. Each directory builds one static library; the programs at the
# bottom are links of the modules they need plus a platform layer.

add_subdirectory(common)
add_subdirectory(qcvm)
add_subdirectory(net)
add_subdirectory(slist)
add_subdirectory(game)
add_subdirectory(simd)
add_subdirectory(server)
add_subdirectory(renderer)
add_subdirectory(sound)
add_subdirectory(client)
add_subdirectory(platform)
