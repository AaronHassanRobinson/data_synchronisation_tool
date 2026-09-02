add_executable(sync_tests unit_tests.c)
target_link_libraries(sync_tests PRIVATE client_core server_core)

add_test(NAME unit COMMAND sync_tests)

# End-to-end: real binaries, real sockets, TLS, compression, watcher, reconnect.
if(NOT WIN32)
    add_test(NAME integration
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration.sh $<TARGET_FILE:sync_server> $<TARGET_FILE:sync_client>)
    set_tests_properties(integration PROPERTIES TIMEOUT 300)
endif()
