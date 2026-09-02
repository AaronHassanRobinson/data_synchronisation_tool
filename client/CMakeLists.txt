add_executable(sync_client clientMain.c
                    ../shared/protocol.c
        ../shared/list.c
        ../shared/list.h)


target_link_libraries(sync_client PRIVATE shared)