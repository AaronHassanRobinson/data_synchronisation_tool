set(WATCHER_SOURCES watcher.c watcher_poll.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND WATCHER_SOURCES watcher_inotify.c)
elseif(WIN32)
    list(APPEND WATCHER_SOURCES watcher_win32.c)
endif()

add_library(client_core STATIC
        config.c
        db.c
        eventQueue.c
        scanner.c
        session.c
        sync.c
        ${WATCHER_SOURCES})
target_include_directories(client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(client_core PUBLIC shared)

add_executable(sync_client clientMain.c)
target_link_libraries(sync_client PRIVATE client_core)
