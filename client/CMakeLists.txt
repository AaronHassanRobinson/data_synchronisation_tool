add_executable(sync_client clientMain.c
        ../shared/protocol.c
        ../shared/sha256.c
        ../shared/cdc.c
        ../shared/cdcProtocol.c
        ../shared/fileUtil.c)

target_link_libraries(sync_client PRIVATE shared)
