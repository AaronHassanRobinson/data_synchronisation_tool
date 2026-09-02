add_library(shared INTERFACE
        protocol.h
        protocol.c
        sha256.h
        sha256.c
        cdc.h
        cdc.c
        cdcProtocol.h
        cdcProtocol.c
        fileUtil.h
        fileUtil.c)

target_include_directories(shared INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
