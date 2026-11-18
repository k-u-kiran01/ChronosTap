cmake_minimum_required(VERSION 3.20)
project(chronostap LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(OpenSSL REQUIRED)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

# 1. Compile eBPF programs & generate libbpf skeletons
foreach(BPF_PROG recorder redirector)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/generated/${BPF_PROG}.skel.h
        COMMAND clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I${CMAKE_SOURCE_DIR}/bpf
                -c ${CMAKE_SOURCE_DIR}/bpf/${BPF_PROG}.bpf.c
                -o ${CMAKE_BINARY_DIR}/generated/${BPF_PROG}.bpf.o
        COMMAND bpftool gen skeleton ${CMAKE_BINARY_DIR}/generated/${BPF_PROG}.bpf.o
                > ${CMAKE_BINARY_DIR}/generated/${BPF_PROG}.skel.h
        DEPENDS ${CMAKE_SOURCE_DIR}/bpf/${BPF_PROG}.bpf.c ${CMAKE_SOURCE_DIR}/bpf/common.h
        COMMENT "[eBPF] Compiling and generating skeleton for ${BPF_PROG}.bpf.c"
    )
endforeach()

# 2. Generate Bison Parser & Flex Scanner for SQL AST Canonicalizer
BISON_TARGET(SqlParser src/sql_ast/sql_parser.y ${CMAKE_BINARY_DIR}/generated/sql_parser.tab.cpp
             DEFINES_FILE ${CMAKE_BINARY_DIR}/generated/sql_parser.tab.hpp)
FLEX_TARGET(SqlLexer src/sql_ast/sql_lexer.l ${CMAKE_BINARY_DIR}/generated/sql_lexer.yy.cpp)
ADD_FLEX_BISON_DEPENDENCY(SqlLexer SqlParser)

# 3. Build main C++20 chronostap executable
add_executable(chronostap
    src/cli/main.cpp
    src/protocol/pg_wire.cpp
    src/protocol/http_parser.cpp
    src/sql_ast/canonicalizer.cpp
    src/storage/cas_store.cpp
    src/replay/epoll_mock_server.cpp
    src/diff/regression_engine.cpp
    ${CMAKE_BINARY_DIR}/generated/recorder.skel.h
    ${CMAKE_BINARY_DIR}/generated/redirector.skel.h
    ${BISON_SqlParser_OUTPUTS}
    ${FLEX_SqlLexer_OUTPUTS}
)

target_include_directories(chronostap PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/bpf
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(chronostap PRIVATE bpf elf z OpenSSL::Crypto pthread)

enable_testing()
add_executable(test_ast
    tests/unit/test_ast.cpp
    src/sql_ast/canonicalizer.cpp
    ${BISON_SqlParser_OUTPUTS}
    ${FLEX_SqlLexer_OUTPUTS}
)
target_include_directories(test_ast PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)
add_test(NAME SqlAstCanonicalizerTest COMMAND test_ast)
