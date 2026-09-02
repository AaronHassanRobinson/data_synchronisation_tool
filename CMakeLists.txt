cmake_minimum_required(VERSION 3.20)
project(data_synchronisation_tool C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(SYNC_WITH_ZSTD "Compress messages with zstd when the library is available" ON)
option(SYNC_WITH_TLS "Encrypt the transport with TLS (OpenSSL) when available" ON)
option(SYNC_BUILD_TESTS "Build the unit + integration tests" ON)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra)
endif()

# Homebrew keeps OpenSSL/zstd out of the default search path on macOS.
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3 /opt/homebrew/opt/zstd /usr/local/opt/openssl@3 /usr/local/opt/zstd)
endif()

add_subdirectory(shared)
add_subdirectory(client)
add_subdirectory(server)

if(SYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
