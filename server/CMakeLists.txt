cmake_minimum_required(VERSION 3.16)
project(AdaptiveSyncServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Protobuf ────────────────────────────────────────────────
find_package(Protobuf REQUIRED)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../proto/adaptivesync.proto
)

# ─── OpenSSL (for SHA-256) ──────────────────────────────────
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
else()
    message(STATUS "OpenSSL not found — using embedded SHA-256 implementation")
endif()

# ─── Platform detection ─────────────────────────────────────
if(WIN32)
    set(PLATFORM_LIBS ws2_32 mswsock kernel32)
    set(PLATFORM_DEFS _WIN32_WINNT=0x0601)  # Windows 7+
elseif(UNIX)
    set(PLATFORM_LIBS pthread)
    set(PLATFORM_DEFS __linux__)
endif()

# ─── Server executable ──────────────────────────────────────
add_executable(adaptivesync_server
    src/main.cpp
    src/server.cpp
    src/session.cpp
    src/file_manager.cpp
    src/file_mutex.cpp
    src/negotiator.cpp
    src/zero_copy_sender.cpp
    src/delta_sync_handler.cpp
    ${PROTO_SRCS}
)

target_include_directories(adaptivesync_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}          # Generated protobuf headers
    ${Protobuf_INCLUDE_DIRS}
)

target_compile_definitions(adaptivesync_server PRIVATE
    ${PLATFORM_DEFS}
)

target_link_libraries(adaptivesync_server PRIVATE
    ${Protobuf_LIBRARIES}
    ${PLATFORM_LIBS}
    $<$<BOOL:${OpenSSL_FOUND}>:OpenSSL::Crypto>
)

# ─── Compiler warnings ──────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(adaptivesync_server PRIVATE -Wall -Wextra -Wpedantic -O2)
elseif(MSVC)
    target_compile_options(adaptivesync_server PRIVATE /W4 /O2)
endif()
