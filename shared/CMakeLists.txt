add_library(shared INTERFACE
        protocol.h
        protocol.c
        list.h
        list.c)

target_include_directories(shared INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)