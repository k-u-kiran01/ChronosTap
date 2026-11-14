cmake_minimum_required(VERSION 3.20)
project(chronostap LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(OpenSSL REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/bpf)

# Placeholder executable target
add_executable(chronostap
    src/cli/main.cpp
    src/capture/ringbuf_consumer.cpp
    src/protocol/http_parser.cpp
    src/protocol/pg_wire.cpp
    src/sql_ast/canonicalizer.cpp
    src/storage/cas_store.cpp
    src/replay/epoll_mock_server.cpp
    src/replay/http_replayer.cpp
    src/diff/regression_engine.cpp
)

target_link_libraries(chronostap PRIVATE bpf elf z OpenSSL::Crypto pthread)
