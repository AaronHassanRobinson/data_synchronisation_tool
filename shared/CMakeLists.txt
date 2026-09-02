add_library(shared STATIC
        platform.c
        sha256.c
        hmac.c
        cdc.c
        cdcProtocol.c
        compress.c
        fileUtil.c
        protocol.c
        transport.c
        jsonUtil.c
        strmap.c)

target_include_directories(shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(shared PUBLIC ws2_32 bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(shared PUBLIC Threads::Threads)
endif()

# --- optional: zstd ---
if(SYNC_WITH_ZSTD)
    find_path(ZSTD_INCLUDE_DIR zstd.h)
    find_library(ZSTD_LIBRARY NAMES zstd)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
        message(STATUS "zstd: ${ZSTD_LIBRARY}")
        target_include_directories(shared PUBLIC ${ZSTD_INCLUDE_DIR})
        target_link_libraries(shared PUBLIC ${ZSTD_LIBRARY})
        target_compile_definitions(shared PUBLIC SYNC_HAVE_ZSTD=1)
    else()
        message(WARNING "zstd not found - building without message compression")
    endif()
endif()

# --- optional: OpenSSL for TLS ---
if(SYNC_WITH_TLS)
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
        target_link_libraries(shared PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(shared PUBLIC SYNC_HAVE_TLS=1)
    else()
        message(WARNING "OpenSSL not found - building WITHOUT transport encryption")
    endif()
endif()
