add_library(server_core STATIC
        chunkStore.c
        serverConfig.c
        serverSession.c)
target_include_directories(server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(server_core PUBLIC shared)

add_executable(sync_server serverMain.c)
target_link_libraries(sync_server PRIVATE server_core)
