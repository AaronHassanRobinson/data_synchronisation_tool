add_executable(sync_server serverMain.c
        ../shared/protocol.c
        ../shared/sha256.c
        ../shared/cdc.c
        ../shared/cdcProtocol.c
        ../shared/fileUtil.c)

target_link_libraries(sync_server PRIVATE shared)
