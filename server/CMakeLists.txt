add_executable(sync_server serverMain.c
        ../shared/protocol.c
        ../shared/list.c
        ../shared/list.h)


target_link_libraries(sync_server PRIVATE shared)